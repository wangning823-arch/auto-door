#!/bin/bash
echo "now=$(date '+%F %T')"
python3 /tmp/rate_btu_window.py garage-1388 09:57:00 25
echo "=== boots/crash since 09:56 ==="
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
grep -E 'BOOT rst=|\[CRASH\]' "$f" | grep -E '2026-10-01 (09:5[6-9]|1[0-9]:)' || true
echo "=== devices ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
