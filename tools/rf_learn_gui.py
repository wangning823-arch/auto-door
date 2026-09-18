# -*- coding: utf-8 -*-
"""
315/433 固定码按键学习向导（Windows GUI）
点「学习上/下/暂停」→ 按遥控 → 成功显示通过，失败可重试
"""
import datetime
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports


def pick_ports():
    ports = [p.device for p in list_ports.comports()]
    if not ports:
        return ["COM3"]
    if "COM3" in ports:
        ports.remove("COM3")
        ports.insert(0, "COM3")
    return ports


KEYS = [
    (0, "学习上（开）", "开", "请短按遥控器【上】"),
    (1, "学习下（关）", "关", "请短按遥控器【下】"),
    (2, "学习暂停", "暂停", "请短按遥控器【暂停】"),
]


class RfLearnApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("RF 按键学习向导")
        root.geometry("560x520")
        root.configure(bg="#0f1419")

        self.ser = None
        self.running = False
        self.reader = None
        self.learning_idx = -1  # 当前学习中，-1 空闲
        self.buf = b""
        self.status = tk.StringVar(value="未连接")
        self.port_var = tk.StringVar()
        self.phase_var = tk.StringVar(value="请先连接串口")
        self.hint_var = tk.StringVar(value="")

        # 0/1/2 状态: idle / busy / ok / fail
        self.state = ["idle", "idle", "idle"]
        self.btns: list[tk.Button] = []

        self._build(ports := pick_ports())
        self.port_var.set(ports[0])
        self.root.after(300, self._tick)

    def _build(self, ports):
        top = tk.Frame(self.root, bg="#0f1419")
        top.pack(fill="x", padx=14, pady=(14, 6))
        tk.Label(top, text="串口", bg="#0f1419", fg="#8b9aab").pack(side="left")
        cb = ttk.Combobox(top, textvariable=self.port_var, values=ports, width=10, state="readonly")
        cb.pack(side="left", padx=6)
        self.connect_btn = tk.Button(top, text="连接", bg="#2f80ed", fg="white", width=8, command=self.toggle)
        self.connect_btn.pack(side="left", padx=4)
        tk.Label(top, textvariable=self.status, bg="#0f1419", fg="#3dd68c").pack(side="left", padx=12)

        tk.Label(
            self.root,
            text="流程：连接 → 点「学习上」→ 按遥控【上】→ 显示通过 → 再学下、暂停",
            bg="#0f1419",
            fg="#f2c94c",
            font=("Segoe UI", 10),
            wraplength=500,
            justify="left",
        ).pack(fill="x", padx=14, pady=(4, 8))

        # 三个大按钮
        box = tk.Frame(self.root, bg="#0f1419")
        box.pack(fill="x", padx=14, pady=4)
        for i, (idx, label, _, _) in enumerate(KEYS):
            b = tk.Button(
                box,
                text=label,
                font=("Segoe UI", 13, "bold"),
                width=14,
                height=2,
                bg="#2a3548",
                fg="#e7ecf1",
                state="disabled",
                command=lambda k=idx: self.start_learn(k),
            )
            b.grid(row=i // 1, column=0, sticky="ew", pady=6)
            self.btns.append(b)
        box.columnconfigure(0, weight=1)

        # 状态行
        st = tk.Frame(self.root, bg="#1a2332")
        st.pack(fill="x", padx=14, pady=8)
        self.st_labels = []
        for i, (idx, label, short, _) in enumerate(KEYS):
            lb = tk.Label(st, text=f"{short}: 未学", bg="#1a2332", fg="#8b9aab", width=14, anchor="w")
            lb.pack(side="left", padx=8, pady=8)
            self.st_labels.append(lb)

        tk.Label(self.root, textvariable=self.phase_var, bg="#0f1419", fg="#e7ecf1", font=("Segoe UI", 11, "bold")).pack(
            fill="x", padx=14, pady=(6, 2)
        )
        tk.Label(self.root, textvariable=self.hint_var, bg="#0f1419", fg="#3dd68c", font=("Segoe UI", 10), wraplength=500).pack(
            fill="x", padx=14
        )

        # 附加操作
        extra = tk.Frame(self.root, bg="#0f1419")
        extra.pack(fill="x", padx=14, pady=8)
        for text, cmd in (
            ("刷新状态", self.send_rfkeys),
            ("测试发射 0", lambda: self.send_line("rfplay 0")),
            ("测试发射 1", lambda: self.send_line("rfplay 1")),
            ("测试发射 2", lambda: self.send_line("rfplay 2")),
        ):
            tk.Button(extra, text=text, bg="#2a3548", fg="#c5d0dc", command=cmd).pack(side="left", padx=4)

        # 日志
        tk.Label(self.root, text="日志:", bg="#0f1419", fg="#8b9aab").pack(anchor="w", padx=14)
        self.log_text = tk.Text(
            self.root, height=10, bg="#1a2332", fg="#8b9aab", font=("Consolas", 9), relief="flat", wrap="none"
        )
        self.log_text.pack(fill="both", expand=True, padx=14, pady=(0, 12))

    def log(self, msg: str):
        def _do():
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            self.log_text.insert("end", f"[{ts}] {msg}\n")
            self.log_text.see("end")
            lines = int(self.log_text.index("end-1c").split(".")[0])
            if lines > 250:
                self.log_text.delete("1.0", "80.0")

        self.root.after(0, _do)

    def toggle(self):
        if self.running:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        port = self.port_var.get()
        try:
            self.ser = serial.Serial()
            self.ser.port = port
            self.ser.baudrate = 115200
            self.ser.timeout = 0.25
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
        self.buf = b""
        self.connect_btn.config(text="断开", bg="#c0392b")
        self.status.set(f"已连接 {port}（启动中…）")
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()
        self.log(f"已连接 {port}，等待固件就绪…")
        self.root.after(2500, self._ready)

    def _ready(self):
        if not self.running:
            return
        try:
            self.ser.reset_input_buffer()
            self.ser.write(b"\n")
        except Exception:
            pass
        self.status.set(self.status.get().replace("（启动中…）", "") + " 就绪")
        for b in self.btns:
            b.config(state="normal")
        self.phase_var.set("连接成功，点击按钮开始学习")
        self.hint_var.set("每个键：点按钮 → 看提示按遥控 → 成功显示「通过」")
        self.send_rfkeys()
        self.log("就绪")

    def disconnect(self):
        self.running = False
        self.learning_idx = -1
        time.sleep(0.15)
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.connect_btn.config(text="连接", bg="#2f80ed")
        self.status.set("未连接")
        for b in self.btns:
            b.config(state="disabled")

    def send_line(self, line: str):
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("提示", "请先连接串口")
            return
        try:
            self.ser.write((line + "\n").encode())
            self.ser.flush()
            self.log(f"→ {line}")
        except Exception as e:
            self.log(f"发送失败: {e}")

    def send_rfkeys(self):
        self.send_line("rfkeys")

    def start_learn(self, idx: int):
        if self.learning_idx >= 0:
            return
        if not self.ser or not self.ser.is_open:
            messagebox.showwarning("提示", "请先连接串口")
            return
        self.learning_idx = idx
        _, label, short, hint = KEYS[idx]
        self.btns[idx].config(state="disabled", text="学习中…", bg="#f2c94c", fg="black")
        self.st_labels[idx].config(text=f"{short}: 学习中", fg="#f2c94c")
        self.phase_var.set(f"正在学习：{label}")
        self.hint_var.set(f"{hint}（距离天线 5~10cm，短按一下）")
        self.send_line(f"rflearn {idx}")
        self.log(f"开始学习按键 {idx}（{label}），请按遥控…")

    def _finish_learn(self, idx: int, ok: bool, detail: str = ""):
        _, label, short, _ = KEYS[idx]
        self.learning_idx = -1
        if ok:
            self.state[idx] = "ok"
            self.btns[idx].config(state="normal", text=f"✓ {label}", bg="#1e3d32", fg="#3dd68c")
            self.st_labels[idx].config(text=f"{short}: 通过", fg="#3dd68c")
            self.phase_var.set(f"{label} — 学习通过")
            self.hint_var.set("可点「测试发射」验证，或继续学下一个键")
            self.log(f"按键 {idx} 学习成功")
            # 自动解锁全部按钮
            for i, b in enumerate(self.btns):
                if self.state[i] != "ok":
                    b.config(state="normal")
                else:
                    b.config(state="normal")
        else:
            self.state[idx] = "fail"
            self.btns[idx].config(state="normal", text=f"重试：{label}", bg="#c0392b", fg="white")
            self.st_labels[idx].config(text=f"{short}: 失败", fg="#e74c3c")
            self.phase_var.set(f"{label} — 学习失败")
            self.hint_var.set(detail or "没收到信号：检查 315 DATA→GPIO13、天线、遥控电池，然后点重试")
            self.log(f"按键 {idx} 失败: {detail}")
            for b in self.btns:
                b.config(state="normal")

    def _read_loop(self):
        while self.running and self.ser and self.ser.is_open:
            try:
                raw = self.ser.read(512)
            except Exception:
                time.sleep(0.15)
                continue
            if not raw:
                continue
            self.buf += raw
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if not text:
                    continue
                self._handle(text)

    def _handle(self, text: str):
        # 只显示 RF 相关，避免 NFC/BLE 刷屏
        if text.startswith("[RF]") or "rflearn" in text or "rfplay" in text or "rfkeys" in text:
            self.log(text)

        idx = self.learning_idx
        if idx < 0:
            # rfkeys 刷新标签
            if text.startswith("[RF] key "):
                self._parse_rfkey(text)
            return

        if "学习成功" in text:
            self.root.after(0, lambda: self._finish_learn(idx, True))
            return
        if "保存 NVS 失败" in text or "提不出单帧" in text:
            self.root.after(0, lambda: self._finish_learn(idx, False, text))
            return
        if "抓包失败" in text:
            self.root.after(0, lambda: self._finish_learn(idx, False, "未收到遥控信号"))
            return

        if text.startswith("[RF] key "):
            self._parse_rfkey(text)

    def _parse_rfkey(self, text: str):
        try:
            # [RF] key 0 (open/up): OK, 49 pulses
            parts = text.split()
            ki = int(parts[3])
            ok = ": OK" in text
            if not (0 <= ki < 3):
                return
            short = KEYS[ki][2]
            if ok:
                self.state[ki] = "ok"
                n = 0
                if "pulses" in text:
                    n = int(text.split("pulses")[0].strip().rstrip(",").split()[-1])

                    def _ok(k=ki, n=n, s=short):
                        self.st_labels[k].config(text=f"{s}: 已学 {n}p", fg="#3dd68c")
                        self.btns[k].config(
                            text=f"✓ {KEYS[k][1]}", bg="#1e3d32", fg="#3dd68c", state="normal"
                        )

                    self.root.after(0, _ok)
                    return
            self.state[ki] = "idle"

            def _no(k=ki, s=short):
                self.st_labels[k].config(text=f"{s}: 未学", fg="#8b9aab")

            self.root.after(0, _no)
        except Exception:
            pass

    def _recent(self, n=15) -> str:
        try:
            return self.log_text.get("end-%dl" % n, "end")
        except Exception:
            return ""


def main():
    root = tk.Tk()
    app = RfLearnApp(root)
    root.protocol("WM_DELETE_WINDOW", lambda: (app.disconnect(), root.destroy()))
    root.mainloop()


if __name__ == "__main__":
    main()
