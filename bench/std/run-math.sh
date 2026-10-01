#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-math.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/math-ace" bench/std/math.ace
# Scalar C, as bench/core/run-hotloop.sh: Ace leaves vectorization to `simd`.
cc -std=c11 -O2 -fno-tree-vectorize -fno-tree-slp-vectorize -o "$bench_tmp/math-c" bench/std/math.c -lm

# Discard one run of each binary: see bench/README.md on first-run cost.
"$bench_tmp/math-ace" > /dev/null
"$bench_tmp/math-c" > /dev/null

"$bench_tmp/math-ace" > "$bench_tmp/ace.txt"
"$bench_tmp/math-c" > "$bench_tmp/c.txt"

# Classification is exact, so its checksum must agree; libm may round a
# transcendental result differently in the last bit, so those only warn.
awk 'NR == FNR { sum[$1] = $4; next }
     sum[$1] != $4 {
         if ($1 == "classify") { bad = 1 }
         else { printf "warning: %s checksum differs from libm\n", $1 > "/dev/stderr" }
     }
     END { exit bad }' "$bench_tmp/c.txt" "$bench_tmp/ace.txt" || {
    echo "classify checksum mismatch between Ace and C" >&2
    exit 1
}

printf '%-12s %12s %12s %9s\n' "workload" "Ace ns/op" "C ns/op" "Ace/C"
awk '
    NR == FNR { c[$1] = $2 / $3; next }
    {
        ace = $2 / $3
        printf "%-12s %12.2f %12.2f %8.2fx\n", $1, ace, c[$1], ace / c[$1]
    }
' "$bench_tmp/c.txt" "$bench_tmp/ace.txt"

echo
echo "Ace checksums"
cat "$bench_tmp/ace.txt"
