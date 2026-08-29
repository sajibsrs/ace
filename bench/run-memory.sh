#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-memory.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

echo "Ace compiler report"
echo "--------------------"
./ace --report -o "$bench_tmp/memory" bench/memory.ace

echo
echo "Runtime and allocator report"
echo "----------------------------"
/usr/bin/time -l "$bench_tmp/memory"
