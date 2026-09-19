# -*- coding: utf-8 -*-
"""连续抓包监控：rfcap + 读输出 N 秒 + rfstop"""
import sys
import time

import serial
from serial.tools import list_ports

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


def wait_ready(ser, timeout=35):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        c = ser.read(4096)
        if c:
            buf += c
            if b"[BOOT] ready" in buf:
                return True, buf
        else:
            time.sleep(0.05)
    return b"[BOOT] ready" in buf, buf


port = pick_port()
print(f"port={port} monitor {DURATION}s", flush=True)
ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
time.sleep(0.2)
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

ok, boot = wait_ready(ser, 35)
print("ready", ok, flush=True)
# MAC to confirm which board
bt = boot.decode("utf-8", "replace")
for line in bt.splitlines():
    if "MAC" in line or "mac=" in line or "SoftAP" in line or "GarageDoor-" in line:
        print(" ", line)

ser.reset_input_buffer()
# 关掉本机 rfauto，避免和外部 TX 混淆（若这是接收板）
ser.write(b"rfauto off\n")
ser.flush()
time.sleep(0.5)
print("rfauto off:", ser.read(2048).decode("utf-8", "replace").strip()[-200:])

ser.write(b"rfcap\n")
ser.flush()
print(">> rfcap continuous", flush=True)

buf = b""
end = time.time() + DURATION
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
        # 实时打关键行
        try:
            t = c.decode("utf-8", "replace")
            for ln in t.splitlines():
                if any(
                    k in ln
                    for k in (
                        "AUTO TX",
                        "抓包成功",
                        "抓包失败",
                        "RFCAP",
                        "警告",
                        "听中",
                        "码相同",
                        "码不同",
                        "pulses:",
                    )
                ):
                    print(ln[:200], flush=True)
        except Exception:
            pass

ser.write(b"rfstop\n")
ser.flush()
time.sleep(1.0)
buf += ser.read(8192)
ser.close()

text = buf.decode("utf-8", "replace")
frames = text.count("抓包成功")
print("\n===== SUMMARY =====")
print("frames_success", frames)
print("has_AUTO_TX", "AUTO TX" in text)
print("has_fail_noise", "仅噪声" in text)
print("bytes", len(buf))
# 打印 pulses 行数
n_pulses = sum(1 for ln in text.splitlines() if ln.startswith("[RF] pulses:"))
print("pulses_lines", n_pulses)
