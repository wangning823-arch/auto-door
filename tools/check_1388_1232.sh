echo now=$(date +%F%T)
curl -sS --max-time 15 https://door.wzx.homes/api/devices
echo
journalctl -u garage-gate --since "12:33" --no-pager | grep 1388 | tail -25
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
echo "--- last markers ---"
grep -E 'BOOT|OTA new|OTA OK|BTU reserve' "$f" | tail -15
