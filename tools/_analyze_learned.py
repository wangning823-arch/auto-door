# -*- coding: utf-8 -*-
"""分析 RFDATA 特征：短/长脉冲、周期性、与备份/键间对比"""
from pathlib import Path
import json
import re

BOOT_KEYS = """
RFDATA 0 open 56 1912,116,168,94,117,122,56,236,404,587,24,299,365,272,371,577,46,580,81,236,404,538,127,129,520,437,203,118,496,210,418,229,412,537,102,232,412,210,432,200,436,211,428,533,105,523,118,212,434,198,434,198,445,197,439,195
RFDATA 1 close 80 200,430,504,122,192,436,193,438,498,128,496,132,185,442,495,135,181,450,488,136,180,448,181,447,180,446,489,141,174,456,172,455,173,454,172,456,172,455,172,454,173,454,173,456,473,158,473,150,166,4682,170,455,479,152,164,462,167,459,477,151,476,153,163,467,472,160,156,468,472,153,163,469,160,465,163,467,467,158,158,473
RFDATA 2 stop 54 209,75,102,108,533,261,361,563,56,243,405,207,416,531,96,532,98,219,408,517,110,212,420,512,118,203,427,194,432,194,435,515,107,210,425,184,445,499,123,499,131,189,445,182,441,185,442,182,449,184,439,187,435,206
"""


def parse(text):
    out = {}
    for m in re.finditer(r"RFDATA\s+(\d+)\s+(\w+)\s+(\d+)\s+([0-9,]+)", text):
        idx = int(m.group(1))
        pulses = [int(x) for x in m.group(4).split(",") if x.strip()]
        out[idx] = {"name": m.group(2), "n": int(m.group(3)), "p": pulses}
    return out


