#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-binbox.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release -o "$bench_tmp/binbox-ace" bench/package/binbox.ace

# Discard one run: see bench/README.md on first-run cost.
"$bench_tmp/binbox-ace" > /dev/null
"$bench_tmp/binbox-ace" > "$bench_tmp/ace.txt"

printf '%-16s %12s %14s %14s\n' "board" "ns/tick" "ticks/s" "min..max ns"
awk '{
    printf "%-16s %12.1f %14.0f %7.1f..%.1f\n", $1, $2 / 1000, 1e12 / $2, $3 / 1000, $4 / 1000
}' "$bench_tmp/ace.txt"
