#!/bin/bash
L=/opt/garage-gate/logs/device-garage-1388-20261001.log
# 若已过午夜切到次日文件
D=$(date '+%Y-%m-%d')
L2="/opt/garage-gate/logs/device-garage-1388-${D//-/}.log"
[ -f "$L2" ] && L="$L2"
NOW=$(date '+%H:%M:%S')
echo "=== $NOW  file=$L ==="
echo
echo "--- 最近约15分钟 按分钟行数(空窗检测) ---"
tail -400 "$L" | grep -oE '^2026-[0-9-]+ [0-9]{2}:[0-9]{2}' | sort | uniq -c | tail -15
echo
echo "--- 最近一次 BOOT / 重启迹象(最近500行) ---"
tail -500 "$L" | grep -E 'BOOT\] ready|rst=|Brownout|Guru' | tail -5
echo
echo "--- 最近8条 BTSTAT ---"
grep 'BTSTAT' "$L" | tail -8
echo
echo "--- 最近4条 HEAPPOOL ---"
grep 'HEAPPOOL' "$L" | tail -4
echo
echo "--- 最近 BT air 节流行 ---"
grep 'BT air' "$L" | tail -6
echo
echo "--- nginx 1388 status 最近8条(频率) ---"
grep 'dev/status?id=garage-1388' /var/log/nginx/access.log | tail -8
echo
echo "--- nginx 1388 logs 最近8条 ---"
grep 'dev/logs?id=garage-1388' /var/log/nginx/access.log | tail -8
echo
echo "--- status JSON 卡点字段 ---"
curl -s --max-time 8 'http://127.0.0.1:18080/api/devices/garage-1388'
echo
