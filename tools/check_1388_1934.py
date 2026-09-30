#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Focused check of 1388 after OTA 1934."""
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
    with urllib.request.urlopen(req, context=ctx, timeout=25) as resp:
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
    devs = fetch_json("/api/devices")
    print("OTA", (devs.get("ota") or {}).get("version"))
    for d in devs.get("devices") or []:
        print(d.get("id"), d.get("fw"), "online=", d.get("online"),
              "ago=", d.get("last_seen_ago_s"), "health=", d.get("health"))

    data = fetch_json("/api/devices/garage-1388/logs?lines=600")
    text = data.get("text") or ""
    (OUT / "cmp1388_fw.json").write_text(text, encoding="utf-8")
    lines = text.splitlines()

    for L in lines:
        if "[OTA] new" in L or "[BOOT] rst" in L or "BT air" in L:
            if "[OTA] new" in L or "[BOOT] rst" in L or "held force" in L:
                print("MK", L[:220])

    hp = []
    for L in lines:
        if "HEAPPOOL" in L and "fail=" in L:
            m = re.search(r"fail=(\d+)", L)
            mb = re.search(r"DEF\s+\d+/(\d+)", L)
            if m:
                hp.append((L[:19], int(m.group(1)), int(mb.group(1)) if mb else -1))

    cut = parse_ts("2026-09-30 19:34:00")  # approx 1934; refine from log
    # find 1934 boot
    boot1934 = None
    for L in lines:
        if "0.2.202609301934" in L and ("OTA" in L or "BOOT" in L):
            print("FW1934", L[:200])
            ts = parse_ts(L)
            if ts and boot1934 is None:
                boot1934 = ts
        if "BOOT" in L and "rst=3" in L and parse_ts(L):
            ts = parse_ts(L)
            if ts and (boot1934 is None or ts >= boot1934):
                # keep last boot after1934 mention
                if "0.2.202609301934" in "".join(lines[max(0, lines.index(L)-5):lines.index(L)+1]):
                    boot1934 = ts

    # simpler: last OTA new in file
    last_ota = None
    for L in lines:
        if "[OTA] new" in L:
            print("OTA", L[:220])
            ts = parse_ts(L)
            if ts:
                last_ota = ts
    print("last_ota", last_ota)

    if last_ota:
        cut = last_ota
    post = [r for r in hp if parse_ts(r[0]) and parse_ts(r[0]) >= cut]
    print("HEAPPOOL n_total", len(hp), "post", len(post))
    if post:
        print("post first", post[0], "last", post[-1])
        print("post rate/s", rate([(t, f) for t, f, _ in post]))
        print("post maxblk hist", Counter(x[2] for x in post).most_common(8))
        for row in post[-10:]:
            print(" ", row)

    hf_post = [L for L in lines if "HEAPFAIL" in L and parse_ts(L[:19]) and parse_ts(L[:19]) >= cut]
    hist = Counter()
    for L in hf_post:
        m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
        if m:
            hist[(m.group(1), m.group(2))] += 1
    print("HEAPFAIL post n", len(hf_post), hist.most_common(8))
    for L in hf_post[-8:]:
        print(" HF", L[:200])

    air = [L for L in lines if "BT air" in L and parse_ts(L[:19]) and parse_ts(L[:19]) >= cut]
    print("BT air post n", len(air))
    for L in air[:25]:
        print(" ", L[:200])
    if len(air) > 25:
        print(" ...")
        for L in air[-10:]:
            print(" ", L[:200])

    # rssi/nfc tail
    for L in lines[-5:]:
        print("T", L[:240])


if __name__ == "__main__":
    main()
