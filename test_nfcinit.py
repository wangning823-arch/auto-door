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


print("===== wait boot (8s) =====")
drain(8)

print("\n>> nfcinit")
port.write(b"nfcinit\r\n")
port.flush()
drain(15, "nfcinit")

# 观察 90 秒是否撑住
drain(90, "hold 90s")

port.write(b"status\r\n")
port.flush()
drain(3, "status")

port.close()
print("\n--- done ---")
