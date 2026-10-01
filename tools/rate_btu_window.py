#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Rate-check BTU fail for a device after a cutoff time."""
from __future__ import print_function
import re
import sys
from datetime import datetime
from pathlib import Path

BASE = Path("/opt/garage-gate/logs")
DATE = "20261001"
dev = sys.argv[1] if len(sys.argv) > 1 else "garage-1388"
# cutoff default: now-ish boot window start provided as argv[2] "HH:MM:SS"
cutoff_s = sys.argv[2] if len(sys.argv) > 2 else "07:14:00"
window_min = float(sys.argv[3]) if len(sys.argv) > 3 else 15.0

path = BASE / ("device-%s-%s.log" % (dev, DATE))
if not path.exists():
    # try yesterday file too
    path = BASE / ("device-%s-20260930.log" % dev)
print("file", path)
if not path.exists():
    print("MISSING")
    sys.exit(1)

lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
cut = datetime.strptime("2026-10-01 " + cutoff_s, "%Y-%m-%d %H:%M:%S") if DATE == "20261001" else datetime.strptime("2026-09-30 " + cutoff_s, "%Y-%m-%d %H:%M:%S")
end = cut.timestamp() + window_min * 60

def parse_ts(s):
    m = re.search(r"(2026-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", s)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None

print("cut", cut, "window_min", window_min)
print("== BOOT / OTA / reserve markers ==")
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut:
        continue
    if any(k in L for k in ("[BOOT]", "[OTA]", "BTU reserve", "BT air", "BTUFAIL", "version")):
        if "BTSTAT" in L or "HEAPPOOL" in L:
            continue
        print(ts, L[:220])

print("== BTSTAT series in window ==")
series = []
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut or ts.timestamp() > end:
        continue
    if "[BTSTAT]" not in L:
        continue
    m = re.search(r"inq=(\d+) thin=(\d+) btufail=(\d+) fail=(\d+) maxblk=(\d+)", L)
    if m:
        series.append((ts, int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))

if not series:
    print("NO BTSTAT yet")
    sys.exit(0)

t0, t1 = series[0][0], series[-1][0]
dur = (t1 - t0).total_seconds()
print("n", len(series), "t0", t0, "t1", t1, "dur_s", dur)
print("first", series[0])
print("last", series[-1])
print("btufail_delta", series[-1][3] - series[0][3], "thin_delta", series[-1][2] - series[0][2])
if dur > 0:
    rate = (series[-1][3] - series[0][3]) * 600.0 / dur
    print("btufail_rate_per_10min", round(rate, 2), "PASS" if rate <= 2 else "FAIL")
maxblks = [s[5] for s in series]
print("maxblk min/med/max", min(maxblks), sorted(maxblks)[len(maxblks)//2], max(maxblks))
below = sum(1 for x in maxblks if x < 4112)
print("maxblk<4112", below, "/", len(maxblks))
print("== last 10 BTSTAT ==")
for s in series[-10:]:
    print(s[0], "inq", s[1], "thin", s[2], "btufail", s[3], "fail", s[4], "maxblk", s[5])

print("== BTUFAIL lines in window ==")
nfail = 0
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut or ts.timestamp() > end:
        continue
    if "BTUFAIL" in L:
        nfail += 1
        print(ts, L[:220])
print("BTUFAIL_lines", nfail)

print("== thin lines ==")
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut or ts.timestamp() > end:
        continue
    if "BT thin" in L or "BTU reserve" in L:
        print(ts, L[:220])
