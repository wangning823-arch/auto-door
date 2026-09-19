# -*- coding: utf-8 -*-
import json
from pathlib import Path

CAP = Path(r"C:\Users\goldg\Pictures\rf_capture_20260919_120130.json")
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


def sml(p):
    out = []
    for v in p:
        if v >= 4000:
            out.append("G")
        elif v < 40:
            out.append(".")
        elif v < 250:
            out.append("S")
        elif v < 400:
            out.append("M")
        else:
            out.append("L")
    return "".join(out)


keys = json.loads(KEYS.read_text(encoding="utf-8"))
raw0 = keys["keys"]["0"]["pulses"]
key0 = [x for x in raw0 if x >= 20]
caps = json.loads(CAP.read_text(encoding="utf-8"))

print("key0", len(key0), key0)
print("key0 SML", sml(key0))

pl = caps[0]["pulses"]
print("cap0 full", len(pl))
print("cap0 SML", sml(pl)[:120])

# extract each ~period segment after long gaps
print("\nsegments after gaps>=4000:")
segs = []
cur = []
for v in pl:
    if v >= 4000:
        if len(cur) >= 20:
            segs.append(cur)
        cur = []
    else:
        cur.append(v)
if len(cur) >= 20:
    segs.append(cur)
print(f"n_segs={len(segs)} lens={[len(s) for s in segs]}")

for i, s in enumerate(segs[:4]):
    fl = detect_frame_len(s)
    frame = s[:fl] if 16 <= fl <= 240 else s[:60]
    # match key0 with all offsets
    best = (0, 0, 0)
    for off in range(0, max(1, len(frame) - 30)):
        n = min(len(key0), len(frame) - off, 49)
        if n < 30:
            break
        good = sum(
            1
            for k in range(n)
            if abs(key0[k] - frame[off + k])
            <= max(max(key0[k], frame[off + k]) // 4, 80)
        )
        if good > best[0]:
            best = (good, off, n)
    print(
        f"seg{i}: len={len(s)} fl~{fl} best={best[0]}/{best[2]} "
        f"({100*best[0]/best[2] if best[2] else 0:.0f}%) off={best[1]}"
    )
    print(f"  head={frame[:20]}")
    print(f"  SML={sml(frame)[:60]}")
    if best[2]:
        off = best[1]
        print(f"  key  ={key0[:20]}")
        print(f"  align={frame[off:off+20]}")
