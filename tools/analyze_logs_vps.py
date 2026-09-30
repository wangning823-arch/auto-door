#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Analyze garage device logs on VPS (compatible with older Python)."""
from __future__ import print_function
import re
from collections import Counter
from io import open
from pathlib import Path


def load_lines(path):
    with open(path, "rb") as f:
        raw = f.read()
    return raw.decode("utf-8", "replace").splitlines()


def print_header(title):
    print("\n" + "=" * 20 + " " + title + " " + "=" * 20)


def summarize(name, path):
    print_header(name)
    if not Path(path).exists():
        print("MISSING", path)
        return
    lines = load_lines(path)
    print("file", path)
    print("total_lines", len(lines))
    if lines:
        print("first", lines[0][:220])
        print("last", lines[-1][:220])

    patterns = [
        "rst=4", "panic", "GURU", "HEAPFAIL", "nfc=", "PN532", "HEAPPOOL",
        "log_ship", "LOG_SHIP", "BTU", "rearm", "rearmed", "given back",
        "abort", "setRetries", "SCL=", "fail=", "crash", "boot", "0.2.2026",
        "OTA", "ota", "[LOG]",
    ]
    for p in patterns:
        c = sum(1 for L in lines if p in L)
        print("count[" + p + "]=" + str(c))

    print_header(name + " HEAPPOOL last 15")
    for L in [L for L in lines if "HEAPPOOL" in L][-15:]:
        print(L[:300])

    print_header(name + " HEAPFAIL last 25")
    for L in [L for L in lines if "HEAPFAIL" in L][-25:]:
        print(L[:300])

    print_header(name + " HEAPFAIL size/task histogram (all)")
    hist = Counter()
    for L in lines:
        if "HEAPFAIL" not in L:
            continue
        m = re.search(r"sz=(\d+).*?t=([^ ]+)", L)
        if m:
            hist[(m.group(1), m.group(2))] += 1
    for (sz, task), n in hist.most_common(20):
        print("  sz=" + sz + " task=" + task + " count=" + str(n))

    print_header(name + " fail counter samples after 09:00")
    for L in lines:
        if "HEAPPOOL" in L or "failN=" in L:
            if any(x in L for x in ["2026-09-30 09:", "2026-09-30 10:", "2026-09-30 11:"]):
                print(L[:300])

    print_header(name + " rst=4 / panic / abort after 09:00")
    hits = []
    for L in lines:
        low = L.lower()
        if ("rst=4" in L or "panic" in low or "GURU" in L or "abort" in low):
            if any(x in L for x in ["2026-09-30 09:", "2026-09-30 10:", "2026-09-30 11:"]):
                hits.append(L)
    if not hits:
        print("none")
    else:
        for L in hits[-40:]:
            print(L[:300])

    print_header(name + " NFC / heartbeat interesting after 09:00")
    keys = ["nfc=", "PN532", "NFC", "[LOG]", "HEAPFAIL", "SCL=", "setRetries",
            "abort", "boot", "ready", "0.2.2026", "rst=4", "BTU", "rearm"]
    interesting = []
    for L in lines:
        if any(k in L for k in keys):
            if any(x in L for x in ["2026-09-30 09:", "2026-09-30 10:", "2026-09-30 11:"]):
                interesting.append(L)
    for L in interesting[-40:]:
        print(L[:320])

    print_header(name + " OTA / version markers")
    for L in lines:
        if any(k in L for k in ["OTA", "ota", "0.2.2026", "upgrade", "fw_ver", "FW_VER"]):
            if any(x in L for x in ["2026-09-30", "version", "OTA", "0.2.2026"]):
                print(L[:300])


def main():
    base = Path("/opt/garage-gate/logs")
    summarize("1388", base / "device-garage-1388-20260930.log")
    summarize("dda0", base / "device-garage-dda0-20260930.log")

    print_header("1388 HEAPPOOL fail= timeline after 10:40")
    lines = load_lines(base / "device-garage-1388-20260930.log")
    for L in lines:
        if "HEAPPOOL" in L and (
            "2026-09-30 10:4" in L or "2026-09-30 10:5" in L or "2026-09-30 11:" in L
        ):
            print(L[:300])


if __name__ == "__main__":
    main()
