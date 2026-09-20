#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-compile.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM
runs=3

cd "$repo_dir"
make ace
ace=$repo_dir/ace

# Wall-clock milliseconds of one command with its output discarded.
time_ms() {
    perl -MTime::HiRes=time -e '
        open my $out, ">&", \*STDOUT;
        open STDOUT, ">", "/dev/null";
        open STDERR, ">&", \*STDOUT;
        my $start = time;
        system(@ARGV) == 0 or exit 1;
        printf $out "%.2f\n", (time - $start) * 1000;
    ' -- "$@"
}

# Median of $runs runs.
median_ms() {
    i=0
    while [ "$i" -lt "$runs" ]; do
        time_ms "$@"
        i=$((i + 1))
    done | sort -n | sed -n "$(( (runs + 1) / 2 ))p"
}

ace_code_bytes() {
    "$ace" "$@" --report 2>&1 | awk '/machine code:/ { gsub(",", "", $3); print $3 }'
}

c_text_bytes() {
    size -m "$1" | awk '/Section \(__TEXT, __text\)/ { print $NF }'
}

echo "Generated corpus: median of $runs runs, wall-clock ms per compile-to-object"
printf '%-8s %8s %12s %12s %10s %10s %10s %10s\n' \
    "funcs" "lines" "Ace checked" "Ace release" "cc -O0" "cc -O2" "Ace/-O0" "Ace/-O2"
for n in 100 1000 5000; do
    dir=$bench_tmp/n$n
    ./bench/compile-corpus.sh "$n" "$dir"
    lines=$(wc -l < "$dir/corpus.ace" | tr -d ' ')
    ace_checked=$(cd "$dir" && median_ms "$ace" -c corpus.ace)
    ace_release=$(cd "$dir" && median_ms "$ace" --release -c corpus.ace)
    cc_o0=$(cd "$dir" && median_ms cc -std=c11 -O0 -c corpus.c -o corpus-o0.o)
    cc_o2=$(cd "$dir" && median_ms cc -std=c11 -O2 -c corpus.c -o corpus-o2.o)
    awk -v n="$n" -v l="$lines" -v ak="$ace_checked" -v ar="$ace_release" \
        -v c0="$cc_o0" -v c2="$cc_o2" 'BEGIN {
        printf "%-8s %8d %12.2f %12.2f %10.2f %10.2f %9.2fx %9.2fx\n",
               n, l, ak, ar, c0, c2, ar / c0, ar / c2 }'
done

echo
echo "Generated corpus code size, bytes of machine code (5000 functions)"
dir=$bench_tmp/n5000
ace_bytes=$(cd "$dir" && ace_code_bytes -c corpus.ace)
release_bytes=$(cd "$dir" && ace_code_bytes --release -c corpus.ace)
printf '%-14s %10s\n' "Ace checked" "$ace_bytes" "Ace release" "$release_bytes" \
    "cc -O0" "$(c_text_bytes "$dir/corpus-o0.o")" "cc -O2" "$(c_text_bytes "$dir/corpus-o2.o")"

echo
echo "Ace-only real programs (checked, -c), median of $runs runs"
printf '%-34s %8s %10s\n' "program" "lines" "ms"
for f in bench/cpu.ace bench/calls.ace bench/string.ace bench/memory.ace; do
    lines=$(wc -l < "$f" | tr -d ' ')
    cp "$f" "$bench_tmp/prog.ace"
    ms=$(cd "$bench_tmp" && median_ms "$ace" -c prog.ace)
    printf '%-34s %8s %10s\n' "$f" "$lines" "$ms"
done
lines=$(cat demo/http-simple-server/main.ace pkg/http/mod.ace pkg/http/inc/*.ace \
        | wc -l | tr -d ' ')
ms=$(cd demo/http-simple-server && median_ms "$ace" pkg build)
printf '%-34s %8s %10s\n' "demo/http-simple-server (pkg build)" "$lines" "$ms"
