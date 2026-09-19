# -*- coding: utf-8 -*-
"""BLE 离场关门监控：持续打印 BLE/FSM/RF/门状态，检测 AUTO CLOSE"""
import sys
import time

import serial
from serial.tools import list_ports

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
ports = [p.device for p in list_ports.comports()]
port = "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")
print(f"port={port} watch={DURATION}s", flush=True)

ser = None
for i in range(8):
    try:
        ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
        break
    except Exception as e:
        print("retry", e, flush=True)
        time.sleep(0.5)
if not ser:
    raise SystemExit("open failed")
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

# wait ready
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
print("ready", b"[BOOT] ready" in buf, flush=True)
time.sleep(0.2)
while ser.in_waiting:
    ser.read(4096)

keys = (
    "[BLE]",
    "[FSM]",
    "[RF]",
    "[LOG]",
    "AUTO",
    "rfauto",
    "door=",
    "track",
    "LOST",
    "hits=",
    "MATCH",
    "开",
    "关",
    "SoftAP",
    "paused",
)

t0 = time.time()
partial = b""
closed = False
opened = False
last_door = None
while time.time() - t0 < DURATION:
    c = ser.read(2048)
    if not c:
        continue
    partial += c
    while b"\n" in partial:
        line, partial = partial.split(b"\n", 1)
        text = line.decode("utf-8", "replace").rstrip("\r")
        if not text:
            continue
        if any(k in text for k in keys):
            ts = time.strftime("%H:%M:%S")
            print(f"[{ts}] {text}", flush=True)
        if "AUTO CLOSE" in text or "强→弱→无" in text:
            closed = True
            print("<<< DETECTED AUTO CLOSE >>>", flush=True)
        if "tryAutoOpen" in text or "无→有→强" in text or "AUTO OPEN" in text:
            opened = True
            print("<<< DETECTED AUTO OPEN PATH >>>", flush=True)
        if "door=" in text:
            # crude parse
            if "door=1" in text or "door=0" in text:
                last_door = "closed?"
            elif "door=2" in text:
                last_door = "open?"

ser.close()
print(f"\nDONE closed_event={closed} open_event={opened} door_hint={last_door}", flush=True)
