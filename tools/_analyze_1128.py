# -*- coding: utf-8 -*-
import json
from pathlib import Path
from statistics import median

CAP = Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_112837.json")
KEYS = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)
MIN_US = 8


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


def strip(p, thr=MIN_US):
    return [x for x in p if x >= thr]


def classify(pulses):
    n = len(pulses)
    if n == 0:
        return "empty"
    short = sum(1 for x in pulses if x < 30)
    mid = sum(1 for x in pulses if 80 <= x <= 800)
    med = median(pulses)
    if short / n > 0.7 and med < 30:
        return "NOISE"
    if mid / n > 0.4:
        return "DATA OOK"
    return "mixed"


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


def best_match(tx, rx, max_off=150):
    if not tx or not rx:
        return -1, 0, 0
    best = (-1, 0, 0)
    ntx = min(len(tx), 120)
    for off in range(0, min(max_off, len(rx))):
        n = min(ntx, len(rx) - off, 80)
        if n < 16:
            break
        good = 0
        for i in range(n):
            a, b = tx[i], rx[off + i]
            mx = max(a, b, 1)
            if abs(a - b) <= max(mx // 4, 80):
                good += 1
        # prefer higher good, then lower off
        if good > best[0] or (good == best[0] and off < best[1]):
            best = (good, off, n)
    return best


keys = json.loads(KEYS.read_text(encoding="utf-8"))
raw0 = keys["keys"]["0"]["pulses"]
key0 = strip(raw0, 20)  # cleaned of glitches
print(f"file={CAP.name}")
print(f"key0 raw={len(raw0)} cleaned(<20us strip)={len(key0)} head={key0[:20]}")

caps = json.loads(CAP.read_text(encoding="utf-8"))
print(f"captures={len(caps)}")

bodies = []
for i, c in enumerate(caps):
    pl = list(c["pulses"])
    n = len(pl)
    fl = detect_frame_len(pl) if n >= 32 else n
    cls = classify(pl)
    short = sum(1 for x in pl if x < 30)
    mid = sum(1 for x in pl if 80 <= x <= 800)
    gaps = sum(1 for x in pl if x >= 4000)
    print(
        f"\n#{i+1} ts={c.get('timestamp')} n={n} frame~{fl} "
        f"{sum(pl)/1000:.1f}ms med={median(pl) if n else 0:.0f} "
        f"short<30={short} mid={mid} gaps={gaps} => {cls}"
    )
    print(f"  head={pl[:20]}")
    body = strip(first_body(pl), MIN_US)
    bodies.append(body)
    print(f"  body_cleaned len={len(body)} head={body[:24]}")
    if body:
        # raw frame vs key0 (no strip on body already stripped)
        good, off, nn = best_match(key0, body)
        pct = 100 * good / nn if nn else 0
        print(f"  vs key0_clean: {good}/{nn} ({pct:.0f}%) off={off}")
        good2, off2, nn2 = best_match(strip(raw0, MIN_US), body)
        pct2 = 100 * good2 / nn2 if nn2 else 0
        print(f"  vs key0_strip8: {good2}/{nn2} ({pct2:.0f}%) off={off2}")

if len(bodies) >= 2:
    print("\npairwise bodies:")
    for i in range(len(bodies)):
        for j in range(i + 1, len(bodies)):
            a, b = bodies[i], bodies[j]
            n = min(len(a), len(b), 60)
            if not n:
                continue
            m = sum(
                1
                for k in range(n)
                if abs(a[k] - b[k]) > max(max(a[k], b[k]) // 4, 80)
            )
            print(f"  #{i+1} vs #{j+1}: mismatch {m}/{n} match={100*(n-m)/n:.0f}%")
