#!/usr/bin/env python
# -*- coding: utf-8 -*-
from __future__ import print_function
import re
from io import open
from pathlib import Path


def load(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").splitlines()


def first_last_fail(lines, label):
    fails = []
    for L in lines:
        if "HEAPPOOL" not in L:
            continue
        m = re.search(r"fail=(\d+)", L)
        if m:
            fails.append((L[:23], int(m.group(1))))
    print(label, "HEAPPOOL samples", len(fails))
    if not fails:
        return
    print("  first", fails[0][0], "fail=", fails[0][1])
    print("  last ", fails[-1][0], "fail=", fails[-1][1])


def count_rst_after(lines, marker):
    n = 0
    for L in lines:
        if "rst=4" in L and marker in L:
            n += 1
    print("rst=4 after", marker, "count=", n)


def nfc_stats_after(lines, marker):
    c = {}
    last = ""
    dead = 0
    setr = 0
    for L in lines:
        if marker not in L and marker.split()[0] not in L:
            pass
        if any(x in L for x in ["2026-09-30 10:4", "2026-09-30 10:5", "2026-09-30 11:"]):
            if "nfc=" in L:
                m = re.search(r"nfc=(\w+)", L)
                if m:
                    c[m.group(1)] = c.get(m.group(1), 0) + 1
                    last = L[:200]
            if "PN532 ready" in L or "PN532 READY" in L:
                print("PN532", L[:160])
            if "DEAD" in L:
                dead += 1
                if dead <= 3:
                    print("DEAD", L[:200])
            if "setRetries" in L:
                setr += 1
    print("nfc counts after upgrade window", c)
    print("dead_lines", dead, "setRetries_lines", setr)
    print("last nfc", last)


base = Path("/opt/garage-gate/logs")
print("==== 1388 ====")
lines = load(base / "device-garage-1388-20260930.log")
count_rst_after(lines, "2026-09-30 10:4")
nfc_stats_after(lines, "2026-09-30 10:40:25")
# upgrade boot markers
for L in lines:
    if "0.2.2026" in L or "upgrade" in L.lower() or "BOOT" in L or "rst=" in L[:40]:
        if any(x in L for x in ["2026-09-30 10:3", "2026-09-30 10:4", "2026-09-30 04:5"]):
            if any(k in L for k in ["rst", "BOOT", "0.2.2026", "PN532", "nfc=ok", "upgrade"]):
                print("MK", L[:220])

print("\n==== dda0 ====")
lines = load(base / "device-garage-dda0-20260930.log")
count_rst_after(lines, "2026-09-30 10:")
# last 5 nfc states
nfc = []
for L in lines:
    if "nfc=" in L and "[LOG]" in L:
        nfc.append(L)
print("last 5 heartbeats:")
for L in nfc[-5:]:
    print(L[:260])
# fail rate samples
print("HEAPPOOL samples dda0 last 8:")
hp = [L for L in lines if "HEAPPOOL" in L]
for L in hp[-8:]:
    print(L[:240])
print("first fail after 11:00:")
for L in hp:
    if "2026-09-30 11:0" in L:
        print(L[:240])
        break
print("DEAD count", sum(1 for L in lines if "DEAD" in L))
print("abort count", sum(1 for L in lines if "abort" in L.lower()))
print("last lines:")
for L in lines[-8:]:
    print(L[:220])
