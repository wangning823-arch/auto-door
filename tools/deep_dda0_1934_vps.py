#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deep dive dda0 post-1934 and netfail correlation."""
from __future__ import print_function
import re
from collections import Counter
from datetime import datetime
from pathlib import Path

path = Path("/opt/garage-gate/logs/device-garage-dda0-20260930.log")
lines = path.read_text(encoding="utf-8", errors="replace").splitlines()


def parse_ts(s):
    # log may be garbled: "2026-09-30 20:10:25 026-09-30 ..."
    m = re.search(r"(2026-09-30 \d{2}:\d{2}:\d{2})", s)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


cut = datetime(2026, 9, 30, 20, 3, 0)

print("==== markers around 20:03 boot ====")
for L in lines:
    ts = parse_ts(L)
    if not ts or not (datetime(2026, 9, 30, 19, 50, 0) <= ts <= datetime(2026, 9, 30, 20, 12, 0)):
        continue
    if any(k in L for k in [
        "BOOT", "OTA", "BT air", "HEAPFAIL", "netfail", "datagate",
        "HEAPPOOL", "rst=", "auth=", "nfc=", "http=",
    ]):
        print(L[:280])

print("\n==== HEAPPOOL after 20:03 (fail timeline) ====")
hp = []
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut:
        continue
    if "HEAPPOOL" not in L or "fail=" not in L:
        continue
    m = re.search(r"fail=(\d+)", L)
    mb = re.search(r"DEF\s+\d+/(\d+)", L)
    if m:
        hp.append((ts, int(m.group(1)), int(mb.group(1)) if mb else -1, L[:240]))
print("n", len(hp))
for row in hp:
    print(row[0].strftime("%H:%M:%S"), "fail=", row[1], "maxblk=", row[2])

print("\n==== HEAPFAIL after 20:03 ====")
hf_post = []
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut or "HEAPFAIL" not in L:
        continue
    m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
    m2 = re.search(r"HEAPFAIL\s+(\S+)", L)
    typ = m2.group(1) if m2 else "?"
    sz = m.group(1) if m else "?"
    big = re.search(r"big8=(\d+)", L)
    hf_post.append((ts, typ, sz, big.group(1) if big else "?", L[:240]))
print("n", len(hf_post))
print("hist", Counter((x[1], x[2]) for x in hf_post).most_common())
for x in hf_post:
    print(x[0].strftime("%H:%M:%S"), x[1], "sz=", x[2], "big8=", x[3])

print("\n==== netfail timeline ====")
for L in lines:
    if "netfail" in L or "datagate" in L:
        ts = parse_ts(L)
        print((ts.strftime("%H:%M:%S") if ts else "?"), L[:240])

print("\n==== BT air full day dda0 ====")
for L in lines:
    if "BT air" in L:
        print(L[:240])

print("\n==== HEAPFAIL full day hist + first/last per type ====")
hf = []
for L in lines:
    if "HEAPFAIL" not in L:
        continue
    ts = parse_ts(L)
    m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
    m2 = re.search(r"HEAPFAIL\s+(\S+)", L)
    typ = m2.group(1) if m2 else "?"
    sz = m.group(1) if m else "?"
    hf.append((ts, typ, sz, L[:240]))
print("n", len(hf), "hist", Counter((x[1], x[2]) for x in hf).most_common(12))
by_type = {}
for x in hf:
    key = (x[1], x[2])
    by_type.setdefault(key, []).append(x)
for key, lst in by_type.items():
    print("TYPE", key, "n", len(lst))
    print("  first", lst[0][3][:200])
    print("  last ", lst[-1][3][:200])

print("\n==== 1388 BTU vs wifi same window comparison markers ====")
# count 1388 datagate
p1388 = Path("/opt/garage-gate/logs/device-garage-1388-20260930.log")
t1388 = p1388.read_text(encoding="utf-8", errors="replace").splitlines()
print("1388 netfail n", sum(1 for L in t1388 if "netfail" in L or "datagate" in L))
print("dda0 netfail n", sum(1 for L in lines if "netfail" in L or "datagate" in L))
print("1388 HEAPFAIL hist", Counter(
    (re.search(r"sz=(\d+)\s+t=(\S+)", L).group(1) if re.search(r"sz=(\d+)\s+t=(\S+)", L) else "?",
     re.search(r"sz=(\d+)\s+t=(\S+)", L).group(2) if re.search(r"sz=(\d+)\s+t=(\S+)", L) else "?")
    for L in t1388 if "HEAPFAIL" in L
).most_common(8))
print("dda0 HEAPFAIL hist", Counter(
    (re.search(r"sz=(\d+)\s+t=(\S+)", L).group(1) if re.search(r"sz=(\d+)\s+t=(\S+)", L) else "?",
     re.search(r"sz=(\d+)\s+t=(\S+)", L).group(2) if re.search(r"sz=(\d+)\s+t=(\S+)", L) else "?")
    for L in lines if "HEAPFAIL" in L
).most_common(8))

print("\n==== garbled lines dda0 (log_ship broken packets) ====")
garbled = 0
for L in lines:
    # non-standard start or doubled timestamps
    if not L.startswith("---") and not L.startswith("2026-") and not L.startswith(" "):
        garbled += 1
    if re.search(r"2026-09-30 \d{2}:\d{2}:\d{2} 026-09-30", L):
        garbled += 1
print("suspicious/garbled count approx", garbled)
for L in lines:
    if re.search(r"2026-09-30 \d{2}:\d{2}:\d{2} 026-09-30", L) or (
        not L.startswith("---") and not L.startswith("2026-") and "[LOG]" not in L and "[HEAP" not in L and "[WEB]" not in L
    ):
        print(L[:220])
        break
# sample garbled
n = 0
for L in lines:
    if re.search(r"2026-09-30 \d{2}:\d{2}:\d{2} 026-09-30", L) or (not L.startswith("---") and not L.startswith("2026-") and len(L) > 20):
        if "zone=0" in L or "fail=" in L or "bytes=" in L:
            print("G", L[:220])
            n += 1
            if n >= 8:
                break

print("\n==== CLASSIC nfc rssi after 20:03 ====")
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut:
        continue
    if "[CLASSIC]" in L or "[LOG]" in L:
        if "nfc=" in L or "[CLASSIC]" in L:
            print(L[:280])
