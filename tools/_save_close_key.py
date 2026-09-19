# -*- coding: utf-8 -*-
"""把已验证有效的关门码写入 key1，备份，再 rfplay 1 确认"""
import json
import time
from pathlib import Path

import serial
from serial.tools import list_ports

BACKUP = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


def main():
    port = pick_port()
    print("port", port, flush=True)
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
        ser.read(8192)

    # 读设备上的 key0（刚验证能关的码）
    ser.write(b"rfexport\n")
    ser.flush()
    time.sleep(1.5)
    out = ser.read(16384).decode("utf-8", "replace")
    print("--- rfexport ---", flush=True)
    print(out, flush=True)

    key0_csv = None
    key0_n = 0
    for line in out.splitlines():
        if line.startswith("RFDATA 0 "):
            parts = line.split()
            # RFDATA 0 open N csv...
            key0_n = int(parts[3])
            key0_csv = " ".join(parts[4:]) if len(parts) > 4 else ""
            # 正确解析：RFDATA 0 open 52 pulses...
            # format: RFDATA 0 open N p1,p2,...
            sp = line.split(" ", 4)
            if len(sp) >= 5:
                key0_n = int(sp[3])
                key0_csv = sp[4].strip()
            break

    if not key0_csv:
        print("ERROR: no key0 from device", flush=True)
        ser.close()
        raise SystemExit(1)

    print(f"key0 n={key0_n} csv_head={key0_csv[:80]}", flush=True)

    # 写入 key1 = 关
    cmd = f"rfset 1 {key0_csv}\n"
    print(f">> rfset 1 ({key0_n} pulses)", flush=True)
    ser.write(cmd.encode())
    ser.flush()
    time.sleep(1.2)
    print(ser.read(8192).decode("utf-8", "replace"), flush=True)

    ser.write(b"rfkeys\n")
    ser.flush()
    time.sleep(0.8)
    print("--- rfkeys ---", flush=True)
    print(ser.read(4096).decode("utf-8", "replace"), flush=True)

    # 重新导出全部并备份
    ser.write(b"rfexport\n")
    ser.flush()
    time.sleep(1.5)
    out2 = ser.read(16384).decode("utf-8", "replace")
    print("--- rfexport2 ---", flush=True)
    print(out2, flush=True)

    keys = {}
    for line in out2.splitlines():
        if not line.startswith("RFDATA "):
            continue
        # RFDATA idx name n csv
        sp = line.split(" ", 4)
        if len(sp) < 5:
            continue
        idx = int(sp[1])
        name = sp[2]
        n = int(sp[3])
        csv = sp[4].strip()
        pulses = [int(x) for x in csv.split(",") if x.strip().isdigit()]
        keys[str(idx)] = {
            "index": idx,
            "name": name,
            "count": len(pulses),
            "pulses": pulses,
        }

    data = {
        "exported_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "source": "verified close key from door test",
        "note": "key0/key1 both hold the verified CLOSE code (door closed OK with rfplay 0); key1 is the canonical close slot",
        "keys": keys,
    }
    BACKUP.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"backup saved: {BACKUP}", flush=True)
    print("keys:", {k: v["count"] for k, v in keys.items()}, flush=True)

    # 再次确认：发 key1（关）
    print(">> rfplay 1", flush=True)
    ser.write(b"rfplay 1\n")
    ser.flush()
    time.sleep(2.5)
    print(ser.read(4096).decode("utf-8", "replace"), flush=True)

    # 也再发一次 key0 对照（同一码）
    print(">> rfplay 0", flush=True)
    ser.write(b"rfplay 0\n")
    ser.flush()
    time.sleep(2.5)
    print(ser.read(4096).decode("utf-8", "replace"), flush=True)

    ser.close()
    print("DONE — 请看门是否再次关闭", flush=True)


if __name__ == "__main__":
    main()
