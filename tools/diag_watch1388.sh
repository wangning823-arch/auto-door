#!/bin/bash
echo "=== watch1388.log tail ==="
tail -n 80 /opt/garage-gate/logs/watch1388.log
echo "=== online_history tail ==="
tail -n 20 /opt/garage-gate/logs/online_history.csv
echo "=== device status again ==="
curl -sS --max-time 15 https://door.wzx.homes/api/devices/garage-1388
echo
echo "=== debug/state peek ==="
curl -sS --max-time 15 http://127.0.0.1:18080/debug/state 2>/dev/null | head -c 3000 || true
echo
