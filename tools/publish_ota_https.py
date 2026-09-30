#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Publish firmware.bin to VPS via HTTPS (no SSH)."""
import hashlib
import json
import ssl
import urllib.request
from pathlib import Path

BASE = "https://door.wzx.homes"
ROOT = Path(r"D:\mimo\车库门自动化\garage_door_firmware")
BIN = ROOT / ".pio" / "build" / "esp32dev" / "firmware.bin"
VERH = ROOT / "src" / "fw_version.h"


def read_ver():
    for line in VERH.read_text(encoding="utf-8").splitlines():
        if "FW_VERSION" in line and '"' in line:
            return line.split('"')[1]
    raise SystemExit("no FW_VERSION")


def main():
    ver = read_ver()
    blob = BIN.read_bytes()
    sha = hashlib.sha256(blob).hexdigest()
    print("fw", ver, "size", len(blob), "sha", sha)
    ctx = ssl._create_unverified_context()

    def call(path, data=None, headers=None, method=None, raw=None):
        h = dict(headers or {})
        body = None
        if raw is not None:
            body = raw
            h.setdefault("Content-Type", "application/octet-stream")
        elif data is not None:
            body = json.dumps(data).encode("utf-8")
            h.setdefault("Content-Type", "application/json")
        req = urllib.request.Request(BASE + path, data=body, headers=h, method=method)
        with urllib.request.urlopen(req, context=ctx, timeout=180) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")

    st, txt = call("/api/login", {"password": ""})
    print("login", st, txt[:120])
    tok = json.loads(txt).get("token") or ""
    st, txt = call(
        "/api/ota/upload?notify=0&version=" + ver,
        headers={"X-Garage-Token": tok, "Content-Type": "application/octet-stream"},
        method="POST",
        raw=blob,
    )
    print("upload", st, txt[:400])
    st, txt = call("/api/devices", headers={"X-Garage-Token": tok})
    print("devices", txt[:500])


if __name__ == "__main__":
    main()
