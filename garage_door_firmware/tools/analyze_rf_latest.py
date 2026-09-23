# -*- coding: utf-8 -*-
import json
from pathlib import Path

path = Path(r"C:\Users\goldg\Pictures\rf_capture_20260918_184221.json")
caps = json.loads(path.read_text(encoding="utf-8"))
print(f"captures: {len(caps)}")
print("timestamps:", [c["timestamp"] for c in caps])
for i, c in enumerate(caps):
    pl = c["pulses"]
    print(f"#{i+1} n={len(pl)} total_ms={sum(pl)/1000:.1f} head={pl[:12]}")


def body_after_long(p, lo=3500, hi=6000):
    for j, v in enumerate(p):
        if lo <= v <= hi:
            return [x for x in p[j + 1 :] if x >= 150]
    return [x for x in p if x >= 150]


def mm(a, b, nmax=80):
    n = min(len(a), len(b), nmax)
    m = 0
    for k in range(n):
        x, y = a[k], b[k]
        if abs(x - y) > max(x, y) // 4 + 80:
            m += 1
    return m, n


print("--- long gaps ---")
for i, c in enumerate(caps):
    lg = [(j, v) for j, v in enumerate(c["pulses"]) if v > 3000]
    print(f"#{i+1}: {lg[:10]} count={len(lg)}")

bodies = [body_after_long(c["pulses"]) for c in caps]
print("--- pairwise body first 60 ---")
for i in range(len(bodies)):
    for j in range(i + 1, len(bodies)):
        m, n = mm(bodies[i], bodies[j], 60)
        print(f"{i+1} vs {j+1}: {m}/{n} ({100*m/n:.1f}%)")

print("--- body first 30 ---")
for i, b in enumerate(bodies):
    print(f"{i+1}: {b[:30]}")

# bits thr=450 for 315 short pulses


def bits(b, lo=450, hi=700):
    out = []
    for v in b:
        if v > 2500:
            out.append("G")
        elif v > hi:
            out.append("H")
        elif v >= lo:
            out.append("1")
        else:
            out.append("0")
    return "".join(out)


print("--- bits thr 450/700 ---")
for i, b in enumerate(bodies):
    s = bits(b)
    print(f"#{i+1} len={len(s)}: {s[:70]}")

print("--- bit pairwise ---")
seqs = [bits(b) for b in bodies]
for i in range(len(seqs)):
    for j in range(i + 1, len(seqs)):
        a, b = seqs[i], seqs[j]
        n = min(len(a), len(b))
        m = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
        print(f"{i+1} vs {j+1}: {m}/{n} ({100*m/n:.1f}%)")

# repeated frame: 50-pulse period detection on raw after long gap
print("--- 50-period repeat check on raw ---")
for i, c in enumerate(caps):
    pl = c["pulses"]
    cut = 0
    for j, v in enumerate(pl):
        if 3500 <= v <= 6000:
            cut = j
            break
    seg = pl[cut + 1 : cut + 1 + 150] if cut or True else pl[:150]
    if len(seg) < 100:
        continue
    mism = cmp = 0
    for k in range(min(50, len(seg) - 50)):
        a, b = seg[k], seg[k + 50]
        if abs(a - b) <= max(max(a, b) // 4, 80):
            mism += 1
        cmp += 1
    print(f"#{i+1} period50 match {mism}/{cmp} = {100*mism/cmp:.1f}%")
