# -*- coding: utf-8 -*-
"""开连续抓包，再发 rfplay 0，看本机能否收到自己发的 key0"""
import time

import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


port = pick_port()
print("port", port, flush=True)
ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
time.sleep(0.2)
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
print("ready", b"[BOOT] ready" in buf, flush=True)
time.sleep(0.3)
while ser.in_waiting:
    ser.read(8192)

# 停掉可能残留的连续抓包
ser.write(b"rfstop\n")
ser.flush()
time.sleep(0.4)
ser.read(4096)

print(">> rfcap", flush=True)
ser.write(b"rfcap\n")
ser.flush()
time.sleep(1.0)
pre = ser.read(8192)
print(pre.decode("utf-8", "replace"), flush=True)

print(">> rfplay 0", flush=True)
ser.write(b"rfplay 0\n")
ser.flush()

buf = b""
ok_marker = "抓包成功".encode("utf-8")
end = time.time() + 15
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
        if b"RFCAP_END" in buf or (ok_marker in buf and b"pulses:" in buf):
            time.sleep(0.4)
            buf += ser.read(8192)
            if b"RFCAP_END" in buf or "码相同".encode() in buf or "码不同".encode() in buf:
                break

ser.write(b"rfstop\n")
ser.flush()
time.sleep(0.5)
buf += ser.read(8192)
ser.close()

text = buf.decode("utf-8", "replace")
print("===== CAPTURE =====", flush=True)
print(text, flush=True)
print(
    "summary: success",
    text.count("抓包成功"),
    "same",
    "码相同" in text,
    "diff",
    "码不同" in text,
    "noise",
    "仅噪声" in text,
    flush=True,
)
