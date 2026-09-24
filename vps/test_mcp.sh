#!/bin/bash
set -e
BASE=http://127.0.0.1:18080
echo '=== health ==='
curl -sS "$BASE/health"; echo
echo '=== initialize ==='
curl -sS -X POST "$BASE/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0"}}}'
echo
echo '=== initialized ==='
curl -sS -o /dev/null -w '%{http_code}\n' -X POST "$BASE/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","method":"notifications/initialized"}'
echo '=== tools/list ==='
curl -sS -X POST "$BASE/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
echo
echo '=== tools/call open_garage ==='
curl -sS -X POST "$BASE/mcp" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"open_garage","arguments":{}}}'
echo
echo '=== dev/poll ==='
curl -sS "$BASE/dev/poll"; echo
echo '=== external mcp initialize ==='
curl -sS -X POST https://door.wzx.homes/mcp \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json, text/event-stream' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"ext","version":"0"}}}'
echo
echo DONE
