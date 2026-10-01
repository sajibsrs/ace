#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-collections.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/collections-ace" bench/std/collections.ace
# Ace does not vectorize; keep C scalar too so the ratio measures the
# containers rather than SIMD, as bench/core/run-hotloop.sh does.
cc -std=c11 -O2 -fno-tree-vectorize -fno-tree-slp-vectorize -o "$bench_tmp/collections-c" bench/std/collections.c

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/collections-ace" > /dev/null
"$bench_tmp/collections-c" > /dev/null

"$bench_tmp/collections-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/collections-c" > "$bench_tmp/c.txt"

# Both sides run one algorithm, so every checksum must agree.
if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
          END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/c.txt"; then
    echo "checksum mismatch between Ace and C" >&2
    exit 1
fi

printf '%-16s %12s %12s %9s\n' "workload" "Ace ns/op" "C ns/op" "Ace/C"
awk '
    NR == FNR { c[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.2f %12.2f %8.2fx\n", $1, ace, c[$1], ace / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/ace.txt"

echo
echo "Ace checksums"
cat "$bench_tmp/ace.txt"
