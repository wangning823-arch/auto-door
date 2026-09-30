#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Focused dda0 window (VPS python3 compatible)."""
from __future__ import print_function
import re
from datetime import datetime
from pathlib import Path

path = Path("/opt/garage-gate/logs/device-garage-dda0-20260930.log")
lines = path.read_text(encoding="utf-8", errors="replace").splitlines()


def parse_ts(s):
    m = re.search(r"(2026-09-30 \d{2}:\d{2}:\d{2})", s)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


print("=== BOOTs after 23:00 ===")
for L in lines:
    if "[BOOT]" in L and "rst=" in L:
        ts = parse_ts(L)
        if ts and ts.hour >= 23:
            print(ts, L[:200])

cut = datetime(2026, 9, 30, 23, 31, 54)
print("\n=== after 23:31:54 events ===")
for L in lines:
    ts = parse_ts(L)
    if not ts or ts < cut:
        continue
    if any(k in L for k in ("BT air", "BT thin", "BTUFAIL", "reserve", "hold4k", "datagate", "netfail", "HEAPPOOL")):
        if "HEAPPOOL" in L:
            continue
        print(ts, L[:240])
