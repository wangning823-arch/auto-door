#!/bin/bash
echo "now=$(date '+%F %T')"
echo "=== devices ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
echo "=== file $f ==="
if [ ! -f "$f" ]; then echo MISSING; exit 0; fi
wc -l "$f"
ls -l "$f"
echo "=== BOOT/OTA/BTU reserve after 07:39 ==="
grep -E 'BOOT|OTA new|BTU reserve|BTUFAIL|BT thin|datagate|netfail|CRASH|panic' "$f" | grep -E '2026-10-01 (07:(39|[4-5][0-9])|08:)' | tail -80 || true
echo "=== BTSTAT series after 07:39:30 ==="
grep '\[BTSTAT\]' "$f" | grep -E '2026-10-01 (07:(39|[4-5][0-9])|08:)' || true
echo "=== last 25 lines ==="
tail -n 25 "$f"
