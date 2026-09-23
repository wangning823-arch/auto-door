# -*- coding: utf-8 -*-
import json
from pathlib import Path

path = Path(r"C:\Users\goldg\Pictures\rf_capture_20260918_183347.json")
caps = json.loads(path.read_text(encoding="utf-8"))
print(f"file: {path.name}")
print(f"captures: {len(caps)}")


def split_parts(p):
    for i, v in enumerate(p):
        if 3500 <= v <= 4500:
            return p[:i], p[i + 1 :]
    return [], p


def body(p):
    return [v for v in p if v >= 150]


def mismatch_num(a, b, nmax=100):
    n = min(len(a), len(b), nmax)
    mism = 0
    for k in range(n):
        x, y = a[k], b[k]
        if abs(x - y) > max(x, y) // 4 + 80:
            mism += 1
    return mism, n


def bits_pwm(b, lo=600):
    out = []
    for v in b:
        if v > 2000:
            out.append("G")
        elif v > 1500:
            out.append("?")
        else:
            out.append("1" if v >= lo else "0")
    return "".join(out)


print("\n=== Structure ===")
bodies, pres = [], []
for i, c in enumerate(caps):
    pre, post = split_parts(c["pulses"])
    bd = body(post)
    pres.append(pre)
    bodies.append(bd)
    print(
        f"#{i+1} t={c['timestamp']} n={c['count']} "
        f"pre={len(pre)} body={len(bd)} total_ms={sum(c['pulses'])/1000:.1f}"
    )
    print(f"   head16: {c['pulses'][:16]}")

print("\n=== Preamble (before ~4ms gap) ===")
if all(len(p) > 0 for p in pres):
    p0 = pres[0]
    for i, pre in enumerate(pres):
        n = min(len(p0), len(pre))
        mism = sum(1 for a, b in zip(p0[:n], pre[:n]) if abs(a - b) > max(a, b) // 4 + 80)
        print(f" pre#{i+1}: len={len(pre)} vs #{1}, mism={mism}/{n}")
    print(" pre#1:", pres[0])
else:
    print(" 无 ~4ms 间隔（可能不是同一结构，或间隔不在3500-4500）")
    # find long gaps
    for i, c in enumerate(caps):
        longs = [(j, v) for j, v in enumerate(c["pulses"]) if v > 3000]
        print(f"  #{i+1} long_gaps: {longs[:5]}")

print("\n=== Body numeric pairwise (first 60) ===")
for i in range(len(bodies)):
    for j in range(i + 1, len(bodies)):
        mism, n = mismatch_num(bodies[i], bodies[j], 60)
        print(f" {i+1} vs {j+1}: mism={mism}/{n} ({100*mism/n if n else 0:.1f}%)")

print("\n=== Bit PWM thr=600 ===")
seqs = []
for i, b in enumerate(bodies):
    s = bits_pwm(b)
    seqs.append(s)
    print(f"#{i+1} len={len(s)}: {s[:80]}")

print("\n=== Bit pairwise ===")
for i in range(len(seqs)):
    for j in range(i + 1, len(seqs)):
        a, b = seqs[i], seqs[j]
        n = min(len(a), len(b))
        mism = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
        print(f" {i+1} vs {j+1}: {mism}/{n} ({100*mism/n if n else 0:.1f}%)")

# Compare with old 433 capture
old = Path(r"C:\Users\goldg\Pictures\rf_capture_20260918_173303.json")
if old.exists():
    caps433 = json.loads(old.read_text(encoding="utf-8"))
    print("\n=== vs 433 file (same remote?) ===")
    print(f"433 #1 n={caps433[0]['count']} total={sum(caps433[0]['pulses'])/1000:.1f}ms")
    print(f"new #1 n={caps[0]['count']} total={sum(caps[0]['pulses'])/1000:.1f}ms")
    mism, n = mismatch_num(
        body(split_parts(caps433[0]["pulses"])[1]),
        bodies[0],
        80,
    )
    print(f"433#1 vs new#1 body: mism={mism}/{n}")

# Pulse width histogram
print("\n=== Width buckets (all bodies) ===")
from collections import Counter
cnt = Counter()
for b in bodies:
    for v in b:
        if v < 200:
            cnt["<200"] += 1
        elif v < 500:
            cnt["200-500"] += 1
        elif v < 700:
            cnt["500-700"] += 1
        elif v < 1000:
            cnt["700-1000"] += 1
        elif v < 3000:
            cnt["1k-3k"] += 1
        else:
            cnt[">3k"] += 1
for k in ["<200", "200-500", "500-700", "700-1000", "1k-3k", ">3k"]:
    print(f" {k}: {cnt.get(k,0)}")

# Fixed vs rolling conclusion
same_low = 0
total_pairs = 0
for i in range(len(seqs)):
    for j in range(i + 1, len(seqs)):
        total_pairs += 1
        a, b = seqs[i], seqs[j]
        n = min(len(a), len(b))
        mism = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
        if n and mism / n < 0.05:
            same_low += 1
print(f"\n=== Conclusion hint ===")
print(f"pairs with <5% bit mismatch: {same_low}/{total_pairs}")
if same_low == total_pairs and total_pairs > 0:
    print("=> 多次抓包高度一致：固定码特征")
elif same_low == 0:
    print("=> 两两差异都大：滚码或噪声/错位严重")
else:
    print("=> 部分一致部分不一致：需看是否错位/混到另一只遥控")
