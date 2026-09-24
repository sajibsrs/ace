#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-json.XXXXXX")
trap 'rm -rf "$bench_tmp" "$repo_dir/bench/package/json/build"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

# The benchmark imports pkg/json, so it is a package built in release mode.
# There is no C column: compare runs of this script across builds instead.
(cd bench/package/json && ../../../ace pkg build --release > /dev/null)
cp bench/package/json/build/json_bench "$bench_tmp/json-ace"

# Discard one run: see bench/README.md on first-run cost.
"$bench_tmp/json-ace" > /dev/null
"$bench_tmp/json-ace" > "$bench_tmp/ace.txt"

printf '%-10s %12s %10s\n' "workload" "Ace ns/op" "Ace ns/B"
awk '{
    per_byte = $4 > 0 ? sprintf("%10.2f", $2 / $4) : sprintf("%10s", "-")
    printf "%-10s %12.2f %s\n", $1, $2 / $3, per_byte
}' "$bench_tmp/ace.txt"

echo
echo "Ace checksums"
cat "$bench_tmp/ace.txt"
