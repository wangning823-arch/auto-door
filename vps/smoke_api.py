#!/usr/bin/env python3
import json
import urllib.request

BASE = "http://127.0.0.1:18080"

def req(path, data=None, headers=None, method=None):
    h = headers or {}
    body = None
    if data is not None:
        body = json.dumps(data).encode("utf-8")
        h.setdefault("Content-Type", "application/json")
    r = urllib.request.Request(BASE + path, data=body, headers=h, method=method)
    try:
        with urllib.request.urlopen(r, timeout=8) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")

# password from VPS file or GARRAGE_UI_PASSWORD env — never hardcode
import os
try:
    pw = open("/opt/garage-gate/ui_password").read().strip()
    print("pw_file_len", len(pw))
except Exception as e:
    pw = os.environ.get("GARAGE_UI_PASSWORD", "")
    print("pw_file_err", e)

code, txt = req("/api/login", {"password": pw})
print("login", code, txt[:120])

token = ""
if code == 200:
    token = json.loads(txt).get("token") or ""
print("token_len", len(token))

h = {"X-Garage-Token": token}
code, txt = req("/api/devices", headers=h)
print("devices", code, txt[:400])

code, txt = req(
    "/dev/status?id=garage-test1",
    {
        "id": "garage-test1",
        "fw": "0.2.202609241116",
        "role": "lab",
        "nfc": {"ok": 1},
        "web": 1,
        "rf": {"open": 1, "close": 1},
        "sta": 1,
        "heap": 120000,
    },
    method="POST",
)
print("status", code, txt)

code, txt = req("/api/devices", headers=h)
print("devices2", code, txt[:500])

code, txt = req("/dev/poll?id=garage-test1&fw=0.1.0")
print("poll_old", code, txt)

code, txt = req("/api/devices/garage-test1/logs?lines=5", headers=h)
print("logs", code, txt[:200])

code, txt = req("/api/devices/garage-test1", headers=h)
print("detail", code, txt[:400])
