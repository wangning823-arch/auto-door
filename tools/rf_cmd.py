# -*- coding: utf-8 -*-
import sys
import time

import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


def main():
    cmds = sys.argv[1:] or ["rfkeys", "rfexport"]
    port = pick_port()
    print(f"port={port} cmds={cmds}", flush=True)
    ser = None
    for i in range(8):
        try:
            ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
            break
        except Exception as e:
            print("retry", e, flush=True)
            time.sleep(0.5)
    if not ser:
        raise SystemExit("open failed")
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass

    # drain boot
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
    print("ready", b"[BOOT] ready" in buf, flush=True)
    time.sleep(0.3)
    while ser.in_waiting:
        ser.read(4096)

    for cmd in cmds:
        print(f">> {cmd}", flush=True)
        ser.write((cmd + "\n").encode())
        ser.flush()
        end = time.time() + 2.5
        out = b""
        while time.time() < end:
            c = ser.read(4096)
            if c:
                out += c
        print(out.decode("utf-8", "replace"), flush=True)
        print("---", flush=True)
    ser.close()


if __name__ == "__main__":
    main()
