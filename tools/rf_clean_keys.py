# -*- coding: utf-8 -*-
"""清洗 NVS 学习码毛刺（<20us），写回设备并更新备份"""
import json
import sys
import time
from pathlib import Path

import serial
from serial.tools import list_ports

BACKUP = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)
MIN_US = 20


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


def clean(p):
    return [x for x in p if x >= MIN_US]


def wait_ready(ser, timeout=12):
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if b"[BOOT] ready" in buf:
                return True
        else:
            time.sleep(0.05)
    return b"[BOOT] ready" in buf


def main():
    data = json.loads(BACKUP.read_text(encoding="utf-8"))
    keys = data["keys"]
    cmds = []
    for k, v in keys.items():
        raw = v["pulses"]
        cl = clean(raw)
        removed = len(raw) - len(cl)
        csv = ",".join(str(x) for x in cl)
        print(f"key{k} ({v['name']}): {len(raw)} -> {len(cl)} (removed {removed} glitches)")
        print(f"  head={cl[:16]}")
        cmds.append(f"rfset {k} {csv}")
        v["pulses"] = cl
        v["count"] = len(cl)

    data["cleaned_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
    data["note"] = f"stripped pulses < {MIN_US}us"
    BACKUP.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"backup updated: {BACKUP}")

    port = pick_port()
    print(f"port={port}")
    ser = None
    for _ in range(5):
        try:
            ser = serial.Serial(port, 115200, timeout=0.25, exclusive=True)
            break
        except Exception as e:
            print("retry", e)
            time.sleep(0.4)
    if not ser:
        sys.exit(1)
    time.sleep(0.2)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    ok = wait_ready(ser, 8)
    print("ready:", ok)
    ser.reset_input_buffer()
    for c in cmds:
        print(">>", c[:60], "...")
        ser.write((c + "\n").encode())
        ser.flush()
        time.sleep(0.35)
        resp = ser.read(4096).decode("utf-8", errors="replace")
        print(resp.strip()[-300:])

    ser.write(b"rfkeys\n")
    ser.flush()
    time.sleep(0.5)
    print("--- rfkeys ---")
    print(ser.read(8192).decode("utf-8", errors="replace"))
    ser.close()


if __name__ == "__main__":
    main()
