#!/usr/bin/env python3
"""车库门网关：多设备 VPS 控制台 + ESP32 轮询 + HTTPS 管理 API。

技术选型（有意做小）：
  - Python 3 标准库 only（VPS Python 3.6 兼容；零 pip）
  - 每设备独立 pending / update / 状态 / 日志（id=garage-xxxx）
  - 指令 TTL 8s；update 粘性 600s
  - HTTPS 由前面 Nginx 终结

对外：
  设备（HTTP 明文，nginx 放行）：
    GET  /dev/poll?id=&fw=
    POST /dev/logs?id=
    POST /dev/status
    GET  /ota/version | /ota/firmware.bin
  管理（HTTPS + token）：
    POST /api/login
    GET  /api/devices | /api/devices/{id} | /api/devices/{id}/logs
    POST /api/devices/{id}/open|close|update
    POST /api/open|close|update          # 不带 id → 主门
    GET  /api/ota
    POST /api/ota/upload?version=        # raw firmware.bin
  兼容：/xiaoai/* /mcp /health /
"""
from __future__ import print_function

import json
import os
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn
from urllib.parse import parse_qs, unquote, urlparse

try:
    from http.server import ThreadingHTTPServer  # py3.7+
except ImportError:
    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True


HOST = "127.0.0.1"
PORT = 18080

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.join(BASE_DIR, "web")
OTA_DIR = os.path.join(BASE_DIR, "ota")
LOG_DIR = os.path.join(BASE_DIR, "logs")
UI_PASSWORD_FILE = os.path.join(BASE_DIR, "ui_password")

_ui_tokens = set()
_ui_lock = threading.Lock()

MCP_SERVER_NAME = "garage-gate"
MCP_SERVER_VERSION = "0.2.0"
MCP_PROTOCOL_DEFAULT = "2024-11-05"
MCP_PROTOCOL_KNOWN = ("2025-03-26", "2024-11-05", "2024-10-07")

PENDING_TTL_S = 8.0
UPDATE_TTL_S = 600.0
UPDATE_NOTIFY_GAP_S = 300.0
MIN_SET_GAP = 2.0
ONLINE_S = 45.0  # 超过则列表显示离线
DEFAULT_DEVICE = "default"  # 兼容旧固件 / 不带 id 的主门

LOG_LINES = []
_lock = threading.Lock()
_devices = {}  # id -> device dict

try:
    from queue import Queue, Empty
except ImportError:
    from Queue import Queue, Empty

_sse_lock = threading.Lock()
_sse_queues = {}


def _log(msg):
    line = time.strftime("%Y-%m-%d %H:%M:%S") + " " + msg
    print(line, flush=True)
    LOG_LINES.append(line)
    if len(LOG_LINES) > 200:
        del LOG_LINES[:-200]


def _ui_password():
    try:
        with open(UI_PASSWORD_FILE, "r") as f:
            p = f.read().strip()
            if p:
                return p
    except Exception:
        pass
    return os.environ.get("GARAGE_UI_PASSWORD", "garage")


def _check_ui_token(token):
    if not token:
        return False
    with _ui_lock:
        return token in _ui_tokens


def _now():
    return time.time()


def _new_device(dev_id):
    return {
        "id": dev_id,
        "fw": "",
        "role": "door" if dev_id != "lab" else "lab",
        "name": "",
        "last_seen": 0.0,
        "status": {},
        "pending": None,
        "pending_ts": 0.0,
        "last_set_ts": 0.0,
        "update_sticky": False,
        "update_ts": 0.0,
        "update_notify_ts": 0.0,
    }


def _device_locked(dev_id, create=True):
    d = _devices.get(dev_id)
    if d is None and create:
        d = _new_device(dev_id)
        _devices[dev_id] = d
    return d


def _touch_locked(d, fw=None):
    d["last_seen"] = _now()
    if fw:
        d["fw"] = fw


def _is_online(d, now=None):
    now = now if now is not None else _now()
    return (now - d.get("last_seen", 0)) <= ONLINE_S


def _expire_pending_locked(d):
    if d["pending"] is None:
        return None
    age = _now() - d["pending_ts"]
    if age > PENDING_TTL_S:
        expired = d["pending"]
        d["pending"] = None
        d["pending_ts"] = 0.0
        return expired
    return None


