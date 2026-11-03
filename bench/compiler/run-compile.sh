#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
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

# Machine code of a C object: Mach-O's __text, or ELF's .text sections
# (.text.startup holds main at -O2).
c_text_bytes() {
    case $(uname -s) in
        Darwin) size -m "$1" | awk '/Section \(__TEXT, __text\)/ { print $NF }' ;;
        *) size -A "$1" | awk '/^\.text/ { n += $2 } END { print n }' ;;
    esac
}

# Growth of a time against the previous size's time, blank for the first size.
growth() {
    awk -v cur="$1" -v prev="$2" 'BEGIN {
        if (prev > 0) printf "%.1fx", cur / prev; else printf "-" }'
}

# Rust is optional: its columns read n/a without rustc.
rust_ms() {
    if command -v rustc > /dev/null; then
        median_ms rustc --edition 2021 -C opt-level="$1" --emit=obj corpus.rs -o "corpus-rs$1.o"
    else
        echo n/a
    fi
}

echo "Generated corpus: median of $runs runs, wall-clock ms per compile-to-object"
echo "(growth = Ace debug time over the previous size; linear code growth is the size ratio)"
printf '%-8s %8s %12s %12s %10s %10s %10s %10s %8s\n' \
    "funcs" "lines" "Ace debug" "Ace release" "cc -O0" "cc -O2" "Ace/-O0" "Ace/-O2" "growth"
prev=0
for n in 100 1000 5000 20000; do
    dir=$bench_tmp/n$n
    ./bench/compiler/compile-corpus.sh "$n" "$dir"
    lines=$(wc -l < "$dir/corpus.ace" | tr -d ' ')
    ace_debug=$(cd "$dir" && median_ms "$ace" --debug -c corpus.ace)
    ace_release=$(cd "$dir" && median_ms "$ace" --release -c corpus.ace)
    cc_o0=$(cd "$dir" && median_ms cc -std=c11 -O0 -c corpus.c -o corpus-o0.o)
    cc_o2=$(cd "$dir" && median_ms cc -std=c11 -O2 -c corpus.c -o corpus-o2.o)
    awk -v n="$n" -v l="$lines" -v ak="$ace_debug" -v ar="$ace_release" \
        -v c0="$cc_o0" -v c2="$cc_o2" -v g="$(growth "$ace_debug" "$prev")" 'BEGIN {
        printf "%-8s %8d %12.2f %12.2f %10.2f %10.2f %9.2fx %9.2fx %8s\n",
               n, l, ak, ar, c0, c2, ar / c0, ar / c2, g }'
    prev=$ace_debug
done

echo
echo "Same corpus in C++ (clang++ -std=c++17) and Rust (rustc --edition 2021),"
echo "median of $runs runs, wall-clock ms per compile-to-object"
printf '%-8s %10s %10s %12s %12s\n' "funcs" "c++ -O0" "c++ -O2" "rustc -O0" "rustc -O2"
for n in 100 1000 5000 20000; do
    dir=$bench_tmp/n$n
    cxx_o0=$(cd "$dir" && median_ms clang++ -std=c++17 -O0 -c corpus.cpp -o corpus-cxx0.o)
    cxx_o2=$(cd "$dir" && median_ms clang++ -std=c++17 -O2 -c corpus.cpp -o corpus-cxx2.o)
    rs_o0=$(cd "$dir" && rust_ms 0)
    rs_o2=$(cd "$dir" && rust_ms 2)
    printf '%-8s %10s %10s %12s %12s\n' "$n" "$cxx_o0" "$cxx_o2" "$rs_o0" "$rs_o2"
done

