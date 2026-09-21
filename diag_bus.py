import serial
import sys
import time


def drain(port, seconds, label=""):
    if label:
        print(f"\n===== {label} =====")
    t0 = time.time()
    while time.time() - t0 < seconds:
        chunk = port.read(512)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", errors="replace"))
            sys.stdout.flush()
        else:
            time.sleep(0.05)


def send(port, cmd):
    port.write((cmd + "\r\n").encode())
    port.flush()
    print(f"\n>> {cmd}")


port = serial.Serial("COM3", 115200, timeout=0.2)
# 不 reset，只读当前状态
print("===== current state (5s) =====")
drain(port, 5)

send(port, "status")
drain(port, 2, "status")

send(port, "buspull")
drain(port, 2, "buspull")

send(port, "sclhold")
drain(port, 2, "sclhold")

send(port, "sclrelease")
drain(port, 2, "sclrelease")

send(port, "buspull")
drain(port, 3, "final buspull")

port.close()
print("\n--- done ---")