def set_pending(cmd, dev_id=None):
    if cmd == "update":
        return request_update("api", dev_id)
    dev_id = dev_id or DEFAULT_DEVICE
    now = _now()
    with _lock:
        d = _device_locked(dev_id)
        stale = _expire_pending_locked(d)
        if stale:
            _log("[%s] pending expired before set: %s" % (dev_id, stale))
        if now - d["last_set_ts"] < MIN_SET_GAP:
            return False, "debounce"
        d["pending"] = cmd
        d["pending_ts"] = now
        d["last_set_ts"] = now
        return True, "ok"


def request_update(reason="api", dev_id=None):
    dev_id = dev_id or DEFAULT_DEVICE
    now = _now()
    with _lock:
        d = _device_locked(dev_id)
        if d["update_sticky"]:
            return True, "already"
        if reason == "auto" and (now - d["update_notify_ts"]) < UPDATE_NOTIFY_GAP_S:
            return False, "cooldown"
        d["update_sticky"] = True
        d["update_ts"] = now
        if reason == "auto":
            d["update_notify_ts"] = now
        _log("[%s] update requested (%s)" % (dev_id, reason))
        return True, "ok"


def _maybe_auto_update_locked(d, device_fw):
    if not device_fw:
        return False
    try:
        remote = _ota_version_info().get("version") or ""
    except Exception:
        remote = ""
    # 只升级「更旧 → 更新」；相等或本地更新都不动（防降级）
    if not remote or device_fw == remote or device_fw >= remote:
        return False
    now = _now()
    if d["update_sticky"]:
        return False
    if (now - d["update_notify_ts"]) < UPDATE_NOTIFY_GAP_S:
        return False
    d["update_sticky"] = True
    d["update_ts"] = now
    d["update_notify_ts"] = now
    _log("[%s] auto update fw=%s -> %s" % (d["id"], device_fw, remote))
    return True


def take_pending(dev_id=None, device_fw=None):
    dev_id = dev_id or DEFAULT_DEVICE
    now = _now()
    with _lock:
        d = _device_locked(dev_id)
        _touch_locked(d, device_fw)
        expired = _expire_pending_locked(d)
        if expired:
            _log("[%s] pending expired unclaimed: %s" % (dev_id, expired))
        cmd = d["pending"]
        d["pending"] = None
        if cmd:
            return cmd
        if d["update_sticky"]:
            if now - d["update_ts"] > UPDATE_TTL_S:
                d["update_sticky"] = False
                _log("[%s] update sticky expired" % dev_id)
            else:
                d["update_sticky"] = False
                return "update"
        if device_fw and _maybe_auto_update_locked(d, device_fw):
            d["update_sticky"] = False
            return "update"
        return None


def update_device_status(dev_id, status):
    with _lock:
        d = _device_locked(dev_id)
        _touch_locked(d, (status or {}).get("fw") or d.get("fw"))
        if isinstance(status, dict):
            old = d.get("status") or {}
            merged = dict(old)
            merged.update(status)
            d["status"] = merged
            if status.get("role") in ("door", "lab"):
                d["role"] = status["role"]
            if status.get("name"):
                d["name"] = str(status["name"])[:32]


def peek_state(dev_id=None):
    now = _now()
    with _lock:
        ids = [dev_id] if dev_id else list(_devices.keys())
        out = {"devices": {}}
        for i in ids:
            d = _devices.get(i) if dev_id else _devices[i]
            if d is None:
                continue
            expired = _expire_pending_locked(d)
            if expired:
                _log("[%s] pending expired on peek: %s" % (d["id"], expired))
            age = ttl_left = None
            if d["pending"]:
                age = round(now - d["pending_ts"], 1)
                ttl_left = round(max(0.0, PENDING_TTL_S - (now - d["pending_ts"])), 1)
            out["devices"][d["id"]] = {
                "pending": d["pending"],
                "pending_age_s": age,
                "pending_ttl_s": PENDING_TTL_S,
                "pending_ttl_left_s": ttl_left,
                "update_sticky": bool(d["update_sticky"]),
                "online": _is_online(d, now),
            }
        # 兼容旧字段：主门
        main = _devices.get(DEFAULT_DEVICE) or next(iter(_devices.values()), None)
        if main:
            out.update(out["devices"].get(main["id"], {}))
        return out


