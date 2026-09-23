#!/usr/bin/env python3
"""车库门 MVP 网关：HTTP 触发 + 极简 MCP Server → 设备轮询取令。

技术选型（有意做小）：
  - Python 3 标准库 only（VPS Python 3.6 兼容；零 pip）
  - 进程内存 flag（单台、重启丢 pending 可接受）
  - 指令 TTL（默认 8s）：设备未及时 /dev/poll 则自动作废
  - HTTPS 由前面 Nginx 终结

对外 API：
  GET|POST /xiaoai/open   HTTP 触发 → 挂一条 open
  GET|POST /xiaoai/close  挂一条 close
  GET       /dev/poll     ESP32 轮询 → 有令则消费
  GET       /health       探活
  POST      /mcp          MCP Streamable HTTP（JSON-RPC 2.0）
  GET       /mcp          MCP SSE（旧传输：先建立事件流）
  POST      /messages     MCP 旧传输客户端回传（带 sessionId）

MCP 工具：
  open_garage  — 打开车库门
  close_garage — 关闭车库门（可选）

生产绑定：127.0.0.1:18080，nginx 反代 https://door.wzx.homes/mcp
"""
from __future__ import print_function

import json
import os
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler
from http.server import HTTPServer
from socketserver import ThreadingMixIn
from urllib.parse import parse_qs, urlparse

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

# 手机页会话 token（进程内即可；重启后重新登录）
_ui_tokens = set()
_ui_lock = threading.Lock()

MCP_SERVER_NAME = "garage-gate"
MCP_SERVER_VERSION = "0.1.0"
# 多协议版本：按客户端 initialize 返回其请求的版本，否则回默认
MCP_PROTOCOL_DEFAULT = "2024-11-05"
MCP_PROTOCOL_KNOWN = (
    "2025-03-26",
    "2024-11-05",
    "2024-10-07",
)

_lock = threading.Lock()
_pending = None
_pending_ts = 0.0
MIN_SET_GAP = 2.0
_last_set_ts = 0.0
# 在线升级令：独立于 8s 开关门 TTL，粘到被认领或超 UPDATE_TTL_S
_update_sticky = False
_update_ts = 0.0
_update_notify_ts = 0.0
# 指令有效期：设备未在此时间内认领则作废（防「人走了门才开」）
# ESP32 默认约 5s 轮询一次 → 留 ~2 拍余量；可按需改 5～15
PENDING_TTL_S = 8.0
# update 指令：粘性标记，直到设备认领；勿用 8s TTL（蓝牙忙时易过期）
UPDATE_TTL_S = 600.0
# 自动比对 fw 后下发 update 的最短间隔，避免每 3s 刷屏
UPDATE_NOTIFY_GAP_S = 300.0

LOG_LINES = []

# 旧版 HTTP+SSE：sessionId -> 是否仍挂起（简化：进程内单例广播）
_sse_lock = threading.Lock()
_sse_clients = {}  # sid -> list of response-like queues 不适用；用简单广播队列
try:
    from queue import Queue, Empty
except ImportError:
    from Queue import Queue, Empty  # py2 fallback unused

_sse_queues = {}  # sid -> Queue


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


def _append_device_log(text):
    try:
        if not os.path.isdir(LOG_DIR):
            os.makedirs(LOG_DIR)
        day = time.strftime("%Y%m%d")
        path = os.path.join(LOG_DIR, "device-%s.log" % day)
        with open(path, "a") as f:
            f.write(text)
            if not text.endswith("\n"):
                f.write("\n")
        # 只保留 14 天
        try:
            names = sorted(os.listdir(LOG_DIR))
            if len(names) > 14:
                for n in names[:-14]:
                    os.remove(os.path.join(LOG_DIR, n))
        except Exception:
            pass
        return True
    except Exception as e:
        _log("device log write fail: %s" % e)
        return False


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


_CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def _expire_pending_locked(now):
    """持锁调用：超时未认领的 pending 作废。返回被丢弃的 cmd 或 None。"""
    global _pending, _pending_ts
    if _pending is None:
        return None
    age = now - _pending_ts
    if age > PENDING_TTL_S:
        expired = _pending
        _pending = None
        _pending_ts = 0.0
        return expired
    return None


