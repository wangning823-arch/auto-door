#!/bin/bash
L=/opt/garage-gate/logs/device-garage-1388-20261001.log
echo "=== 18:28后按分钟统计日志行数(空窗检测) ==="
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/{print substr($0,1,16)}' "$L" | sort | uniq -c | tail -30
echo
echo "=== 1821启动后是否有重启/brownout/ready ==="
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/' "$L" | grep -E 'rst:0x|Brownout|Guru|BOOT\] ready' | tail -10
echo
echo "=== BT air 行(18:28后, 节流后应约60s一条) ==="
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/ && /BT air/' "$L" | tail -25
echo
echo "=== nginx 最近 dev/status ==="
grep 'dev/status' /var/log/nginx/access.log | tail -8
echo
echo "=== nginx 最近 dev/logs ==="
grep 'dev/logs' /var/log/nginx/access.log | tail -8
echo
echo "=== 最近 BTSTAT ==="
grep 'BTSTAT' "$L" | tail -6
echo
echo "=== 1821下 HEAPFAIL / drop 计数 ==="
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/' "$L" | grep -cE 'HEAPFAIL'
awk '/^2026-10-01 18:(2[89]|[3-5][0-9])/ && /BT air/' "$L" | grep -c 'drop'
