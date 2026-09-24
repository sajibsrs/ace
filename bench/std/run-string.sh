#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-string.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/string-ace" bench/std/string.ace
cc -std=c11 -O2 -o "$bench_tmp/string-c" bench/std/string.c

# A process pays page-in and first-touch costs that the earliest timed
# workloads absorb, so discard one run of each binary before the timed one.
"$bench_tmp/string-ace" > /dev/null
"$bench_tmp/string-c" > /dev/null

"$bench_tmp/string-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/string-c" > "$bench_tmp/c.txt"

# Rows whose Ace and C sides share one contract must agree on every checksum.
for workload in parse-f64-short parse-f64-long parse-f64 parse-int parse-hex \
                trim ascii-fold-64 utf8-valid; do
    ace_sum=$(awk -v w="$workload" '$1 == w { print $4 }' "$bench_tmp/ace.txt")
    c_sum=$(awk -v w="$workload" '$1 == w { print $4 }' "$bench_tmp/c.txt")
    if [ -z "$ace_sum" ] || [ "$ace_sum" != "$c_sum" ]; then
        echo "$workload checksum mismatch: Ace=$ace_sum C=$c_sum" >&2
        exit 1
    fi
done

printf '%-24s %14s %14s %10s\n' "workload" "Ace ns/op" "C ns/op" "Ace/C"
awk '
    NR == FNR { c[$1] = $2 / $3; next }
    {
        cname = $1
        if ($1 == "format-f64-shortest") cname = "format-f64-17g"
        ace = $2 / $3
        printf "%-24s %14.2f %14.2f %9.2fx\n", $1, ace, c[cname], ace / c[cname]
    }
' "$bench_tmp/c.txt" "$bench_tmp/ace.txt"

echo
echo "Ace checksums"
cat "$bench_tmp/ace.txt"
echo
echo "C checksums"
cat "$bench_tmp/c.txt"
