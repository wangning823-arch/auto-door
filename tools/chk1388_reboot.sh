#!/bin/bash
L=/opt/garage-gate/logs/device-garage-1388-20261001.log
NOW=$(date '+%Y-%m-%d %H:%M')
echo "=== 当前时间 $NOW ==="
echo
echo "=== 全天 BOOT ready / rst 记录 ==="
grep -E 'BOOT\] ready|BOOT\] rst=' "$L" | tail -20
echo
echo "=== 18:28 后所有重启迹象(rst/brownout/guru/wdt) ==="
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/,0' "$L" | grep -E 'rst=|rst:0x|Brownout|Guru|WDT|panic|boot:`' | tail -20
echo "(空=无重启)"
echo
echo "=== 18:28后 最后30行(看是否还在持续产出) ==="
tail -30 "$L"
echo
echo "=== 18:28后按分钟行数 ==="
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/{print substr($0,1,16)}' "$L" | sort | uniq -c | tail -40
echo
echo "=== nginx 1388 最近 status/logs ==="
grep -E 'dev/(status|logs)\?id=garage-1388' /var/log/nginx/access.log | tail -10
