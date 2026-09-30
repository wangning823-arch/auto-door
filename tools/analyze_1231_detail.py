#!/usr/bin/env python
# -*- coding: utf-8 -*-
from __future__ import print_function
from io import open
from pathlib import Path
import re


def load(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").splitlines()


lines = load("/opt/garage-gate/logs/device-garage-1388-20260930.log")

print("==== around 12:36-12:38 BOOT/rst/panic/heap ====")
for L in lines:
    if not L.startswith("2026-09-30 12:3"):
        continue
    if any(k in L for k in ["BOOT", "rst=", "panic", "GURU", "HEAPFAIL", "hold4k", "given back",
                              "rearmed", "0.2.2026", "[CLASSIC]", "[LOG]", "[NFC]", "inquiry"]):
        if L.startswith("2026-09-30 12:36") or L.startswith("2026-09-30 12:37") or L.startswith("2026-09-30 12:38"):
            print(L[:300])

print("\n==== fail rate by 30s after 12:37:05 ====")
hp = []
for L in lines:
    if "HEAPPOOL" in L and L[:16] >= "2026-09-30 12:37":
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        if m:
            hp.append((L[:19], int(m.group(1)), int(mb.group(1)) if mb else -1))
print("n", len(hp))
if hp:
    print("first", hp[0], "last", hp[-1])
    # group by 30s
    buckets = {}
    for ts, fail, mb in hp:
        key = ts[:16] + str(int(ts[17:19]) // 30 * 30).zfill(2)
        buckets.setdefault(key, []).append((fail, mb))
    for k in sorted(buckets):
        vals = buckets[k]
        print(k, "fail", vals[0][0], "->", vals[-1][0], "maxblk", vals[-1][1])

print("\n==== maxblk histogram after 12:37 ====")
from collections import Counter
c = Counter()
for L in lines:
    if "HEAPPOOL" in L and L[:16] >= "2026-09-30 12:37":
        m = re.search(r"DEF\s+\d+/(\d+)", L)
        if m:
            c[m.group(1)] += 1
print(c.most_common(10))

print("\n==== last 20 lines ====")
for L in lines[-20:]:
    print(L[:280])
