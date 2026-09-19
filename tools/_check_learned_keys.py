# -*- coding: utf-8 -*-
"""读取设备 rfkeys/RFDATA，打印脉宽特征并对照备份码"""
import json
import time
from pathlib import Path

import serial
from serial.tools import list_ports

ports = [p.device for p in list_ports.comports()]
port = "COM3" if "COM3" in ports else (ports[0] if ports else None)
print("port", port)
if not port:
    raise SystemExit(1)

ser = serial.Serial(port, 115200, timeout=0.3, exclusive=True)
time.sleep(0.2)
try:
    ser.dtr = False
    ser.rts = False
except Exception:
    pass

# wait ready-ish
buf = b""
end = time.time() + 8
while time.time() < end:
    c = ser.read(4096)
    if c:
        buf += c
        if b"[BOOT] ready" in buf:
            break
    else:
        time.sleep(0.05)

ser.reset_input_buffer()
ser.write(b"rfkeys\n")
ser.flush()
time.sleep(1.0)
raw = ser.read(16384).decode("utf-8", "replace")
print("----- rfkeys -----")
print(raw)

# also export full RFDATA for each key
out_lines = [raw]
for i in range(4):
    ser.reset_input_buffer()
    ser.write(f"rfexport {i}\n".encode() if False else b"rfkeys\n")
    # exportKeyCsv is triggered how? check: RFDATA printed by rfkeys maybe
ser.reset_input_buffer()
ser.write(b"rfkeys\n")
ser.flush()
time.sleep(0.5)
more = ser.read(16384).decode("utf-8", "replace")
out_lines.append(more)
ser.close()

text = "\n".join(out_lines)

# Parse RFDATA lines if present
import re

rfdatas = re.findall(r"RFDATA\s+(\d+)\s+(\w+)\s+(\d+)\s+([0-9,]+)", text)
key_status = re.findall(r"\[RF\] key\s+(\d+)[^\n]*", text)
print("----- parsed status -----")
for line in key_status:
    print(line)

backup_path = Path(r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json")
backup = json.loads(backup_path.read_text(encoding="utf-8")) if backup_path.exists() else None


def stats(pulses):
    if not pulses:
        return None
    n = len(pulses)
    total = sum(pulses)
    shorts = [v for v in pulses if v < 400]
    longs = [v for v in pulses if 400 <= v <= 2500]
    gaps = [v for v in pulses if v >= 4000]
    # 重复周期探测
    bestL, bestScore = 0, 0
    if n >= 32:
        maxL = min(n // 2, 400)
        for L in range(16, maxL + 1):
            cmp = 0
            score = 0
            check = min(n - L, L * 2)
            for i in range(check):
                a, b = pulses[i], pulses[i + L]
                mx = max(a, b)
                d = abs(a - b)
                if d <= max(mx // 4, 80):
                    score += 1
                cmp += 1
            if cmp >= 16 and score * 10 >= cmp * 8:
                if score > bestScore or (score == bestScore and L > bestL):
                    bestScore, bestL = score, L
    # 短/长二值化特征
    thr = 350
    bits = "".join("L" if v >= 400 else ("S" if v < 400 else "G") for v in pulses if v < 4000)
    return {
        "n": n,
        "total_ms": total / 1000,
        "head": pulses[:24],
        "short_n": len(shorts),
        "long_n": len(longs),
        "gap_n": len(gaps),
        "short_avg": (sum(shorts) / len(shorts)) if shorts else 0,
        "long_avg": (sum(longs) / len(longs)) if longs else 0,
        "period": bestL,
        "period_match": (100.0 * bestScore / (min(n - bestL, bestL * 2) if bestL else 1)) if bestL else 0,
        "bits_head": bits[:80],
        "ratio_long_short": (len(longs) / len(shorts)) if shorts else None,
    }


def frame_match(a, b, nmax=80):
    n = min(len(a), len(b), nmax)
    if n == 0:
        return None
    m = 0
    for i in range(n):
        x, y = a[i], b[i]
        if abs(x - y) > max(x, y) // 4 + 80:
            m += 1
    return m, n, 100.0 * m / n


parsed = {}
for idx_s, name, cnt, csv in rfdatas:
    pulses = [int(x) for x in csv.split(",") if x.strip()]
    parsed[int(idx_s)] = {"name": name, "count": int(cnt), "pulses": pulses}
    st = stats(pulses)
    print(f"\n===== RFDATA {idx_s} {name} n={cnt} =====")
    if st:
        print(f"  pulses={st['n']} total={st['total_ms']:.1f}ms head={st['head']}")
        print(f"  short(<400)={st['short_n']} avg={st['short_avg']:.0f}  long(400-2500)={st['long_n']} avg={st['long_avg']:.0f}  ratio={st['ratio_long_short']}")
        print(f"  gaps(>=4000)={st['gap_n']} period={st['period']} match={st['period_match']:.1f}%")
        print(f"  bits: {st['bits_head']}")

if not parsed:
    # fallback parse from [RF] key lines only; try extract pulses: lines
    print("No RFDATA parsed; trying pulses: lines")
    for m in re.finditer(r"按键\s+(\d+)\s+单帧\s+(\d+)\s+脉冲:\s*([0-9,]+)", text):
        idx = int(m.group(1))
        pulses = [int(x) for x in m.group(3).split(",") if x.strip()]
        parsed[idx] = {"name": "learn", "count": len(pulses), "pulses": pulses}
        st = stats(pulses)
        print(f"\n===== learned key {idx} =====")
        print(st)

if backup:
    print("\n===== vs backup =====")
    for idx, meta in backup.get("keys", {}).items():
        bi = int(idx)
        bp = meta.get("pulses") or []
        print(f"backup[{bi}] name={meta.get('name')} n={meta.get('count')} head={bp[:16]}")
        if bi in parsed:
            mm = frame_match(parsed[bi]["pulses"], bp)
            print(f"  match vs current: {mm}")

# 结论启发
print("\n===== 特征结论 =====")
for idx, item in parsed.items():
    p = item["pulses"]
    st = stats(p) or {}
    ok = True
    reasons = []
    if st.get("n", 0) < 24:
        ok = False
        reasons.append("帧太短")
    if st.get("short_n", 0) < 6 or st.get("long_n", 0) < 6:
        ok = False
        reasons.append("短/长脉冲不足")
    if st.get("period"):
        reasons.append(f"有重复周期 L={st['period']} ({st['period_match']:.0f}%)")
    else:
        reasons.append("未检出明显周期")
    # 与备份是否像同一遥控家族
    if backup and str(idx) in backup.get("keys", {}):
        mm = frame_match(p, backup["keys"][str(idx)].get("pulses") or [])
        if mm:
            reasons.append(f"与备份不匹配 {mm[0]}/{mm[1]} ({mm[2]:.0f}%) 不同码" if mm[2] > 30 else f"与备份接近 匹配误差{mm[0]}/{mm[1]}")
    print(f"key{idx}: {'PASS' if ok else 'FAIL'} | " + "; ".join(reasons))
