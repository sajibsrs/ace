#!/bin/sh
# Builds ace-mcp and drives one scripted session over stdin, checking each
# response. Not part of `make test`.
set -e
cd "$(dirname "$0")/.."
./ace -o ace-mcp mcp/main.ace

out=$(printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"definition","arguments":{"file":"lsp/main.ace","line":107,"column":9}}}' \
  '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"hover","arguments":{"file":"lsp/main.ace","line":107,"column":9}}}' \
  '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"definition","arguments":{"file":"lsp/main.ace","line":1,"column":1}}}' \
  '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nope","arguments":{}}}' \
  '{"jsonrpc":"2.0","id":7,"method":"nope"}' \
  | ./ace-mcp)

fail=0
expect() {
  if ! printf '%s\n' "$out" | grep -F "\"id\":$1," | grep -qF "$2"; then
    echo "FAIL id $1: expected $2"
    fail=1
  fi
}
expect 1 '"protocolVersion":"2025-06-18"'
expect 2 '"name":"definition"'
expect 2 '"name":"hover"'
expect 3 '/std/mem.ace:83:5"'
expect 4 'pub copy(noalias dst: []u8, src: []u8)'
expect 5 '"isError":true'
expect 6 'unknown tool'
expect 7 '"code":-32601'
[ "$(printf '%s\n' "$out" | wc -l)" -eq 7 ] || { echo "FAIL: expected 7 responses"; fail=1; }
[ $fail -eq 0 ] && echo "ace-mcp: ok"
exit $fail
