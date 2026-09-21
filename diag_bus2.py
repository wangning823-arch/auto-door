import serial
import sys
import time

# 不触发 DTR/RTS 复位
port = serial.Serial()
port.port = "COM3"
port.baudrate = 115200
port.timeout = 0.2
port.dtr = False
port.rts = False
port.open()
port.reset_input_buffer()


def drain(seconds, label=""):
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


def send(cmd):
    print(f"\n>> {cmd}")
    port.write((cmd + "\r\n").encode())
    port.flush()


drain(4, "current")
send("buspull")
drain(3, "buspull")
send("sclhold")
drain(3, "sclhold")
send("sclrelease")
drain(3, "sclrelease")
send("buspull")
drain(3, "buspull2")
port.close()
print("\n--- done ---")
