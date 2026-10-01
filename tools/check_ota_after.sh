#!/bin/bash
set -euo pipefail
DATE=$(date +%Y%m%d)
echo "DATE=$DATE"
for d in garage-1388 garage-dda0; do
  f="/opt/garage-gate/logs/device-${d}-${DATE}.log"
  echo "==== $d $f ===="
  if [ -f "$f" ]; then
    grep -E 'BOOT|OTA new|BTU reserve|BT air|version' "$f" | tail -40 || true
  else
    echo MISSING
  fi
done
echo "=== devices ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
