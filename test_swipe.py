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


print("===== wait boot (5s) =====")
drain(5)

print("\n>> rfkeys")
port.write(b"rfkeys\r\n")
port.flush()
drain(2, "rfkeys")

print("\n>> status")
port.write(b"status\r\n")
port.flush()
drain(2, "status")

print("\n>> 监听 60s，nfcinit 后请多次贴手机/刷卡")
port.write(b"nfcinit\r\n")
port.flush()
drain(10, "nfcinit")

drain(60, "swipe card")

print("\n>> status 终态")
port.write(b"status\r\n")
port.flush()
drain(3, "status")

port.close()
print("\n--- done ---")
