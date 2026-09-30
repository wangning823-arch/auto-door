#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Deep BTU analysis: dda0 vs 1388 after latest boot, BT air, HEAPFAIL, rate."""
from __future__ import print_function
import re
from collections import Counter
from datetime import datetime
from pathlib import Path

BASE = Path("/opt/garage-gate/logs")
DATE = "20260930"
files = {
    "dda0": BASE / ("device-garage-dda0-%s.log" % DATE),
    "1388": BASE / ("device-garage-1388-%s.log" % DATE),
}


def parse_ts(s):
    m = re.search(r"(2026-09-30 \d{2}:\d{2}:\d{2})", s)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


def after_boot(lines, last_boot):
    out = []
    for L in lines:
        ts = parse_ts(L)
        if ts and ts >= last_boot:
            out.append(L)
    return out


def analyze(name, path, boot_hint="23:3"):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    print("=" * 70)
    print(name, "lines", len(lines), "file", path)

    boots = []
    for L in lines:
        if "[BOOT]" in L and "rst=" in L:
            boots.append((parse_ts(L), L[:200]))
    print("-- BOOTs --")
    for ts, L in boots[-10:]:
        print(ts, L)

    last_boot = None
    for ts, _ in boots:
        if ts and ts.strftime("%H:%M")[:4] == boot_hint:
            last_boot = ts
    if last_boot is None and boots:
        last_boot = boots[-1][0]
    print("last_boot", last_boot)
    if not last_boot:
        return

    post = after_boot(lines, last_boot)
    print("post_boot_lines", len(post))

    print("-- BT air --")
    air = [L for L in post if "BT air" in L]
    print("n", len(air))
    for L in air[:25]:
        print(" ", L[:220])
    if len(air) > 25:
        print(" ...")
        for L in air[-10:]:
            print(" ", L[:220])

    print("-- thin / BTUFAIL / reserve --")
    for L in post:
        if "BT thin" in L or "BTUFAIL" in L or "reserve" in L or "hold4k" in L or "BT air" in L:
            if "BT air" in L:
                continue
            print(L[:240])

    print("-- BTSTAT series --")
    series = []
    for L in post:
        if "[BTSTAT]" not in L:
            continue
        m = re.search(
            r"inq=(\d+) thin=(\d+) btufail=(\d+) fail=(\d+) maxblk=(\d+)", L
        )
        if m:
            series.append(
                (
                    parse_ts(L),
                    int(m.group(1)),
                    int(m.group(2)),
                    int(m.group(3)),
                    int(m.group(4)),
                    int(m.group(5)),
                )
            )
    if series:
        t0, t1 = series[0][0], series[-1][0]
        dur = (t1 - t0).total_seconds() if t0 and t1 else 0
        print(
            "series n",
            len(series),
            "t0",
            t0,
            "t1",
            t1,
            "dur_s",
            dur,
            "btufail",
            series[-1][3],
            "thin",
            series[-1][2],
            "inq",
            series[-1][1],
            "last_maxblk",
            series[-1][5],
        )
        maxblks = [s[5] for s in series]
        print(
            "maxblk min/med/max",
            min(maxblks),
            sorted(maxblks)[len(maxblks) // 2],
            max(maxblks),
        )
        below = sum(1 for x in maxblks if x < 4112)
        print("maxblk<4112 ratio %d/%d" % (below, len(maxblks)))
        if dur > 0:
            print("btufail_rate_per_10min", round(series[-1][3] * 600.0 / dur, 2))
        step = max(1, len(series) // 15)
        for s in series[::step]:
            print(
                " ",
                s[0],
                "inq",
                s[1],
                "thin",
                s[2],
                "btufail",
                s[3],
                "fail",
                s[4],
                "maxblk",
                s[5],
            )
        print(" ... last 8 ...")
        for s in series[-8:]:
            print(
                " ",
                s[0],
                "inq",
                s[1],
                "thin",
                s[2],
                "btufail",
                s[3],
                "fail",
                s[4],
                "maxblk",
                s[5],
            )

    print("-- HEAPFAIL post-boot by task/size --")
    hf = []
    for L in post:
        if "HEAPFAIL" not in L:
            continue
        m = re.search(r"sz=(\d+)\s+t=(\S+).*big8=(\d+)", L)
        if m:
            hf.append((int(m.group(1)), m.group(2), int(m.group(3)), L[:220]))
    print("n", len(hf))
    print(Counter((sz, t) for sz, t, _, _ in hf).most_common(20))
    for row in hf[-15:]:
        print(row[0], row[1], "big8=", row[2])

    print("-- HEAPPOOL post --")
    mx = []
    for L in post:
        if "HEAPPOOL" not in L:
            continue
        mb = re.search(r"8BIT\s+\d+/(\d+)", L)
        mf = re.search(r"fail=(\d+)", L)
        if mb:
            mx.append((parse_ts(L), int(mb.group(1)), int(mf.group(1)) if mf else -1))
    if mx:
        print("n", len(mx))
        print("min", min(x[1] for x in mx), "max", max(x[1] for x in mx))
        below = sum(1 for x in mx if x[1] < 4112)
        print("HEAPPOOL maxblk<4112 %d/%d" % (below, len(mx)))
        for row in mx[-15:]:
            print(row[0], "max8", row[1], "fail", row[2])

    print("-- markers --")
    for key in ("netfail", "datagate", "[OTA]", "hold4k", "reserve"):
        c = sum(1 for L in post if key in L)
        if c:
            print(key, c)
    for L in post:
        if "netfail" in L or "datagate" in L or "[OTA]" in L:
            print(L[:220])


for name, path in files.items():
    if path.exists():
        analyze(name, path)
    else:
        print("MISSING", path)
