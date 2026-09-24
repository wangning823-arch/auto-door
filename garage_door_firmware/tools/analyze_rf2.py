# -*- coding: utf-8 -*-
"""对齐 + 位解码深入分析"""
import json
from pathlib import Path

caps = json.loads(
    Path(r"C:\Users\goldg\Pictures\rf_capture_20260918_173303.json").read_text(
        encoding="utf-8"
    )
)


def split_parts(p):
    for i, v in enumerate(p):
        if 3500 <= v <= 4500:
            return p[:i], p[i + 1 :]
    return [], p


def body(p):
    return [v for v in p if v >= 150]


def best_shift_match(a, b, max_shift=30):
    """找 b 相对 a 的最佳移位，返回 (最小失配率, shift, n)"""
    best = (1.0, 0, 0)
    nmax = min(len(a), len(b))
    for shift in range(-max_shift, max_shift + 1):
        mism = cmp = 0
        for i in range(nmax):
            j = i + shift
            if j < 0 or j >= len(b):
                continue
            x, y = a[i], b[j]
            if abs(x - y) > max(max(x, y) // 4, 80):
                mism += 1
            cmp += 1
        if cmp < 40:
            continue
        rate = mism / cmp
        if rate < best[0]:
            best = (rate, shift, cmp)
    return best


bodies = [body(split_parts(c["pulses"])[1]) for c in caps]
pres = [split_parts(c["pulses"])[0] for c in caps]

print("=== 前导(4ms gap 前)是否固定 ===")
print(f"长度: {[len(p) for p in pres]}")
print(f"preamble #1: {pres[0]}")
all_same = all(
    len(p) == len(pres[0])
    and all(abs(x - y) <= max(max(x, y) // 4, 80) for x, y in zip(p, pres[0]))
    for p in pres
)
print(f"六次前导是否一致: {all_same}")

print("\n=== 数据段最佳对齐失配率 ===")
for i in range(len(bodies)):
    for j in range(i + 1, len(bodies)):
        rate, shift, n = best_shift_match(bodies[i], bodies[j])
        print(
            f" {i+1} vs {j+1}: best_rate={rate*100:.1f}% shift={shift} n={n} "
            f"(len {len(bodies[i])}/{len(bodies[j])})"
        )

# PWM 按位解码：每个数据位 = 一个高脉宽（忽略低脉宽，只取 high）
# 由于我们记录的是交替 H/L 序列，从 body[0] 起试两种相位


def bits_even(body, lo=550):
    """偶数位(0,2,4...)当 high"""
    bits = []
    for i in range(0, len(body), 2):
        v = body[i]
        if v > 1500:
            bits.append("?")
        else:
            bits.append("1" if v >= lo else "0")
    return "".join(bits)


def bits_odd(body, lo=550):
    bits = []
    for i in range(1, len(body), 2):
        v = body[i]
        if v > 1500:
            bits.append("?")
        else:
            bits.append("1" if v >= lo else "0")
    return "".join(bits)


def bits_pair0(body):
    """相邻对：短+长=01，长+短=10，长+长/短短=特殊"""
    out = []
    i = 0
    while i + 1 < len(body):
        a, b = body[i], body[i + 1]
        if a > 2000 or b > 2000:
            i += 1
            continue
        sa = 1 if a >= 600 else 0
        sb = 1 if b >= 600 else 0
        if sa == 0 and sb == 1:
            out.append("0")
        elif sa == 1 and sb == 0:
            out.append("1")
        elif sa == 1 and sb == 1:
            out.append("L")
        else:
            out.append("S")
        i += 2
    return "".join(out)


print("\n=== PWM 位序列（even 相位）===")
for i, b in enumerate(bodies):
    s = bits_even(b)
    print(f"#{i+1} len={len(s)}: {s}")


print("\n=== PWM 位序列（odd 相位）===")
for i, b in enumerate(bodies):
    s = bits_odd(b)
    print(f"#{i+1} len={len(s)}: {s}")


def xor_diff_rate(a, b):
    n = min(len(a), len(b))
    if n == 0:
        return 1.0, 0
    mism = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
    return mism / n, n


print("\n=== even 相位位串两两比较 ===")
ev = [bits_even(b) for b in bodies]
for i in range(len(ev)):
    for j in range(i + 1, len(ev)):
        r, n = xor_diff_rate(ev[i], ev[j])
        print(f" {i+1} vs {j+1}: {r*100:.1f}% ({int(r*n)}/{n})")

print("\n=== odd 相位位串两两比较 ===")
od = [bits_odd(b) for b in bodies]
for i in range(len(od)):
    for j in range(i + 1, len(od)):
        r, n = xor_diff_rate(od[i], od[j])
        print(f" {i+1} vs {j+1}: {r*100:.1f}% ({int(r*n)}/{n})")

print("\n=== 高低配对解码 ===")
pr = [bits_pair0(b) for b in bodies]
for i, s in enumerate(pr):
    print(f"#{i+1} len={len(s)}: {s}")
print("配对两两比较:")
for i in range(len(pr)):
    for j in range(i + 1, len(pr)):
        r, n = xor_diff_rate(pr[i], pr[j])
        print(f" {i+1} vs {j+1}: {r*100:.1f}% ({int(r*n)}/{n})")

# 时长统计
print("\n=== 总时长 / 高脉冲统计 ===")
for i, (c, b) in enumerate(zip(caps, bodies)):
    highs = [b[k] for k in range(0, len(b), 2)]
    lows = [b[k] for k in range(1, len(b), 2)]
    print(
        f"#{i+1} total={sum(c['pulses'])/1000:.2f}ms "
        f"body={len(b)} "
        f"avgH={sum(highs)/len(highs):.0f} avgL={sum(lows)/len(lows):.0f} "
        f"minH={min(highs)} maxH={max(highs)}"
    )

# 是否固定码：even 位串去噪后是否高度一致
print("\n=== 结论辅助：固定内容应有片段完全一致 ===")
# 找最长公共子串长度（位串）
def lcs_len(a, b, cap=80):
    # 简单 DP，限制长度
    a, b = a[:cap], b[:cap]
    m, n = len(a), len(b)
    dp = [[0] * (n + 1) for _ in range(m + 1)]
    best = 0
    for i in range(m):
        for j in range(n):
            if a[i] == b[j]:
                dp[i + 1][j + 1] = dp[i][j] + 1
                best = max(best, dp[i + 1][j + 1])
    return best


for phase_name, seqs in (("even", ev), ("odd", od), ("pair", pr)):
    print(f" {phase_name} LCS (前80位):")
    for i in range(len(seqs)):
        for j in range(i + 1, min(i + 3, len(seqs))):
            if j <= i:
                continue
            L = lcs_len(seqs[i], seqs[j], 80)
            print(f"   {i+1} vs {j+1}: LCS={L}")