def _device_summary(d, now=None):
    now = now if now is not None else _now()
    st = d.get("status") or {}
    return {
        "id": d["id"],
        "name": d.get("name") or d["id"],
        "role": d.get("role") or "",
        "fw": d.get("fw") or st.get("fw") or "",
        "online": _is_online(d, now),
        "last_seen": int(d.get("last_seen") or 0),
        "last_seen_ago_s": int(max(0, now - d.get("last_seen", 0))) if d.get("last_seen") else -1,
        "update_sticky": bool(d.get("update_sticky")),
        "pending": d.get("pending"),
        "health": _health_bits(st),
    }


def _health_bits(st):
    nfc = (st or {}).get("nfc") or {}
    if isinstance(nfc, dict):
        if nfc.get("ok"):
            nfc_s = "ok"
        elif nfc.get("absent"):
            nfc_s = "nochip"
        elif nfc.get("deferred"):
            nfc_s = "defer"
        else:
            nfc_s = "wait"
    else:
        nfc_s = str(nfc or "-")
    rf = (st or {}).get("rf") or {}
    if isinstance(rf, dict):
        rf_ok = bool(rf.get("open") or rf.get("close"))
        rf_s = "ok" if rf_ok else "nokey"
    else:
        rf_s = str(rf or "-")
    web = (st or {}).get("web")
    if web is None:
        web_s = "-"
    else:
        web_s = "ok" if web else "down"
    return {"nfc": nfc_s, "web": web_s, "rf": rf_s, "sta": "ok" if (st or {}).get("sta") else "off"}


def _append_device_log(text, dev_id=None):
    dev_id = (dev_id or DEFAULT_DEVICE).replace("/", "_").replace("..", "_")[:48]
    try:
        if not os.path.isdir(LOG_DIR):
            os.makedirs(LOG_DIR)
        day = time.strftime("%Y%m%d")
        path = os.path.join(LOG_DIR, "device-%s-%s.log" % (dev_id, day))
        # 兼容旧文件名
        if dev_id == DEFAULT_DEVICE:
            legacy = os.path.join(LOG_DIR, "device-%s.log" % day)
            if os.path.isfile(legacy) and not os.path.isfile(path):
                path = legacy
        with open(path, "a") as f:
            f.write(text)
            if not text.endswith("\n"):
                f.write("\n")
        try:
            names = sorted(
                n for n in os.listdir(LOG_DIR)
                if n.startswith("device-") and n.endswith(".log")
            )
            if len(names) > 14:
                for n in names[:-14]:
                    os.remove(os.path.join(LOG_DIR, n))
        except Exception:
            pass
        return True
    except Exception as e:
        _log("device log write fail: %s" % e)
        return False


def _read_device_log(dev_id, day=None, lines=200):
    dev_id = (dev_id or DEFAULT_DEVICE).replace("/", "_").replace("..", "_")[:48]
    day = day or time.strftime("%Y%m%d")
    if not day.isdigit() or len(day) != 8:
        return ""
    path = os.path.join(LOG_DIR, "device-%s-%s.log" % (dev_id, day))
    if dev_id == DEFAULT_DEVICE:
        legacy = os.path.join(LOG_DIR, "device-%s.log" % day)
        if os.path.isfile(legacy):
            path = legacy
    if not os.path.isfile(path):
        return ""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            all_lines = f.readlines()
        n = max(1, min(int(lines or 200), 2000))
        return "".join(all_lines[-n:])
    except Exception as e:
        return "read fail: %s\n" % e


def _ota_version_info():
    meta_path = os.path.join(OTA_DIR, "version.json")
    bin_path = os.path.join(OTA_DIR, "firmware.bin")
    info = {"version": "", "size": 0, "sha256": "", "updated": 0}
    try:
        with open(meta_path, "r") as f:
            info.update(json.loads(f.read()))
    except Exception:
        pass
    try:
        st = os.stat(bin_path)
        info["size"] = st.st_size
        info["updated"] = int(st.st_mtime)
    except Exception:
        info["size"] = 0
    return info


