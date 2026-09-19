# -*- coding: utf-8 -*-
import json
from pathlib import Path
from statistics import median

files = [
    r"C:\Users\goldg\Pictures\rf_capture_20260919_112157.json",
    r"C:\Users\goldg\Pictures\rf_capture_20260919_091640.json",
    r"C:\Users\goldg\Pictures\rf_capture_20260919_091159.json",
    r"C:\Users\goldg\Pictures\rf_capture_20260919_085702.json",
    r"C:\Users\goldg\Pictures\rf_capture_20260918_184221.json",
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


def classify(pulses):
    n = len(pulses)
    if n == 0:
        return "empty"
    short = sum(1 for p in pulses if p < 30)
    mid = sum(1 for p in pulses if 80 <= p <= 800)
    med = median(pulses)
    if short / n > 0.7 and med < 30:
        return "NOISE (mostly <30us glitches)"
    if mid / n > 0.4:
        return "DATA-like OOK"
    return "mixed/unknown"


def body(c):
    pl = list(c["pulses"])
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


def mm(a, b, nmax=80):
    n = min(len(a), len(b), nmax)
    if n == 0:
        return 0, 0
    m = sum(1 for k in range(n) if abs(a[k] - b[k]) > max(max(a[k], b[k]) // 4, 80))
    return m, n


# load learned open key
keys = json.loads(
    Path(r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json").read_text(
        encoding="utf-8"
    )
)
open_key = keys["keys"]["0"]["pulses"]

for fp in files:
    p = Path(fp)
    if not p.exists():
        print("missing", fp)
        continue
    caps = json.loads(p.read_text(encoding="utf-8"))
    print("=" * 70)
    print(p.name, "captures:", len(caps))
    bodies = []
    for i, c in enumerate(caps):
        pl = list(c["pulses"])
        n = len(pl)
        fl = detect_frame_len(pl) if n >= 32 else n
        cls = classify(pl)
        total_ms = sum(pl) / 1000
        short = sum(1 for x in pl if x < 30)
        mid = sum(1 for x in pl if 80 <= x <= 800)
        gaps = sum(1 for x in pl if x >= 4000)
        med = median(pl) if n else 0
        print(
            f"  #{i+1} ts={c.get('timestamp','?')} n={n} frame~{fl} "
            f"{total_ms:.1f}ms med={med:.0f} short<30={short} mid={mid} gaps={gaps} => {cls}"
        )
        print(f"     head={pl[:16]}")
        b = body(c)
        bodies.append(b)
        if len(b) >= 16:
            m, nn = mm(open_key, b, 60)
            pct = (100 * (nn - m) / nn) if nn else 0
            print(f"     body_len={len(b)} match_vs_key0={nn-m}/{nn} ({pct:.0f}%)")
            print(f"     body={b[:30]}")

    if len(bodies) >= 2:
        print("  pairwise:")
        for i in range(len(bodies)):
            for j in range(i + 1, len(bodies)):
                m, nn = mm(bodies[i], bodies[j], 80)
                if nn:
                    print(
                        f"    #{i+1} vs #{j+1}: mismatch {m}/{nn} ({100*m/nn:.0f}%) "
                        f"match={100*(nn-m)/nn:.0f}%"
                    )

print("\nlearned key0 open head:", open_key[:30])
