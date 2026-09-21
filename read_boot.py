import serial
import sys
import time

port = serial.Serial()
port.port = "COM3"
port.baudrate = 115200
port.timeout = 0.2
port.dtr = False
port.rts = False
port.open()
port.reset_input_buffer()

t0 = time.time()
while time.time() - t0 < 8:
    chunk = port.read(512)
    if chunk:
        sys.stdout.write(chunk.decode("utf-8", errors="replace"))
        sys.stdout.flush()
    else:
        time.sleep(0.05)

port.close()
print("\n--- done ---")