# Second scaling dimension: one function with many locals, and many types.
# The small-size cc comparison is flattered by cc's startup and header cost;
# the growth column is the signal.
for shape in long types; do
    echo
    echo "Scaling shape '$shape': median of $runs runs, wall-clock ms per compile-to-object"
    printf '%-8s %8s %12s %10s %10s %8s\n' "count" "lines" "Ace debug" "cc -O0" "cc -O2" "growth"
    prev=0
    for n in 1000 5000 20000; do
        dir=$bench_tmp/$shape$n
        ./bench/compiler/scaling-corpus.sh "$shape" "$n" "$dir"
        lines=$(wc -l < "$dir/corpus.ace" | tr -d ' ')
        ace_debug=$(cd "$dir" && median_ms "$ace" --debug -c corpus.ace)
        cc_o0=$(cd "$dir" && median_ms cc -std=c11 -O0 -c corpus.c -o corpus-o0.o)
        # clang -O2 crashes on the largest 'long' corpus; say so, not 0.00
        cc_o2=$(cd "$dir" && median_ms cc -std=c11 -O2 -c corpus.c -o corpus-o2.o)
        [ -n "$cc_o2" ] || cc_o2=failed
        printf '%-8s %8s %12.2f %10.2f %10s %8s\n' "$n" "$lines" "$ace_debug" "$cc_o0" \
            "$cc_o2" "$(growth "$ace_debug" "$prev")"
        prev=$ace_debug
    done
done

echo
echo "Idiomatic generic corpus (bench/compiler/generic-corpus.sh, 5000 functions):"
echo "containers, generics, polymorphism, derived equality in each language's own"
echo "idiom; median of $runs runs, wall-clock ms per compile-to-object"
dir=$bench_tmp/generic5000
./bench/compiler/generic-corpus.sh 5000 "$dir"
printf '%-16s %10s\n' \
    "Ace --debug" "$(cd "$dir" && median_ms "$ace" --debug -c corpus.ace)" \
    "Ace --release" "$(cd "$dir" && median_ms "$ace" --release -c corpus.ace)" \
    "cc -O0" "$(cd "$dir" && median_ms cc -std=c11 -O0 -c corpus.c -o c0.o)" \
    "cc -O2" "$(cd "$dir" && median_ms cc -std=c11 -O2 -c corpus.c -o c2.o)" \
    "clang++ -O0" "$(cd "$dir" && median_ms clang++ -std=c++20 -O0 -c corpus.cpp -o x0.o)" \
    "clang++ -O2" "$(cd "$dir" && median_ms clang++ -std=c++20 -O2 -c corpus.cpp -o x2.o)" \
    "rustc -O0" "$(cd "$dir" && rust_ms 0)" \
    "rustc -O2" "$(cd "$dir" && rust_ms 2)"

echo
echo "Generated corpus code size, bytes of machine code (5000 functions)"
dir=$bench_tmp/n5000
ace_bytes=$(cd "$dir" && ace_code_bytes --debug -c corpus.ace)
release_bytes=$(cd "$dir" && ace_code_bytes --release -c corpus.ace)
printf '%-14s %10s\n' "Ace debug" "$ace_bytes" "Ace release" "$release_bytes" \
    "cc -O0" "$(c_text_bytes "$dir/corpus-o0.o")" "cc -O2" "$(c_text_bytes "$dir/corpus-o2.o")"

echo
echo "Ace-only real programs (debug, -c), median of $runs runs"
printf '%-34s %8s %10s\n' "program" "lines" "ms"
for f in bench/core/cpu.ace bench/core/calls.ace bench/std/string.ace bench/std/memory.ace; do
    lines=$(wc -l < "$f" | tr -d ' ')
    cp "$f" "$bench_tmp/prog.ace"
    ms=$(cd "$bench_tmp" && median_ms "$ace" --debug -c prog.ace)
    printf '%-34s %8s %10s\n' "$f" "$lines" "$ms"
done
lines=$(cat demo/http-simple-server/main.ace pkg/http/mod.ace pkg/http/inc/*.ace \
        | wc -l | tr -d ' ')
ms=$(cd demo/http-simple-server && median_ms "$ace" pkg build)
printf '%-34s %8s %10s\n' "demo/http-simple-server (pkg build)" "$lines" "$ms"