def period(p, minL=16, maxL=240):
    n = len(p)
    best = (0, 0)
    for L in range(minL, min(maxL, n // 2) + 1):
        cmp = score = 0
        check = min(n - L, L * 2)
        for i in range(check):
            a, b = p[i], p[i + L]
            mx = max(a, b)
            if abs(a - b) <= max(mx // 4, 80):
                score += 1
            cmp += 1
        if cmp >= 16 and score * 10 >= cmp * 8:
            if score > best[1] or (score == best[1] and L > best[0]):
                best = (L, score)
    if not best[0]:
        return 0, 0.0
    L = best[0]
    cmp = min(n - L, L * 2)
    return L, 100.0 * best[1] / cmp


def analyze(p):
    n = len(p)
    glitches = [v for v in p if v < 80]
    shorts = [v for v in p if 80 <= v < 400]
    longs = [v for v in p if 400 <= v <= 3500]
    huge = [v for v in p if v > 3500 and v < 8000]
    L, mpct = period(p)
    # 偶奇交替一致性（OOK 常见）
    # 简化：相邻对 (s,l) 或 (l,s)
    def avg(xs):
        return sum(xs) / len(xs) if xs else 0

    # 中段去掉首尾各 10% 再看周期
    body = p[max(0, n // 10) : n - max(0, n // 10) or n]
    Lb, Mb = period(body) if len(body) >= 32 else (0, 0)

    return {
        "n": n,
        "glitch_lt80": glitches,
        "glitch_n": len(glitches),
        "short_n": len(shorts),
        "long_n": len(longs),
        "huge_n": len(huge),
        "huge": huge,
        "short_avg": avg(shorts),
        "long_avg": avg(longs),
        "period": L,
        "period_pct": mpct,
        "body_period": Lb,
        "body_period_pct": Mb,
        "first": p[0],
        "head": p[:20],
        "mid": p[n // 3 : n // 3 + 20],
    }


def match(a, b, nmax=80):
    n = min(len(a), len(b), nmax)
    m = 0
    details = []
    for i in range(n):
        x, y = a[i], b[i]
        if abs(x - y) > max(x, y) // 4 + 80:
            m += 1
            if len(details) < 6:
                details.append((i, x, y))
    return m, n, 100.0 * m / n, details


keys = parse(BOOT_KEYS)
backup = json.loads(
    Path(r"D:\mimo\车库门自动化\garage_door_firmware\tools\rf_keys_backup.json").read_text(
        encoding="utf-8"
    )
)

print("=" * 60)
print("设备当前键值特征")
print("=" * 60)
for idx in sorted(keys):
    k = keys[idx]
    a = analyze(k["p"])
    print(f"\nkey{idx} ({k['name']}) n={a['n']}")
    print(f"  head: {a['head']}")
    print(f"  mid : {a['mid']}")
    print(
        f"  glitch<80us={a['glitch_n']} {a['glitch_lt80'][:10]} | "
        f"short80-400={a['short_n']} avg={a['short_avg']:.0f} | "
        f"long400-3500={a['long_n']} avg={a['long_avg']:.0f} | huge={a['huge_n']} {a['huge']}"
    )
    print(
        f"  period L={a['period']} ({a['period_pct']:.0f}%)  "
        f"body L={a['body_period']} ({a['body_period_pct']:.0f}%)  first={a['first']}"
    )

print("\n" + "=" * 60)
print("与备份码对比")
print("=" * 60)
for idx, meta in backup.get("keys", {}).items():
    bi = int(idx)
    bp = meta.get("pulses") or []
    print(f"backup[{bi}] {meta.get('name')} n={meta.get('count')} head={bp[:16]}")
    if bi in keys:
        m, n, pct, det = match(keys[bi]["p"], bp)
        print(f"  vs device key{bi}: mismatches {m}/{n} ({pct:.1f}%) samples={det}")

print("\n" + "=" * 60)
print("键间两两（看是否同一遥控家族/是否学串）")
print("=" * 60)
idxs = sorted(keys)
for i in range(len(idxs)):
    for j in range(i + 1, len(idxs)):
        a, b = keys[idxs[i]]["p"], keys[idxs[j]]["p"]
        m, n, pct, _ = match(a, b)
        print(f"key{idxs[i]} vs key{idxs[j]}: mismatch {m}/{n} ({pct:.1f}%)")

# 对照「干净固定码」启发式（以 backup close 为金标准）
print("\n" + "=" * 60)
print("是否符合 315 固定码特征（启发式）")
print("=" * 60)


def verdict(idx, p, label):
    a = analyze(p)
    scores = []
    ok = True
    # 1 帧长
    if 32 <= a["n"] <= 80:
        scores.append(f"帧长{n if False else a['n']}∈[32,80] OK")
    elif a["n"] >= 24:
        scores.append(f"帧长{a['n']}偏短但可用")
    else:
        ok = False
        scores.append(f"帧长{a['n']}过短 FAIL")
    # 2 毛刺
    if a["glitch_n"] == 0:
        scores.append("无<80us毛刺 OK")
    elif a["glitch_n"] <= 4:
        scores.append(f"少量毛刺{a['glitch_n']}个 可接受")
    else:
        ok = False
        scores.append(f"毛刺过多{a['glitch_n']}个 FAIL")
    # 3 短长双峰
    if a["short_n"] >= 10 and a["long_n"] >= 10:
        scores.append("短/长双峰 OK")
    else:
        ok = False
        scores.append(f"双峰不足 short={a['short_n']} long={a['long_n']} FAIL")
    # 4 周期
    if a["period"] and a["period_pct"] >= 80:
        scores.append(f"有重复周期 L={a['period']} {a['period_pct']:.0f}% OK")
    elif a["body_period"] and a["body_period_pct"] >= 80:
        scores.append(f"中段有周期 L={a['body_period']} {a['body_period_pct']:.0f}% OK")
    else:
        scores.append(f"无清晰周期（单帧也可能） 弱")
    # 5 首脉冲异常
    if a["first"] > 2000:
        scores.append(f"首脉冲{a['first']}us 偏长（像残留间隔） 弱")
    # 6 与金标准脉宽均值接近
    if a["short_avg"] and 100 <= a["short_avg"] <= 280 and a["long_avg"] and 400 <= a["long_avg"] <= 650:
        scores.append(f"短均{a['short_avg']:.0f}/长均{a['long_avg']:.0f} 接近常见315 OK")
    else:
        scores.append(f"短均{a['short_avg']:.0f}/长均{a['long_avg']:.0f} 偏离常见 弱")
    status = "PASS" if ok else "FAIL"
    print(f"\n{label}: {status}")
    for s in scores:
        print(f"  - {s}")
    return ok


verdict(1, keys[1]["p"], "key1 close（旧备份，金标准参考）")
verdict(0, keys[0]["p"], "key0 open（本次学习）")
verdict(2, keys[2]["p"], "key2 stop")

# 给 key0 建议：截掉首脉冲后再看
p0 = keys[0]["p"]
print("\n" + "=" * 60)
print("key0 去掉首脉冲/毛刺后再评估")
print("=" * 60)
clean = [v for v in p0 if v >= 80 and v < 8000]
# 去掉可能的前导长间隔感
if clean and clean[0] > 1500:
    clean = clean[1:]
a = analyze(clean)
print(f"clean n={a['n']} head={clean[:20]}")
print(
    f"glitch={a['glitch_n']} short={a['short_n']} avg={a['short_avg']:.0f} "
    f"long={a['long_n']} avg={a['long_avg']:.0f} period={a['period']} ({a['period_pct']:.0f}%)"
)
m, n, pct, det = match(clean, keys[1]["p"])
print(f"clean vs key1: mismatch {m}/{n} ({pct:.1f}%)")
if 0 in {int(i) for i, *_ in []}:
    pass
mb = match(clean, backup["keys"]["0"]["pulses"])
print(f"clean vs backup0: mismatch {mb[0]}/{mb[1]} ({mb[2]:.1f}%)")
