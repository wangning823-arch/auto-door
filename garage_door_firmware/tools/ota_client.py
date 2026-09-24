# -*- coding: utf-8 -*-
"""车库门 ESP32 桌面 OTA 客户端

填 xxxx（或完整主机名 / STA IP）→ 检测在线 → 选固件 → 更新 → 看成功状态。
依赖：标准库 + requests（可选）。不依赖 zeroconf。
"""

from __future__ import annotations

import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Optional
from urllib import error as urlerror
from urllib import request as urlrequest

try:
    import requests  # type: ignore

    _HAS_REQUESTS = True
except Exception:
    _HAS_REQUESTS = False

APP_TITLE = "车库门 OTA 升级"
FW_ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEFAULT_BIN = os.path.join(FW_ROOT, ".pio", "build", "esp32dev", "firmware.bin")
ESPOTA_CANDIDATES = [
    os.path.expanduser(
        r"~\.platformio\packages\framework-arduinoespressif32\tools\espota.py"
    ),
    os.path.expanduser(r"~\.platformio\packages\framework-arduinoespressif32\tools\espota.py"),
]
OTA_PORT = 3232


# ---------- 网络发现 / 探测 ----------

def normalize_host(raw: str) -> str:
    """支持：4位xxxx / garage-xxxx / garage-xxxx.local / IP。"""
    s = (raw or "").strip()
    if not s:
        raise ValueError("请输入 xxxx、主机名或 IP")
    if re.fullmatch(r"[0-9A-Fa-f]{4}", s):
        return f"garage-{s.lower()}.local"
    if re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", s):
        return s
    if s.lower().endswith(".local"):
        return s
    if s.lower().startswith("garage-"):
        return s + ".local"
    return s


def mdns_resolve(name: str, timeout: float = 1.6) -> Optional[str]:
    """最小 mDNS A 记录查询（Windows 无 Bonjour 时也能试一下）。"""
    if re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", name):
        return name
    qname = name if name.endswith(".local") else name + ".local"
    # 编码 QNAME
    parts = []
    for label in qname.rstrip(".").split("."):
        b = label.encode("utf-8")
        parts.append(bytes([len(b)]) + b)
    parts.append(b"\x00")
    q = b"".join(parts)
    # header: id=0, flags=0, qd=1; QTYPE A=1, QCLASS IN=1
    header = struct.pack("!HHHHHH", 0, 0x0100, 1, 0, 0, 0)
    query = header + q + struct.pack("!HH", 1, 1)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.sendto(query, ("224.0.0.251", 5353))
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                data, _ = sock.recvfrom(2048)
            except socket.timeout:
                break
            ip = _parse_mdns_a(data)
            if ip:
                return ip
    except OSError:
        return None
    finally:
        sock.close()
    return None


def _parse_mdns_a(data: bytes) -> Optional[str]:
    if len(data) < 12:
        return None
    _, flags, qd, an, _, _ = struct.unpack("!HHHHHH", data[:12])
    if an == 0:
        return None
    # 跳过 question
    i = 12
    for _ in range(qd):
        while i < len(data) and data[i] != 0:
            i += 1 + data[i]
        i += 1 + 4  # null + type/class
    for _ in range(an):
        if i >= len(data):
            return None
        if data[i] & 0xC0 == 0xC0:
            i += 2
        else:
            while i < len(data) and data[i] != 0:
                i += 1 + data[i]
            i += 1
        if i + 10 > len(data):
            return None
        rtype, rclass, _ttl, rdlen = struct.unpack("!HHIH", data[i : i + 10])
        i += 10
        if i + rdlen > len(data):
            return None
        rdata = data[i : i + rdlen]
        i += rdlen
        if rtype == 1 and rdlen == 4:
            return socket.inet_ntoa(rdata)
    return None


