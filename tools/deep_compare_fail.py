#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare fail causes on dda0 vs 1388 after both on 1435."""
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


def fail_rate(points):
    if len(points) < 2:
        return None
    t0, t1 = parse_ts(points[0][0]), parse_ts(points[-1][0])
    if not t0 or not t1:
        return None
    dt = (t1 - t0).total_seconds()
    if dt <= 0:
        return None
    return (points[-1][1] - points[0][1]) / dt


def analyze(dev, lines_n=600):
    data = fetch_json("/api/devices/%s/logs?lines=%d" % (dev, lines_n))
    text = data.get("text") or ""
    (OUT / ("cmp_%s.json" % dev)).write_text(text, encoding="utf-8")
    lines = text.splitlines()
    print("\n" + "=" * 24, dev, "=" * 24)

    fw = ""
    for L in lines:
        if "[OTA] new" in L or "0.2.2026" in L and ("BOOT" in L or "OTA" in L):
            if "0.2.2026" in L:
                m = re.search(r"0\.2\.2026\d+", L)
                if m:
                    fw = m.group(0)
                    if "[OTA] new" in L or "[BOOT]" in L:
                        print("MK", L[:200])
    print("fw_seen", fw)

    hp = []
    for L in lines:
        if "HEAPPOOL" not in L or "fail=" not in L:
            continue
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        mf = re.search(r"DEF\s+(\d+)/", L)
        if m:
            hp.append((L[:19], int(m.group(1)),
                       int(mb.group(1)) if mb else -1,
                       int(mf.group(1)) if mf else -1))
    print("HEAPPOOL n", len(hp))
    if hp:
        pts = [(t, f) for t, f, _, _ in hp]
        print(" first", hp[0])
        print(" last ", hp[-1])
        print(" fail_rate_per_s", fail_rate(pts))
        print(" maxblk_hist", Counter(x[2] for x in hp).most_common(8))
        print(" last 10 fail/maxblk:")
        for row in hp[-10:]:
            print("  ", row[0], "fail=", row[1], "maxblk=", row[2], "free=", row[3])

    hf = [L for L in lines if "HEAPFAIL" in L]
    hist = Counter()
    big8 = Counter()
    for L in hf:
        m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
        b = re.search(r"big8=(\d+)", L)
        if m:
            hist[(m.group(1), m.group(2))] += 1
        if b:
            big8[int(b.group(1))] += 1
    print("HEAPFAIL n", len(hf), "hist", hist.most_common(10), "big8", big8.most_common(8))
    for L in hf[-12:]:
        print(" HF", L[:220])

    # heartbeat fail= is NFC fail, not heap; heap fail from HEAPPOOL
    nfc = Counter()
    for L in lines:
        if "[LOG]" in L:
            m = re.search(r"nfc=(\w+)", L)
            if m:
                nfc[m.group(1)] += 1
    print("nfc_hist", dict(nfc))

    # last log lines
    for L in lines[-5:]:
        print(" TAIL", L[:240])
    return hp, hf


def main():
    devs = fetch_json("/api/devices")
    print("OTA", (devs.get("ota") or {}).get("version"))
    for d in devs.get("devices") or []:
        print(" ", d.get("id"), d.get("fw"), "online=", d.get("online"),
              "health=", d.get("health"), "ago=", d.get("last_seen_ago_s"))
    analyze("garage-1388")
    analyze("garage-dda0")


if __name__ == "__main__":
    main()
