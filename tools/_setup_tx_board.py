# -*- coding: utf-8 -*-
"""新发射板：写入 key0 + rfauto on，并确认周期发射"""
import json
import time
from pathlib import Path

import serial
from serial.tools import list_ports

KEYS = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


def wait_ready(ser, timeout=40):
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


data = json.loads(KEYS.read_text(encoding="utf-8"))
cl = [x for x in data["keys"]["0"]["pulses"] if x >= 20]
csv = ",".join(str(x) for x in cl)
print(f"key0 cleaned={len(cl)} pulses")

port = pick_port()
print("port", port)
ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
time.sleep(0.2)
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

ok, boot = wait_ready(ser, 40)
print("ready", ok)
tail = boot.decode("utf-8", "replace")
print(boot[-600:].decode("utf-8", "replace") if isinstance(boot, bytes) else "")

ser.reset_input_buffer()
# 写 key0
cmd = f"rfset 0 {csv}\n"
print(">> rfset 0 ...", len(cl), "pulses")
ser.write(cmd.encode())
ser.flush()
time.sleep(0.8)
print(ser.read(4096).decode("utf-8", "replace"))

ser.write(b"rfkeys\n")
ser.flush()
time.sleep(0.6)
print("--- rfkeys ---")
print(ser.read(4096).decode("utf-8", "replace"))

ser.write(b"rfauto on\n")
ser.flush()
time.sleep(1.5)
print("--- rfauto on ---")
print(ser.read(4096).decode("utf-8", "replace"))

# 再听 ~6s 看 interval
print("--- watch 6s for AUTO TX ---")
buf = b""
end = time.time() + 6
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
print(buf.decode("utf-8", "replace"))
ser.close()
print("DONE — 板子可拔下接 TX 模块，上电会自动每 5s 发")
