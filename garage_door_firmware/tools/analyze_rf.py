# -*- coding: utf-8 -*-
import json
from pathlib import Path

path = Path(r"C:\Users\goldg\Pictures\rf_capture_20260918_173303.json")
caps = json.loads(path.read_text(encoding="utf-8"))
print(f"captures: {len(caps)}")


def split_parts(p):
    for i, v in enumerate(p):
        if 3500 <= v <= 4500:
            return p[:i], p[i + 1 :]
    return [], p


def norm_body(post):
    return [v for v in post if v >= 150]


def mismatch_num(a, b, nmax=100):
    n = min(len(a), len(b), nmax)
    mism = 0
    for k in range(n):
        x, y = a[k], b[k]
        if abs(x - y) > max(x, y) // 4 + 80:
            mism += 1
    return mism, n


print("\n=== Structure ===")
pres, posts, bodies = [], [], []
for i, c in enumerate(caps):
    pre, post = split_parts(c["pulses"])
    body = norm_body(post)
    pres.append(pre)
    posts.append(post)
    bodies.append(body)
    print(
        f"#{i+1} t={c['timestamp']} n={c['count']} "
        f"pre={len(pre)} post={len(post)} body={len(body)} "
        f"total_ms={sum(c['pulses'])/1000:.1f}"
    )

print("\n=== Preamble compare vs #1 ===")
p0 = pres[0]
for i, pre in enumerate(pres):
    n = min(len(p0), len(pre))
    mism = sum(1 for a, b in zip(p0[:n], pre[:n]) if abs(a - b) > max(a, b) // 4 + 80)
    print(f" pre#{i+1}: len={len(pre)} vs {len(p0)}, mism={mism}/{n}")

print("\n=== Body numeric pairwise (first 80) ===")
for i in range(len(bodies)):
    for j in range(i + 1, len(bodies)):
        mism, n = mismatch_num(bodies[i], bodies[j], 80)
        print(f" {i+1} vs {j+1}: mism={mism}/{n} ({100*mism/n if n else 0:.1f}%)")

print("\n=== Body first 50 pulses ===")
for i, b in enumerate(bodies):
    print(f"#{i+1}: {b[:50]}")


def to_bits(body, thr=600):
    bits = []
    for v in body:
        if v > 2000:
            bits.append("G")
        elif v > thr:
            bits.append("1")
        else:
            bits.append("0")
    return "".join(bits)


print("\n=== Bitstring thr=600 (S=0 L=1) first 90 ===")
seqs = []
for i, b in enumerate(bodies):
    s = to_bits(b)
    seqs.append(s)
    print(f"#{i+1} len={len(s)}: {s[:90]}")

print("\n=== Bitstring pairwise mismatch ===")
for i in range(len(seqs)):
    for j in range(i + 1, len(seqs)):
        a, b = seqs[i], seqs[j]
        n = min(len(a), len(b))
        mism = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
        print(f" {i+1} vs {j+1}: {mism}/{n} ({100*mism/n if n else 0:.1f}%)")

# Try PWM bit decode on alternating pattern after sync
# Many Chinese fixed codes: bit = high width; short~400=0 long~800=1, low ~ complementary


def pwm_bits(body):
    # take only pulses that look like data highs (skip gaps)
    bits = []
    for v in body:
        if v < 150 or v > 1500:
            continue
        bits.append("1" if v >= 600 else "0")
    return "".join(bits)


print("\n=== Single-frame content hash (body numeric first 60) ===")
for i, b in enumerate(bodies):
    h = 0x811C9DC5
    for x in b[:60]:
        h ^= x
        h = (h * 0x01000193) & 0xFFFFFFFF
    print(f"#{i+1} hash60=0x{h:08X}")

# Find repeating frame length on body
def detect_frame(p, min_l=16, max_l=300):
    n = len(p)
    if n < min_l * 2:
        return n, 0
    best_l, best_s = n, -1
    for L in range(min_l, min(max_l, n // 2) + 1):
        s = c = 0
        check = min(n - L, L * 2)
        for i in range(check):
            a, b = p[i], p[i + L]
            if abs(a - b) <= max(max(a, b) // 4, 80):
                s += 1
            c += 1
        if c >= 16 and s * 10 >= c * 8:
            if s > best_s or (s == best_s and L > best_l):
                best_s, best_l = s, L
    return best_l, best_s


print("\n=== Frame length detect on body ===")
frames = []
for i, b in enumerate(bodies):
    fl, sc = detect_frame(b)
    frames.append(b[:fl])
    print(f"#{i+1} frame_len={fl} score={sc}")

print("\n=== First-frame numeric pairwise ===")
for i in range(len(frames)):
    for j in range(i + 1, len(frames)):
        mism, n = mismatch_num(frames[i], frames[j], 999)
        print(f" {i+1} vs {j+1}: mism={mism}/{n} ({100*mism/n if n else 0:.1f}%)")

print("\n=== First-frame bits pairwise thr=600 ===")
fb = [to_bits(f) for f in frames]
for i, s in enumerate(fb):
    print(f"#{i+1} flen={len(s)}: {s[:70]}")
for i in range(len(fb)):
    for j in range(i + 1, len(fb)):
        a, b = fb[i], fb[j]
        n = min(len(a), len(b))
        mism = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
        print(f" {i+1} vs {j+1}: {mism}/{n} ({100*mism/n if n else 0:.1f}%)")
