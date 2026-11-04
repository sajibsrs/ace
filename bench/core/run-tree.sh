#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-tree.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/tree-ace" bench/core/tree.ace
cc -std=c11 -O2 -o "$bench_tmp/tree-c" bench/core/tree.c
# C++ builds with clang++ -std=c++20 -O2 (C++20 for tree.cpp's designated
# initializers; the other benches build the same for one rule).
clang++ -std=c++20 -O2 -o "$bench_tmp/tree-cpp" bench/core/tree.cpp
# Rust builds with rustc -C opt-level=2 (as -O2) -C codegen-units=1 (one
# unit, as C's one translation unit) -C overflow-checks=off (C's unsigned
# wrap; also the opt-level default). Bounds checks stay on, as written.
rustc -C opt-level=2 -C codegen-units=1 -C overflow-checks=off -o "$bench_tmp/tree-rs" bench/core/tree.rs

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/tree-ace" > /dev/null
"$bench_tmp/tree-c" > /dev/null
"$bench_tmp/tree-cpp" > /dev/null
"$bench_tmp/tree-rs" > /dev/null

"$bench_tmp/tree-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/tree-c" > "$bench_tmp/c.txt"
"$bench_tmp/tree-cpp" > "$bench_tmp/cpp.txt"
"$bench_tmp/tree-rs" > "$bench_tmp/rs.txt"

for variant in c cpp rs; do
    if ! awk 'NR == FNR { sum[$1] = $4; next } sum[$1] != $4 { bad = 1 }
              END { exit bad }' "$bench_tmp/ace.txt" "$bench_tmp/$variant.txt"; then
        echo "checksum mismatch between Ace and $variant" >&2
        exit 1
    fi
done

printf '%-16s %12s %12s %12s %12s %10s %10s %10s\n' "workload" "Ace ns/node" "C ns/node" "C++ ns/node" "Rust ns/node" "Ace/C" "C++/C" "Rust/C"
awk '
    FILENAME == ARGV[1] { c[$1] = $2 / $3; next }
    FILENAME == ARGV[2] { cpp[$1] = $2 / $3; next }
    FILENAME == ARGV[3] { rs[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-16s %12.2f %12.2f %12.2f %12.2f %9.2fx %9.2fx %9.2fx\n", $1, ace, c[$1], cpp[$1], rs[$1], ace / c[$1], cpp[$1] / c[$1], rs[$1] / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/cpp.txt" "$bench_tmp/rs.txt" "$bench_tmp/ace.txt"
