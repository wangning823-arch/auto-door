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


print("===== wait boot (6s) =====")
drain(6)

print("\n>> nfcinit")
port.write(b"nfcinit\r\n")
port.flush()
drain(8, "nfcinit")

print("\n>> nfcscan (贴卡 30s!)")
port.write(b"nfcscan\r\n")
port.flush()
drain(35, "nfcscan")

port.close()
print("\n--- done ---")
