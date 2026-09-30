#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Explain 1388 wifi HEAPFAIL burst around 19:46."""
from __future__ import print_function
import json
import re
import ssl
import urllib.request
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


def in_window(L, start, end):
    ts = parse_ts(L)
    return ts is not None and start <= ts <= end


def main():
    data = fetch_json("/api/devices/garage-1388/logs?lines=900")
    text = data.get("text") or ""
    (OUT / "cmp1388_fw.json").write_text(text, encoding="utf-8")
    lines = text.splitlines()

    start = datetime(2026, 9, 30, 19, 43, 0)
    end = datetime(2026, 9, 30, 19, 50, 0)

    print("==== window 19:43-19:50 markers ====")
    for L in lines:
        if not in_window(L, start, end):
            continue
        if any(k in L for k in [
            "HEAPFAIL", "HEAPPOOL", "netfail", "datagate", "[BOOT]", "[OTA]",
            "BT air", "CRASH", "rst=", "[LOG]", "[CLASSIC]", "NFC", "PN532",
            "hold4k", "given back", "rearm",
        ]):
            # filter noise: keep HEAPFAIL all, HEAPPOOL all, others selective
            keep = True
            if "[LOG]" in L or "[CLASSIC]" in L:
                # only print if nearby fail/maxblk/netfail context - print all LOG in window for heap
                keep = True
            if keep:
                print(L[:300])

    print("\n==== HEAPFAIL burst detail ====")
    burst = [L for L in lines if "HEAPFAIL" in L and in_window(L, start, end)]
    print("n", len(burst))
    for L in burst:
        print(L[:240])

    print("\n==== HEAPPOOL around burst ====")
    hp = [L for L in lines if "HEAPPOOL" in L and in_window(L, start, end)]
    for L in hp:
        print(L[:240])

    print("\n==== netfail / STA / other events full day last 40 ====")
    events = [L for L in lines if any(k in L for k in [
        "netfail", "datagate", "force STA", "BOOT rst", "OTA new", "BT air",
        "HEAPFAIL", "CRASH",
    ])]
    for L in events[-40:]:
        print(L[:260])


if __name__ == "__main__":
    main()
