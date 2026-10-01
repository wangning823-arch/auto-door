#!/bin/bash
echo "now=$(date '+%F %T')"
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== journal 1388 last 25 ==="
journalctl -u garage-gate --no-pager | grep 1388 | tail -25
echo "=== 1388 log last 15 ==="
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
tail -n 15 "$f" 2>/dev/null || true
