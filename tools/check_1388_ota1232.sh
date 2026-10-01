#!/bin/bash
echo now=$(date +%F\ %T)
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
echo "=== file mtime ==="
ls -l "$f"
echo "=== OTA/BOOT 12:33+ ==="
grep -E 'OTA|BOOT|BTU reserve|CRASH' "$f" | awk '$2 >= "12:33:00"' | tail -40
echo "=== last 20 lines ==="
tail -n 20 "$f"
echo "=== devices ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
