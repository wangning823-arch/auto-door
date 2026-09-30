#!/usr/bin/env python3
import json, ssl, urllib.request
ctx = ssl._create_unverified_context()
req = urllib.request.Request("https://door.wzx.homes/api/devices")
with urllib.request.urlopen(req, context=ctx, timeout=20) as r:
    d = json.loads(r.read().decode("utf-8", "replace"))
print("OTA", (d.get("ota") or {}).get("version"))
for x in d.get("devices") or []:
    print(x.get("id"), "fw=", x.get("fw"), "online=", x.get("online"),
          "ago=", x.get("last_seen_ago_s"), "health=", x.get("health"),
          "rssi=", x.get("rssi"), "heap=", x.get("heap"), "maxblk=", x.get("maxblk"))
