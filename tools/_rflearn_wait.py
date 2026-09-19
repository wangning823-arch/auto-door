# -*- coding: utf-8 -*-
"""交互式/等待式 rflearn：rflearn 0 [超时秒]"""
import sys
import time

import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    return "COM3" if "COM3" in ports else (ports[0] if ports else "COM3")


def main():
    idx = sys.argv[1] if len(sys.argv) > 1 else "0"
    wait_s = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
    cmd = f"rflearn {idx}\n"
    port = pick_port()
    print(f"port={port} >> {cmd.strip()} wait={wait_s}s", flush=True)
    ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
    time.sleep(0.2)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass

    end = time.time() + 12
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

    # 若还在连续抓包等状态，先 rfstop
    ser.write(b"rfstop\n")
    ser.flush()
    time.sleep(0.3)
    ser.read(4096)

    ser.write(cmd.encode())
    ser.flush()
    print("已发送，请按遥控对应键...", flush=True)

    buf = b""
    end = time.time() + wait_s
    while time.time() < end:
        c = ser.read(4096)
        if c:
            buf += c
            text = buf.decode("utf-8", "replace")
            for ln in text.splitlines():
                if any(
                    k in ln
                    for k in (
                        "学习",
                        "抓包成功",
                        "抓包失败",
                        "单帧",
                        "保存",
                        "失败",
                        "RFDATA",
                        "按键",
                        "pulses:",
                        "仅噪声",
                    )
                ):
                    print(ln[:240], flush=True)
            if "学习成功" in text or "按键" in text and "成功" in text:
                time.sleep(0.5)
                buf += ser.read(8192)
                break
            if "学习失败" in text or "保存 NVS 失败" in text:
                break
        else:
            time.sleep(0.05)

    print("\n===== FULL =====", flush=True)
    print(buf.decode("utf-8", "replace"), flush=True)
    ser.close()


if __name__ == "__main__":
    main()
