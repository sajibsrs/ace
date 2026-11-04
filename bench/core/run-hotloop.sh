#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-hotloop.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/hotloop-ace" bench/core/hotloop.ace
# Scalar C is the Ace/C column; C's auto-vectorized build is the reference
# for Ace's own auto-vectorizer (spec Compiler #6). clang and gcc both take
# gcc's spelling of the vectorizers' switches.
cc -std=c11 -O2 -fno-tree-vectorize -fno-tree-slp-vectorize -o "$bench_tmp/hotloop-c" bench/core/hotloop.c
# C++ builds with clang++ -std=c++20 -O2 (C++20 for tree.cpp's designated
# initializers; the other benches build the same for one rule).
clang++ -std=c++20 -O2 -fno-tree-vectorize -fno-tree-slp-vectorize -o "$bench_tmp/hotloop-cpp" bench/core/hotloop.cpp
# Rust builds with rustc -C opt-level=2 (as -O2) -C codegen-units=1 (one
# unit, as C's one translation unit) -C overflow-checks=off (C's unsigned
# wrap; also the opt-level default). Bounds checks stay on, as written.
rustc -C opt-level=2 -C codegen-units=1 -C overflow-checks=off -C no-vectorize-loops -C no-vectorize-slp -o "$bench_tmp/hotloop-rs" bench/core/hotloop.rs
cc -std=c11 -O2 -o "$bench_tmp/hotloop-c-vector" bench/core/hotloop.c

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/hotloop-ace" > /dev/null
"$bench_tmp/hotloop-c" > /dev/null
"$bench_tmp/hotloop-cpp" > /dev/null
"$bench_tmp/hotloop-rs" > /dev/null
"$bench_tmp/hotloop-c-vector" > /dev/null

"$bench_tmp/hotloop-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/hotloop-c" > "$bench_tmp/c.txt"
"$bench_tmp/hotloop-cpp" > "$bench_tmp/cpp.txt"
"$bench_tmp/hotloop-rs" > "$bench_tmp/rs.txt"
"$bench_tmp/hotloop-c-vector" > "$bench_tmp/c-vector.txt"

for variant in c c-vector cpp rs; do
    if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
              END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/$variant.txt"; then
        echo "checksum mismatch between Ace and $variant" >&2
        exit 1
    fi
done

printf '%-16s %12s %12s %12s %12s %14s %10s %10s %10s\n' "workload" "Ace ns/op" "C ns/op" "C++ ns/op" "Rust ns/op" "C vector" "Ace/C" "C++/C" "Rust/C"
awk '
    FILENAME == ARGV[1] { c[$1] = $2 / $3; next }
    FILENAME == ARGV[2] { cpp[$1] = $2 / $3; next }
    FILENAME == ARGV[3] { rs[$1] = $2 / $3; next }
    FILENAME == ARGV[4] { x[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.3f %12.3f %12.3f %12.3f %14.3f %9.2fx %9.2fx %9.2fx\n", $1, ace, c[$1], cpp[$1], rs[$1], x[$1], ace / c[$1], cpp[$1] / c[$1], rs[$1] / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/cpp.txt" "$bench_tmp/rs.txt" "$bench_tmp/c-vector.txt" "$bench_tmp/ace.txt"
