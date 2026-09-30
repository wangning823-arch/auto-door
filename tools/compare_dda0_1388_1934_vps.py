#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare dda0 vs 1388 post-1934 heap fail types on VPS."""
from __future__ import print_function
import re
from collections import Counter
from datetime import datetime
from pathlib import Path

base = Path("/opt/garage-gate/logs")
files = {
    "1388": base / "device-garage-1388-20260930.log",
    "dda0": base / "device-garage-dda0-20260930.log",
}


def parse_ts(s):
    try:
        return datetime.strptime(s[:19], "%Y-%m-%d %H:%M:%S")
    except Exception:
        return None


def analyze(name, path):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    print("=" * 70)
    print(name, "total_lines", len(lines))
    if lines:
        print("first", lines[0][:220])
        print("last", lines[-1][:220])

    print("-- OTA/BOOT --")
    last_ota = None
    for L in lines:
        if "[OTA] new" in L:
            print(L[:220])
            last_ota = parse_ts(L)
        if "[BOOT]" in L and "rst=" in L:
            print(L[:220])
        if "0.2.2026" in L and any(k in L for k in ["BOOT", "OTA", "rst="]):
            if "HEAP" not in L:
                print(L[:220])
    print("last_ota", last_ota)

    print("-- BT air markers --")
    air = [L for L in lines if "BT air" in L]
    air_types = Counter()
    for L in air:
        if "held force" in L:
            air_types["force"] += 1
        elif "drop" in L:
            air_types["drop"] += 1
        elif "re-hold" in L or "rehold" in L:
            air_types["rehold"] += 1
        elif "held" in L:
            air_types["held_other"] += 1
        else:
            air_types["other"] += 1
    print("air n", len(air), air_types)
    for L in air[:8]:
        print(" ", L[:220])
    if len(air) > 8:
        print(" ...")
        for L in air[-5:]:
            print(" ", L[:220])

    print("-- HEAPPOOL --")
    hp = []
    for L in lines:
        if "HEAPPOOL" not in L or "fail=" not in L:
            continue
        m = re.search(r"fail=(\d+)", L)
        mb = re.search(r"DEF\s+\d+/(\d+)", L)
        maxblk = int(mb.group(1)) if mb else -1
        if m:
            hp.append({
                "ts": L[:19],
                "fail": int(m.group(1)),
                "maxblk": maxblk,
                "raw": L[:240],
            })
    print("n", len(hp))
    if hp:
        print("first", hp[0]["ts"], "fail=", hp[0]["fail"], "maxblk=", hp[0]["maxblk"])
        print("last ", hp[-1]["ts"], "fail=", hp[-1]["fail"], "maxblk=", hp[-1]["maxblk"])
        print("maxblk hist", Counter(x["maxblk"] for x in hp).most_common(10))
        t0 = parse_ts(hp[0]["ts"])
        t1 = parse_ts(hp[-1]["ts"])
        if t0 and t1 and (t1 - t0).total_seconds() > 0:
            dt = (t1 - t0).total_seconds()
            print("window_dt_s", dt,
                  "fail_rate_per_s",
                  round((hp[-1]["fail"] - hp[0]["fail"]) / dt, 4))
        if last_ota:
            post = [x for x in hp if parse_ts(x["ts"]) and parse_ts(x["ts"]) >= last_ota]
            if len(post) >= 2:
                p0, p1 = post[0], post[-1]
                dt = (parse_ts(p1["ts"]) - parse_ts(p0["ts"])).total_seconds()
                print("post n", len(post), "fail", p0["fail"], "->", p1["fail"],
                      "rate/s",
                      round((p1["fail"] - p0["fail"]) / dt, 4) if dt > 0 else None)
                print("post maxblk hist", Counter(x["maxblk"] for x in post).most_common(8))
                print("post samples last8:")
                for row in post[-8:]:
                    print(" ", row["ts"], "fail=", row["fail"], "maxblk=", row["maxblk"])
            pre = [x for x in hp if parse_ts(x["ts"]) and last_ota and parse_ts(x["ts"]) < last_ota]
            if len(pre) >= 2:
                q0, q1 = pre[0], pre[-1]
                dt = (parse_ts(q1["ts"]) - parse_ts(q0["ts"])).total_seconds()
                print("pre n", len(pre), "fail", q0["fail"], "->", q1["fail"],
                      "rate/s",
                      round((q1["fail"] - q0["fail"]) / dt, 4) if dt > 0 else None)
                print("pre maxblk hist", Counter(x["maxblk"] for x in pre).most_common(8))

    print("-- HEAPFAIL --")
    hf = []
    for L in lines:
        if "HEAPFAIL" not in L:
            continue
        m = re.search(r"sz=(\d+)\s+t=(\S+)", L)
        m2 = re.search(r"HEAPFAIL\s+(\S+)", L)
        typ = m2.group(1) if m2 else (m.group(2) if m else "?")
        sz = m.group(1) if m else "?"
        ts = parse_ts(L[:19])
        hf.append({
            "ts": ts,
            "raw": L,
            "typ": typ,
            "sz": sz,
            "post": bool(ts and last_ota and ts >= last_ota),
        })
    print("total", len(hf), "post", sum(1 for x in hf if x["post"]))
    print("hist_all", Counter((x["typ"], x["sz"]) for x in hf).most_common(15))
    if last_ota:
        print("hist_post", Counter((x["typ"], x["sz"]) for x in hf if x["post"]).most_common(15))
        print("hist_pre", Counter((x["typ"], x["sz"]) for x in hf if not x["post"]).most_common(10))
    print("samples post:")
    for x in [x for x in hf if x["post"]][-12:]:
        print(" ", x["raw"][:240])
    print("samples pre:")
    for x in [x for x in hf if not x["post"]][-5:]:
        print(" ", x["raw"][:240])

    print("-- net/datagate --")
    net = [L for L in lines if "netfail" in L or "datagate" in L]
    print("n", len(net))
    for L in net[-8:]:
        print(L[:240])

    print("-- HTTP/poll/status offline-ish --")
    useful = []
    for L in lines:
        if "HEAPPOOL" in L or "HEAPFAIL" in L:
            continue
        if any(k in L for k in [
            "netfail", "datagate", "poll fail", "status fail",
            "connect fail", "offline", "no route", "refused", "resolve",
            "timeout",
        ]):
            useful.append(L)
        elif any(k in L for k in ["[HTTP]", "[POLL]", "[STATUS]", "[LOG]", "[CLASSIC]"]):
            useful.append(L)
    print("useful", len(useful))
    for L in useful[-20:]:
        print(L[:260])

    print("-- LOG/CLASSIC tail --")
    log_n = sum(1 for L in lines if "[LOG]" in L)
    classic_n = sum(1 for L in lines if "[CLASSIC]" in L)
    print("[LOG]", log_n, "[CLASSIC]", classic_n)
    for L in lines[-30:]:
        if any(k in L for k in [
            "[LOG]", "[CLASSIC]", "nfc=", "rssi=", "HEAP", "fail=",
            "BT air", "BOOT", "OTA", "netfail", "status", "poll",
            "datagate",
        ]):
            print(L[:260])


def main():
    for name, path in files.items():
        analyze(name, path)


if __name__ == "__main__":
    main()
