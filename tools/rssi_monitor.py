# -*- coding: utf-8 -*-
"""
ESP32 蓝牙 RSSI 监控（Windows）
- 连 COM 口，实时画 经典蓝牙 + BLE 特征 RSSI
- 同时写入 CSV 日志
用法：python rssi_monitor.py
"""
import csv
import datetime
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

# LOG 行例：
# [LOG] door=2 ... | rssi=-127 ... | ble_rssi=-19 f=MiCarCDB8
# [BLE] match_rssi=-19 label=MiCarCDB8
# [BLE] track MiCarCDB8 rssi=-19
RE_CLASSIC = re.compile(r"\|\s*rssi=(-?\d+)")
RE_BLE_RSSI = re.compile(r"ble_rssi=(-?\d+)")
RE_MATCH = re.compile(r"match_rssi=(-?\d+)")
RE_TRACK = re.compile(r"\[BLE\]\s+track\s+(\S+)\s+rssi=(-?\d+)")
RE_LABEL = re.compile(r"label=(\S+)")


class RssiMonitorApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("ESP32 蓝牙 RSSI 监控")
        root.geometry("920x620")
        root.configure(bg="#0f1419")

        self.ser = None
        self.running = False
        self.reader = None
        self.t0 = time.time()
        self.points = []  # list of dict t, classic, ble, label
        self.ble_label = tk.StringVar(value="-")
        self.status = tk.StringVar(value="未连接")
        self.port_var = tk.StringVar()
        self.max_points = 400  # 约 2+ 分钟（采样约 3–4s 一点）

        ports = [p.device for p in list_ports.comports()]
        if not ports:
            ports = ["COM3"]
        if "COM3" in ports:
            ports.remove("COM3")
            ports.insert(0, "COM3")

        style = ttk.Style()
        try:
            style.theme_use("clam")
        except Exception:
            pass

        top = tk.Frame(root, bg="#0f1419")
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

        tk.Button(
            top, text="清空曲线", bg="#2a3548", fg="#c5d0dc", width=8, command=self.clear_chart
        ).pack(side="left", padx=4)

        tk.Label(top, textvariable=self.status, bg="#0f1419", fg="#3dd68c").pack(
            side="left", padx=16
        )
        tk.Label(top, text="BLE 特征:", bg="#0f1419", fg="#8b9aab").pack(side="left")
        tk.Label(top, textvariable=self.ble_label, bg="#0f1419", fg="#f2c94c").pack(
            side="left", padx=4
        )

        self.fig = Figure(figsize=(9, 4.2), dpi=100, facecolor="#1a2332")
        self.ax = self.fig.add_subplot(111)
        self.ax.set_facecolor("#0f1419")
        self.ax.tick_params(colors="#8b9aab")
        self.ax.set_xlabel("时间 (秒)", color="#8b9aab")
        self.ax.set_ylabel("RSSI (dBm)", color="#8b9aab")
        self.ax.set_ylim(-110, -10)
        self.ax.grid(True, alpha=0.25, color="#8b9aab")
        for spine in self.ax.spines.values():
            spine.set_color("#2e3d52")
        (self.line_c,) = self.ax.plot([], [], color="#2f80ed", lw=1.5, label="经典蓝牙")
        (self.line_b,) = self.ax.plot([], [], color="#3dd68c", lw=1.8, label="BLE 特征")
        leg = self.ax.legend(loc="upper right", facecolor="#1a2332", edgecolor="#2e3d52")
        for t in leg.get_texts():
            t.set_color("#e7ecf1")

        self.canvas = FigureCanvasTkAgg(self.fig, master=root)
        self.canvas.get_tk_widget().pack(fill="both", expand=True, padx=12, pady=4)

        bottom = tk.Frame(root, bg="#0f1419")
        bottom.pack(fill="x", padx=12, pady=8)
        tk.Label(bottom, text="日志:", bg="#0f1419", fg="#8b9aab").pack(side="left")
        self.log_path = tk.StringVar(value=self.default_log_path())
        tk.Label(
            bottom, textvariable=self.log_path, bg="#0f1419", fg="#6b7c8f", font=("Segoe UI", 8)
        ).pack(side="left", padx=8)

        self.root.after(200, self.poll_ui)

    def default_log_path(self):
        d = datetime.datetime.now().strftime("%Y%m%d")
        folder = r"D:\mimo\车库门自动化\ble_logs"
        try:
            import os

            os.makedirs(folder, exist_ok=True)
        except Exception:
            folder = "."
        return rf"{folder}\rssi_{d}_{datetime.datetime.now().strftime('%H%M%S')}.csv"

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
        self.status.set(f"已连接 {port} @115200")
        self.reader = threading.Thread(target=self.read_loop, daemon=True)
        self.reader.start()

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
        self.status.set("未连接")

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
        classic = None
        ble = None
        label = None

        m_ble = RE_BLE_RSSI.search(line)
        if m_ble:
            ble = int(m_ble.group(1))
        m_match = RE_MATCH.search(line)
        if m_match:
            ble = int(m_match.group(1))
        m_track = RE_TRACK.search(line)
        if m_track:
            label = m_track.group(1)
            ble = int(m_track.group(2))
        m_lab = RE_LABEL.search(line)
        if m_lab and label is None:
            label = m_lab.group(1)

        if classic is None and "|" in line and "rssi=" in line and "ble_rssi=" in line:
            m_c = RE_CLASSIC.search(line)
            if m_c:
                classic = int(m_c.group(1))
        elif classic is None and line.startswith("[LOG]"):
            m_c = re.search(r"\brssi=(-?\d+)", line)
            if m_c:
                classic = int(m_c.group(1))

        if classic is None and ble is None:
            return

        if label:
            self.root.after(0, lambda v=label: self.ble_label.set(v))

        t = time.time() - self.t0
        # LOG 行会同时含 classic+ble，记一点
        if classic is not None or ble is not None:
            self.points.append(
                {
                    "t": t,
                    "classic": classic,
                    "ble": ble if ble is not None else (
                        self.points[-1]["ble"] if self.points else None
                    ),
                    "line": line[:200],
                }
            )
            if len(self.points) > self.max_points:
                self.points = self.points[-self.max_points :]
            self.append_csv(t, classic, ble, label, line)

    def append_csv(self, t, classic, ble, label, line):
        path = self.log_path.get()
        try:
            newf = not __import__("os").path.exists(path)
            with open(path, "a", newline="", encoding="utf-8-sig") as f:
                w = csv.writer(f)
                if newf:
                    w.writerow(["time_iso", "t_sec", "classic_rssi", "ble_rssi", "ble_label", "raw"])
                w.writerow(
                    [
                        datetime.datetime.now().isoformat(timespec="seconds"),
                        f"{t:.1f}",
                        "" if classic is None else classic,
                        "" if ble is None else ble,
                        label or "",
                        line,
                    ]
                )
        except Exception:
            pass

    def clear_chart(self):
        self.points.clear()
        self.line_c.set_data([], [])
        self.line_b.set_data([], [])
        self.ax.relim()
        self.ax.autoscale_view(scalex=True, scaley=False)
        self.ax.set_ylim(-110, -10)
        self.canvas.draw_idle()

    def poll_ui(self):
        if self.points:
            xs = [p["t"] for p in self.points]
            yc = [p["classic"] if p["classic"] is not None else None for p in self.points]
            yb = [p["ble"] if p["ble"] is not None else None for p in self.points]
            # matplotlib 不接受 None，用 nan
            yc = [float("nan") if v is None else v for v in yc]
            yb = [float("nan") if v is None else v for v in yb]
            self.line_c.set_data(xs, yc)
            self.line_b.set_data(xs, yb)
            if xs:
                # 横轴固定显示最近 120 秒
                self.ax.set_xlim(max(0, xs[-1] - 120), max(60, xs[-1] + 2))
            vals = [v for v in yc + yb if v == v]
            if vals:
                lo = min(min(vals) - 10, -100)
                hi = max(max(vals) + 10, -20)
                self.ax.set_ylim(lo, hi)
            self.canvas.draw_idle()
        self.root.after(400, self.poll_ui)

    def on_close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    app = RssiMonitorApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
