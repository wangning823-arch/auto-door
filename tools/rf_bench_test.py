# -*- coding: utf-8 -*-
"""rfbench：每 10s 发一次，RX 持续听，对比学习码稳定性"""
import sys
import time

import serial
from serial.tools import list_ports


def pick_port():
    ports = [p.device for p in list_ports.comports()]
    if "COM3" in ports:
        return "COM3"
    return ports[0] if ports else "COM3"


def drain(ser, seconds=0.3):
    end = time.time() + seconds
    buf = b""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
        else:
            time.sleep(0.02)
    return buf


def main():
    key = sys.argv[1] if len(sys.argv) > 1 else "0"
    rounds = sys.argv[2] if len(sys.argv) > 2 else "6"
    n = int(rounds)
    budget = 3.0 + n * 10.5 + 10

    port = pick_port()
    print(f"port={port} key={key} rounds={n} budget={budget:.0f}s", flush=True)

    # 多试几次开串口（COM 可能忙）
    ser = None
    last_err = None
    for i in range(5):
        try:
            ser = serial.Serial(port, 115200, timeout=0.25, exclusive=True)
            break
        except Exception as e:
            last_err = e
            time.sleep(0.5)
    if ser is None:
        print(f"open failed: {last_err}")
        sys.exit(1)

    time.sleep(0.2)
    try:
        ser.dtr = False
        ser.rts = False
    except Exception:
        pass

    boot = drain(ser, 1.0)
    print("--- boot/drain ---", flush=True)
    print(boot.decode("utf-8", errors="replace")[-500:], flush=True)

    # 探活
    ser.reset_input_buffer()
    for attempt in range(3):
        ser.write(b"help\n")
        ser.flush()
        pre = drain(ser, 1.2)
        text = pre.decode("utf-8", errors="replace")
        print(f"--- help try{attempt} ({len(pre)}B) ---", flush=True)
        print(text[-800:], flush=True)
        if "rfbench" in text or "rfloop" in text:
            break
    else:
        # 即使 help 不完整也继续试发
        print("!! help 未确认 rfbench，仍尝试发送", flush=True)

    cmd = f"rfbench {key} {n}\n".encode()
    print(f">> {cmd.decode().strip()}", flush=True)
    ser.reset_input_buffer()
    ser.write(cmd)
    ser.flush()

    buf = b""
    end = time.time() + budget
    last_line = ""
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            text = buf.decode("utf-8", errors="replace")
            lines = [p for p in text.splitlines() if p.strip()]
            if lines and lines[-1] != last_line:
                # 只刷关键行
                cur = lines[-1]
                if any(
                    k in cur
                    for k in (
                        "轮次",
                        "BENCH",
                        "PASS",
                        "FAIL",
                        "WEAK",
                        "轮 ",
                        "预热",
                        "等待",
                        "发射",
                        "unknown",
                        "用法",
                    )
                ):
                    print(cur[:160], flush=True)
                    last_line = cur
            if "BENCH" in text and "平均匹配" in text:
                time.sleep(0.6)
                buf += drain(ser, 0.5)
                break
        else:
            time.sleep(0.05)

    print("\n--- rfbench full ---", flush=True)
    print(buf.decode("utf-8", errors="replace"))
    ser.close()


if __name__ == "__main__":
    main()
