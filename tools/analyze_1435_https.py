#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Analyze 1388 logs after OTA 0.2.202609301435 via HTTPS."""
from __future__ import print_function
import json
import re
import ssl
import urllib.request
from collections import Counter
from datetime import datetime
from pathlib import Path

BASE = "https://door.wzx.homes"
OUT = Path(r"D:\mimo\车库门自动化\tools\logs_1388_1435.json")


def fetch(dev_id, lines=800):
    ctx = ssl._create_unverified_context()
    url = "%s/api/devices/%s/logs?lines=%d" % (BASE, dev_id, lines)
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, context=ctx, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8", "replace"))
    return data.get("text") or ""


def fetch_devices():
    ctx = ssl._create_unverified_context()
    req = urllib.request.Request(BASE + "/api/devices")
    with urllib.request.urlopen(req, context=ctx, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def parse_ts(line):
    if not line.startswith("2026-"):
        return None
    try:
        return datetime.strptime(line[:19], "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


def main():
    devs = fetch_devices()
    print("==== devices ====")
    for d in devs.get("devices") or []:
        print(d.get("id"), "fw=", d.get("fw"), "online=", d.get("online"),
              "health=", d.get("health"), "last_seen_ago=", d.get("last_seen_ago_s"))

    text = fetch("garage-1388", 800)
    OUT.write_text(text, encoding="utf-8")
    lines = text.splitlines()
    print("\n1388 lines", len(lines), "saved", OUT)
    if lines:
        print("first", lines[0][:220])
        print("last", lines[-1][:220])

    print("\n==== version / BOOT / hold / give / rst ====")
    for L in lines:
        if any(k in L for k in ["0.2.2026", "hold4k", "given back", "rearmed",
                                  "[BOOT]", "rst=", "reserve"]):
            print(L[:240])

    print("\n==== HEAPPOOL fail / maxblk series ====")
    hp = []
    for L in lines:
        if "HEAPPOOL" not in L:
            continue
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        mf = re.search(r"DEF\s+(\d+)/", L)
        if m:
            hp.append({
                "ts": L[:19],
                "fail": int(m.group(1)),
                "maxblk": int(mb.group(1)) if mb else -1,
                "def_free": int(mf.group(1)) if mf else -1,
                "line": L[:240],
            })
    print("samples", len(hp))
    if hp:
        print("first", hp[0]["ts"], "fail=", hp[0]["fail"], "maxblk=", hp[0]["maxblk"])
        print("last ", hp[-1]["ts"], "fail=", hp[-1]["fail"], "maxblk=", hp[-1]["maxblk"])
        if len(hp) >= 2:
            t0 = parse_ts(hp[0]["ts"])
            t1 = parse_ts(hp[-1]["ts"])
            dt = (t1 - t0).total_seconds() if t0 and t1 else 0
            dfail = hp[-1]["fail"] - hp[0]["fail"]
            print("dt_s", dt, "fail_delta", dfail,
                  "fail_per_s", round(dfail / dt, 4) if dt > 0 else None)
        print("maxblk hist", Counter(x["maxblk"] for x in hp).most_common(10))
        # print every ~N
        step = max(1, len(hp) // 30)
        for row in hp[::step]:
            print(" ", row["ts"], "fail=", row["fail"], "maxblk=", row["maxblk"], "def_free=", row["def_free"])

    print("\n==== HEAPFAIL ====")
    hf = [L for L in lines if "HEAPFAIL" in L]
    print("count", len(hf))
    hist = Counter()
    for L in hf:
        m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
        if m:
            hist[(m.group(1), m.group(2))] += 1
    print("hist", hist.most_common(10))
    for L in hf[-20:]:
        print(L[:240])

    print("\n==== CLASSIC / LOG tail ====")
    for L in lines[-25:]:
        print(L[:280])

    # post-upgrade fail rate if we can find upgrade boot
    upgrade_ts = None
    for L in lines:
        if "0.2.202609301435" in L and "[OTA]" in L:
            print("OTA MARK", L[:240])
            upgrade_ts = parse_ts(L)
        if "0.2.202609301435" in L and "BOOT" in L:
            print("BOOT1435", L[:240])
            if upgrade_ts is None:
                upgrade_ts = parse_ts(L)
    print("upgrade_ts", upgrade_ts)
    if upgrade_ts and hp:
        after = [x for x in hp if parse_ts(x["ts"]) and parse_ts(x["ts"]) >= upgrade_ts]
        print("after_upgrade_samples", len(after))
        if after:
            print("after first", after[0]["ts"], after[0]["fail"], after[0]["maxblk"])
            print("after last ", after[-1]["ts"], after[-1]["fail"], after[-1]["maxblk"])
            t0 = parse_ts(after[0]["ts"])
            t1 = parse_ts(after[-1]["ts"])
            dt = (t1 - t0).total_seconds() if t0 and t1 else 0
            dfail = after[-1]["fail"] - after[0]["fail"]
            print("after fail_per_s", round(dfail / dt, 4) if dt > 0 else None,
                  "dt", dt, "delta", dfail)
            print("after maxblk hist", Counter(x["maxblk"] for x in after).most_common(8))


if __name__ == "__main__":
    main()
