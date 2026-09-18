# -*- coding: utf-8 -*-
"""
433MHz RF 抓包波形查看器（Windows）
- 连接 ESP32 串口，触发 rfcap 抓包
- 可视化脉冲波形，多次抓包叠加对比
- 自动判断固定码/滚码
用法：python rf_capture_viewer.py
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

# [RF] pulses: 350,700,350,...
RE_PULSES = re.compile(r"\[RF\]\s+pulses:\s+(.+)")
RE_COUNT = re.compile(r"\[RF\]\s+抓包成功：(\d+)\s+个脉冲")
RE_SAME = re.compile(r"\[RF\].*码相同")
RE_DIFF = re.compile(r"\[RF\].*码不同")


class CaptureData:
    """一次抓包数据"""

    def __init__(self, pulses: list[int], timestamp: str = None):
        self.pulses = pulses
        self.timestamp = timestamp or datetime.datetime.now().strftime("%H:%M:%S")
        self.label = f"#{timestamp or self.timestamp}"

    @property
    def count(self) -> int:
        return len(self.pulses)

    @property
    def hash(self) -> int:
        h = 0x811C9DC5
        for p in self.pulses:
            h ^= p
            h = (h * 0x01000193) & 0xFFFFFFFF
        return h

    def to_dict(self):
        return {"timestamp": self.timestamp, "pulses": self.pulses, "count": self.count}


class RfCaptureApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("433MHz RF 抓包查看器")
        root.geometry("1000x680")
        root.configure(bg="#0f1419")

        self.ser = None
        self.running = False
        self.reader = None
        self.captures: list[CaptureData] = []
        self.capturing = False
        self.pulse_buf = []
        self.waiting_pulses = False

        self.status = tk.StringVar(value="未连接")
        self.port_var = tk.StringVar()
        self.result_var = tk.StringVar(value="-")
        self.count_var = tk.StringVar(value="0 次抓包")

        # 列出串口
        ports = [p.device for p in list_ports.comports()]
        if not ports:
            ports = ["COM3"]
        if "COM3" in ports:
            ports.remove("COM3")
            ports.insert(0, "COM3")

        self._build_ui(ports)
        self.root.after(200, self.poll_ui)

    def _build_ui(self, ports):
        style = ttk.Style()
        try:
            style.theme_use("clam")
        except Exception:
            pass

        # 顶部工具栏
        top = tk.Frame(self.root, bg="#0f1419")
        top.pack(fill="x", padx=12, pady=10)

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
            text="抓包 (rfcap)",
            bg="#3dd68c",
            fg="black",
            width=12,
            command=self.trigger_capture,
            state="disabled",
        )
        self.cap_btn.pack(side="left", padx=4)

        tk.Button(
            top, text="清空", bg="#2a3548", fg="#c5d0dc", width=6, command=self.clear_all
        ).pack(side="left", padx=4)

        tk.Button(
            top, text="保存", bg="#2a3548", fg="#c5d0dc", width=6, command=self.save_captures
        ).pack(side="left", padx=4)

        tk.Button(
            top, text="加载", bg="#2a3548", fg="#c5d0dc", width=6, command=self.load_captures
        ).pack(side="left", padx=4)

        tk.Label(top, textvariable=self.status, bg="#0f1419", fg="#3dd68c").pack(
            side="left", padx=16
        )
        tk.Label(top, textvariable=self.count_var, bg="#0f1419", fg="#8b9aab").pack(
            side="left", padx=8
        )
        tk.Label(top, text="结果:", bg="#0f1419", fg="#8b9aab").pack(side="left")
        tk.Label(top, textvariable=self.result_var, bg="#0f1419", fg="#f2c94c").pack(
            side="left", padx=4
        )

        # 显示模式切换
        mode_frame = tk.Frame(self.root, bg="#0f1419")
        mode_frame.pack(fill="x", padx=12, pady=(0, 4))

        self.view_mode = tk.StringVar(value="wave")
        tk.Radiobutton(
            mode_frame,
            text="波形图",
            variable=self.view_mode,
            value="wave",
            bg="#0f1419",
            fg="#e7ecf1",
            selectcolor="#1a2332",
            command=self.redraw,
        ).pack(side="left", padx=8)
        tk.Radiobutton(
            mode_frame,
            text="脉冲时序",
            variable=self.view_mode,
            value="timing",
            bg="#0f1419",
            fg="#e7ecf1",
            selectcolor="#1a2332",
            command=self.redraw,
        ).pack(side="left", padx=8)

        self.overlay_var = tk.BooleanVar(value=True)
        tk.Checkbutton(
            mode_frame,
            text="叠加对比",
            variable=self.overlay_var,
            bg="#0f1419",
            fg="#e7ecf1",
            selectcolor="#1a2332",
            command=self.redraw,
        ).pack(side="left", padx=8)

        # 图表
        self.fig = Figure(figsize=(9.5, 4.5), dpi=100, facecolor="#1a2332")
        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor("#0f1419")
        self.ax.tick_params(colors="#8b9aab")
        self.ax.grid(True, alpha=0.25, color="#8b9aab")
        for spine in self.ax.spines.values():
            spine.set_color("#2e3d52")

        self.canvas = FigureCanvasTkAgg(self.fig, master=self.root)
        self.canvas.get_tk_widget().pack(fill="both", expand=True, padx=12, pady=4)

        # 底部日志
        bottom = tk.Frame(self.root, bg="#0f1419")
        bottom.pack(fill="x", padx=12, pady=8)

        tk.Label(bottom, text="日志:", bg="#0f1419", fg="#8b9aab").pack(side="left")
        self.log_text = tk.Text(
            bottom,
            height=5,
            bg="#1a2332",
            fg="#8b9aab",
            font=("Consolas", 9),
            relief="flat",
        )
        self.log_text.pack(fill="x", pady=4)

    def log(self, msg: str):
        def _do():
            self.log_text.insert("end", msg + "\n")
            self.log_text.see("end")
            # 只保留最近 200 行
            lines = int(self.log_text.index("end-1c").split(".")[0])
            if lines > 200:
                self.log_text.delete("1.0", "100.0")

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
            self.ser = serial.Serial(port, 115200, timeout=0.3)
        except Exception as e:
            messagebox.showerror("打开串口失败", str(e))
            return
        self.running = True
        self.connect_btn.config(text="断开", bg="#c0392b")
        self.cap_btn.config(state="normal")
        self.status.set(f"已连接 {port}")
        self.reader = threading.Thread(target=self.read_loop, daemon=True)
        self.reader.start()
        self.log(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] 已连接 {port}")

    def disconnect(self):
        self.running = False
        time.sleep(0.2)
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.connect_btn.config(text="连接", bg="#2f80ed")
        self.cap_btn.config(state="disabled")
        self.status.set("未连接")

    def trigger_capture(self):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("提示", "请先连接串口")
            return
        if self.capturing:
            return
        self.capturing = True
        self.pulse_buf = []
        self.waiting_pulses = False
        self.cap_btn.config(state="disabled", text="抓包中...", bg="#f2c94c")
        self.log(f"[{datetime.datetime.now().strftime('%H:%M:%S')}] 发送 rfcap，请按遥控器...")
        try:
            self.ser.write(b"rfcap\n")
        except Exception as e:
            self.log(f"[ERROR] 发送失败: {e}")
            self.capturing = False
            self.cap_btn.config(state="normal", text="抓包 (rfcap)", bg="#3dd68c")

    def read_loop(self):
        buf = ""
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
            if not line:
                continue
            self.handle_line(line)

    def handle_line(self, line: str):
        # 日志所有 RF 相关行
        if line.startswith("[RF]") or "rfcap" in line.lower():
            self.log(line)

        # 抓包成功，开始收集脉冲
        m_count = RE_COUNT.search(line)
        if m_count:
            self.waiting_pulses = True
            self.pulse_buf = []
            return

        # 脉冲数据行
        m_pulses = RE_PULSES.search(line)
        if m_pulses and self.waiting_pulses:
            data = m_pulses.group(1)
            # 可能有 "...(+N)" 结尾
            data = re.sub(r"\s*\.\.\..*$", "", data)
            try:
                pulses = [int(x.strip()) for x in data.split(",") if x.strip().isdigit()]
                self.pulse_buf = pulses
                self.waiting_pulses = False
                # 创建捕获记录
                cap = CaptureData(self.pulse_buf)
                self.captures.append(cap)
                self.root.after(0, lambda c=cap: self.on_capture(c))
            except Exception as e:
                self.log(f"[ERROR] 解析脉冲失败: {e}")
            return

        # 对比结果
        if RE_SAME.search(line):
            self.root.after(0, lambda: self.result_var.set("固定码（可克隆）"))
            self.root.after(0, lambda: self.result_var.configure(fg="#3dd68c"))
        elif RE_DIFF.search(line):
            self.root.after(0, lambda: self.result_var.set("滚码（需解码）"))
            self.root.after(0, lambda: self.result_var.configure(fg="#f2c94c"))

        # 抓包失败/超时
        if "抓包失败" in line or "超时" in line:
            self.root.after(0, self.capture_done)

    def on_capture(self, cap: CaptureData):
        self.capture_done()
        self.count_var.set(f"{len(self.captures)} 次抓包")
        self.redraw()
        # 多次抓包时自动对比
        if len(self.captures) >= 2:
            self.compare_captures()

    def capture_done(self):
        self.capturing = False
        self.cap_btn.config(state="normal", text="抓包 (rfcap)", bg="#3dd68c")

    def compare_captures(self):
        if len(self.captures) < 2:
            return
        a = self.captures[-2]
        b = self.captures[-1]
        if a.count == b.count:
            mismatches = sum(
                1
                for x, y in zip(a.pulses, b.pulses)
                if abs(x - y) > max(x, y) // 5 and abs(x - y) > 50
            )
            if mismatches == 0:
                self.result_var.set("固定码（码相同）")
                self.result_var.configure(fg="#3dd68c")
                self.log("[COMPARE] 码相同 → 固定码，可克隆")
            else:
                self.result_var.set(f"滚码? ({mismatches}处不同)")
                self.result_var.configure(fg="#f2c94c")
                self.log(f"[COMPARE] {mismatches} 处脉冲不同 → 可能是滚码")
        else:
            self.result_var.set("滚码? (脉冲数不同)")
            self.result_var.configure(fg="#f2c94c")
            self.log("[COMPARE] 脉冲数不同 → 可能是滚码")

    def to_square_wave(self, pulses: list[int]) -> tuple[list, list]:
        """脉冲序列 → 方波坐标"""
        xs = [0.0]
        ys = [1]
        level = 1
        t = 0
        for p in pulses:
            t += p / 1000.0  # ms
            xs.append(t)
            ys.append(level)
            level = 0 if level else 1
            xs.append(t)
            ys.append(level)
        return xs, ys

    def redraw(self):
        self.ax.clear()
        self.ax.set_facecolor("#0f1419")
        self.ax.tick_params(colors="#8b9aab")
        self.ax.grid(True, alpha=0.25, color="#8b9aab")
        for spine in self.ax.spines.values():
            spine.set_color("#2e3d52")

        if not self.captures:
            self.ax.set_title("无数据 — 连接串口后点「抓包」", color="#8b9aab")
            self.canvas.draw_idle()
            return

        colors = ["#2f80ed", "#3dd68c", "#f2c94c", "#e74c3c", "#9b59b6", "#1abc9c"]
        overlay = self.overlay_var.get()
        mode = self.view_mode.get()

        show = self.captures if overlay else self.captures[-1:]

        if mode == "wave":
            self.ax.set_xlabel("时间 (ms)", color="#8b9aab")
            self.ax.set_ylabel("信号", color="#8b9aab")
            self.ax.set_ylim(-0.2, 1.2)
            for i, cap in enumerate(show):
                xs, ys = self.to_square_wave(cap.pulses)
                c = colors[i % len(colors)]
                label = f"#{len(self.captures) - len(show) + i + 1} ({cap.count})"
                self.ax.plot(xs, ys, color=c, lw=1.2, label=label, drawstyle="steps-post")
            self.ax.legend(
                loc="upper right", facecolor="#1a2332", edgecolor="#2e3d52", fontsize=8
            )
            for t in self.ax.get_legend().get_texts():
                t.set_color("#e7ecf1")
        else:
            # 脉冲时序：柱状图显示每个脉冲宽度
            self.ax.set_xlabel("脉冲序号", color="#8b9aab")
            self.ax.set_ylabel("宽度 (us)", color="#8b9aab")
            for i, cap in enumerate(show):
                c = colors[i % len(colors)]
                label = f"#{len(self.captures) - len(show) + i + 1}"
                idx = range(len(cap.pulses))
                self.ax.bar(idx, cap.pulses, color=c, alpha=0.7, label=label, width=1.0)
            self.ax.legend(
                loc="upper right", facecolor="#1a2332", edgecolor="#2e3d52", fontsize=8
            )
            for t in self.ax.get_legend().get_texts():
                t.set_color("#e7ecf1")

        # 标题
        n = len(self.captures)
        if n >= 2:
            self.ax.set_title(f"已抓 {n} 次 | 对比结果: {self.result_var.get()}", color="#e7ecf1")
        else:
            self.ax.set_title(f"已抓 {n} 次（再抓一次对比）", color="#e7ecf1")

        self.fig.tight_layout()
        self.canvas.draw_idle()

    def clear_all(self):
        self.captures.clear()
        self.result_var.set("-")
        self.result_var.configure(fg="#f2c94c")
        self.count_var.set("0 次抓包")
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
            data = [c.to_dict() for c in self.captures]
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
            self.log(f"[SAVE] 已保存到 {path}")
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
            self.log(f"[LOAD] 加载 {len(self.captures)} 次抓包")
            if len(self.captures) >= 2:
                self.compare_captures()
        except Exception as e:
            messagebox.showerror("加载失败", str(e))

    def poll_ui(self):
        self.root.after(300, self.poll_ui)

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
