#!/bin/bash
echo "now=$(date '+%F %T')"
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== journal dda0 last 30 ==="
journalctl -u garage-gate --no-pager | grep dda0 | tail -30
echo "=== online_history dda0 ==="
grep dda0 /opt/garage-gate/logs/online_history.csv | tail -20
