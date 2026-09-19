# -*- coding: utf-8 -*-
"""探测当前 COM3 是不是新板：看 boot / keys / rfauto"""
import time

import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


port = pick_port()
print("port", port)
ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
time.sleep(0.2)
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

end = time.time() + 25
buf = b""
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
        if b"[BOOT] ready" in buf:
            break
    else:
        time.sleep(0.05)

text = buf.decode("utf-8", "replace")
print("--- boot ---")
print(text[-2500:])
print("has_ready", "[BOOT] ready" in text)

ser.reset_input_buffer()
ser.write(b"rfkeys\n")
ser.flush()
time.sleep(0.8)
keys = ser.read(8192).decode("utf-8", "replace")
print("--- rfkeys ---")
print(keys)

ser.write(b"status\n")
ser.flush()
time.sleep(0.6)
st = ser.read(4096).decode("utf-8", "replace")
print("--- status ---")
print(st)
ser.close()
