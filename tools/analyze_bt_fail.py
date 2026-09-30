#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Correlate classic BT rssi=-127 windows with heap fail storms."""
from __future__ import print_function
import re
from io import open
from pathlib import Path


def load(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").splitlines()


def analyze(path, label, windows):
    lines = load(path)
    print("=" * 20, label, "=" * 20)
    classic = []
    for L in lines:
        if "[CLASSIC]" in L:
            classic.append(L)
    print("classic_lines", len(classic))
    # sample rssi and fail timeline per window
    for name, start, end in windows:
        print("\n--- window", name, start, "->", end, "---")
        cls = [L for L in classic if start <= L[:19] <= end]
        hp = [L for L in lines if "HEAPPOOL" in L and start <= L[:19] <= end]
        hf = [L for L in lines if "HEAPFAIL" in L and start <= L[:19] <= end]
        log = [L for L in lines if "[LOG]" in L and start <= L[:19] <= end]
        print("classic", len(cls), "heappool", len(hp), "heapfail", len(hf), "log", len(log))
        # rssi values
        vals = []
        for L in cls:
            m = re.search(r"rssi=(-?\d+)", L)
            if m:
                vals.append(int(m.group(1)))
        if vals:
            print("rssi min/max/n", min(vals), max(vals), len(vals))
            n127 = sum(1 for v in vals if v <= -127)
            print("rssi<=-127 count", n127)
        # fail series
        fails = []
        for L in hp:
            m = re.search(r"fail=(\d+)", L)
            mb = re.search(r"maxblk=(\d+)|/(\d+)\s", L)
            if m:
                fails.append((L[:19], int(m.group(1))))
            # extract largest free from HEAPPOOL format DEF x/y
        if fails:
            print("fail first/last", fails[0], fails[-1])
            if len(fails) >= 2:
                dt = 0
                try:
                    from datetime import datetime
                    t0 = datetime.strptime(fails[0][0], "%Y-%m-%d %H:%M:%S")
                    t1 = datetime.strptime(fails[-1][0], "%Y-%m-%d %H:%M:%S")
                    dt = (t1 - t0).total_seconds()
                except Exception:
                    dt = 0
                if dt > 0:
                    rate = (fails[-1][1] - fails[0][1]) / dt
                    print("fail_delta", fails[-1][1] - fails[0][1], "rate_per_s", round(rate, 3))
        # maxblk samples
        maxblks = []
        for L in hp:
            m = re.search(r"DEF\s+\d+/(\d+)", L)
            if m:
                maxblks.append(int(m.group(1)))
        if maxblks:
            print("maxblk min/max", min(maxblks), max(maxblks))
        # last 8 classic / log
        for L in cls[-6:]:
            print("C", L[:220])
        for L in log[-4:]:
            print("L", L[:240])
        for L in hf[-8:]:
            print("F", L[:220])


def find_long_127_gaps(path, label):
    """Find consecutive classic rssi<=-127 or missing classic samples gaps."""
    lines = load(path)
    classic = [L for L in lines if "[CLASSIC]" in L]
    print("\n====", label, "classic rssi<=-127 runs ====")
    from datetime import datetime
    runs = []
    cur = None
    for L in classic:
        m = re.search(r"rssi=(-?\d+)", L)
        if not m:
            continue
        r = int(m.group(1))
        ts = None
        try:
            ts = datetime.strptime(L[:19], "%Y-%m-%d %H:%M:%S")
        except Exception:
            pass
        bad = r <= -127
        if bad:
            if cur is None:
                cur = {"start": ts, "end": ts, "n": 0, "minr": r}
            else:
                cur["end"] = ts
            cur["n"] += 1
            cur["minr"] = min(cur["minr"], r)
        else:
            if cur is not None:
                runs.append(cur)
                cur = None
    if cur:
        runs.append(cur)
    runs = [r for r in runs if r["n"] >= 2]
    for r in runs[-15:]:
        dur = None
        if r["start"] and r["end"]:
            dur = (r["end"] - r["start"]).total_seconds()
        print("run n=", r["n"], "from", r["start"], "to", r["end"], "dur_s", dur)


base = Path("/opt/garage-gate/logs")
# yesterday and today
for fname, label in [
    ("device-garage-1388-20260929.log", "1388-29"),
    ("device-garage-1388-20260930.log", "1388-30"),
    ("device-garage-dda0-20260929.log", "dda0-29"),
    ("device-garage-dda0-20260930.log", "dda0-30"),
]:
    p = base / fname
    if not p.exists():
        print("missing", p)
        continue
    find_long_127_gaps(p, label)

# focused windows: yesterday evening / any high fail periods
analyze(
    base / "device-garage-1388-20260929.log",
    "1388 yesterday samples",
    [
        ("day", "2026-09-29 00:00:00", "2026-09-29 23:59:59"),
    ],
)
analyze(
    base / "device-garage-1388-20260930.log",
    "1388 today post-upgrade",
    [
        ("post-ota", "2026-09-30 10:40:25", "2026-09-30 11:45:00"),
        ("pre-ota", "2026-09-30 04:00:00", "2026-09-30 04:55:00"),
    ],
)
analyze(
    base / "device-garage-dda0-20260930.log",
    "dda0 today",
    [
        ("today", "2026-09-30 00:00:00", "2026-09-30 11:45:00"),
    ],
)
