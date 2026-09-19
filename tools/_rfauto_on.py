# -*- coding: utf-8 -*-
import time

import serial
from serial.tools import list_ports

ports = [p.device for p in list_ports.comports()]
port = "COM3" if "COM3" in ports else ports[0]
print("port", port)
ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
time.sleep(0.2)
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

# drain boot spam up to ready or 40s（NFC I2C 错误约 20s 后才 ready）
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
text = buf.decode("utf-8", "replace")
print("boot_tail:\n", text[-500:])
print("has_ready", "[BOOT] ready" in text)

# probe
ser.reset_input_buffer()
ser.write(b"help\n")
ser.flush()
time.sleep(0.6)
help_txt = ser.read(8192).decode("utf-8", "replace")
print("help_has_rfauto", "rfauto" in help_txt)
print(help_txt[-400:])

ser.reset_input_buffer()
ser.write(b"rfauto on\n")
ser.flush()
time.sleep(1.5)
out = ser.read(8192).decode("utf-8", "replace")
print("--- rfauto on resp ---")
print(out)
ser.close()
print("DONE closed")
