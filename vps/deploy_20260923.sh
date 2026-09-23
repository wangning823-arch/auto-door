#!/bin/bash
set -e
mkdir -p /opt/garage-gate/web /opt/garage-gate/ota /opt/garage-gate/logs

/usr/bin/python3 - <<'PY'
import hashlib, json, os
p = "/opt/garage-gate/ota/firmware.bin"
h = hashlib.sha256(open(p, "rb").read()).hexdigest()
ver = "0.2.202609231923"
info = {"version": ver, "sha256": h, "url": "/ota/firmware.bin"}
json.dump(info, open("/opt/garage-gate/ota/version.json", "w"))
print("version.json", info)
print("size", os.path.getsize(p))
PY

CONF=/etc/nginx/conf.d/door.wzx.homes.conf
cp -a "$CONF" "$CONF.bak.$(date +%Y%m%d%H%M%S)"
cat > "$CONF" <<'EOF'
server {
    server_name door.wzx.homes;

    location / {
        proxy_pass http://127.0.0.1:18080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_read_timeout 30s;
        proxy_send_timeout 30s;
    }

    listen 443 ssl; # managed by Certbot
    ssl_certificate /etc/letsencrypt/live/door.wzx.homes/fullchain.pem; # managed by Certbot
    ssl_certificate_key /etc/letsencrypt/live/door.wzx.homes/privkey.pem; # managed by Certbot
    include /etc/letsencrypt/options-ssl-nginx.conf; # managed by Certbot
    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem; # managed by Certbot
}

server {
    listen 80;
    server_name door.wzx.homes;

    location = /dev/poll {
        proxy_pass http://127.0.0.1:18080;
        proxy_set_header Host $host;
    }
    location = /dev/logs {
        proxy_pass http://127.0.0.1:18080;
        proxy_set_header Host $host;
        client_max_body_size 256k;
    }
    location ^~ /ota/ {
        proxy_pass http://127.0.0.1:18080;
        proxy_set_header Host $host;
        proxy_read_timeout 120s;
    }
    location / {
        return 301 https://$host$request_uri;
    }
}
EOF

nginx -t
systemctl reload nginx
systemctl restart garage-gate
sleep 0.6
echo '=== health ==='
curl -sS http://127.0.0.1:18080/health; echo
echo '=== ui ==='
curl -sS -o /dev/null -w '%{http_code} %{content_type}\n' http://127.0.0.1:18080/
echo '=== ota ==='
curl -sS http://127.0.0.1:18080/ota/version; echo
echo '=== logs post ==='
echo '[test] hello from deploy' | curl -sS -X POST --data-binary @- http://127.0.0.1:18080/dev/logs; echo
echo '=== login (password comes from /opt/garage-gate/ui_password) ==='
# 勿把明文密码写进脚本；联调时用：
#   curl -sS -X POST http://127.0.0.1:18080/api/login \
#     -H 'Content-Type: application/json' \
#     -d "{\"password\":\"$(cat /opt/garage-gate/ui_password)\"}"
echo ok
