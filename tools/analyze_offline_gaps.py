#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Analyze dda0 offline gaps and 1388 1913 behavior."""
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


def analyze_offline(dev, lines_n=800):
    data = fetch_json("/api/devices/%s/logs?lines=%d" % (dev, lines_n))
    text = data.get("text") or ""
    (OUT / ("offline_%s.json" % dev)).write_text(text, encoding="utf-8")
    lines = text.splitlines()
    print("\n" + "=" * 22, dev, "=" * 22)

    # device-time lines with timestamps
    ts_lines = []
    for L in lines:
        ts = parse_ts(L)
        if ts:
            ts_lines.append((ts, L))
    print("timestamped_lines", len(ts_lines))
    if ts_lines:
        print("span", ts_lines[0][0], "->", ts_lines[-1][0])

    # gaps between consecutive device-time events (>20s)
    print("-- time gaps >20s --")
    gaps = []
    for i in range(1, len(ts_lines)):
        dt = (ts_lines[i][0] - ts_lines[i - 1][0]).total_seconds()
        if dt >= 20:
            gaps.append((ts_lines[i - 1][0], ts_lines[i][0], dt, ts_lines[i - 1][1][:80], ts_lines[i][1][:80]))
    print("gap_count", len(gaps))
    for g in gaps[-15:]:
        print("  gap", g[0], "->", g[1], "dt=", g[2], "s")
        print("    before", g[3])
        print("    after ", g[4])

    # rx intervals (server receive)
    rx = []
    for L in lines:
        if L.startswith("--- rx"):
            m = re.search(r"--- rx (\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})", L)
            if m:
                rx.append(parse_ts(m.group(1)))
    rx = [t for t in rx if t]
    print("-- rx receive intervals --")
    if len(rx) >= 2:
        dts = [(rx[i] - rx[i - 1]).total_seconds() for i in range(1, len(rx))]
        print("rx_n", len(rx), "min/max/avg dt",
              min(dts), max(dts), int(sum(dts) / len(dts)))
        big = [(rx[i], dts[i]) for i in range(len(dts)) if dts[i] >= 25]
        print("rx_gaps>=25s", len(big))
        for t, dt in big[-10:]:
            print("  rx_gap", t, "dt=", dt)

    # netfail / reconnect / log_ship markers
    print("-- netfail / BOOT / OTA / BT air --")
    for L in lines:
        if any(k in L for k in ["netfail", "[BOOT]", "[OTA] new", "BT air",
                                  "datagate", "CRASH", "rst="]):
            if any(x in L for x in ["netfail", "[BOOT]", "[OTA] new", "BT air",
                                     "datagate", "CRASH"]):
                print(L[:230])

    # fail series last
    hp = []
    for L in lines:
        if "HEAPPOOL" in L and "fail=" in L:
            m = re.search(r"fail=(\d+)", L)
            mb = re.search(r"DEF\s+\d+/(\d+)", L)
            if m:
                hp.append((L[:19], int(m.group(1)), int(mb.group(1)) if mb else -1))
    print("-- HEAPPOOL last 8 --")
    for row in hp[-8:]:
        print(" ", row)
    if hp and len(hp) >= 2:
        t0, t1 = parse_ts(hp[0][0]), parse_ts(hp[-1][0])
        dt = (t1 - t0).total_seconds() if t0 and t1 else 0
        if dt > 0:
            print("fail_rate/s", round((hp[-1][1] - hp[0][1]) / dt, 4), "dt", dt)

    # LOG lines timeline density
    logs = [t for t, L in ts_lines if "[LOG]" in L]
    if len(logs) >= 2:
        log_dts = [(logs[i] - logs[i - 1]).total_seconds() for i in range(1, len(logs))]
        print("-- [LOG] intervals --")
        print("n", len(logs), "min/max/avg", min(log_dts), max(log_dts), int(sum(log_dts) / len(log_dts)))
        big = [(logs[i], log_dts[i]) for i in range(len(log_dts)) if log_dts[i] >= 25]
        print("log_gaps>=25s", len(big))
        for t, dt in big[-8:]:
            print("  log_gap", t, "dt=", dt)

    print("-- tail --")
    for L in lines[-6:]:
        print(L[:240])


def main():
    devs = fetch_json("/api/devices")
    print("OTA", (devs.get("ota") or {}).get("version"))
    for d in devs.get("devices") or []:
        print(" ", d.get("id"), d.get("fw"), "online=", d.get("online"),
              "ago=", d.get("last_seen_ago_s"), "health=", d.get("health"))
    analyze_offline("garage-dda0", 800)
    analyze_offline("garage-1388", 400)


if __name__ == "__main__":
    main()