def set_pending(cmd):
    global _pending, _pending_ts, _last_set_ts, _update_sticky, _update_ts
    if cmd == "update":
        return request_update("api")
    now = time.time()
    with _lock:
        stale = _expire_pending_locked(now)
        if stale:
            _log("pending expired before set: %s (ttl=%.0fs)" % (stale, PENDING_TTL_S))
        if now - _last_set_ts < MIN_SET_GAP:
            return False, "debounce"
        _pending = cmd
        _pending_ts = now
        _last_set_ts = now
        return True, "ok"


def request_update(reason="api"):
    """粘性 update 令：设备下一次 /dev/poll 认领后清除。"""
    global _update_sticky, _update_ts, _update_notify_ts
    now = time.time()
    with _lock:
        if _update_sticky:
            return True, "already"
        # 自动比对：同一版本落后时勿每 3s 重复下发
        if reason == "auto" and (now - _update_notify_ts) < UPDATE_NOTIFY_GAP_S:
            return False, "cooldown"
        _update_sticky = True
        _update_ts = now
        if reason == "auto":
            _update_notify_ts = now
        _log("update requested (%s)" % reason)
        return True, "ok"


def _maybe_auto_update(device_fw):
    """poll 带了 fw 且落后于 ota/version.json → 下发 update。
    调用方须已持 _lock（take_pending）。
    """
    global _update_sticky, _update_ts, _update_notify_ts
    if not device_fw:
        return False
    try:
        remote = _ota_version_info().get("version") or ""
    except Exception:
        remote = ""
    if not remote or device_fw == remote:
        return False
    now = time.time()
    if _update_sticky:
        return False
    if (now - _update_notify_ts) < UPDATE_NOTIFY_GAP_S:
        return False
    _update_sticky = True
    _update_ts = now
    _update_notify_ts = now
    _log("auto update fw=%s -> %s" % (device_fw, remote))
    return True


def take_pending(device_fw=None):
    global _pending, _update_sticky, _update_ts
    now = time.time()
    with _lock:
        expired = _expire_pending_locked(now)
        if expired:
            _log("pending expired unclaimed: %s (ttl=%.0fs)" % (expired, PENDING_TTL_S))
        cmd = _pending
        _pending = None
        if cmd:
            return cmd
        # update 粘性：过期后自动作废（设备长期不在线时勿一直挂着）
        if _update_sticky:
            if now - _update_ts > UPDATE_TTL_S:
                _update_sticky = False
                _log("update sticky expired")
            else:
                _update_sticky = False
                return "update"
        if device_fw and _maybe_auto_update(device_fw):
            _update_sticky = False
            return "update"
        return None


def peek_state():
    now = time.time()
    with _lock:
        expired = _expire_pending_locked(now)
        if expired:
            _log("pending expired on peek: %s (ttl=%.0fs)" % (expired, PENDING_TTL_S))
        age = None
        ttl_left = None
        if _pending:
            age = round(now - _pending_ts, 1)
            ttl_left = round(max(0.0, PENDING_TTL_S - (now - _pending_ts)), 1)
        return {
            "pending": _pending,
            "pending_age_s": age,
            "pending_ttl_s": PENDING_TTL_S,
            "pending_ttl_left_s": ttl_left,
            "update_sticky": bool(_update_sticky),
        }


def _mcp_tools():
    return [
        {
            "name": "open_garage",
            "description": "打开车库门（发送开门指令，由车库控制器执行）",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "additionalProperties": False,
            },
        },
        {
            "name": "close_garage",
            "description": "关闭车库门（发送关门指令）",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "additionalProperties": False,
            },
        },
        {
            "name": "garage_status",
            "description": "查看网关当前待下发指令状态",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "additionalProperties": False,
            },
        },
    ]


def _mcp_tool_call(name, arguments):
    name = (name or "").strip()
    if name == "open_garage":
        ok, why = set_pending("open")
        _log("mcp open_garage -> pending=%s (%s)" % (ok, why))
        text = "已请求打开车库门" if ok else "指令去抖中，请稍后再试(%s)" % why
        return False, text
    if name == "close_garage":
        ok, why = set_pending("close")
        _log("mcp close_garage -> pending=%s (%s)" % (ok, why))
        text = "已请求关闭车库门" if ok else "指令去抖中，请稍后再试(%s)" % why
        return False, text
    if name == "garage_status":
        st = peek_state()
        return False, "状态: " + json.dumps(st, ensure_ascii=False)
    return True, "未知工具: %s" % name


