#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-hotloop.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/hotloop-ace" bench/hotloop.ace
# Ace leaves vectorization to explicit `simd`, so scalar C is the comparison
# target; the auto-vectorized build is a reference column, not a goal.
cc -std=c11 -O2 -fno-vectorize -fno-slp-vectorize \
    -o "$bench_tmp/hotloop-c" bench/hotloop.c
cc -std=c11 -O2 -o "$bench_tmp/hotloop-c-vector" bench/hotloop.c

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/hotloop-ace" > /dev/null
"$bench_tmp/hotloop-c" > /dev/null
"$bench_tmp/hotloop-c-vector" > /dev/null

"$bench_tmp/hotloop-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/hotloop-c" > "$bench_tmp/c.txt"
"$bench_tmp/hotloop-c-vector" > "$bench_tmp/c-vector.txt"

for variant in c c-vector; do
    if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
              END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/$variant.txt"; then
        echo "checksum mismatch between Ace and $variant" >&2
        exit 1
    fi
done

printf '%-16s %12s %12s %14s %10s\n' "workload" "Ace ns/op" "C ns/op" "C vector" "Ace/C"
awk '
    FILENAME == ARGV[1] { c[$1] = $2 / $3; next }
    FILENAME == ARGV[2] { cv[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.3f %12.3f %14.3f %9.2fx\n", $1, ace, c[$1], cv[$1], ace / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/c-vector.txt" "$bench_tmp/ace.txt"
