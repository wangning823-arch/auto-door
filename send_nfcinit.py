import serial
import sys
import time

port = serial.Serial("COM3", 115200, timeout=0.2)
port.reset_input_buffer()
port.write(b"nfcinit\r\n")
port.flush()
print(">> nfcinit sent")
t0 = time.time()
while time.time() - t0 < 20:
    chunk = port.read(512)
    if chunk:
        sys.stdout.write(chunk.decode("utf-8", errors="replace"))
        sys.stdout.flush()
    else:
        time.sleep(0.05)
port.close()
print("\n--- done ---")
