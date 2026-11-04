#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-raytrace.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --debug --report -o "$bench_tmp/raytrace-debug" bench/core/raytrace.ace
./ace --release --report -o "$bench_tmp/raytrace-release" bench/core/raytrace.ace
cc -std=c11 -O0 -o "$bench_tmp/raytrace-c-o0" bench/core/raytrace.c -lm
cc -std=c11 -O2 -o "$bench_tmp/raytrace-c-o2" bench/core/raytrace.c -lm
# C++ builds with clang++ -std=c++20 -O2 (C++20 for tree.cpp's designated
# initializers; the other benches build the same for one rule).
clang++ -std=c++20 -O2 -o "$bench_tmp/raytrace-cpp-o2" bench/core/raytrace.cpp
# Rust builds with rustc -C opt-level=2 (as -O2) -C codegen-units=1 (one
# unit, as C's one translation unit) -C overflow-checks=off (C's unsigned
# wrap; also the opt-level default). Bounds checks stay on, as written.
rustc -C opt-level=2 -C codegen-units=1 -C overflow-checks=off -o "$bench_tmp/raytrace-rs-o2" bench/core/raytrace.rs

expected=
for variant in ace-debug ace-release c-o0 c-o2 cpp-o2 rs-o2; do
    case "$variant" in
        ace-debug) exe="$bench_tmp/raytrace-debug" ;;
        ace-release) exe="$bench_tmp/raytrace-release" ;;
        c-o0) exe="$bench_tmp/raytrace-c-o0" ;;
        c-o2) exe="$bench_tmp/raytrace-c-o2" ;;
        cpp-o2) exe="$bench_tmp/raytrace-cpp-o2" ;;
        rs-o2) exe="$bench_tmp/raytrace-rs-o2" ;;
    esac

    echo
    echo "$variant"
    echo "-----------"
    $exe > /dev/null # discard one run: see bench/README.md on first-run cost
    output=$($exe)
    printf '%s\n' "$output"
    elapsed=$(printf '%s\n' "$output" | awk '/^elapsed ns:/ { print $3 }')
    echo "$variant $elapsed" >> "$bench_tmp/elapsed.txt"
    checksum=$(printf '%s\n' "$output" | awk '/^checksum:/ { print $2 }')
    if [ -z "$expected" ]; then
        expected=$checksum
    elif [ "$checksum" != "$expected" ]; then
        echo "checksum mismatch: got $checksum, expected $expected" >&2
        exit 1
    fi
done

echo
printf '%-12s %14s %10s\n' "variant" "elapsed ns" "vs C -O2"
awk 'NR == FNR { if ($1 == "c-o2") c = $2; next }
     { printf "%-12s %14d %9.2fx\n", $1, $2, $2 / c }' \
    "$bench_tmp/elapsed.txt" "$bench_tmp/elapsed.txt"
