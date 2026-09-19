# -*- coding: utf-8 -*-
import json
from pathlib import Path
from statistics import median

FILES = [
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_130820.json"),
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_125726.json"),
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_125322.json"),
]
KEYS = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)


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


def strip(p, thr=8):
    return [x for x in p if x >= thr]


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
        good = sum(
            1
            for i in range(n)
            if abs(tx[i] - rx[off + i]) <= max(max(tx[i], rx[off + i]) // 4, 80)
        )
        if good > best[0] or (good == best[0] and off < best[1]):
            best = (good, off, n)
    return best


def classify(pl):
    n = len(pl)
    if n == 0:
        return "empty"
    short = sum(1 for x in pl if x < 30)
    data = sum(1 for x in pl if 80 <= x < 5000)
    if short / n > 0.7 and median(pl) < 30:
        return "NOISE"
    if data / n > 0.4:
        return "DATA OOK"
    return "mixed"


keys = json.loads(KEYS.read_text(encoding="utf-8"))
key0 = strip(keys["keys"]["0"]["pulses"], 20)
print(f"key0 cleaned={len(key0)}")

for fp in FILES:
    if not fp.exists():
        print("missing", fp)
        continue
    caps = json.loads(fp.read_text(encoding="utf-8"))
    print("=" * 70)
    print(fp.name, "captures=", len(caps))
    bodies = []
    for i, c in enumerate(caps):
        pl = list(c["pulses"])
        n = len(pl)
        data = sum(1 for x in pl if 80 <= x < 5000)
        short = sum(1 for x in pl if x < 30)
        gaps = sum(1 for x in pl if x >= 4000)
        cls = classify(pl)
        print(
            f"  #{i+1} ts={c.get('timestamp')} n={n} data={data} short={short} "
            f"gaps={gaps} med={median(pl):.0f} => {cls}"
        )
        print(f"     head={pl[:18]}")
        body = strip(first_body(pl), 8)
        bodies.append(body)
        if body:
            good, off, nn = best_match(key0, body)
            pct = 100 * good / nn if nn else 0
            print(f"     body={len(body)} vs key0 {good}/{nn} ({pct:.0f}%) off={off}")

    if len(bodies) >= 2:
        print("  pairwise:")
        for i in range(len(bodies)):
            for j in range(i + 1, min(i + 3, len(bodies))):
                a, b = bodies[i], bodies[j]
                m0 = min(len(a), len(b), 60)
                if not m0:
                    continue
                m = sum(
                    1
                    for k in range(m0)
                    if abs(a[k] - b[k]) > max(max(a[k], b[k]) // 4, 80)
                )
                print(
                    f"    #{i+1} vs #{j+1}: mismatch {m}/{m0} "
                    f"match={100*(m0-m)/m0:.0f}%"
                )
