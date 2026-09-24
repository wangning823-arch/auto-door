#!/usr/bin/env python3
import os

cands = []
for root in ("/etc/nginx/sites-enabled", "/etc/nginx/conf.d", "/etc/nginx/sites-available"):
    if not os.path.isdir(root):
        continue
    for n in os.listdir(root):
        fp = os.path.join(root, n)
        if not os.path.isfile(fp):
            continue
        try:
            t = open(fp, encoding="utf-8", errors="replace").read()
        except Exception:
            continue
        if "door.wzx.homes" in t and "/dev/poll" in t:
            cands.append(fp)

print("cands", cands)
for fp in cands:
    t = open(fp, encoding="utf-8", errors="replace").read()
    if "/dev/status" in t:
        print("already", fp)
        continue
    needle = "location = /dev/logs {"
    add = (
        "location = /dev/status {\n"
        "        proxy_pass http://127.0.0.1:18080;\n"
        "        proxy_http_version 1.1;\n"
        "        proxy_set_header Host $host;\n"
        "        proxy_set_header X-Real-IP $remote_addr;\n"
        "        client_max_body_size 32k;\n"
        "    }\n"
        "\n    location = /dev/logs {"
    )
    if needle not in t:
        print("no needle", fp)
        continue
    t = t.replace(needle, add, 1)
    open(fp, "w", encoding="utf-8").write(t)
    print("patched", fp)
