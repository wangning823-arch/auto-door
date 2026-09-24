# -*- coding: utf-8 -*-
"""边发边收回环：发送 rfloop 并打印结果"""
import sys
import time

import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if "COM3" in ports:
        return "COM3"
    return ports[0] if ports else "COM3"


def main():
    key = sys.argv[1] if len(sys.argv) > 1 else "0"
    reps = sys.argv[2] if len(sys.argv) > 2 else "2"
    port = pick_port()
    print(f"port={port}")
    ser = serial.Serial(port, 115200, timeout=0.3)
    time.sleep(2.5)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    ser.reset_input_buffer()

    end = time.time() + 1.5
    while time.time() < end:
        if ser.read(4096):
            end = time.time() + 0.4

    cmd = f"rfloop {key} {reps}\n".encode()
    # 先探活
    ser.write(b"help\n")
    ser.flush()
    time.sleep(0.5)
    pre = ser.read(8192)
    print("--- pre help ---")
    print(pre.decode("utf-8", errors="replace")[-800:])
    ser.reset_input_buffer()

    print(f">> {cmd.decode().strip()}")
    ser.write(cmd)
    ser.flush()

    buf = b""
    end = time.time() + 12
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if b"LOOPBACK" in buf and (
                b"PASS" in buf or b"FAIL" in buf or b"WEAK" in buf
            ):
                time.sleep(0.4)
                buf += ser.read(4096)
                break
        else:
            time.sleep(0.05)

    text = buf.decode("utf-8", errors="replace")
    print("--- rfloop ---")
    print(text if text.strip() else "(no output)")
    ser.close()


if __name__ == "__main__":
    main()
