# -*- coding: utf-8 -*-
"""复位后从开机日志抓 RFDATA，保存备份并校验"""
import json
import re
import time
from pathlib import Path

import serial
from serial.tools import list_ports

OUT = Path(__file__).resolve().parent / "rf_keys_backup.json"
RE_RFDATA = re.compile(r"^RFDATA\s+(\d+)\s+(\S+)\s+(\d+)\s+(.+)$")
NAMES = {0: "open", 1: "close", 2: "stop", 3: "lock"}


def parse_csv(s: str):
    return [int(x) for x in s.split(",") if x.strip().isdigit()]


def main():
    ports = [p.device for p in list_ports.comports()]
    port = "COM3" if "COM3" in ports else ports[0]
    print(f"port {port}")
    ser = serial.Serial(port, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass
    time.sleep(0.4)
    ser.reset_input_buffer()

    # 打开串口可能不复位；主动等一段时间收开机日志
    # 若未见 boot，发送 DTR 脉冲不可靠，改为等用户可再复位
    print("等待开机日志 12s（若未复位则发 help 探活）...")
    keys = {}
    boot_seen = False
    buf = b""
    end = time.time() + 12.0
    sent_help = False
    while time.time() < end:
        chunk = ser.read(1024)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if not text:
                    continue
                if "Garage Door Controller" in text or "[BOOT] RF keys" in text:
                    boot_seen = True
                if text.startswith("[RF] key") or text.startswith("[BOOT]"):
                    print(text)
                m = RE_RFDATA.match(text)
                if m:
                    print(text[:80] + ("..." if len(text) > 80 else ""))
                    idx = int(m.group(1))
                    keys[str(idx)] = {
                        "index": idx,
                        "name": m.group(2),
                        "count": int(m.group(3)),
                        "pulses": parse_csv(m.group(4)),
                    }
        else:
            if not boot_seen and not sent_help and time.time() + 0 > 0:
                # 3s 后仍未见 boot，说明板子在跑旧逻辑没自动导出，尝试命令
                if time.time() > end - 9 and not sent_help:
                    ser.write(b"rfexport\n")
                    sent_help = True
                    print("(未见开机导出，已尝试发送 rfexport)")
            time.sleep(0.05)

    print("\n=== verify ===")
    for i in range(4):
        k = keys.get(str(i))
        if not k:
            print(f"key {i} ({NAMES[i]}): 无")
            continue
        n = len(k["pulses"])
        ok = n == k["count"] and n >= 10
        print(f"key {i} ({k['name']}): meta={k['count']} parsed={n} {'OK' if ok else 'FAIL'}")
        if not ok:
            raise SystemExit(f"key {i} data corrupt")

    if not keys:
        print("FAIL: 没有 RFDATA。请按一下板子复位键或重新上电后再跑本脚本。")
        ser.close()
        raise SystemExit(1)

    backup = {
        "exported_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "source": "esp32 boot RFDATA (nvs)",
        "keys": keys,
    }
    OUT.write_text(json.dumps(backup, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nsaved {OUT}")
    print(f"RESULT: {len(keys)} key(s) OK — 已写入仓库待 commit")
    ser.close()


if __name__ == "__main__":
    main()
