#!/bin/bash
# Wait helper not used remotely; rate check runs after window.
DATE=$(date +%Y%m%d)
echo "now=$(date '+%F %T') DATE=$DATE"
f="/opt/garage-gate/logs/device-garage-1388-${DATE}.log"
if [ ! -f "$f" ]; then echo MISSING; exit 0; fi
wc -l "$f"
echo "--- post boot markers ---"
grep -E 'BOOT|BTU reserve|BT air|OTA|version' "$f" | grep -E '2026-10-01 07:(1[5-9]|[2-9][0-9]):' | tail -40 || true
echo "--- BTSTAT after 07:15:33 ---"
grep '\[BTSTAT\]' "$f" | grep -E '2026-10-01 07:(1[5-9]|[2-9][0-9]):' | tail -30 || true
echo "--- BTUFAIL / thin / reserve after 07:15:33 ---"
grep -E 'BTUFAIL|BT thin|BTU reserve' "$f" | grep -E '2026-10-01 07:(1[5-9]|[2-9][0-9]):' || true
