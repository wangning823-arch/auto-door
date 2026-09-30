#!/bin/bash
set -euo pipefail
DATE=$(date +%Y%m%d)
echo "DATE=$DATE"
echo "=== logs dir ==="
ls -lt /opt/garage-gate/logs/ | head -30
for d in garage-dda0 garage-1388; do
  f="/opt/garage-gate/logs/device-${d}-${DATE}.log"
  echo "==== $d $f ===="
  if [ -f "$f" ]; then
    wc -l "$f"
    echo -n "BTUFAIL="; grep -c BTUFAIL "$f" || true
    echo -n "BTSTAT="; grep -c BTSTAT "$f" || true
    echo -n "HEAPPOOL="; grep -c HEAPPOOL "$f" || true
    echo -n "HEAPFAIL="; grep -c HEAPFAIL "$f" || true
    echo "--- markers ---"
    grep -E 'BOOT|OTA new|BTSTAT|BTUFAIL' "$f" | tail -60 || true
  else
    echo MISSING
  fi
done
echo "=== devices api ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices || true
echo
echo "=== journal garage-gate ==="
journalctl -u garage-gate -n 30 --no-pager || true
