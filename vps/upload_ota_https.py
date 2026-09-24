#!/usr/bin/env python3
"""HTTPS upload firmware.bin and notify devices (no SSH/SCP needed)."""
import json
import os
import ssl
import sys
import urllib.request

BASE = os.environ.get("GARAGE_BASE", "https://door.wzx.homes")
PW = os.environ.get("GARAGE_UI_PASSWORD", "")
if not PW:
    raise SystemExit("set GARAGE_UI_PASSWORD (VPS /opt/garage-gate/ui_password)")
BIN = sys.argv[1] if len(sys.argv) > 1 else r"D:\mimo\车库门自动化\garage_door_firmware\.pio\build\esp32dev\firmware.bin"
VER = sys.argv[2] if len(sys.argv) > 2 else ""
ONLY = sys.argv[3] if len(sys.argv) > 3 else ""  # optional device id

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
    r = urllib.request.Request(BASE + path, data=body, headers=h, method=method)
    with urllib.request.urlopen(r, context=ctx, timeout=120) as resp:
        return resp.status, resp.read().decode("utf-8", "replace")


st, txt = call("/api/login", {"password": PW})
tok = json.loads(txt)["token"]
print("login", st)

qs = "?notify=1"
if VER:
    qs += "&version=" + VER
if ONLY:
    qs += "&id=" + ONLY

with open(BIN, "rb") as f:
    blob = f.read()
print("bin", BIN, "size", len(blob))
st, txt = call(
    "/api/ota/upload" + qs,
    headers={"X-Garage-Token": tok, "Content-Type": "application/octet-stream"},
    method="POST",
    raw=blob,
)
print("upload", st, txt)
st, txt = call("/api/devices", headers={"X-Garage-Token": tok})
print("devices", txt)
