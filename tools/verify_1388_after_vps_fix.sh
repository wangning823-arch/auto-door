#!/bin/bash
echo "now=$(date '+%F %T')"
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== OTA version ==="
curl -sS --max-time 10 https://door.wzx.homes/ota/version
echo
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
echo "=== 1388 BOOT/CRASH after 09:56 ==="
if [ -f "$f" ]; then
  grep -E 'BOOT|CRASH|BTU reserve|BTUFAIL|BTSTAT' "$f" | grep -E '2026-10-01 09:5|2026-10-01 10:' | tail -40 || true
  echo "--- last 12 ---"
  tail -n 12 "$f"
fi
