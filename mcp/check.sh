#!/bin/sh
# Builds ace-mcp and drives one scripted session over stdin, checking each
# response. Not part of `make test`.
set -e
cd "$(dirname "$0")/.."
./ace -o ace-mcp mcp/main.ace
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf 'main(): int {\n    x: int = "a"\n    return y\n}\n' > "$tmp/bad.ace"
printf 'main(): int { s: string = "\\q" return 0 }\n' > "$tmp/lex.ace"

out=$(printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list"}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"definition","arguments":{"file":"lsp/main.ace","line":107,"column":9}}}' \
  '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"hover","arguments":{"file":"lsp/main.ace","line":107,"column":9}}}' \
  '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"definition","arguments":{"file":"lsp/main.ace","line":1,"column":1}}}' \
  '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"nope","arguments":{}}}' \
  '{"jsonrpc":"2.0","id":7,"method":"nope"}' \
  "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"diagnostics\",\"arguments\":{\"file\":\"$tmp/bad.ace\"}}}" \
  "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"diagnostics\",\"arguments\":{\"file\":\"$tmp/lex.ace\"}}}" \
  '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"diagnostics","arguments":{"file":"mcp/main.ace"}}}' \
  '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"std_lookup","arguments":{"name":"str.format"}}}' \
  '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"std_lookup","arguments":{"name":"fmt.Buffer.printf"}}}' \
  '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"std_lookup","arguments":{"name":"str.nothing"}}}' \
  | ./ace-mcp 2>/dev/null)

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
expect 2 '"name":"diagnostics"'
expect 2 '"name":"std_lookup"'
expect 8 'bad.ace:2:14: type mismatch: string given where int expected'
expect 8 'bad.ace:3:12: unknown name'
expect 9 "lex.ace:1:27: unknown escape sequence '\\\\q'"
expect 10 '"text":"no errors"'
expect 11 '/std/str.ace:236\n```ace\npub format(destination: []char, value: int): (usize, bool)'
expect 11 '/std/str.ace:473\n'
expect 12 'pub Buffer.printf(self, format: string): int'
expect 13 '"isError":true'
[ "$(printf '%s\n' "$out" | wc -l)" -eq 13 ] || { echo "FAIL: expected 13 responses"; fail=1; }
[ $fail -eq 0 ] && echo "ace-mcp: ok"
exit $fail