def _mcp_handle_rpc(msg):
    """处理单条 JSON-RPC，返回 dict 或 None（notification）。"""
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
        # 若客户端给了未知版本，仍回传其版本（部分客户端严格匹配）
        if client_ver:
            proto = client_ver
        result = {
            "protocolVersion": proto,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {
                "name": MCP_SERVER_NAME,
                "version": MCP_SERVER_VERSION,
            },
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
        return {
            "jsonrpc": "2.0",
            "id": mid,
            "result": {"tools": _mcp_tools()},
        }

    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        try:
            is_err, text = _mcp_tool_call(name, args)
        except Exception as e:
            is_err, text = True, "调用失败: %s" % e
        result = {
            "content": [{"type": "text", "text": text}],
            "isError": bool(is_err),
        }
        return {"jsonrpc": "2.0", "id": mid, "result": result}

    if is_notification:
        return None

    return {
        "jsonrpc": "2.0",
        "id": mid,
        "error": {"code": -32601, "message": "Method not found: %s" % method},
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "GarageGateMVP/0.2"
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

        # 批量或单条
        if isinstance(payload, list):
            out = []
            for item in payload:
                r = _mcp_handle_rpc(item)
                if r is not None:
                    out.append(r)
            if not out:
                # notification-only → 202
                self._send_raw(202, "application/json", b"")
                return
            body = out if len(out) > 1 else out[0]
        else:
            r = _mcp_handle_rpc(payload)
            if r is None:
                self._send_raw(202, "application/json", b"")
                return
            body = r

        # Streamable HTTP：客户端要 SSE 则用 text/event-stream 包一层
        accept = (self.headers.get("Accept") or "").lower()
        if "text/event-stream" in accept and "application/json" not in accept:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            sse = b"event: message\ndata: " + data + b"\n\n"
            self._send_raw(
                200,
                "text/event-stream; charset=utf-8",
                sse,
                extra_headers=[("MCP-Protocol-Version", body.get("result", {}).get("protocolVersion", MCP_PROTOCOL_DEFAULT) if isinstance(body, dict) else MCP_PROTOCOL_DEFAULT)],
            )
            return

        self._send_json(200, body)

    def _mcp_sse_get(self):
        """旧 HTTP+SSE 传输：GET /mcp 建流，并下发 endpoint 事件。"""
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
            self.wfile.write(
                ("event: endpoint\ndata: %s\n\n" % endpoint).encode("utf-8")
            )
            self.wfile.flush()
            # 心跳 + 转发队列中的 RPC 响应
            last_hb = time.time()
            while True:
                try:
                    item = q.get(timeout=15.0)
                    if item is None:
                        break
                    data = json.dumps(item, ensure_ascii=False)
                    self.wfile.write(
                        ("event: message\ndata: %s\n\n" % data).encode("utf-8")
                    )
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
            # 无 SSE 会话：若只是 notification，202；否则退回 JSON（Streamable 兼容）
            if not results:
                self._send_raw(202, "application/json", b"")
                return
            self._send_json(200, results[0] if len(results) == 1 else results)
            return

        for r in results:
            q.put(r)
        self._send_raw(202, "application/json", b"")

    def _route(self, method):
        path = self._path()

        if path == "/health":
            st = peek_state()
            st["ok"] = True
            st["mcp"] = True
            self._send_json(200, st)
            return

        if path in ("/xiaoai/open",) and method in ("GET", "POST"):
            if method == "POST":
                self._read_body()
            ok, why = set_pending("open")
            _log("xiaoai open -> pending=%s (%s)" % (ok, why))
            st = peek_state()
            st["result"] = "ok" if ok else why
            self._send_json(200, st)
            return

        if path in ("/xiaoai/close",) and method in ("GET", "POST"):
            if method == "POST":
                self._read_body()
            ok, why = set_pending("close")
            _log("xiaoai close -> pending=%s (%s)" % (ok, why))
            st = peek_state()
            st["result"] = "ok" if ok else why
            self._send_json(200, st)
            return

        if path in ("/xiaoai/update",) and method in ("GET", "POST"):
            if method == "POST":
                self._read_body()
            ok, why = request_update("xiaoai")
            st = peek_state()
            st["result"] = "ok" if ok else why
            self._send_json(200, st)
            return

        if path in ("/dev/poll",) and method == "GET":
            qs = self._query()
            fw = (qs.get("fw") or [""])[0].strip()
            cmd = take_pending(fw)
            if cmd:
                _log("dev poll consumed cmd=%s fw=%s" % (cmd, fw or "-"))
            self._send_json(200, {"cmd": cmd, "ts": int(time.time())})
            return

        if path in ("/debug/state",) and method == "GET":
            self._send_json(200, {"state": peek_state(), "log": LOG_LINES[-20:]})
            return

        # ===== 手机页 / API =====
        if method == "GET" and path in ("/", "/index.html"):
            self._send_file(os.path.join(WEB_DIR, "index.html"))
            return
        if method == "GET" and path in ("/style.css", "/app.js"):
            self._send_file(os.path.join(WEB_DIR, path.lstrip("/")))
            return

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

        if path in ("/api/open", "/api/close", "/api/update") and method == "POST":
            self._read_body()
            tok = self.headers.get("X-Garage-Token") or ""
            if not _check_ui_token(tok):
                self._send_json(401, {"ok": 0, "error": "unauthorized"})
                return
            if path.endswith("update"):
                ok, why = request_update("api")
                msg = "已请求检查更新" if ok else (
                    "已在队列中" if why == "already" else "失败")
            else:
                cmd = "open" if path.endswith("open") else "close"
                ok, why = set_pending(cmd)
                _log("ui %s -> pending=%s (%s)" % (cmd, ok, why))
                msg = ("已请求开门" if cmd == "open" else "已请求关门") if ok else (
                    "指令去抖中，请稍后再试" if why == "debounce" else "失败")
            self._send_json(200 if ok else 429, {
                "ok": 1 if ok else 0,
                "message": msg,
                "result": "ok" if ok else why,
            })
            return

        # ===== 设备日志上报（HTTP 明文，nginx 放行 /dev/logs）=====
        if path in ("/dev/logs", "/logs") and method == "POST":
            raw = self._read_body(max_n=200000)
            text = raw.decode("utf-8", "replace") if raw else ""
            if text.strip():
                _append_device_log(text)
                _log("device log %d bytes" % len(text))
            self._send_json(200, {"ok": 1})
            return

        # ===== 在线 OTA =====
        if path in ("/ota/version", "/ota/version.json") and method == "GET":
            self._send_json(200, _ota_version_info())
            return
        if path in ("/ota/firmware.bin",) and method == "GET":
            self._send_file(os.path.join(OTA_DIR, "firmware.bin"),
                            content_type="application/octet-stream")
            return

        # ===== MCP =====
        if path in ("/mcp",):
            if method == "POST":
                self._mcp_post()
                return
            if method == "GET":
                # Streamable 也可能 GET 要 SSE；旧传输也用 GET /mcp
                self._mcp_sse_get()
                return
            if method == "DELETE":
                self._send_raw(200, "application/json", b"{}")
                return

        if path in ("/messages",) and method == "POST":
            self._mcp_messages_post()
            return

        # 有的客户端写 /mcp/sse
        if path in ("/mcp/sse",) and method == "GET":
            self._mcp_sse_get()
            return
        if path in ("/mcp/messages",) and method == "POST":
            self._mcp_messages_post()
            return

        self._send_json(404, {"error": "not found", "path": path})

    def do_GET(self):  # noqa: N802
        self._route("GET")

    def do_POST(self):  # noqa: N802
        self._route("POST")

    def do_DELETE(self):  # noqa: N802
        self._route("DELETE")


def main():
    for d in (WEB_DIR, OTA_DIR, LOG_DIR):
        if not os.path.isdir(d):
            os.makedirs(d)
    httpd = ThreadingHTTPServer((HOST, PORT), Handler)
    _log("listen http://%s:%s ui=/ ota=/ota mcp=https://door.wzx.homes/mcp" % (HOST, PORT))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        _log("shutdown")
        httpd.server_close()


if __name__ == "__main__":
    main()
