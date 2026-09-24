#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-calls.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/calls-ace" bench/core/calls.ace
cc -std=c11 -O2 -o "$bench_tmp/calls-c" bench/core/calls.c
cc -std=c11 -O2 -DALLOW_INLINE -o "$bench_tmp/calls-c-inline" bench/core/calls.c

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/calls-ace" > /dev/null
"$bench_tmp/calls-c" > /dev/null
"$bench_tmp/calls-c-inline" > /dev/null

"$bench_tmp/calls-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/calls-c" > "$bench_tmp/c.txt"
"$bench_tmp/calls-c-inline" > "$bench_tmp/c-inline.txt"

for variant in c c-inline; do
    if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
              END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/$variant.txt"; then
        echo "checksum mismatch between Ace and $variant" >&2
        exit 1
    fi
done

printf '%-16s %12s %12s %14s %10s\n' "workload" "Ace ns/op" "C ns/op" "C inlined" "Ace/C"
awk '
    FILENAME == ARGV[1] { c[$1] = $2 / $3; next }
    FILENAME == ARGV[2] { ci[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.2f %12.2f %14.2f %9.2fx\n", $1, ace, c[$1], ci[$1], ace / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/c-inline.txt" "$bench_tmp/ace.txt"
