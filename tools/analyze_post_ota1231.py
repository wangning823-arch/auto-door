#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Post-OTA 1388 airbag/BTU fail check."""
from __future__ import print_function
import re
from datetime import datetime
from io import open
from pathlib import Path


def load(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace").splitlines()


def main():
    path = Path("/opt/garage-gate/logs/device-garage-1388-20260930.log")
    lines = load(path)
    print("total", len(lines))
    print("last", lines[-1][:240] if lines else "")

    # OTA / version markers
    print("\n==== version / OTA / BOOT ====")
    for L in lines:
        if any(k in L for k in ["0.2.2026", "[OTA] new", "[BOOT]", "reserve given", "reserve rearmed", "hold4k"]):
            if any(x in L for x in ["2026-09-30 12:", "2026-09-30 13:", "2026-09-30 11:", "2026-09-30 10:4"]):
                print(L[:260])

    # find upgrade boot time for 1231
    upgrade = None
    for L in lines:
        if "0.2.202609301231" in L or ("[OTA] new" in L and "1231" in L):
            print("FW MARK", L[:220])
        if "[BOOT]" in L and "2026-09-30 12:" in L:
            print("BOOT", L[:220])
            if upgrade is None and "rst=" in L:
                upgrade = L[:19]

    if upgrade is None:
        # fallback: last boot after 12:00
        for L in lines:
            if "2026-09-30 12:" in L and "[BOOT]" in L and "rst=" in L:
                upgrade = L[:19]
                print("BOOT fallback", L[:220])

    print("upgrade_ts_guess", upgrade)

    # HEAPPOOL fail series after 12:00
    print("\n==== HEAPPOOL fail after 12:00 (sample) ====")
    hp = []
    for L in lines:
        if "HEAPPOOL" not in L:
            continue
        if not L.startswith("2026-09-30 12:") and not L.startswith("2026-09-30 13:"):
            continue
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        if m:
            fail = int(m.group(1))
            maxb = int(mb.group(1)) if mb else -1
            hp.append((L[:19], fail, maxb))

    print("samples", len(hp))
    if hp:
        print("first", hp[0])
        print("last", hp[-1])
        print("fail_delta", hp[-1][1] - hp[0][1])
        try:
            t0 = datetime.strptime(hp[0][0], "%Y-%m-%d %H:%M:%S")
            t1 = datetime.strptime(hp[-1][0], "%Y-%m-%d %H:%M:%S")
            dt = (t1 - t0).total_seconds()
            if dt > 0:
                print("dt_s", round(dt, 1), "fail_per_s", round((hp[-1][1] - hp[0][1]) / dt, 4))
        except Exception as e:
            print("dt err", e)
        maxblks = [x[2] for x in hp if x[2] >= 0]
        if maxblks:
            print("maxblk min/max/avg", min(maxblks), max(maxblks), sum(maxblks) / len(maxblks))
        # print every ~2min
        for row in hp[:: max(1, len(hp) // 25)][:30]:
            print("  ", row[0], "fail=", row[1], "maxblk=", row[2])

    # HEAPFAIL after 12:00
    print("\n==== HEAPFAIL after 12:00 ====")
    for L in lines:
        if "HEAPFAIL" in L and L.startswith("2026-09-30 12:"):
            print(L[:240])

    # classic rssi after 12:00
    print("\n==== CLASSIC / LOG last 15 after 12:00 ====")
    cls = [L for L in lines if ("[CLASSIC]" in L or "[LOG]" in L) and L[:16] >= "2026-09-30 12:"]
    for L in cls[-15:]:
        print(L[:280])

    # rearm / give logs
    print("\n==== reserve give/rearm after 12:00 ====")
    for L in lines:
        if ("given back" in L or "rearmed" in L or "hold4k" in L) and L[:16] >= "2026-09-30 12:":
            print(L[:240])

    # watch log tail
    w = Path("/opt/garage-gate/logs/watch1388.log")
    if w.exists():
        print("\n==== watch1388 tail ====")
        print("\n".join(w.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]))


if __name__ == "__main__":
    main()