def resolve_ip(host: str) -> tuple[str, str]:
    """返回 (用于HTTP/espota的地址, 解析方式说明)。"""
    if re.fullmatch(r"\d{1,3}(\.\d{1,3}){3}", host):
        return host, "IP直连"
    # 1) 系统 getaddrinfo（有 mDNS 解析器时最快）
    try:
        infos = socket.getaddrinfo(host, None, socket.AF_INET)
        for info in infos:
            ip = info[4][0]
            if ip and not ip.startswith("127."):
                return ip, "系统DNS"
    except OSError:
        pass
    # 2) 自建 mDNS
    ip = mdns_resolve(host)
    if ip:
        return ip, "mDNS"
    raise RuntimeError(
        f"无法解析 {host}。请确认与设备同一 2.4G 局域网，或直接填 STA IP。"
    )


def http_get_json(url: str, timeout: float = 2.5) -> dict:
    if _HAS_REQUESTS:
        r = requests.get(url, timeout=timeout)
        r.raise_for_status()
        return r.json()
    with urlrequest.urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def http_get_text(url: str, timeout: float = 2.5) -> str:
    if _HAS_REQUESTS:
        r = requests.get(url, timeout=timeout)
        r.raise_for_status()
        return r.text
    with urlrequest.urlopen(url, timeout=timeout) as resp:
        return resp.read().decode("utf-8", "replace")


def tcp_open(ip: str, port: int, timeout: float = 1.0) -> bool:
    try:
        with socket.create_connection((ip, port), timeout=timeout):
            return True
    except OSError:
        return False


def find_espota() -> Optional[str]:
    for p in ESPOTA_CANDIDATES:
        if os.path.isfile(p):
            return p
    # 兜底：扫 .platformio/packages
    root = os.path.expanduser(r"~\.platformio\packages")
    if os.path.isdir(root):
        for name in os.listdir(root):
            cand = os.path.join(root, name, "tools", "espota.py")
            if os.path.isfile(cand):
                return cand
    return None


def find_python() -> str:
    return sys.executable or "python"


def read_local_fw_hint(bin_path: str) -> str:
    """从 firmware.bin 里浅搜 FW 字符串（启发式，失败则显示文件时间）。"""
    try:
        st = os.stat(bin_path)
        mtime = time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))
        size_kb = st.st_size // 1024
        return f"本地 bin · {size_kb} KB · {mtime}"
    except OSError:
        return "本地 bin 不存在"


# ---------- 探测结果 ----------

class ProbeResult:
    def __init__(self) -> None:
        self.ok = False
        self.host = ""
        self.ip = ""
        self.how = ""
        self.online = False
        self.fw = ""
        self.build = ""
        self.sta = False
        self.ota = False
        self.can_update = False
        self.tcp_ota = False
        self.api = ""  # /ota | legacy | none
        self.error = ""
        self.ap_ip = ""
        self.uptime_ms = 0


def probe(host_raw: str) -> ProbeResult:
    r = ProbeResult()
    try:
        host = normalize_host(host_raw)
        r.host = host
        ip, how = resolve_ip(host)
        r.ip = ip
        r.how = how
    except Exception as e:
        r.error = str(e)
        return r

    base = f"http://{ip}"
    # 优先 /ota
    try:
        data = http_get_json(base + "/ota")
        if isinstance(data, dict) and data.get("ok"):
            r.online = True
            r.api = "/ota"
            r.fw = str(data.get("fw") or "")
            r.build = str(data.get("build") or "")
            r.sta = bool(data.get("sta"))
            r.ota = bool(data.get("ota"))
            r.can_update = bool(data.get("can_update"))
            r.ap_ip = str(data.get("ap_ip") or "")
            r.uptime_ms = int(data.get("uptime_ms") or 0)
            r.ok = True
    except Exception:
        pass

    # 旧固件：探测首页
    if not r.online:
        try:
            text = http_get_text(base + "/")
            if "车库门" in text or "GarageDoor" in text:
                r.online = True
                r.api = "legacy"
                r.fw = "未知（旧固件无 /ota）"
                r.error = ""
        except Exception as e:
            if not r.error:
                r.error = f"HTTP 不可达: {e}"

    if r.online:
        r.tcp_ota = tcp_open(ip, OTA_PORT, 1.0)
        if r.api == "/ota":
            r.can_update = bool(r.sta and r.ota) or r.tcp_ota
        else:
            r.can_update = r.tcp_ota
            if not r.tcp_ota:
                r.error = "首页可开，但 OTA 口 3232 不通（可能 STA/OTA 未就绪）"
    return r


