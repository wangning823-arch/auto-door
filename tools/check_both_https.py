#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Check 1388 (post-1435) and dda0 (post power-cycle) via HTTPS."""
from __future__ import print_function
import json
import re
import ssl
import urllib.request
from collections import Counter
from datetime import datetime
from pathlib import Path

BASE = "https://door.wzx.homes"
OUT_DIR = Path(r"D:\mimo\车库门自动化\tools")


def fetch(url_path):
    ctx = ssl._create_unverified_context()
    req = urllib.request.Request(BASE + url_path)
    with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def parse_ts(line):
    if not line.startswith("2026-"):
        return None
    try:
        return datetime.strptime(line[:19], "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


def analyze(dev, lines_n=600):
    data = fetch("/api/devices/%s/logs?lines=%d" % (dev, lines_n))
    text = data.get("text") or ""
    path = OUT_DIR / ("logs_%s_snap.json" % dev)
    path.write_text(text, encoding="utf-8")
    lines = text.splitlines()
    print("\n" + "=" * 20, dev, "=" * 20)
    print("lines", len(lines), "saved", path)
    if lines:
        print("first", lines[0][:220])
        print("last", lines[-1][:220])

    print("-- markers --")
    for L in lines:
        if any(k in L for k in ["0.2.2026", "[OTA] new", "[BOOT]", "hold4k",
                                  "given back", "rearmed", "PN532", "nfc=",
                                  "DEAD", "abort", "rst=", "CRASH"]):
            if any(x in L for x in ["0.2.2026", "[OTA] new", "[BOOT]", "hold4k",
                                      "given back", "rearmed", "DEAD", "rst=",
                                      "CRASH", "PN532 ready", "nfc=ok", "nfc=defer"]):
                print(L[:240])

    hp = []
    for L in lines:
        if "HEAPPOOL" not in L or "fail=" not in L:
            continue
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        if m:
            hp.append({
                "ts": L[:19],
                "fail": int(m.group(1)),
                "maxblk": int(mb.group(1)) if mb else -1,
            })
    print("-- HEAPPOOL --")
    print("samples", len(hp))
    if hp:
        print("first", hp[0])
        print("last ", hp[-1])
        print("maxblk hist", Counter(x["maxblk"] for x in hp).most_common(8))
        # rate over whole window
        t0, t1 = parse_ts(hp[0]["ts"]), parse_ts(hp[-1]["ts"])
        if t0 and t1:
            dt = (t1 - t0).total_seconds()
            if dt > 0:
                print("window_dt_s", dt, "fail_rate_per_s",
                      round((hp[-1]["fail"] - hp[0]["fail"]) / dt, 4))
        # last 15 samples
        print("last 15 samples:")
        for row in hp[-15:]:
            print(" ", row["ts"], "fail=", row["fail"], "maxblk=", row["maxblk"])

    print("-- HEAPFAIL --")
    hf = [L for L in lines if "HEAPFAIL" in L]
    hist = Counter()
    for L in hf:
        m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
        if m:
            hist[(m.group(1), m.group(2))] += 1
    print("count", len(hf), "hist", hist.most_common(8))
    for L in hf[-10:]:
        print(L[:220])

    # nfc / classic / log tail
    print("-- NFC/CLASSIC/LOG tail --")
    for L in lines[-20:]:
        print(L[:280])

    # fail counts by nfc state in LOG lines
    nfc_c = Counter()
    seen_vals = []
    rssi_vals = []
    for L in lines:
        if "[LOG]" in L:
            m = re.search(r"nfc=(\w+)", L)
            if m:
                nfc_c[m.group(1)] += 1
            m = re.search(r"rssi=(-?\d+)", L)
            if m:
                rssi_vals.append(int(m.group(1)))
            m = re.search(r"seen=(\d+)", L)
            if m:
                seen_vals.append(int(m.group(1)))
        if "[CLASSIC]" in L:
            m = re.search(r"rssi=(-?\d+)", L)
            if m:
                rssi_vals.append(int(m.group(1)))
    print("nfc hist", dict(nfc_c))
    if rssi_vals:
        print("rssi min/max/n", min(rssi_vals), max(rssi_vals), len(rssi_vals),
              "n<=-127", sum(1 for v in rssi_vals if v <= -127))
    if seen_vals:
        print("seen first/last/delta", seen_vals[0], seen_vals[-1],
              seen_vals[-1] - seen_vals[0])


def main():
    devs = fetch("/api/devices")
    print("==== devices ====")
    for d in devs.get("devices") or []:
        print(d.get("id"), "fw=", d.get("fw"), "online=", d.get("online"),
              "health=", d.get("health"), "ago=", d.get("last_seen_ago_s"),
              "ota=", (devs.get("ota") or {}).get("version"))
    analyze("garage-1388", 700)
    analyze("garage-dda0", 700)


if __name__ == "__main__":
    main()