def _save_ota_bin(data, version):
    import hashlib

    if not os.path.isdir(OTA_DIR):
        os.makedirs(OTA_DIR)
    bin_path = os.path.join(OTA_DIR, "firmware.bin")
    with open(bin_path, "wb") as f:
        f.write(data)
    sha = hashlib.sha256(data).hexdigest()
    meta = {
        "version": version or "",
        "sha256": sha,
        "size": len(data),
        "updated": int(time.time()),
        "url": "/ota/firmware.bin",
    }
    with open(os.path.join(OTA_DIR, "version.json"), "w") as f:
        f.write(json.dumps(meta))
    _log("ota upload version=%s size=%d sha=%s..." % (version, len(data), sha[:12]))
    return meta


_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png": "image/png",
    ".ico": "image/x-icon",
    ".svg": "image/svg+xml",
}


def _mcp_tools():
    return [
        {
            "name": "open_garage",
            "description": "打开车库门（主门）",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "id": {"type": "string", "description": "设备 id，省略=主门"}
                },
            },
        },
        {
            "name": "close_garage",
            "description": "关闭车库门（主门）",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "id": {"type": "string", "description": "设备 id，省略=主门"}
                },
            },
        },
        {
            "name": "garage_status",
            "description": "查看设备在线/待下发指令状态",
            "inputSchema": {"type": "object", "properties": {}},
        },
    ]


def _mcp_tool_call(name, arguments):
    name = (name or "").strip()
    dev_id = (arguments or {}).get("id") or None
    if name == "open_garage":
        ok, why = set_pending("open", dev_id)
        _log("mcp open_garage -> pending=%s (%s)" % (ok, why))
        return False, ("已请求打开车库门" if ok else "指令去抖中，请稍后再试(%s)" % why)
    if name == "close_garage":
        ok, why = set_pending("close", dev_id)
        _log("mcp close_garage -> pending=%s (%s)" % (ok, why))
        return False, ("已请求关闭车库门" if ok else "指令去抖中，请稍后再试(%s)" % why)
    if name == "garage_status":
        return False, "状态: " + json.dumps(peek_state(), ensure_ascii=False)
    return True, "未知工具: %s" % name


