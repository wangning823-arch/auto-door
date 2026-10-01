#!/bin/bash
echo "now=$(date '+%F %T')"
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== journal 1388 since 09:45 ==="
journalctl -u garage-gate --since "09:45" --no-pager | grep -i 1388 | tail -30
