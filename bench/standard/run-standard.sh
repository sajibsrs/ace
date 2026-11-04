#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-standard.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

benches="nbody spectralnorm fannkuch binarytrees mandelbrot"
variants="ace-debug ace-release c-o0 c-o2 cpp-o2 rs-o2"

for bench in $benches; do
    src=bench/standard/$bench
    ./ace --debug -o "$bench_tmp/$bench-ace-debug" "$src.ace"
    ./ace --release -o "$bench_tmp/$bench-ace-release" "$src.ace"
    # -ffp-contract=off keeps C and C++ from fusing a multiply and add, which
    # Ace and Rust never do: every variant computes the same float operations.
    cc -std=c11 -O0 -ffp-contract=off -o "$bench_tmp/$bench-c-o0" "$src.c" -lm
    cc -std=c11 -O2 -ffp-contract=off -o "$bench_tmp/$bench-c-o2" "$src.c" -lm
    clang++ -std=c++20 -O2 -ffp-contract=off -o "$bench_tmp/$bench-cpp-o2" "$src.cpp"
    # Rust builds as in bench/core: -O2's opt-level, one codegen unit, C's
    # unsigned wrap; bounds checks stay on, as written.
    rustc -C opt-level=2 -C codegen-units=1 -C overflow-checks=off -o "$bench_tmp/$bench-rs-o2" "$src.rs"

    echo
    echo "$bench"
    echo "-----------"
    expected=
    for variant in $variants; do
        exe="$bench_tmp/$bench-$variant"
        $exe > /dev/null # discard one run: see bench/README.md on first-run cost
        output=$($exe)
        elapsed=$(printf '%s\n' "$output" | awk '/^elapsed ns:/ { print $3 }')
        checksum=$(printf '%s\n' "$output" | awk '/^checksum:/ { print $2 }')
        printf '%-12s %14s  checksum %s\n' "$variant" "$elapsed" "$checksum"
        echo "$bench $variant $elapsed" >> "$bench_tmp/elapsed.txt"
        if [ -z "$expected" ]; then
            expected=$checksum
        elif [ "$checksum" != "$expected" ]; then
            echo "$bench checksum mismatch: $variant got $checksum, expected $expected" >&2
            exit 1
        fi
    done
done

echo
echo "elapsed vs C -O2"
printf '%-14s' "bench"
for variant in $variants; do printf ' %11s' "$variant"; done
echo
awk -v variants="$variants" '
    { t[$1, $2] = $3; if (!($1 in seen)) { seen[$1] = 1; order[++n] = $1 } }
    END {
        nv = split(variants, v, " ")
        for (i = 1; i <= n; i++) {
            printf "%-14s", order[i]
            for (j = 1; j <= nv; j++) printf " %10.2fx", t[order[i], v[j]] / t[order[i], "c-o2"]
            printf "\n"
        }
    }' "$bench_tmp/elapsed.txt"