def _mcp_handle_rpc(msg):
    if not isinstance(msg, dict):
        return {
            "jsonrpc": "2.0",
            "id": None,
            "error": {"code": -32600, "message": "Invalid Request"},
        }
    mid = msg.get("id")
    method = msg.get("method") or ""
    params = msg.get("params") or {}
    is_notification = "id" not in msg

    if method == "initialize":
        client_ver = params.get("protocolVersion") or MCP_PROTOCOL_DEFAULT
        proto = client_ver if client_ver in MCP_PROTOCOL_KNOWN else MCP_PROTOCOL_DEFAULT
        if client_ver:
            proto = client_ver
        result = {
            "protocolVersion": proto,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": MCP_SERVER_NAME, "version": MCP_SERVER_VERSION},
            "instructions": "使用 open_garage 打开车库门。",
        }
        _log("mcp initialize proto=%s" % proto)
        return {"jsonrpc": "2.0", "id": mid, "result": result}

    if method in ("notifications/initialized", "notifications/cancelled"):
        _log("mcp notification %s" % method)
        return None

    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}

    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": _mcp_tools()}}

    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        try:
            is_err, text = _mcp_tool_call(name, args)
        except Exception as e:
            is_err, text = True, "调用失败: %s" % e
        result = {"content": [{"type": "text", "text": text}], "isError": bool(is_err)}
        return {"jsonrpc": "2.0", "id": mid, "result": result}

    if is_notification:
        return None

    return {
        "jsonrpc": "2.0",
        "id": mid,
        "error": {"code": -32601, "message": "Method not found: %s" % method},
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "GarageGateMVP/0.3"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def _send_json(self, code, obj, extra_headers=None):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if extra_headers:
            for k, v in extra_headers:
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _send_raw(self, code, content_type, body_bytes, extra_headers=None):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body_bytes)))
        self.send_header("Cache-Control", "no-store")
        if extra_headers:
            for k, v in extra_headers:
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body_bytes)

    def _read_body(self, max_n=65536):
        try:
            n = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            n = 0
        if n <= 0:
            return b""
        if n > max_n:
            return b""
        return self.rfile.read(n)

    def _send_file(self, path, content_type=None):
        if not path or not os.path.isfile(path):
            self._send_json(404, {"error": "not found", "path": path})
            return
        ext = os.path.splitext(path)[1].lower()
        ct = content_type or _CONTENT_TYPES.get(ext, "application/octet-stream")
        with open(path, "rb") as f:
            body = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _path(self):
        return urlparse(self.path).path.rstrip("/") or "/"

    def _query(self):
        return parse_qs(urlparse(self.path).query)

    def _q(self, key, default=""):
        v = self._query().get(key) or [default]
        return (v[0] or default).strip()

    def _auth(self):
        tok = self.headers.get("X-Garage-Token") or ""
        return _check_ui_token(tok)

    def _require_auth(self):
        if self._auth():
            return True
        self._send_json(401, {"ok": 0, "error": "unauthorized"})
        return False

    def _mcp_post(self):
        raw = self._read_body()
        if not raw:
            self._send_json(400, {"error": "empty body"})
            return
        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send_json(400, {"error": "invalid json"})
            return

        if isinstance(payload, list):
            out = []
            for item in payload:
                r = _mcp_handle_rpc(item)
                if r is not None:
                    out.append(r)
            if not out:
                self._send_raw(202, "application/json", b"")
                return
            body = out if len(out) > 1 else out[0]
        else:
            r = _mcp_handle_rpc(payload)
            if r is None:
                self._send_raw(202, "application/json", b"")
                return
            body = r

        accept = (self.headers.get("Accept") or "").lower()
        if "text/event-stream" in accept and "application/json" not in accept:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            sse = b"event: message\ndata: " + data + b"\n\n"
            self._send_raw(200, "text/event-stream; charset=utf-8", sse)
            return

        self._send_json(200, body)

    def _mcp_sse_get(self):
        sid = uuid.uuid4().hex
        q = Queue()
        with _sse_lock:
            _sse_queues[sid] = q

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()

        endpoint = "/messages?sessionId=%s" % sid
        try:
            self.wfile.write(("event: endpoint\ndata: %s\n\n" % endpoint).encode("utf-8"))
            self.wfile.flush()
            last_hb = time.time()
            while True:
                try:
                    item = q.get(timeout=15.0)
                    if item is None:
                        break
                    data = json.dumps(item, ensure_ascii=False)
                    self.wfile.write(("event: message\ndata: %s\n\n" % data).encode("utf-8"))
                    self.wfile.flush()
                except Empty:
                    pass
                except Exception:
                    break
                now = time.time()
                if now - last_hb >= 15.0:
                    try:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
                        last_hb = now
                    except Exception:
                        break
        finally:
            with _sse_lock:
                _sse_queues.pop(sid, None)
            try:
                self.close_connection = True
            except Exception:
                pass

    def _mcp_messages_post(self):
        qs = self._query()
        sid = (qs.get("sessionId") or [""])[0]
        raw = self._read_body()
        if not raw:
            self._send_json(400, {"error": "empty body"})
            return
        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send_json(400, {"error": "invalid json"})
            return

        if isinstance(payload, list):
            results = []
            for item in payload:
                r = _mcp_handle_rpc(item)
                if r is not None:
                    results.append(r)
        else:
            r = _mcp_handle_rpc(payload)
            results = [r] if r is not None else []

        with _sse_lock:
            q = _sse_queues.get(sid)
        if q is None:
            if not results:
                self._send_raw(202, "application/json", b"")
                return
            self._send_json(200, results[0] if len(results) == 1 else results)
            return

        for r in results:
            q.put(r)
        self._send_raw(202, "application/json", b"")

    def _cmd_for_device(self, cmd, dev_id=None):
        if not self._require_auth():
            return
        dev_id = dev_id or self._q("id") or DEFAULT_DEVICE
        if cmd == "update":
            ok, why = request_update("api", dev_id)
            msg = "已请求检查更新" if ok else ("已在队列中" if why == "already" else "失败")
        else:
            ok, why = set_pending(cmd, dev_id)
            _log("ui %s %s -> pending=%s (%s)" % (dev_id, cmd, ok, why))
            label = "开门" if cmd == "open" else "关门"
            msg = ("已请求%s" % label) if ok else (
                "指令去抖中，请稍后再试" if why == "debounce" else "失败")
        self._send_json(200 if ok else 429, {
            "ok": 1 if ok else 0,
            "message": msg,
            "result": "ok" if ok else why,
            "id": dev_id,
        })

    def _route(self, method):
        path = self._path()

        if path == "/health":
            st = peek_state()
            st["ok"] = True
            st["mcp"] = True
            self._send_json(200, st)
            return

        # ===== 小爱 / 旧触发（默认主门）=====
        if path in ("/xiaoai/open",) and method in ("GET", "POST"):
            if method == "POST":
                self._read_body()
            ok, why = set_pending("open", self._q("id") or None)
            _log("xiaoai open -> pending=%s (%s)" % (ok, why))
            st = peek_state()
            st["result"] = "ok" if ok else why
            self._send_json(200, st)
            return

        if path in ("/xiaoai/close",) and method in ("GET", "POST"):
            if method == "POST":
                self._read_body()
            ok, why = set_pending("close", self._q("id") or None)
            _log("xiaoai close -> pending=%s (%s)" % (ok, why))
            st = peek_state()
            st["result"] = "ok" if ok else why
            self._send_json(200, st)
            return

        if path in ("/xiaoai/update",) and method in ("GET", "POST"):
            if method == "POST":
                self._read_body()
            ok, why = request_update("xiaoai", self._q("id") or None)
            st = peek_state()
            st["result"] = "ok" if ok else why
            self._send_json(200, st)
            return

        # ===== 设备侧 =====
        if path in ("/dev/poll",) and method == "GET":
            fw = self._q("fw")
            dev_id = self._q("id") or DEFAULT_DEVICE
            cmd = take_pending(dev_id, fw)
            if cmd:
                _log("[%s] dev poll consumed cmd=%s fw=%s" % (dev_id, cmd, fw or "-"))
            self._send_json(200, {"cmd": cmd, "ts": int(time.time())})
            return

        if path in ("/dev/logs", "/logs") and method == "POST":
            raw = self._read_body(max_n=200000)
            text = raw.decode("utf-8", "replace") if raw else ""
            dev_id = self._q("id") or DEFAULT_DEVICE
            if text.strip():
                _append_device_log(text, dev_id)
                _log("[%s] device log %d bytes" % (dev_id, len(text)))
            self._send_json(200, {"ok": 1})
            return

        if path in ("/dev/status",) and method == "POST":
            raw = self._read_body(max_n=8192)
            dev_id = self._q("id")
            status = {}
            try:
                status = json.loads(raw.decode("utf-8") if raw else "{}")
            except Exception:
                status = {}
            if isinstance(status, dict) and status.get("id"):
                dev_id = dev_id or status.get("id")
            dev_id = dev_id or DEFAULT_DEVICE
            update_device_status(dev_id, status)
            self._send_json(200, {"ok": 1, "id": dev_id})
            return

        if path in ("/debug/state",) and method == "GET":
            self._send_json(200, {"state": peek_state(), "log": LOG_LINES[-20:]})
            return

        # ===== 静态页 =====
        if method == "GET" and path in ("/", "/index.html"):
            self._send_file(os.path.join(WEB_DIR, "index.html"))
            return
        if method == "GET" and path in ("/style.css", "/app.js"):
            self._send_file(os.path.join(WEB_DIR, path.lstrip("/")))
            return

        # ===== 登录 / 管理 API =====
        if path in ("/api/login",) and method == "POST":
            raw = self._read_body()
            try:
                body = json.loads(raw.decode("utf-8") if raw else "{}")
            except Exception:
                body = {}
            pw = (body.get("password") or "").strip()
            if pw and pw == _ui_password():
                token = uuid.uuid4().hex
                with _ui_lock:
                    _ui_tokens.add(token)
                self._send_json(200, {"ok": 1, "token": token})
                return
            self._send_json(401, {"ok": 0, "error": "unauthorized"})
            return

        # 兼容旧手机页：/api/open|close|update
        if path in ("/api/open", "/api/close", "/api/update") and method == "POST":
            self._read_body()
            cmd = path.rsplit("/", 1)[-1]
            self._cmd_for_device(cmd, self._q("id") or DEFAULT_DEVICE)
            return

        # 设备定向指令
        if method == "POST" and path.startswith("/api/devices/"):
            parts = path.split("/")  # api devices <id> <cmd>
            if len(parts) == 5 and parts[4] in ("open", "close", "update", "toggle"):
                self._read_body()
                self._cmd_for_device(parts[4], unquote(parts[3]))
                return

        if path in ("/api/devices",) and method == "GET":
            if not self._require_auth():
                return
            now = _now()
            with _lock:
                items = [_device_summary(_devices[k], now) for k in sorted(_devices.keys())]
            self._send_json(200, {"ok": 1, "devices": items, "ota": _ota_version_info()})
            return

        if method == "GET" and path.startswith("/api/devices/"):
            parts = path.split("/")
            if len(parts) >= 4:
                dev_id = unquote(parts[3])
                if len(parts) == 4:
                    if not self._require_auth():
                        return
                    with _lock:
                        d = _devices.get(dev_id)
                        if not d:
                            self._send_json(404, {"ok": 0, "error": "no device"})
                            return
                        summary = _device_summary(d)
                        detail = dict(d.get("status") or {})
                    summary["status"] = detail
                    self._send_json(200, {"ok": 1, "device": summary, "ota": _ota_version_info()})
                    return
                if len(parts) == 5 and parts[4] == "logs":
                    if not self._require_auth():
                        return
                    day = self._q("day") or time.strftime("%Y%m%d")
                    lines = self._q("lines") or "200"
                    text = _read_device_log(dev_id, day=day, lines=lines)
                    self._send_json(200, {"ok": 1, "id": dev_id, "day": day, "text": text})
                    return

        if path in ("/api/ota",) and method == "GET":
            if not self._require_auth():
                return
            self._send_json(200, {"ok": 1, "ota": _ota_version_info()})
            return

        if path in ("/api/ota/upload",) and method == "POST":
            if not self._require_auth():
                return
            version = self._q("version")
            data = self._read_body(max_n=8 * 1024 * 1024)
            if not data or len(data) < 1000:
                self._send_json(400, {"ok": 0, "error": "empty or too small"})
                return
            meta = _save_ota_bin(data, version)
            # 上传后通知：指定 id 只发一台；默认发给所有已注册设备
            if self._q("notify") not in ("0", "false", "no"):
                only = self._q("id")
                with _lock:
                    ids = [only] if only else list(_devices.keys())
                for i in ids:
                    request_update("api", i)
            self._send_json(200, {"ok": 1, "ota": meta})
            return

        if path in ("/api/logs",) and method == "GET":
            if not self._require_auth():
                return
            dev_id = self._q("id") or DEFAULT_DEVICE
            day = self._q("day") or time.strftime("%Y%m%d")
            lines = self._q("lines") or "200"
            text = _read_device_log(dev_id, day=day, lines=lines)
            self._send_json(200, {"ok": 1, "id": dev_id, "day": day, "text": text})
            return

        # ===== OTA 静态（设备）=====
        if path in ("/ota/version", "/ota/version.json") and method == "GET":
            info = _ota_version_info()
            did = self._q("id")
            if did:
                with _lock:
                    d = _device_locked(did)
                    _touch_locked(d)
                _log("[%s] ota version fetch remote=%s" % (did, info.get("version")))
            self._send_json(200, info)
            return
        if path in ("/ota/firmware.bin",) and method == "GET":
            did = self._q("id")
            if did:
                with _lock:
                    d = _device_locked(did)
                    _touch_locked(d)
                _log("[%s] ota firmware.bin fetch" % did)
            self._send_file(os.path.join(OTA_DIR, "firmware.bin"),
                            content_type="application/octet-stream")
            return

        # ===== MCP =====
        if path in ("/mcp",):
            if method == "POST":
                self._mcp_post()
                return
            if method == "GET":
                self._mcp_sse_get()
                return
            if method == "DELETE":
                self._send_raw(200, "application/json", b"{}")
                return

        if path in ("/messages",) and method == "POST":
            self._mcp_messages_post()
            return

        if path in ("/mcp/sse",) and method == "GET":
            self._mcp_sse_get()
            return
        if path in ("/mcp/messages",) and method == "POST":
            self._mcp_messages_post()
            return

        self._send_json(404, {"error": "not found", "path": path})

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_DELETE(self):
        self._route("DELETE")


def main():
    for d in (WEB_DIR, OTA_DIR, LOG_DIR):
        if not os.path.isdir(d):
            os.makedirs(d)
    with _lock:
        _device_locked(DEFAULT_DEVICE)
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    _log("listen http://%s:%s ui=/ api=/api devices=/api/devices" % (HOST, PORT))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        _log("shutdown")
        httpd.server_close()


if __name__ == "__main__":
    main()