# ---------- 编译 / 上传 ----------

def run_espota(
    ip_or_host: str,
    bin_path: str,
    log_cb,
    progress_cb,
) -> int:
    espota = find_espota()
    if not espota:
        log_cb("找不到 espota.py（.platformio/packages/...）\n")
        return 2
    if not os.path.isfile(bin_path):
        log_cb(f"固件文件不存在: {bin_path}\n")
        return 3

    py = find_python()
    cmd = [
        py,
        espota,
        "-i",
        ip_or_host,
        "-p",
        str(OTA_PORT),
        "-f",
        bin_path,
        "--timeout",
        "30",
    ]
    # 旧版 espota 不认 --timeout 时去掉
    log_cb("执行: " + " ".join(cmd) + "\n")
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=FW_ROOT,
        )
    except Exception as e:
        log_cb(f"启动 espota 失败: {e}\n")
        return 1

    assert proc.stdout is not None
    last_pct = -1
    for line in proc.stdout:
        line = line.rstrip("\n")
        if line:
            log_cb(line + "\n")
        m = re.search(r"(\d+)\s*%", line)
        if m:
            pct = int(m.group(1))
            if pct != last_pct:
                last_pct = pct
                progress_cb(pct)
        # 常见 espota 英文
        if "Sending" in line and last_pct < 0:
            progress_cb(1)
        if re.search(r"Auth Failed|timeout|Timeout|Invalid|Error|error", line, re.I):
            progress_cb(-1)
    rc = proc.wait()
    if rc == 0:
        progress_cb(100)
        return 0
    # 部分 espota 版本不认 --timeout：去掉后重试一次
    if "--timeout" in cmd:
        log_cb("重试（去掉 --timeout）…\n")
        cleaned = []
        skip = False
        for c in cmd:
            if skip:
                skip = False
                continue
            if c == "--timeout":
                skip = True
                continue
            cleaned.append(c)
        try:
            p2 = subprocess.run(
                cleaned,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=FW_ROOT,
                timeout=180,
            )
            for line in (p2.stdout or "").splitlines():
                log_cb(line + "\n")
                m = re.search(r"(\d+)\s*%", line)
                if m:
                    progress_cb(int(m.group(1)))
            if p2.returncode == 0:
                progress_cb(100)
                return 0
            return p2.returncode
        except Exception as e:
            log_cb(f"重试失败: {e}\n")
            return rc
    return rc


def run_build(log_cb) -> int:
    py = find_python()
    # 优先 MIMO_PYTHON 环境的 platformio
    env = os.environ.copy()
    cmd = [py, "-m", "platformio", "run", "-e", "esp32dev"]
    log_cb("编译: " + " ".join(cmd) + "\n")
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            cwd=FW_ROOT,
            env=env,
        )
    except Exception as e:
        log_cb(f"启动编译失败: {e}\n")
        return 1
    assert proc.stdout is not None
    for line in proc.stdout:
        log_cb(line if line.endswith("\n") else line + "\n")
    return proc.wait()


# ---------- GUI ----------

class OtaApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title(APP_TITLE)
        root.geometry("720x640")
        root.minsize(640, 560)

        self.busy = False
        self.probe: Optional[ProbeResult] = None
        self.last_fw = ""
        self.last_build = ""

        self._build_ui()
        self.log(f"工作目录: {FW_ROOT}\n")
        self.log(f"默认固件: {DEFAULT_BIN}\n")
        if _HAS_REQUESTS:
            self.log("HTTP: requests\n")
        else:
            self.log("HTTP: urllib\n")
        espota = find_espota()
        self.log(f"espota: {espota or '未找到'}\n")
        self.bin_var.set(DEFAULT_BIN)
        self.log(f"提示: {read_local_fw_hint(DEFAULT_BIN)}\n")

    def _build_ui(self) -> None:
        pad = {"padx": 10, "pady": 6}
        frm = ttk.Frame(self.root)
        frm.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        # 设备
        box = ttk.LabelFrame(frm, text="设备")
        box.pack(fill=tk.X, **pad)

        row = ttk.Frame(box)
        row.pack(fill=tk.X, padx=8, pady=8)
        ttk.Label(row, text="主机 (xxxx / 名称 / IP)").pack(side=tk.LEFT)
        self.host_var = tk.StringVar()
        ttk.Entry(row, textvariable=self.host_var, width=28).pack(
            side=tk.LEFT, padx=8
        )
        ttk.Button(row, text="检测在线", command=self.on_probe).pack(side=tk.LEFT)
        ttk.Button(row, text="自动刷新", command=self.on_probe).pack(
            side=tk.LEFT, padx=4
        )

        self.status_var = tk.StringVar(value="未检测")
        self.status_lbl = ttk.Label(
            box, textvariable=self.status_var, font=("Segoe UI", 11, "bold")
        )
        self.status_lbl.pack(anchor=tk.W, padx=8, pady=(0, 4))
        self.info = tk.Text(box, height=6, width=80, state=tk.DISABLED)
        self.info.pack(fill=tk.X, padx=8, pady=(0, 8))

        # 固件
        box2 = ttk.LabelFrame(frm, text="固件与更新")
        box2.pack(fill=tk.X, **pad)
        r2 = ttk.Frame(box2)
        r2.pack(fill=tk.X, padx=8, pady=8)
        ttk.Label(r2, text="固件 bin").pack(side=tk.LEFT)
        self.bin_var = tk.StringVar()
        ttk.Entry(r2, textvariable=self.bin_var).pack(
            side=tk.LEFT, fill=tk.X, expand=True, padx=8
        )
        ttk.Button(r2, text="浏览…", command=self.on_browse).pack(side=tk.LEFT)

        r3 = ttk.Frame(box2)
        r3.pack(fill=tk.X, padx=8, pady=(0, 8))
        self.btn_build = ttk.Button(r3, text="先编译", command=self.on_build)
        self.btn_build.pack(side=tk.LEFT)
        self.btn_upd = ttk.Button(
            r3, text="开始更新", command=self.on_update
        )
        self.btn_upd.pack(side=tk.LEFT, padx=8)
        self.btn_chk = ttk.Button(
            r3, text="更新后复检", command=self.on_probe
        )
        self.btn_chk.pack(side=tk.LEFT)

        self.pbar = ttk.Progressbar(box2, maximum=100, mode="determinate")
        self.pbar.pack(fill=tk.X, padx=8, pady=(0, 8))

        # 日志
        box3 = ttk.LabelFrame(frm, text="日志 / 结果")
        box3.pack(fill=tk.BOTH, expand=True, **pad)
        self.log_box = tk.Text(
            box3, height=14, wrap=tk.WORD, bg="#111820", fg="#d7e0ea"
        )
        self.log_box.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)
        self.log_box.tag_config("ok", foreground="#3dd68c")
        self.log_box.tag_config("err", foreground="#ff6b6b")
        self.log_box.tag_config("warn", foreground="#f2c94c")
        self.log_box.tag_config("info", foreground="#8b9aab")

    def log(self, text: str, tag: str = "info") -> None:
        def _():
            self.log_box.insert(tk.END, text, tag)
            self.log_box.see(tk.END)

        self.root.after(0, _)

    def set_status(self, text: str, color: str) -> None:
        def _():
            self.status_var.set(text)
            self.status_lbl.configure(foreground=color)

        self.root.after(0, _)

    def set_info(self, lines: list[str]) -> None:
        def _():
            self.info.configure(state=tk.NORMAL)
            self.info.delete("1.0", tk.END)
            self.info.insert("1.0", "\n".join(lines))
            self.info.configure(state=tk.DISABLED)

        self.root.after(0, _)

    def set_busy(self, busy: bool) -> None:
        self.busy = busy
        state = tk.DISABLED if busy else tk.NORMAL

        def _():
            self.btn_build.configure(state=state)
            self.btn_upd.configure(state=state)
            self.btn_chk.configure(state=state)

        self.root.after(0, _)

    def on_browse(self) -> None:
        p = filedialog.askopenfilename(
            title="选择 firmware.bin",
            initialdir=os.path.dirname(self.bin_var.get() or DEFAULT_BIN),
            filetypes=[("ESP32 bin", "*.bin"), ("All", "*.*")],
        )
        if p:
            self.bin_var.set(p)
            self.log(f"选择固件: {p}\n")
            self.log(read_local_fw_hint(p) + "\n")

    def on_probe(self) -> None:
        if self.busy:
            return
        host = self.host_var.get().strip()
        if not host:
            messagebox.showwarning(APP_TITLE, "请先填写 xxxx / 主机名 / IP")
            return
        self.set_busy(True)
        self.set_status("检测中…", "#f2c94c")
        self.log(f"检测 {host} …\n")

        def worker():
            r = probe(host)
            self.probe = r
            self.root.after(0, lambda: self._after_probe(r))

        threading.Thread(target=worker, daemon=True).start()

    def _after_probe(self, r: ProbeResult) -> None:
        self.set_busy(False)
        if not r.online:
            self.set_status("离线 / 不可达", "#ff6b6b")
            self.set_info(
                [
                    f"目标: {r.host or self.host_var.get()}",
                    f"解析: {r.ip or '-'} ({r.how or '-'})",
                    f"错误: {r.error or '无响应'}",
                    f"本地固件: {read_local_fw_hint(self.bin_var.get())}",
                ]
            )
            self.log(f"检测失败: {r.error or '离线'}\n", "err")
            return

        self.last_fw = r.fw
        self.last_build = r.build
        color = "#3dd68c" if r.can_update else "#f2c94c"
        title = "在线 · 可更新" if r.can_update else "在线 · 暂不可更新"
        self.set_status(title, color)
        can_txt = "是" if r.can_update else "否"
        if not r.can_update and r.error:
            can_txt += f"（{r.error}）"
        self.set_info(
            [
                f"目标: {r.host}    解析: {r.ip} ({r.how})",
                f"在线: 是    接口: {r.api}    STA: {'是' if r.sta else '否'}",
                f"固件版本: {r.fw or '-'}    编译时间: {r.build or '-'}",
                f"OTA服务: {'就绪' if r.ota else '未就绪'}    TCP:{OTA_PORT}: "
                f"{'通' if r.tcp_ota else '不通'}",
                f"能否更新: {can_txt}    运行: {r.uptime_ms // 1000}s",
                f"本地固件: {read_local_fw_hint(self.bin_var.get())}",
            ]
        )
        self.log(
            f"在线 {r.ip}  fw={r.fw or '?'}  can_update={r.can_update}\n",
            "ok",
        )

    def on_build(self) -> None:
        if self.busy:
            return
        self.set_busy(True)
        self.set_status("编译中…", "#f2c94c")
        self.pbar.configure(mode="indeterminate")
        self.pbar.start(8)

        def worker():
            rc = run_build(lambda t: self.log(t))

            def done():
                self.pbar.stop()
                self.pbar.configure(mode="determinate", value=0)
                self.set_busy(False)
                if rc == 0:
                    self.set_status("编译完成，可更新", "#3dd68c")
                    self.log("编译成功 ✓\n", "ok")
                    if not os.path.isfile(self.bin_var.get()):
                        self.bin_var.set(DEFAULT_BIN)
                    self.log(read_local_fw_hint(self.bin_var.get()) + "\n")
                else:
                    self.set_status(f"编译失败 rc={rc}", "#ff6b6b")
                    self.log(f"编译失败 rc={rc}\n", "err")

            self.root.after(0, done)

        threading.Thread(target=worker, daemon=True).start()

    def on_update(self) -> None:
        if self.busy:
            return
        host = self.host_var.get().strip()
        bin_path = self.bin_var.get().strip()
        if not host:
            messagebox.showwarning(APP_TITLE, "请填写 xxxx / 主机名 / IP")
            return
        if not os.path.isfile(bin_path):
            messagebox.showerror(APP_TITLE, f"固件不存在:\n{bin_path}\n可先点「先编译」")
            return

        # 先快速探测
        self.set_busy(True)
        self.set_status("更新前检测…", "#f2c94c")
        self.pbar["value"] = 0

        def worker():
            try:
                host_n = normalize_host(host)
                ip, how = resolve_ip(host_n)
            except Exception as e:
                self.root.after(
                    0,
                    lambda: (
                        self.set_busy(False),
                        self.set_status("无法解析设备", "#ff6b6b"),
                        self.log(f"解析失败: {e}\n", "err"),
                    ),
                )
                return

            pre = probe(host_n)
            self.probe = pre
            if not pre.online:
                self.root.after(
                    0,
                    lambda: (
                        self.set_busy(False),
                        self.set_status("设备离线，已取消", "#ff6b6b"),
                        self.log("更新前检测失败，取消上传。\n", "err"),
                    ),
                )
                return
            if pre.api == "/ota" and not pre.can_update and not pre.tcp_ota:
                self.root.after(
                    0,
                    lambda: (
                        self.set_busy(False),
                        self.set_status("OTA 未就绪，已取消", "#ff6b6b"),
                        self.log(
                            "设备在线但 can_update=0（STA/OTA 未就绪）。\n",
                            "err",
                        ),
                    ),
                )
                return

            old_fw, old_build = pre.fw, pre.build
            self.log(
                f"开始 OTA → {ip} ({how})  旧版本={old_fw or '?'}\n",
                "warn",
            )
            self.root.after(0, lambda: self.set_status("上传中…", "#f2c94c"))

            def pct(p: int):
                def _():
                    if p >= 0:
                        self.pbar["value"] = p

                self.root.after(0, _)

            rc = run_espota(
                ip,
                bin_path,
                lambda t: self.log(t),
                pct,
            )

            if rc != 0:
                self.root.after(
                    0,
                    lambda: (
                        self.set_busy(False),
                        self.pbar.configure(value=0),
                        self.set_status(f"更新失败 rc={rc}", "#ff6b6b"),
                        self.log(f"espota 失败 rc={rc}\n", "err"),
                    ),
                )
                return

            self.log("espota 返回成功，等待设备重启…\n", "ok")
            self.root.after(0, lambda: self.set_status("等待重启…", "#f2c94c"))

            # 轮询最多 ~45s
            ok = False
            new_r: Optional[ProbeResult] = None
            for i in range(18):
                time.sleep(2.5)
                new_r = probe(host_n)
                if new_r.online:
                    ok = True
                    break

            def finish():
                self.set_busy(False)
                self.pbar.configure(value=100 if ok else 0)
                if not ok:
                    self.set_status("上传完成但设备未恢复在线", "#ff6b6b")
                    self.log("设备重启后未能探测到，请手动复检。\n", "err")
                    return
                self.probe = new_r
                assert new_r is not None
                changed = True
                if old_fw and new_r.fw and old_fw == new_r.fw:
                    # 版本号可能未改，看 build
                    if old_build and new_r.build and old_build == new_r.build:
                        changed = False
                self.set_info(
                    [
                        f"目标: {new_r.host}    解析: {new_r.ip} ({new_r.how})",
                        f"在线: 是    接口: {new_r.api}",
                        f"固件版本: {new_r.fw or '-'}    编译时间: {new_r.build or '-'}",
                        f"OTA服务: {'就绪' if new_r.ota else '未就绪'}    "
                        f"能否更新: {'是' if new_r.can_update else '否'}",
                        f"本地固件: {read_local_fw_hint(bin_path)}",
                        f"与更新前: {'版本/编译时间已变化 ✓' if changed else '字段未变（可能未改 FW_VERSION）'}",
                    ]
                )
                if changed:
                    self.set_status("更新成功 ✓ 设备已恢复在线", "#3dd68c")
                    self.log(
                        f"更新成功。新版本={new_r.fw or '?'} build={new_r.build or '?'}\n",
                        "ok",
                    )
                else:
                    self.set_status("更新完成 · 设备在线（版本号未变）", "#f2c94c")
                    self.log(
                        "设备已恢复在线，但 fw/build 未变化——"
                        "若确定烧了同一版本可忽略；改 config.h 的 FW_VERSION 后更易区分。\n",
                        "warn",
                    )
                self.log("请点「更新后复检」随时再看状态。\n", "info")

            self.root.after(0, finish)

        threading.Thread(target=worker, daemon=True).start()


def main() -> None:
    root = tk.Tk()
    try:
        style = ttk.Style()
        if sys.platform == "win32":
            style.theme_use("vista")
    except Exception:
        pass
    OtaApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
