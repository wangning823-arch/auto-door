#!/bin/bash
echo "now=$(date '+%F %T')"
echo "=== devices api ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== device 1388 detail ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices/garage-1388
echo
echo "=== journal garage-gate last 80 ==="
journalctl -u garage-gate -n 80 --no-pager
echo "=== logs dir latest ==="
ls -lt /opt/garage-gate/logs | head -15
echo "=== 1388 log last modified + last lines ==="
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
ls -l "$f"
tail -n 20 "$f" || true
echo "=== any new BTU reserve anywhere ==="
grep -R "BTU reserve" /opt/garage-gate/logs | tail -20 || true
