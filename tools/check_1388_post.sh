#!/bin/bash
DATE=$(date +%Y%m%d)
echo "DATE=$DATE now=$(date '+%F %T')"
f="/opt/garage-gate/logs/device-garage-1388-${DATE}.log"
echo "file=$f"
if [ -f "$f" ]; then
  wc -l "$f"
  echo "--- last 50 ---"
  tail -n 50 "$f"
  echo "--- markers >= 07:00 ---"
  grep -E 'BTU reserve|BTUFAIL|BTSTAT|BOOT|OTA|version' "$f" | grep -E '2026-10-01 0[7-9]:' | tail -60 || true
else
  echo MISSING
fi
echo "--- journal since 07:10 ---"
journalctl -u garage-gate --since "07:10" --no-pager | tail -50
