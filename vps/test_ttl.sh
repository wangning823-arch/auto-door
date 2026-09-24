#!/bin/bash
set -e
BASE=http://127.0.0.1:18080
echo '=== restart check health ==='
curl -sS "$BASE/health"; echo
echo '=== set pending via xiaoai ==='
curl -sS -X POST "$BASE/xiaoai/open"; echo
echo '=== age ~0 ==='
curl -sS "$BASE/health"; echo
echo '=== wait 9s for TTL ==='
sleep 9
echo '=== after TTL peek ==='
curl -sS "$BASE/health"; echo
echo '=== poll should be null ==='
curl -sS "$BASE/dev/poll"; echo
echo '=== set again and claim within 1s ==='
# bypass debounce: wait if needed
sleep 2
curl -sS -X POST "$BASE/xiaoai/open"; echo
curl -sS "$BASE/dev/poll"; echo
echo '=== external health ==='
curl -sS https://door.wzx.homes/health; echo
echo DONE
