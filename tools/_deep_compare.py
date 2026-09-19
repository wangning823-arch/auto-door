# -*- coding: utf-8 -*-
import json
from pathlib import Path


def load_caps(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def load_key(idx="0"):
    d = json.loads(
        Path(
            r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json"
        ).read_text(encoding="utf-8")
    )
    return d["keys"][idx]["pulses"]


def strip_glitch(p, min_v=8):
    return [x for x in p if x >= min_v]


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


def best_align_match(tx, rx, max_off=120):
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
            d = abs(a - b)
            if d <= max(mx // 4, 80):
                good += 1
        score = good * 1000 - off
        if score > best[0] * 1000 - best[1]:
            best = (good, off, n)
    return best


def bits_from(p, thr_short=250, thr_long=400):
    """Pulse-width bit decode: pair high/low durations into symbols if alternating."""
    out = []
    for v in p:
        if v >= 4000:
            out.append("G")
        elif v < 40:
            out.append(".")  # glitch filtered earlier mostly
        elif v < thr_short:
            out.append("S")
        elif v < thr_long:
            out.append("M")
        else:
            out.append("L")
    return "".join(out)


def pairwise_body_mismatch(a, b, nmax=60):
    n = min(len(a), len(b), nmax)
    m = 0
    diffs = []
    for i in range(n):
        x, y = a[i], b[i]
        if abs(x - y) > max(max(x, y) // 4, 80):
            m += 1
            if len(diffs) < 8:
                diffs.append((i, x, y))
    return m, n, diffs


key0 = load_key("0")
key0c = strip_glitch(key0, 8)
key1 = load_key("1")
key1c = strip_glitch(key1, 8)
print(f"key0 raw={len(key0)} cleaned={len(key0c)} head={key0c[:24]}")
print(f"key0 SML={bits_from(key0c)[:70]}")
print(f"key1 raw={len(key1)} cleaned={len(key1c)} head={key1c[:24]}")
print(f"key1 SML={bits_from(key1c)[:70]}")

# 09:16 good captures
caps = load_caps(r"C:\Users\goldg\Pictures\rf_capture_20260919_091640.json")
print("\n=== 09:16 real-looking captures ===")
bodies = []
for i, c in enumerate(caps):
    raw = c["pulses"]
    b = first_body(raw)
    bc = strip_glitch(b, 8)
    bodies.append(bc)
    print(f"#{i+1} n={len(raw)} body={len(b)} cleaned={len(bc)} head={bc[:24]}")
    print(f"   SML={bits_from(bc)[:70]}")

# cross match cleaned key0 vs each body
print("\n=== align key0_clean vs each 091640 body ===")
for i, b in enumerate(bodies):
    good, off, n = best_align_match(key0c, b)
    pct = 100 * good / n if n else 0
    print(f"  body#{i+1}: good={good}/{n} ({pct:.0f}%) off={off}")
    if n:
        m, nn, diffs = pairwise_body_mismatch(key0c, b[off:], 40)
        print(f"    first40 mismatches={m}/{nn} diffs={diffs}")

print("\n=== align key1_clean vs each 091640 body ===")
for i, b in enumerate(bodies):
    good, off, n = best_align_match(key1c, b)
    pct = 100 * good / n if n else 0
    print(f"  body#{i+1}: good={good}/{n} ({pct:.0f}%) off={off}")

# compare two 091640 bodies cleaned
print("\n=== 091640 pairwise cleaned ===")
for i in range(len(bodies)):
    for j in range(i + 1, len(bodies)):
        m, n, diffs = pairwise_body_mismatch(bodies[i], bodies[j], 60)
        print(f"  #{i+1} vs #{j+1}: {m}/{n} ({100*m/n if n else 0:.0f}%) diffs={diffs[:4]}")

# yesterday good file
caps2 = load_caps(r"C:\Users\goldg\Pictures\rf_capture_20260918_184221.json")
print("\n=== 0918 18:42 vs key0 ===")
for i, c in enumerate(caps2):
    b = strip_glitch(first_body(c["pulses"]), 8)
    good, off, n = best_align_match(key0c, b)
    print(f"  #{i+1} cleaned={len(b)} match key0 {good}/{n} ({100*good/n if n else 0:.0f}%) off={off}")
    print(f"    head={b[:20]} SML={bits_from(b)[:50]}")

# Is 091640 same protocol family as key0? Compare symbol histograms
def hist(p):
    s = bits_from(p)
    return {k: s.count(k) for k in "SMLG."}

print("\nkey0 hist", hist(key0c))
for i, b in enumerate(bodies):
    print(f"091640#{i+1} hist", hist(b))
