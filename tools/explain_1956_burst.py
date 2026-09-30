#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Investigate 1388 fail burst ~19:56-19:58."""
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


def in_win(L, a, b):
    ts = parse_ts(L)
    return ts is not None and a <= ts <= b


def main():
    data = fetch_json("/api/devices/garage-1388/logs?lines=800")
    text = data.get("text") or ""
    (OUT / "cmp1388_fw.json").write_text(text, encoding="utf-8")
    lines = text.splitlines()

    start = datetime(2026, 9, 30, 19, 53, 0)
    end = datetime(2026, 9, 30, 20, 1, 0)

    print("==== HEAPFAIL 19:53-20:01 ====")
    hf = [L for L in lines if "HEAPFAIL" in L and in_win(L, start, end)]
    print("n", len(hf))
    for L in hf:
        print(L[:240])

    print("\n==== HEAPPOOL 19:55-19:58 ====")
    for L in lines:
        if "HEAPPOOL" in L and in_win(L, datetime(2026, 9, 30, 19, 55, 0),
                                       datetime(2026, 9, 30, 19, 58, 30)):
            print(L[:240])

    print("\n==== netfail / BT air / LOG around burst ====")
    for L in lines:
        if not in_win(L, datetime(2026, 9, 30, 19, 54, 0),
                       datetime(2026, 9, 30, 19, 58, 30)):
            continue
        if any(k in L for k in ["netfail", "datagate", "BT air", "[LOG]",
                                  "BOOT", "OTA new", "HEAPFAIL", "CRASH"]):
            print(L[:280])

    # also scan whole day for wifi HEAPFAIL with big8 < 2308
    print("\n==== all wifi HEAPFAIL big8<2308 today ====")
    for L in lines:
        if "HEAPFAIL" in L and "wifi" in L and "sz=2308" in L:
            m = re.search(r"big8=(\d+)", L)
            if m and int(m.group(1)) < 2308:
                print(L[:240])


if __name__ == "__main__":
    main()
