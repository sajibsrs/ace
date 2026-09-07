#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-string.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/string-ace" bench/string.ace
cc -std=c11 -O2 -o "$bench_tmp/string-c" bench/string.c

"$bench_tmp/string-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/string-c" > "$bench_tmp/c.txt"

ace_parse=$(awk '$1 == "parse-f64" { print $4 }' "$bench_tmp/ace.txt")
c_parse=$(awk '$1 == "parse-f64" { print $4 }' "$bench_tmp/c.txt")
ace_parse_short=$(awk '$1 == "parse-f64-short" { print $4 }' "$bench_tmp/ace.txt")
c_parse_short=$(awk '$1 == "parse-f64-short" { print $4 }' "$bench_tmp/c.txt")
ace_parse_long=$(awk '$1 == "parse-f64-long" { print $4 }' "$bench_tmp/ace.txt")
c_parse_long=$(awk '$1 == "parse-f64-long" { print $4 }' "$bench_tmp/c.txt")
if [ -z "$ace_parse" ] || [ "$ace_parse" != "$c_parse" ] ||
   [ -z "$ace_parse_short" ] || [ "$ace_parse_short" != "$c_parse_short" ] ||
   [ -z "$ace_parse_long" ] || [ "$ace_parse_long" != "$c_parse_long" ]; then
    echo "parse checksum mismatch: Ace=$ace_parse C=$c_parse" >&2
    echo "short parse checksum mismatch: Ace=$ace_parse_short C=$c_parse_short" >&2
    echo "long parse checksum mismatch: Ace=$ace_parse_long C=$c_parse_long" >&2
    exit 1
fi

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
