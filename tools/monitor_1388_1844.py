#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Monitor 1388 fail/BT air after OTA 1844 for ~10 minutes."""
from __future__ import print_function
import json
import re
import ssl
import time
import urllib.request
from collections import Counter
from datetime import datetime
from pathlib import Path

BASE = "https://door.wzx.homes"
OUT = Path(r"D:\mimo\车库门自动化\tools")
DURATION_S = 600
INTERVAL_S = 45


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


def sample(dev):
    data = fetch_json("/api/devices/%s/logs?lines=400" % dev)
    text = data.get("text") or ""
    lines = text.splitlines()
    hp = []
    for L in lines:
        if "HEAPPOOL" not in L or "fail=" not in L:
            continue
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        if m:
            hp.append((L[:19], int(m.group(1)), int(mb.group(1)) if mb else -1))
    air = [L for L in lines if "BT air" in L or "BTU" in L or "HEAPFAIL" in L or "[OTA] new" in L or "[BOOT] rst" in L]
    nfc = Counter()
    for L in lines:
        if "[LOG]" in L:
            m = re.search(r"nfc=(\w+)", L)
            if m:
                nfc[m.group(1)] += 1
    inquiry = sum(1 for L in lines if "auto inquiry" in L or "[BT] auto" in L)
    return {
        "lines": len(lines),
        "hp": hp,
        "air": air[-20:],
        "nfc": dict(nfc),
        "inquiry_lines": inquiry,
        "last_hp": hp[-1] if hp else None,
        "fail_rate": rate([(t, f) for t, f, _ in hp]),
        "maxblk_hist": Counter(x[2] for x in hp).most_common(6),
        "hf4112": sum(1 for L in lines if "HEAPFAIL" in L and "4112" in L),
        "hf_total": sum(1 for L in lines if "HEAPFAIL" in L),
        "fw": None,
        "tail": lines[-3:] if lines else [],
    }


def main():
    print("=== monitor 1388 1844 start", datetime.now().strftime("%H:%M:%S"), "===")
    devs = fetch_json("/api/devices")
    for d in devs.get("devices") or []:
        print("dev", d.get("id"), d.get("fw"), "online=", d.get("online"),
              "ago=", d.get("last_seen_ago_s"), "health=", d.get("health"))
    print("ota", (devs.get("ota") or {}).get("version"))

    samples = []
    t_end = time.time() + DURATION_S
    n = 0
    while time.time() < t_end:
        n += 1
        s = sample("garage-1388")
        samples.append(s)
        print("\n--- sample", n,
              datetime.now().strftime("%H:%M:%S"),
              "last_hp=", s["last_hp"],
              "rate/s=", None if s["fail_rate"] is None else round(s["fail_rate"], 4),
              "maxblk=", s["maxblk_hist"],
              "hf=", s["hf_total"], "hf4112=", s["hf4112"],
              "nfc=", s["nfc"],
              "inquiry_lines=", s["inquiry_lines"])
        for L in s["air"]:
            print(" ", L[:220])
        for L in s["tail"]:
            print(" T", L[:200])
        remaining = t_end - time.time()
        if remaining <= 0:
            break
        time.sleep(min(INTERVAL_S, remaining))

    print("\n========== SUMMARY ==========")
    print("samples", len(samples))
    # overall fail trend using first/last hp from concatenated analysis
    all_hp = []
    for s in samples:
        all_hp.extend(s["hp"])
    # unique by ts
    seen = set()
    uniq = []
    for row in all_hp:
        if row[0] not in seen:
            seen.add(row[0])
            uniq.append(row)
    uniq.sort(key=lambda x: x[0])
    if uniq:
        print("fail first", uniq[0], "last", uniq[-1])
        print("fail_rate_overall/s", rate([(t, f) for t, f, _ in uniq]))
        print("maxblk hist", Counter(x[2] for x in uniq).most_common(8))
        print("last 8 fail samples:")
        for row in uniq[-8:]:
            print(" ", row)
    # BT air events
    print("BT air / OTA / HEAPFAIL markers across samples:")
    for s in samples:
        for L in s["air"]:
            if "BT air" in L or "HEAPFAIL" in L or "[OTA] new" in L or "BOOT rst" in L:
                print(L[:240])
    print("monitor end", datetime.now().strftime("%H:%M:%S"))


if __name__ == "__main__":
    main()
