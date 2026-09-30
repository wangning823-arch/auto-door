#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare 1388 fail rate: 1844 vs 1913."""
from __future__ import print_function
import json
import re
import ssl
import urllib.request
from collections import Counter
from datetime import datetime
from pathlib import Path

BASE = "https://door.wzx.homes"
OUT = Path(r"D:\mimo\车库门自动化\tools")


def fetch_json(path):
    ctx = ssl._create_unverified_context()
    req = urllib.request.Request(BASE + path)
    with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def parse_ts(s):
    try:
        return datetime.strptime(s[:19], "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


def rate(points):
    if len(points) < 2:
        return None
    t0, t1 = parse_ts(points[0][0]), parse_ts(points[-1][0])
    if not t0 or not t1:
        return None
    dt = (t1 - t0).total_seconds()
    if dt <= 0:
        return None
    return (points[-1][1] - points[0][1]) / dt


def main():
    data = fetch_json("/api/devices/garage-1388/logs?lines=1200")
    text = data.get("text") or ""
    (OUT / "cmp1388_fw.json").write_text(text, encoding="utf-8")
    lines = text.splitlines()

    boots = []
    for L in lines:
        if "[OTA] new" in L or ("[BOOT] rst" in L and "2026-09-30" in L):
            if "[OTA] new" in L or "[BOOT] rst" in L:
                print("MK", L[:220])
                ts = parse_ts(L)
                if ts:
                    boots.append((ts, L[:120]))

    hp = []
    hf = []
    for L in lines:
        if "HEAPPOOL" in L and "fail=" in L:
            m = re.search(r"fail=(\d+)", L)
            mb = re.search(r"DEF\s+\d+/(\d+)", L)
            if m:
                hp.append((L[:19], int(m.group(1)), int(mb.group(1)) if mb else -1))
        if "HEAPFAIL" in L:
            hf.append(L)

    print("HEAPPOOL n", len(hp))
    if hp:
        print("first", hp[0], "last", hp[-1])
        print("overall_rate", rate([(t, f) for t, f, _ in hp]))

    # split by OTA 1913 boot ~19:19:53
    cut = parse_ts("2026-09-30 19:19:53")
    pre = [r for r in hp if parse_ts(r[0]) and parse_ts(r[0]) < cut]
    post = [r for r in hp if parse_ts(r[0]) and parse_ts(r[0]) >= cut]
    print("\n==== pre-1913 (mostly 1844 window) ====")
    print("n", len(pre))
    if pre:
        print("first", pre[0], "last", pre[-1])
        print("rate/s", rate([(t, f) for t, f, _ in pre]))
        print("maxblk hist", Counter(x[2] for x in pre).most_common(8))
        # last 10 pre
        for row in pre[-8:]:
            print(" ", row)
    print("\n==== post-1913 ====")
    print("n", len(post))
    if post:
        print("first", post[0], "last", post[-1])
        print("rate/s", rate([(t, f) for t, f, _ in post]))
        print("maxblk hist", Counter(x[2] for x in post).most_common(8))
        for row in post[-12:]:
            print(" ", row)

    # HEAPFAIL split
    print("\n==== HEAPFAIL pre/post ====")
    pre_hf = [L for L in hf if parse_ts(L[:19]) and parse_ts(L[:19]) < cut]
    post_hf = [L for L in hf if parse_ts(L[:19]) and parse_ts(L[:19]) >= cut]
    def hist(hf_list):
        c = Counter()
        for L in hf_list:
            m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
            if m:
                c[(m.group(1), m.group(2))] += 1
        return c
    print("pre n", len(pre_hf), hist(pre_hf))
    print("post n", len(post_hf), hist(post_hf))
    print("post HEAPFAIL samples:")
    for L in post_hf[-15:]:
        print(" ", L[:220])

    # BT air
    print("\n==== BT air ====")
    air = [L for L in lines if "BT air" in L]
    print("air_lines", len(air))
    for L in air[-20:]:
        print(L[:220])

    # fail per 60s buckets post 1913
    print("\n==== post-1913 fail by 60s bucket ====")
    buckets = {}
    for t, f, mb in post:
        ts = parse_ts(t)
        if not ts:
            continue
        key = ts.replace(second=0, microsecond=0)
        # 60s bucket
        key = datetime(ts.year, ts.month, ts.day, ts.hour, ts.minute // 60 * 60)
        buckets.setdefault(key, []).append((t, f, mb))
    for k in sorted(buckets):
        rows = buckets[k]
        print(k.strftime("%H:%M"), "fail", rows[0][1], "->", rows[-1][1],
              "maxblk_last", rows[-1][2],
              "delta", rows[-1][1] - rows[0][1])


if __name__ == "__main__":
    main()
