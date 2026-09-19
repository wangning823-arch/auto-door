# -*- coding: utf-8 -*-
import json
from pathlib import Path
from statistics import median

CAP = Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_120130.json")
KEYS = Path(
    r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
)
print("cap exists", CAP.exists(), "size", CAP.stat().st_size if CAP.exists() else 0)


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


keys = json.loads(KEYS.read_text(encoding="utf-8"))
raw0 = keys["keys"]["0"]["pulses"]
key0 = strip(raw0, 20)
print(f"key0 cleaned={len(key0)} head={key0[:20]}")

caps = json.loads(CAP.read_text(encoding="utf-8"))
print(f"captures={len(caps)}")
bodies = []
for i, c in enumerate(caps):
    pl = list(c["pulses"])
    n = len(pl)
    short = sum(1 for x in pl if x < 30)
    mid = sum(1 for x in pl if 80 <= x <= 800)
    data = sum(1 for x in pl if 80 <= x < 5000)
    gaps = sum(1 for x in pl if x >= 4000)
    print(
        f"\n#{i+1} ts={c.get('timestamp')} n={n} data~{data} "
        f"short={short} mid={mid} gaps={gaps} med={median(pl):.0f}"
    )
    print(f"  head={pl[:24]}")
    body = strip(first_body(pl), 8)
    bodies.append(body)
    print(f"  body len={len(body)} head={body[:24]}")
    good, off, nn = best_match(key0, body)
    pct = 100 * good / nn if nn else 0
    print(f"  vs key0: {good}/{nn} ({pct:.0f}%) off={off}")

if len(bodies) >= 2:
    print("\npairwise:")
    for i in range(len(bodies)):
        for j in range(i + 1, len(bodies)):
            a, b = bodies[i], bodies[j]
            n = min(len(a), len(b), 80)
            if not n:
                continue
            m = sum(1 for k in range(n) if abs(a[k] - b[k]) > max(max(a[k], b[k]) // 4, 80))
            print(f"  #{i+1} vs #{j+1}: mismatch {m}/{n} match={100*(n-m)/n:.0f}%")

# Also try: TX multi-frame capture - strip inter-frame gaps and take one frame period
print("\n--- period detect on full capture #1 ---")
if caps:
    pl = caps[0]["pulses"]
    fl = detect_frame_len(pl)
    print(f"detect_frame_len full={fl}")
    # try match key0 against sliding windows of period fl
    if fl > 16 and fl < len(pl):
        for off in range(0, min(fl, 100), 1):
            n = min(len(key0), len(pl) - off, 60)
            if n < 40:
                break
            good = sum(
                1
                for i in range(n)
                if abs(key0[i] - pl[off + i]) <= max(max(key0[i], pl[off + i]) // 4, 80)
            )
            if good >= 40:
                print(f"  window off={off} good={good}/{n} ({100*good/n:.0f}%)")
                break
        else:
            # report best
            best = (0, 0, 0)
            for off in range(0, min(len(pl) - 40, 200)):
                n = min(len(key0), len(pl) - off, 60)
                good = sum(
                    1
                    for i in range(n)
                    if abs(key0[i] - pl[off + i])
                    <= max(max(key0[i], pl[off + i]) // 4, 80)
                )
                if good > best[0]:
                    best = (good, off, n)
            print(f"  best window good={best[0]}/{best[2]} off={best[1]}")
