#!/bin/bash
echo "now=$(date '+%F %T')"
python3 /tmp/rate_btu_window.py garage-1388 07:39:30 15
echo "=== devices ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== 1388 reserve/btu lines ==="
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
grep -E 'BTU reserve|BTUFAIL|BT thin|BOOT' "$f" | grep -E '2026-10-01 07:(39|4[0-9]|5[0-9]):' | tail -40 || true
