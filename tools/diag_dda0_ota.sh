#!/bin/bash
echo "now=$(date '+%F %T')"
echo "=== devices ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== journal dda0 ==="
journalctl -u garage-gate --since "07:54" --no-pager | grep -i dda0 | tail -40
echo "=== dda0 log tail ==="
f=/opt/garage-gate/logs/device-garage-dda0-$(date +%Y%m%d).log
if [ -f "$f" ]; then
  wc -l "$f"
  tail -n 40 "$f"
  echo "--- OTA/BTU after 07:50 ---"
  grep -E 'OTA|BOOT|BTU reserve|BTUFAIL|version' "$f" | grep -E '2026-10-01 07:5|2026-10-01 08:' | tail -40 || true
else
  echo MISSING
fi
