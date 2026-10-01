#!/bin/bash
# dda0 BTU fail 现状速览
LOG=/opt/garage-gate/logs/device-garage-dda0-$(date +%Y%m%d).log
echo "now=$(date '+%F %T')"
echo "=== version / boot ==="
grep -E 'version|BOOT.*rst=' "$LOG" | tail -8
echo "=== BTUFAIL per 10min (today) ==="
grep 'BTUFAIL' "$LOG" | awk '{
  split($2,t,":");
  b=int(t[2]/10)*10;
  key=sprintf("%s:%02d", t[1], b);
  c[key]++;
} END { for (k in c) print k, c[k] }' | sort
echo "=== last BTUFAIL line ==="
grep 'BTUFAIL' "$LOG" | tail -1
echo "=== last 10 HEAP BT thin ==="
grep 'BT thin before' "$LOG" | tail -10
