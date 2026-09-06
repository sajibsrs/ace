#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-memory.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

for mode in checked release; do
    echo "Ace $mode compiler report"
    echo "--------------------------"
    ./ace "--$mode" --report -o "$bench_tmp/memory-$mode" bench/memory.ace

    echo
    echo "Ace $mode runtime and allocator report"
    echo "--------------------------------------"
    # Validate correctness independently: some restricted macOS environments
    # deny a sysctl used by /usr/bin/time -l and make time itself return nonzero.
    "$bench_tmp/memory-$mode" >/dev/null
    if ! /usr/bin/time -l "$bench_tmp/memory-$mode"; then
        echo "warning: process statistics were unavailable; program validation passed" >&2
    fi
    echo
done
