# -*- coding: utf-8 -*-
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

end = time.time() + 45
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

ser.reset_input_buffer()
ser.write(b"rfauto off\n")
ser.flush()
time.sleep(1.5)
out = ser.read(8192).decode("utf-8", "replace")
print("--- rfauto off ---", flush=True)
print(out, flush=True)

buf = b""
end = time.time() + 7
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
print("--- watch 7s ---", flush=True)
print(buf.decode("utf-8", "replace"), flush=True)
print("still_AUTO", "AUTO TX" in buf, flush=True)
print("saved_OFF", "rfauto OFF" in out or "rfauto OFF" in buf, flush=True)
ser.close()
