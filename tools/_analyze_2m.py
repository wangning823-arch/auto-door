# -*- coding: utf-8 -*-
import json
from pathlib import Path
from statistics import median

CAP = Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_131206.json")
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


def best_match(tx, rx, max_off=200):
    if not tx or not rx:
        return -1, 0, 0, []
    best = (-1, 0, 0, [])
    ntx = min(len(tx), 120)
    for off in range(0, min(max_off, len(rx))):
        n = min(ntx, len(rx) - off, 80)
        if n < 20:
            break
        good = 0
        bad_idx = []
        for i in range(n):
            a, b = tx[i], rx[off + i]
            mx = max(a, b, 1)
            d = abs(a - b)
            tol = max(mx // 4, 80)
            if d <= tol:
                good += 1
            elif len(bad_idx) < 5:
                bad_idx.append((i, a, b))
        if good > best[0] or (good == best[0] and off < best[1]):
            best = (good, off, n, bad_idx)
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
print(f"key0={len(key0)} {key0[:16]}")

caps = json.loads(CAP.read_text(encoding="utf-8"))
print(f"{CAP.name} captures={len(caps)}")

stats = {"full100": 0, "good": 0, "weak": 0, "fail": 0, "noise": 0}
bodies = []

for i, c in enumerate(caps):
    pl = list(c["pulses"])
    n = len(pl)
    data = sum(1 for x in pl if 80 <= x < 5000)
    short = sum(1 for x in pl if x < 30)
    gaps = sum(1 for x in pl if x >= 4000)
    cls = classify(pl)
    print(
        f"\n#{i+1} ts={c.get('timestamp')} n={n} data={data} short={short} "
        f"gaps={gaps} total_ms={sum(pl)/1000:.1f} med={median(pl):.0f} => {cls}"
    )
    print(f"  head={pl[:20]}")

    if cls == "NOISE":
        stats["noise"] += 1
        continue

    body = strip(first_body(pl), 8)
    bodies.append(body)
    good, off, nn, diffs = best_match(key0, body)
    pct = 100 * good / nn if nn else 0
    print(f"  body={len(body)} best align off={off} match {good}/{nn} = {pct:.0f}%")
    if diffs:
        print(f"  sample diffs (idx, key, rx): {diffs}")

    if pct >= 98 and nn >= 45:
        stats["full100"] += 1
        tag = "PERFECT"
    elif pct >= 85 and nn >= 40:
        stats["good"] += 1
        tag = "GOOD"
    elif pct >= 60:
        stats["weak"] += 1
        tag = "WEAK"
    else:
        stats["fail"] += 1
        tag = "FAIL/OTHER"
    print(f"  => {tag}")

# pairwise among good bodies
print("\n--- pairwise good frames ---")
good_bodies = []
for c in caps:
    pl = list(c["pulses"])
    if classify(pl) != "DATA OOK":
        continue
    b = strip(first_body(pl), 8)
    if len(b) >= 40:
        g, o, n, _ = best_match(key0, b)
        if n and g * 100 / n >= 85:
            good_bodies.append((c.get("timestamp"), b, g, n))

for i in range(len(good_bodies)):
    for j in range(i + 1, len(good_bodies)):
        t1, b1, g1, n1 = good_bodies[i]
        t2, b2, g2, n2 = good_bodies[j]
        m0 = min(len(b1), len(b2), 60)
        m = sum(
            1
            for k in range(m0)
            if abs(b1[k] - b2[k]) > max(max(b1[k], b2[k]) // 4, 80)
        )
        print(
            f"  {t1} vs {t2}: frame-internal mismatch {m}/{m0} "
            f"match={100*(m0-m)/m0:.0f}%  (each vs key {g1}/{n1}, {g2}/{n2})"
        )

print("\n===== SUMMARY =====")
print(stats)
print(f"good_frames_listed={len(good_bodies)}")
# period between good frames if timestamps allow
