#!/bin/bash
echo "now=$(date '+%F %T')"
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
echo "=== journal since 09:35 ==="
journalctl -u garage-gate --since "09:35" --no-pager | grep -i 1388 | tail -40
echo "=== 1388 log BOOT/OTA after 09:30 ==="
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
if [ -f "$f" ]; then
  tail -n 40 "$f"
  echo "--- markers ---"
  grep -E 'BOOT|OTA new|OTA OK|BTU reserve|CRASH' "$f" | grep -E '2026-10-01 09:[3-9]' | tail -30 || true
fi
