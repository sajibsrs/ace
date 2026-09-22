#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-regex.XXXXXX")
trap 'rm -rf "$bench_tmp" "$repo_dir/bench/regex/build"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

# The Ace side imports pkg/regex, so it is a package built in release mode.
(cd bench/regex && ../../ace pkg build --release > /dev/null)
cp bench/regex/build/regex_bench "$bench_tmp/regex-ace"
cc -std=c11 -O2 -o "$bench_tmp/regex-c" bench/regex.c

# PCRE2 is an optional reference column: skipped when not installed.
pcre2=no
if command -v pcre2-config > /dev/null 2>&1; then
    # shellcheck disable=SC2046
    cc -std=c11 -O2 $(pcre2-config --cflags) -o "$bench_tmp/regex-pcre2" \
        bench/regex-pcre2.c $(pcre2-config --libs8)
    pcre2=yes
fi

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/regex-ace" > /dev/null
"$bench_tmp/regex-c" > /dev/null

"$bench_tmp/regex-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/regex-c" > "$bench_tmp/c.txt"

if [ "$pcre2" = yes ]; then
    "$bench_tmp/regex-pcre2" interp > "$bench_tmp/pcre2.txt"
    "$bench_tmp/regex-pcre2" jit > /dev/null
    "$bench_tmp/regex-pcre2" jit > "$bench_tmp/pcre2-jit.txt"
else
    : > "$bench_tmp/pcre2.txt"
    : > "$bench_tmp/pcre2-jit.txt"
fi

# The Ace and C VMs run identical programs, so any checksum difference is a
# bug. PCRE2 is also leftmost-first; a difference there is reported, not fatal.
if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
          END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/c.txt"; then
    echo "checksum mismatch between Ace and C" >&2
    exit 1
fi
for variant in pcre2 pcre2-jit; do
    if [ -s "$bench_tmp/$variant.txt" ] &&
       ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
              END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/$variant.txt"; then
        echo "warning: checksum mismatch between Ace and $variant" >&2
    fi
done

printf '%-14s %12s %12s %8s %12s %12s\n' \
    "workload" "Ace ns/B" "C ns/B" "Ace/C" "PCRE2 ns/B" "JIT ns/B"
awk '
    FILENAME == ARGV[1] { c[$1] = $2 / $3; next }
    FILENAME == ARGV[2] { p[$1] = $2 / $3; next }
    FILENAME == ARGV[3] { j[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        pv = ($1 in p) ? sprintf("%12.2f", p[$1]) : sprintf("%12s", "-")
        jv = ($1 in j) ? sprintf("%12.2f", j[$1]) : sprintf("%12s", "-")
        printf "%-14s %12.2f %12.2f %7.2fx %s %s\n", $1, ace, c[$1], ace / c[$1], pv, jv
    }
' "$bench_tmp/c.txt" "$bench_tmp/pcre2.txt" "$bench_tmp/pcre2-jit.txt" "$bench_tmp/ace.txt"

echo
echo "Ace checksums"
cat "$bench_tmp/ace.txt"
