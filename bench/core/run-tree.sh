#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-tree.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/tree-ace" bench/core/tree.ace
cc -std=c11 -O2 -o "$bench_tmp/tree-c" bench/core/tree.c

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/tree-ace" > /dev/null
"$bench_tmp/tree-c" > /dev/null

"$bench_tmp/tree-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/tree-c" > "$bench_tmp/c.txt"

if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
          END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/c.txt"; then
    echo "checksum mismatch between Ace and C" >&2
    exit 1
fi

printf '%-16s %12s %12s %10s\n' "workload" "Ace ns/node" "C ns/node" "Ace/C"
awk '
    NR == FNR { c[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.2f %12.2f %9.2fx\n", $1, ace, c[$1], ace / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/ace.txt"
