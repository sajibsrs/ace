#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-calls.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/calls-ace" bench/core/calls.ace
cc -std=c11 -O2 -o "$bench_tmp/calls-c" bench/core/calls.c
# C++ builds with clang++ -std=c++20 -O2 (C++20 for tree.cpp's designated
# initializers; the other benches build the same for one rule).
clang++ -std=c++20 -O2 -o "$bench_tmp/calls-cpp" bench/core/calls.cpp
# Rust builds with rustc -C opt-level=2 (as -O2) -C codegen-units=1 (one
# unit, as C's one translation unit) -C overflow-checks=off (C's unsigned
# wrap; also the opt-level default). Bounds checks stay on, as written.
rustc -C opt-level=2 -C codegen-units=1 -C overflow-checks=off -o "$bench_tmp/calls-rs" bench/core/calls.rs
cc -std=c11 -O2 -DALLOW_INLINE -o "$bench_tmp/calls-c-inline" bench/core/calls.c

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/calls-ace" > /dev/null
"$bench_tmp/calls-c" > /dev/null
"$bench_tmp/calls-cpp" > /dev/null
"$bench_tmp/calls-rs" > /dev/null
"$bench_tmp/calls-c-inline" > /dev/null

"$bench_tmp/calls-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/calls-c" > "$bench_tmp/c.txt"
"$bench_tmp/calls-cpp" > "$bench_tmp/cpp.txt"
"$bench_tmp/calls-rs" > "$bench_tmp/rs.txt"
"$bench_tmp/calls-c-inline" > "$bench_tmp/c-inline.txt"

for variant in c c-inline cpp rs; do
    if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
              END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/$variant.txt"; then
        echo "checksum mismatch between Ace and $variant" >&2
        exit 1
    fi
done

printf '%-16s %12s %12s %12s %12s %14s %10s %10s %10s\n' "workload" "Ace ns/op" "C ns/op" "C++ ns/op" "Rust ns/op" "C inlined" "Ace/C" "C++/C" "Rust/C"
awk '
    FILENAME == ARGV[1] { c[$1] = $2 / $3; next }
    FILENAME == ARGV[2] { cpp[$1] = $2 / $3; next }
    FILENAME == ARGV[3] { rs[$1] = $2 / $3; next }
    FILENAME == ARGV[4] { x[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.2f %12.2f %12.2f %12.2f %14.2f %9.2fx %9.2fx %9.2fx\n", $1, ace, c[$1], cpp[$1], rs[$1], x[$1], ace / c[$1], cpp[$1] / c[$1], rs[$1] / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/cpp.txt" "$bench_tmp/rs.txt" "$bench_tmp/c-inline.txt" "$bench_tmp/ace.txt"
