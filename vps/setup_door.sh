#!/bin/bash
set -e
echo '=== DNS ==='
python3 - <<'PY'
import socket
try:
    print(socket.getaddrinfo('door.wzx.homes', 80))
except Exception as e:
    print('FAIL', e)
PY
echo '=== nslookup local ==='
nslookup door.wzx.homes || true
echo '=== nslookup 8.8.8.8 ==='
nslookup door.wzx.homes 8.8.8.8 || true
echo '=== proxy health ==='
curl -sS -D- http://127.0.0.1/health -H 'Host: door.wzx.homes' -o /tmp/h.body || true
echo
cat /tmp/h.body; echo
echo '=== proxy open+poll ==='
curl -sS -X POST http://127.0.0.1/xiaoai/open -H 'Host: door.wzx.homes'; echo
curl -sS http://127.0.0.1/dev/poll -H 'Host: door.wzx.homes'; echo
echo '=== certbot retry ==='
# certbot --redirect 后必须再打补丁：/dev/poll 保持明文反代（见 nginx-dev-poll-http.conf）
certbot --nginx -d door.wzx.homes --non-interactive --agree-tos --register-unsafely-without-email --redirect || true
ls -la /etc/letsencrypt/live/door.wzx.homes/ 2>/dev/null || echo 'no cert yet'
nginx -t
systemctl reload nginx
echo DONE
