# -*- coding: utf-8 -*-
"""
433MHz RF 抓包波形查看器（Windows）
- 串口触发 rfcap，解析完整脉冲序列并画波形
- 按重复帧对齐对比，区分「按压次数不同」与「码不同」
用法：双击 RF.bat 或 python rf_capture_viewer.py
"""
import datetime
import json
import os
import re
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

import matplotlib

matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

# 固件输出: [RF] pulses: 350,700,350,...
RE_PULSES = re.compile(r"\[RF\]\s+pulses:\s*(.+)")
# 兼容旧格式: [RF] 350,700,...
RE_PULSES_OLD = re.compile(r"^\[RF\]\s+(\d+(?:\s*,\s*\d+)+)\s*$")
RE_COUNT = re.compile(r"\[RF\]\s+抓包成功：(\d+)")
RE_SAME = re.compile(r"码相同|固定码")
RE_DIFF = re.compile(r"码不同|可能是滚码")


def detect_frame_len(pulses: list[int], min_len: int = 16, max_len: int = 400) -> int:
    """检测重复帧长度：pulses[i] ≈ pulses[i+L]"""
    n = len(pulses)
    if n < min_len * 2:
        return n
    best_l = n
    best_score = -1
    max_l = min(max_len, n // 2)
    for length in range(min_len, max_l + 1):
        score = 0
        cmp = 0
        check = min(n - length, length * 2)
        for i in range(check):
            a, b = pulses[i], pulses[i + length]
            mx = max(a, b, 1)
            d = abs(a - b)
            if d <= max(mx // 4, 80):
                score += 1
            cmp += 1
        if cmp >= 16 and score * 10 >= cmp * 8:
            if score > best_score or (score == best_score and length > best_l):
                best_score = score
                best_l = length
    return best_l


def first_frame(pulses: list[int], max_frame: int = 240) -> list[int]:
    fl = min(detect_frame_len(pulses), max_frame, len(pulses))
    return pulses[:fl]


def pulses_equal(a: list[int], b: list[int]) -> tuple[bool, int, int]:
    """返回 (相同?, 差异数, 比较长度)"""
    n = min(len(a), len(b), 240)
    if n == 0:
        return False, 999, 0
    mismatches = 0
    for i in range(n):
        x, y = a[i], b[i]
        mx = max(x, y, 1)
        if abs(x - y) > max(mx // 4, 80):
            mismatches += 1
    return mismatches == 0, mismatches, n


class CaptureData:
    def __init__(self, pulses: list[int], timestamp: str = None):
        self.pulses = pulses
        self.timestamp = timestamp or datetime.datetime.now().strftime("%H:%M:%S")

    @property
    def count(self) -> int:
        return len(self.pulses)

    @property
    def duration_ms(self) -> float:
        return sum(self.pulses) / 1000.0

    @property
    def frame_len(self) -> int:
        return detect_frame_len(self.pulses)

    @property
    def hash(self) -> int:
        h = 0x811C9DC5
        for p in self.pulses:
            h ^= p
            h = (h * 0x01000193) & 0xFFFFFFFF
        return h

    def to_dict(self):
        return {
            "timestamp": self.timestamp,
            "pulses": self.pulses,
            "count": self.count,
        }


class RfCaptureApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("433MHz RF 抓包查看器")
        root.geometry("1040x760")
        root.configure(bg="#0f1419")

        self.ser = None
        self.running = False
        self.reader = None
        self.captures: list[CaptureData] = []
        self.capturing = False
        self.waiting_pulses = False
        self.capture_started = False  # 已看到固件 [RF] 等待信号
        self.capture_deadline = 0.0   # 抓包总超时（工具侧兜底）
        self.board_ready = False
        self.continuous = False
        self._watchdog_started = False

        self.status = tk.StringVar(value="未连接")
        self.port_var = tk.StringVar()
        self.result_var = tk.StringVar(value="-")
        self.count_var = tk.StringVar(value="0 次抓包")
        self.stats_var = tk.StringVar(value="-")

        ports = [p.device for p in list_ports.comports()]
        if not ports:
            ports = ["COM3"]
        if "COM3" in ports:
            ports.remove("COM3")
            ports.insert(0, "COM3")

        self._build_ui(ports)
        self.root.after(300, self.tick_watchdog)

    def _build_ui(self, ports):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except Exception:
            pass

        top = tk.Frame(self.root, bg="#0f1419")
        top.pack(fill="x", padx=12, pady=(10, 4))

        tk.Label(top, text="串口", bg="#0f1419", fg="#8b9aab").pack(side="left")
        self.port_cb = ttk.Combobox(
            top, textvariable=self.port_var, values=ports, width=10, state="readonly"
        )
        self.port_cb.set(ports[0])
        self.port_cb.pack(side="left", padx=6)

        self.connect_btn = tk.Button(
            top, text="连接", bg="#2f80ed", fg="white", width=8, command=self.toggle_connect
        )
        self.connect_btn.pack(side="left", padx=4)

        self.cap_btn = tk.Button(
            top,
            text="开始抓包",
            bg="#3dd68c",
            fg="black",
            width=12,
            command=self.toggle_capture,
            state="disabled",
        )
        self.cap_btn.pack(side="left", padx=4)

        for text, cmd in (("清空", self.clear_all), ("保存", self.save_captures), ("加载", self.load_captures)):
            tk.Button(
                top, text=text, bg="#2a3548", fg="#c5d0dc", width=6, command=cmd
            ).pack(side="left", padx=3)

        tk.Label(top, textvariable=self.status, bg="#0f1419", fg="#3dd68c").pack(
            side="left", padx=12
        )
        tk.Label(top, textvariable=self.count_var, bg="#0f1419", fg="#8b9aab").pack(
            side="left", padx=6
        )
        tk.Label(top, text="结果:", bg="#0f1419", fg="#8b9aab").pack(side="left")
        tk.Label(top, textvariable=self.result_var, bg="#0f1419", fg="#f2c94c").pack(
            side="left", padx=4
        )

        # 第二行：视图控制
        row2 = tk.Frame(self.root, bg="#0f1419")
        row2.pack(fill="x", padx=12, pady=(0, 4))

        self.view_mode = tk.StringVar(value="wave")
        for val, text in (("wave", "方波"), ("frame", "首帧方波"), ("timing", "脉宽柱状")):
            tk.Radiobutton(
                row2,
                text=text,
                variable=self.view_mode,
                value=val,
                bg="#0f1419",
                fg="#e7ecf1",
                selectcolor="#1a2332",
                command=self.redraw,
            ).pack(side="left", padx=6)

        self.overlay_var = tk.BooleanVar(value=True)
        tk.Checkbutton(
            row2,
            text="叠加对比",
            variable=self.overlay_var,
            bg="#0f1419",
            fg="#e7ecf1",
            selectcolor="#1a2332",
            command=self.redraw,
        ).pack(side="left", padx=8)

        self.zoom_var = tk.StringVar(value="frame")
        tk.Label(row2, text="缩放:", bg="#0f1419", fg="#8b9aab").pack(side="left", padx=(16, 4))
        for val, text in (("frame", "首帧"), ("80ms", "80ms"), ("full", "完整")):
            tk.Radiobutton(
                row2,
                text=text,
                variable=self.zoom_var,
                value=val,
                bg="#0f1419",
                fg="#e7ecf1",
                selectcolor="#1a2332",
                command=self.redraw,
            ).pack(side="left", padx=4)

        tk.Label(row2, textvariable=self.stats_var, bg="#0f1419", fg="#6b7c8f").pack(
            side="right", padx=8
        )

        # 图表
        self.fig = Figure(figsize=(10, 4.2), dpi=100, facecolor="#1a2332")
        self.ax = self.fig.add_subplot(111)
        self._style_ax()

        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.get_tk_widget().pack(fill="both", expand=True, padx=12, pady=4)

        # 底部：提示 + 日志
        tip = tk.Label(
            self.root,
            text="建议：短促点按遥控（按一下松手），每次手法一致；模块天线对准遥控器 10~20cm",
            bg="#0f1419",
            fg="#f2c94c",
            font=("Segoe UI", 9),
        )
        tip.pack(fill="x", padx=12)

        bottom = tk.Frame(self.root, bg="#0f1419")
        bottom.pack(fill="both", padx=12, pady=(4, 10))
        tk.Label(bottom, text="串口日志:", bg="#0f1419", fg="#8b9aab").pack(anchor="w")
        self.log_text = tk.Text(
            bottom,
            height=7,
            bg="#1a2332",
            fg="#8b9aab",
            font=("Consolas", 9),
            relief="flat",
            wrap="none",
        )
        self.log_text.pack(fill="both", expand=True, pady=4)

    def _style_ax(self):
        self.ax.set_facecolor("#0f1419")
        self.ax.tick_params(colors="#8b9aab")
        self.ax.grid(True, alpha=0.25, color="#8b9aab")
        for spine in self.ax.spines.values():
            spine.set_color("#2e3d52")

    def log(self, msg: str):
        def _do():
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
            lines = int(self.log_text.index("end-1c").split(".")[0])
            if lines > 300:
                self.log_text.delete("1.0", "80.0")

        self.root.after(0, _do)

    def toggle_connect(self):
        if self.running:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        port = self.port_var.get()
        if not port:
            messagebox.showerror("错误", "请选择串口")
            return
        try:
            # 不拉 DTR/RTS，避免打开串口时复位 ESP32，丢掉第一条命令
            self.ser = serial.Serial()
            self.ser.port = port
            self.ser.baudrate = 115200
            self.ser.timeout = 0.3
            self.ser.dsrdtr = False
            self.ser.rtscts = False
            self.ser.open()
            try:
                self.ser.dtr = False
                self.ser.rts = False
            except Exception:
                pass
            self.ser.reset_input_buffer()
        except Exception as e:
            messagebox.showerror("打开串口失败", str(e))
            return
        self.running = True
        self.capturing = False
        self.capture_started = False
        self.continuous = False
        self.board_ready = False
        self._ready_deadline = time.time() + 35.0
        self.connect_btn.config(text="断开", bg="#c0392b")
        # 启动期间禁用抓包，等板子跑完再允许
        self.cap_btn.config(state="disabled", text="启动中...", bg="#2a3548")
        self.status.set(f"已连接 {port}（等待固件 ready…）")
        self.reader = threading.Thread(target=self.read_loop, daemon=True)
        self.reader.start()
        self.log(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] 已连接 {port}")
        self.log("[*] 等待 [BOOT] ready / help 响应（NFC 初始化约 20s，勿提前点抓包）…")
        self.root.after(500, self._probe_ready)
        if not getattr(self, "_watchdog_started", False):
            self._watchdog_started = True
            self.root.after(400, self.tick_watchdog)

    def _probe_ready(self):
        """轮询 help，直到固件真正进入 loop() 再允许抓包。"""
        if not self.running or not self.ser or not self.ser.is_open:
            return
        if getattr(self, "board_ready", False):
            return
        try:
            self.ser.write(b"help\n")
            self.ser.flush()
        except Exception:
            pass
        if time.time() > self._ready_deadline:
            self.log("[WARN] 35s 仍未见 help 响应，仍尝试启用抓包")
            self._mark_ready("超时强制启用")
            return
        self.root.after(1000, self._probe_ready)

    def _set_cap_btn(self, enabled: bool, text: str, bg: str):
        self.cap_btn.config(state=("normal" if enabled else "disabled"), text=text, bg=bg)

    def _mark_ready(self, why: str):
        if not self.running:
            return
        first = not self.board_ready
        self.board_ready = True
        if self.capturing:
            # 连续抓包中：按钮可点，用于「停止」
            self.cap_btn.config(state="normal", text="停止抓包", bg="#e74c3c")
            return
        self.cap_btn.config(state="normal", text="开始抓包", bg="#3dd68c")
        if first:
            self.status.set(self.status.get().replace("（等待固件 ready…）", "") + " 就绪")
            self.log(f"[*] 固件就绪（{why}），点「开始抓包」持续听，点「停止抓包」结束")
            # 复位后 rfauto 可能丢；就绪后自动打开（固件也会从 NVS 恢复）
            try:
                self.ser.write(b"rfauto on\n")
                self.ser.flush()
                self.log("[*] 已自动发送 rfauto on（每 5s 发开门码）")
            except Exception as e:
                self.log(f"[WARN] 自动 rfauto on 失败: {e}")

    def _on_boot_ready(self):
        self._probe_ready()

    def disconnect(self):
        self.running = False
        time.sleep(0.2)
        if self.ser and self.ser.is_open:
            try:
                if self.capturing:
                    self.ser.write(b"rfstop\n")
                    self.ser.flush()
                    time.sleep(0.2)
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.capturing = False
        self.capture_started = False
        self.continuous = False
        self.board_ready = False
        self.connect_btn.config(text="连接", bg="#2f80ed")
        self.cap_btn.config(state="disabled", text="开始抓包", bg="#2a3548")
        self.status.set("未连接")

    def toggle_capture(self):
        if self.capturing:
            self.stop_capture()
        else:
            self.trigger_capture()

    def stop_capture(self):
        if not self.ser or not self.ser.is_open:
            self.capture_done()
            return
        self.log("[*] 发送 rfstop，停止连续抓包…")
        self.cap_btn.config(state="disabled", text="停止中...", bg="#8e44ad")
        try:
            self.ser.write(b"rfstop\n")
            self.ser.flush()
        except Exception as e:
            self.log(f"[ERROR] rfstop 失败: {e}")
            self.capture_done()
        # 看门狗：若固件 3s 内没回 RFCAP_END，强制恢复 UI
        self.root.after(3000, self._force_stop_ui)

    def _force_stop_ui(self):
        if self.capturing:
            self.log("[WARN] 3s 未收到 RFCAP_END，强制恢复按钮")
            self.capture_done()

    def trigger_capture(self):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("提示", "请先连接串口")
            return
        if self.capturing:
            return
        if not self.board_ready:
            messagebox.showwarning("提示", "固件尚未就绪，请等按钮变为「开始抓包」")
            return
        self.capturing = True
        self.capture_started = False
        self.waiting_pulses = False
        self.continuous = True
        # 连续模式：不设短超时；ACK 用 8s 看门狗
        self.capture_deadline = time.time() + 8.0
        self.cap_btn.config(state="normal", text="停止抓包", bg="#e74c3c")
        self.status.set("连续抓包中 — 收到帧会不断累加；点「停止抓包」结束")
        self.log(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] 发送 rfcap…（连续模式）")
        try:
            self.ser.write(b"rfcap\n")
            self.ser.flush()
        except Exception as e:
            self.log(f"[ERROR] 发送失败: {e}")
            self.capture_done()
            return
        self.root.after(1500, self._check_ack, 1)
        self._tick_capture_ui()

    def _tick_capture_ui(self):
        if not self.capturing:
            return
        if not self.continuous:
            remain = max(0, int(self.capture_deadline - time.time()))
            label = f"抓包中 {remain}s" if self.capture_started else f"等待ACK {remain}s"
            self.cap_btn.config(state="disabled", text=label, bg="#f2c94c")
        else:
            # 连续：按钮保持可点 = 停止
            if time.time() > self.capture_deadline and not self.capture_started:
                # ACK 超时仍未开始
                self.log("[WARN] 8s 未见 ACK，仍保持停止按钮（固件可能已在听）")
                self.capture_deadline = time.time() + 60.0
            n = len(self.captures)
            self.cap_btn.config(state="normal", text=f"停止 ({n}帧)", bg="#e74c3c")
        self.root.after(1000, self._tick_capture_ui)

    def _check_ack(self, attempt: int):
        if not self.capturing:
            return
        if self.capture_started:
            self.cap_btn.config(state="normal", text="停止抓包", bg="#e74c3c")
            return
        if attempt <= 3 and self.ser and self.ser.is_open:
            self.log(f"[*] 未收到固件 ACK，重发 rfcap（{attempt}/3）…")
            try:
                self.ser.write(b"rfcap\n")
                self.ser.flush()
            except Exception as e:
                self.log(f"[ERROR] 重发失败: {e}")
                self.capture_done()
                return
            self.root.after(2000, self._check_ack, attempt + 1)
            return
        self.log("[WARN] 未见 ACK，保持「停止」按钮（固件可能已在连续听）")
        self.capture_started = True

    def tick_watchdog(self):
        # 连续模式：只在 ACK 阶段用短看门狗；已 ACK 则不因时间强制结束
        if (
            self.capturing
            and not self.continuous
            and self.capture_deadline
            and time.time() > self.capture_deadline
        ):
            self.log("[ERROR] 工具侧超时，复位抓包状态（可再点一次）")
            self.capture_done()
        elif (
            self.capturing
            and self.continuous
            and not self.capture_started
            and self.capture_deadline
            and time.time() > self.capture_deadline
        ):
            self.log("[WARN] ACK 看门狗到期，仍保持停止按钮")
            self.capture_started = True
            self.capture_deadline = 0
        self.root.after(400, self.tick_watchdog)

    def read_loop(self):
        while self.running and self.ser and self.ser.is_open:
            try:
                raw = self.ser.readline()
            except Exception:
                time.sleep(0.2)
                continue
            if not raw:
                continue
            try:
                line = raw.decode("utf-8", errors="replace").strip()
            except Exception:
                continue
            if line:
                self.handle_line(line)

    def handle_line(self, line: str):
        if line.startswith("[RF]") or "rfcap" in line.lower() or line.startswith("[CMD]") or "cmds:" in line:
            self.log(line)

        # 固件确认进入连续抓包 → 按钮保持可点的「停止」
        if "RFCAP_OK" in line or "等待信号" in line or "请短按遥控器" in line:
            was_idle = not self.capture_started
            self.capture_started = True
            if self.capturing:
                self.root.after(0, lambda: self.cap_btn.config(
                    state="normal", text="停止抓包", bg="#e74c3c"))
                if was_idle:
                    self.log("[*] 固件 ACK，连续抓包中 — 可反复按遥控，点「停止抓包」结束")

        if "cmds:" in line or "[BOOT] ready" in line:
            self.root.after(0, lambda: self._mark_ready("help/ready"))

        if "RFCAP_FRAME" in line:
            self.log("[*] 本帧完成，继续监听下一帧…")
            return

        if RE_COUNT.search(line):
            self.waiting_pulses = True
            return

        m = RE_PULSES.search(line)
        if m:
            data = m.group(1)
            data = re.sub(r"\s*\.\.\..*$", "", data)
            pulses = []
            for x in re.split(r"[,\s]+", data.strip()):
                if x.isdigit():
                    pulses.append(int(x))
            self.waiting_pulses = False
            if len(pulses) >= 10:
                cap = CaptureData(pulses)
                self.root.after(0, lambda c=cap: self.on_capture_frame(c))
            else:
                self.log("[ERROR] 解析到的脉冲太少")
            return

        m2 = RE_PULSES_OLD.match(line)
        if m2 and self.waiting_pulses:
            pulses = [int(x) for x in m2.group(1).split(",") if x.strip().isdigit()]
            self.waiting_pulses = False
            if len(pulses) >= 10:
                cap = CaptureData(pulses)
                self.root.after(0, lambda c=cap: self.on_capture_frame(c))
            return

        if RE_SAME.search(line) and "结论" in line:
            self.root.after(0, lambda: self.result_var.set("固定码（可克隆）"))
            self.root.after(0, lambda: self.result_var.configure(fg="#3dd68c"))
        elif RE_DIFF.search(line) and ("结论" in line or "滚码" in line):
            self.root.after(0, lambda: self.result_var.set("滚码（需解码）"))
            self.root.after(0, lambda: self.result_var.configure(fg="#f2c94c"))

        if "RFCAP_END" in line:
            self.log("[*] 收到 RFCAP_END，连续抓包结束")
            self.root.after(0, self.capture_done)
            return

        # 连续模式下单次「抓包失败」不退出
        if "抓包失败" in line and not getattr(self, "continuous", False):
            self.root.after(0, self.capture_done)

    def on_capture_frame(self, cap: CaptureData):
        """连续模式：每帧入列表，不结束抓包。"""
        self.captures.append(cap)
        if len(self.captures) > 12:
            self.captures = self.captures[-12:]
        self.count_var.set(f"{len(self.captures)} 次抓包")
        self.stats_var.set(
            f"最近: {cap.count} 脉冲 / {cap.duration_ms:.1f}ms / 帧长≈{cap.frame_len} / hash=0x{cap.hash:08X}"
        )
        self.redraw()
        if len(self.captures) >= 2:
            self.compare_captures()
        if self.capturing:
            self.cap_btn.config(
                state="normal", text=f"停止 ({len(self.captures)}帧)", bg="#e74c3c"
            )
            self.log(f"[*] 已收录第 {len(self.captures)} 帧，继续监听…")

    def on_capture(self, cap: CaptureData):
        self.on_capture_frame(cap)
        if not getattr(self, "continuous", False):
            self.capture_done()

    def capture_done(self):
        self.capturing = False
        self.capture_started = False
        self.continuous = False
        self.waiting_pulses = False
        self.capture_deadline = 0.0
        if self.running and self.board_ready:
            self.cap_btn.config(state="normal", text="开始抓包", bg="#3dd68c")
            self.status.set("就绪 — 点「开始抓包」可再次连续抓")
        elif self.running:
            self.cap_btn.config(state="disabled", text="启动中...", bg="#2a3548")
        else:
            self.cap_btn.config(state="disabled", text="开始抓包", bg="#2a3548")

    def compare_captures(self):
        if len(self.captures) < 2:
            return
        a = self.captures[-2]
        b = self.captures[-1]
        fa = first_frame(a.pulses)
        fb = first_frame(b.pulses)
        same, mismatches, n = pulses_equal(fa, fb)

        if a.count != b.count:
            self.log(
                f"[COMPARE] 总脉冲 {a.count} vs {b.count}（按压时长不同，忽略）；"
                f"单帧 {a.frame_len} vs {b.frame_len}"
            )

        if same:
            self.result_var.set("固定码（单帧相同）")
            self.result_var.configure(fg="#3dd68c")
            self.log(f"[COMPARE] 单帧 {n} 脉冲一致 → 固定码，可克隆")
        else:
            self.result_var.set(f"滚码? 单帧{mismatches}处不同")
            self.result_var.configure(fg="#f2c94c")
            self.log(f"[COMPARE] 单帧 {n} 点中有 {mismatches} 处不同 → 可能是滚码")

    @staticmethod
    def to_square_wave(pulses: list[int]) -> tuple[list, list]:
        xs = [0.0]
        ys = [1.0]
        level = 1
        t = 0.0
        for p in pulses:
            t += p / 1000.0
            xs.append(t)
            ys.append(level)
            level = 0 if level else 1
            xs.append(t)
            ys.append(level)
        return xs, ys

    @staticmethod
    def trim_to_duration(pulses: list[int], max_ms: float) -> list[int]:
        out = []
        total = 0.0
        for p in pulses:
            total += p / 1000.0
            out.append(p)
            if total >= max_ms:
                break
        return out

    def redraw(self):
        self.ax.clear()
        self._style_ax()

        if not self.captures:
            self.ax.set_title("无数据 — 连接串口后点「抓包」，短按遥控器", color="#8b9aab")
            self.canvas.draw_idle()
            return

        colors = ["#2f80ed", "#3dd68c", "#f2c94c", "#e74c3c", "#9b59b6", "#1abc9c"]
        show = self.captures if self.overlay_var.get() else self.captures[-1:]
        mode = self.view_mode.get()
        zoom = self.zoom_var.get()

        if mode == "timing":
            self.ax.set_xlabel("脉冲序号", color="#8b9aab")
            self.ax.set_ylabel("宽度 (us)", color="#8b9aab")
            for i, cap in enumerate(show):
                pulses = cap.pulses[:240]
                c = colors[i % len(colors)]
                self.ax.bar(
                    range(len(pulses)),
                    pulses,
                    color=c,
                    alpha=0.65,
                    width=1.0,
                    label=f"#{len(self.captures) - len(show) + i + 1} ({cap.count})",
                )
            self.ax.legend(
                loc="upper right", facecolor="#1a2332", edgecolor="#2e3d52", fontsize=8
            )
            for t in self.ax.get_legend().get_texts():
                t.set_color("#e7ecf1")
        else:
            self.ax.set_xlabel("时间 (ms)", color="#8b9aab")
            self.ax.set_ylabel("信号", color="#8b9aab")
            self.ax.set_ylim(-0.15, 1.15)
            for i, cap in enumerate(show):
                pulses = cap.pulses
                if mode == "frame":
                    pulses = first_frame(pulses)
                elif zoom == "80ms":
                    pulses = self.trim_to_duration(pulses, 80)
                elif zoom == "frame" and mode == "wave":
                    pulses = first_frame(pulses)
                xs, ys = self.to_square_wave(pulses)
                c = colors[i % len(colors)]
                n = len(self.captures) - len(show) + i + 1
                self.ax.plot(
                    xs,
                    ys,
                    color=c,
                    lw=1.3,
                    label=f"#{n} n={cap.count}",
                    drawstyle="steps-post",
                )
            self.ax.legend(
                loc="upper right", facecolor="#1a2332", edgecolor="#2e3d52", fontsize=8
            )
            for t in self.ax.get_legend().get_texts():
                t.set_color("#e7ecf1")

        n = len(self.captures)
        if n >= 2:
            self.ax.set_title(
                f"已抓 {n} 次 | {self.result_var.get()}", color="#e7ecf1", fontsize=11
            )
        else:
            self.ax.set_title(
                f"已抓 {n} 次（再抓一次做单帧对比）", color="#e7ecf1", fontsize=11
            )
        self.fig.tight_layout()
        self.canvas.draw_idle()

    def clear_all(self):
        self.captures.clear()
        self.result_var.set("-")
        self.result_var.configure(fg="#f2c94c")
        self.count_var.set("0 次抓包")
        self.stats_var.set("-")
        self.redraw()

    def save_captures(self):
        if not self.captures:
            messagebox.showinfo("提示", "没有可保存的数据")
            return
        path = os.path.join(
            os.path.expanduser("~"),
            "Pictures",
            f"rf_capture_{datetime.datetime.now().strftime('%Y%m%d_%H%M%S')}.json",
        )
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", encoding="utf-8") as f:
                json.dump([c.to_dict() for c in self.captures], f, ensure_ascii=False, indent=2)
            self.log(f"[SAVE] {path}")
            messagebox.showinfo("已保存", path)
        except Exception as e:
            messagebox.showerror("保存失败", str(e))

    def load_captures(self):
        from tkinter import filedialog

        path = filedialog.askopenfilename(
            title="加载抓包数据",
            filetypes=[("JSON", "*.json"), ("所有文件", "*.*")],
        )
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            self.captures = [CaptureData(d["pulses"], d.get("timestamp")) for d in data]
            self.count_var.set(f"{len(self.captures)} 次抓包")
            self.redraw()
            self.log(f"[LOAD] {len(self.captures)} 次")
            if len(self.captures) >= 2:
                self.compare_captures()
        except Exception as e:
            messagebox.showerror("加载失败", str(e))

    def on_close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    app = RfCaptureApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
