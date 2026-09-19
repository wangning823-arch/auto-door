# -*- coding: utf-8 -*-
"""对比新学 key0 与早上原遥控抓包；检查首脉冲极性特征"""
import json
from pathlib import Path

KEYS = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)
REMOTES = [
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_091640.json"),
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_130820.json"),
]


def detect_frame_len(pulses, min_len=16, max_len=400):
    n = len(pulses)
    if n < min_len * 2:
        return n
    best_l, best_score = n, -1
    max_l = min(max_len, n // 2)
    for L in range(min_len, max_l + 1):
        score = cmp = 0
        check = min(n - L, L * 2)
        for i in range(check):
            a, b = pulses[i], pulses[i + L]
            mx = max(a, b, 1)
            d = abs(a - b)
            if d <= max(mx // 4, 80):
                score += 1
            cmp += 1
        if cmp >= 16 and score * 10 >= cmp * 8:
            if score > best_score or (score == best_score and L > best_l):
                best_score, best_l = score, L
    return best_l


def first_body(pl):
    start = 0
    for j, v in enumerate(pl):
        if v >= 4000:
            start = j + 1
            break
    pl = pl[start:]
    if len(pl) < 16:
        return pl
    fl = detect_frame_len(pl)
    if fl < 16 or fl > 240:
        fl = min(len(pl), 80)
    return pl[:fl]


def sml(p):
    out = []
    for v in p:
        if v >= 4000:
            out.append("G")
        elif v < 40:
            out.append(".")
        elif v < 250:
            out.append("S")
        elif v < 400:
            out.append("M")
        else:
            out.append("L")
    return "".join(out)


def best_match(tx, rx, max_off=200, invert=False):
    if invert:
        rx = rx  # polarity invert: swap by shifting parity
        # Invert by comparing tx[i] to rx[off+i] with odd/even swapped:
        # simulate: compare tx to a sequence that starts with opposite level
        # Easier: reverse role - match rx against tx with offset, then report
        pass
    if not tx or not rx:
        return -1, 0, 0
    best = (-1, 0, 0)
    ntx = min(len(tx), 120)
    for off in range(0, min(max_off, len(rx))):
        n = min(ntx, len(rx) - off, 80)
        if n < 20:
            break
        good = 0
        for i in range(n):
            a, b = tx[i], rx[off + i]
            mx = max(a, b, 1)
            if abs(a - b) <= max(mx // 4, 80):
                good += 1
        if good > best[0]:
            best = (good, off, n)
    return best


def match_inverted_polarity(tx, rx, max_off=80):
    """帧内偶奇对调：把 rx 每个脉冲与 tx 按偏移比，但允许相位差1的极性翻转。
    简化：用 tx 翻转——把 tx 视为从空闲低开始，首段应为空闲到首沿。
    实际测：将 rx 与 tx 在 off 处比，同时试 off+1 且忽略首点。
    """
    # Try matching with one-pulse shift as polarity ambiguity proxy
    best = (-1, 0, 0)
    for off in range(0, min(max_off, len(rx))):
        for shift in (0, 1):
            good = 0
            n = 0
            for i in range(len(tx)):
                j = off + i + shift
                if j >= len(rx):
                    break
                if i + shift >= len(tx) and False:
                    break
                a = tx[i]
                b = rx[j]
                mx = max(a, b, 1)
                n += 1
                if abs(a - b) <= max(mx // 4, 80):
                    good += 1
                if n >= 60:
                    break
            if n >= 30 and good > best[0]:
                best = (good, off, n)
    return best


keys = json.loads(KEYS.read_text(encoding="utf-8"))
k0 = keys["keys"]["0"]["pulses"]
k1 = keys["keys"]["1"]["pulses"]
print(f"NEW key0 n={len(k0)} head={k0[:24]}")
print(f"NEW key0 SML={sml(k0)}")
print(f"NEW key1 n={len(k1)} head={k1[:20]}")

# 旧 backup 里若还有 09:16 前的学习对比 — 用 091640 原遥控
for fp in REMOTES:
    if not fp.exists():
        print("missing", fp)
        continue
    caps = json.loads(fp.read_text(encoding="utf-8"))
    print("=" * 60)
    print(fp.name, "n_captures", len(caps))
    for i, c in enumerate(caps[:6]):
        body = first_body(c["pulses"])
        print(f"  #{i+1} n={len(c['pulses'])} body={len(body)} head={body[:16]}")
        print(f"       SML={sml(body)[:70]}")
        g, o, n = best_match(k0, body)
        pct = 100 * g / n if n else 0
        print(f"       vs NEW key0: {g}/{n} ({pct:.0f}%) off={o}")
        # polarity-ish: start offset+1
        g2, o2, n2 = best_match(k0[1:] if k0 else k0, body)
        pct2 = 100 * g2 / n2 if n2 else 0
        print(f"       key0[1:] vs: {g2}/{n2} ({pct2:.0f}%) off={o2}")

# 统计 new key0 脉宽分布
short = sum(1 for x in k0 if x < 250)
long_ = sum(1 for x in k0 if x >= 400)
print(f"\nkey0 short<250={short} long>=400={long_} mid={len(k0)-short-long_}")
print(f"key0 min={min(k0)} max={max(k0)}")
