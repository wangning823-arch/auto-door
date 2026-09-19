# -*- coding: utf-8 -*-
"""从 09:16 原遥控提取单帧（~50 脉冲）→ rfset 0 → rfplay 0"""
import json
import time
from pathlib import Path

import serial
from serial.tools import list_ports

CAP = Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_091640.json")
BACKUP = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)
GAP = 4000
MAX_N = 79  # RF_KEY_MAX_PULSES-1，避免被截断


def first_frame(pl):
    # 跳过开头长间隔
    i = 0
    while i < len(pl) and pl[i] >= GAP:
        i += 1
    frame = []
    while i < len(pl):
        v = pl[i]
        if v >= GAP and len(frame) >= 16:
            break
        if v >= GAP:
            i += 1
            continue
        frame.append(v)
        i += 1
        if len(frame) >= MAX_N:
            break
    return frame


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


caps = json.loads(CAP.read_text(encoding="utf-8"))
# 试每条抓包，取长度最接近 49~60 的干净帧
best = None
for ci, c in enumerate(caps):
    fr = first_frame(c["pulses"])
    print(f"cap#{ci+1}: frame_n={len(fr)} head={fr[:16]}")
    if 45 <= len(fr) <= 65:
        if best is None:
            best = fr
print("chosen n=", len(best) if best else None)
if not best:
    # 退而求其次：取 body 前 49 个
    best = first_frame(caps[1]["pulses"])[:49]
    print("fallback n=", len(best))

csv = ",".join(str(x) for x in best)
print(f"frame={best}")
print(f"csv_len={len(csv)}")

data = json.loads(BACKUP.read_text(encoding="utf-8"))
data["keys"]["0"]["pulses"] = best
data["keys"]["0"]["count"] = len(best)
data["note"] = "key0 single frame from 09:16 original remote"
data["cleaned_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
BACKUP.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")

port = pick_port()
print("port", port)
ser = None
for _ in range(8):
    try:
        ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
        break
    except Exception as e:
        print("retry", e)
        time.sleep(0.5)
if not ser:
    raise SystemExit("open failed")
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

end = time.time() + 40
buf = b""
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
        if b"[BOOT] ready" in buf:
            break
    else:
        time.sleep(0.05)
print("ready", b"[BOOT] ready" in buf)

ser.reset_input_buffer()
cmd = f"rfset 0 {csv}\n"
print(f">> rfset 0 n={len(best)} bytes={len(cmd)}")
ser.write(cmd.encode())
ser.flush()
time.sleep(1.2)
print(ser.read(8192).decode("utf-8", "replace"))

ser.write(b"rfkeys\n")
ser.flush()
time.sleep(0.8)
print(ser.read(4096).decode("utf-8", "replace"))

ser.write(b"rfplay 0\n")
ser.flush()
time.sleep(2.0)
print("--- play ---")
print(ser.read(4096).decode("utf-8", "replace"))
ser.close()
print("DONE")
