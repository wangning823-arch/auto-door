#!/bin/bash
f=/opt/garage-gate/logs/device-garage-1388-$(date +%Y%m%d).log
echo "now=$(date '+%F %T')"
echo "=== all BOOT/CRASH today ==="
grep -E 'BOOT|CRASH|panic|rst=' "$f" | grep -E '2026-10-01' | head -80
echo
echo "=== window 07:55-08:05 (before 08:04 panic) ==="
grep -E '2026-10-01 07:5[5-9]|2026-10-01 08:0[0-5]' "$f" | grep -E 'HEAPFAIL|HEAPPOOL|BTUFAIL|BT thin|BT air|BTU reserve|netfail|datagate|OTA|LOG\]|STATUS|NFC|CRASH|BOOT|fail=' | tail -80
echo
echo "=== window 08:55-09:03 (before 09:01 panic) ==="
grep -E '2026-10-01 08:5[5-9]|2026-10-01 09:0[0-3]' "$f" | grep -E 'HEAPFAIL|HEAPPOOL|BTUFAIL|BT thin|BT air|BTU reserve|netfail|datagate|OTA|LOG\]|STATUS|NFC|CRASH|BOOT|fail=' | tail -80
echo
echo "=== HEAPFAIL all after 07:39 ==="
grep 'HEAPFAIL' "$f" | grep -E '2026-10-01 (07:(39|[4-5][0-9])|08:|09:)' | tail -60
echo
echo "=== netfail/datagate after 07:39 ==="
grep -E 'netfail|datagate|force STA' "$f" | grep -E '2026-10-01 (07:|08:|09:)' | tail -40
