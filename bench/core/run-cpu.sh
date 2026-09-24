#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-cpu.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --checked --report -o "$bench_tmp/cpu-checked" bench/core/cpu.ace
./ace --release --report -o "$bench_tmp/cpu-release" bench/core/cpu.ace
cc -std=c11 -O0 -o "$bench_tmp/cpu-c-o0" bench/core/cpu.c
cc -std=c11 -O2 -o "$bench_tmp/cpu-c-o2" bench/core/cpu.c

expected=
for variant in ace-checked ace-release c-o0 c-o2; do
    case "$variant" in
        ace-checked) exe="$bench_tmp/cpu-checked" ;;
        ace-release) exe="$bench_tmp/cpu-release" ;;
        c-o0) exe="$bench_tmp/cpu-c-o0" ;;
        c-o2) exe="$bench_tmp/cpu-c-o2" ;;
    esac

    echo
    echo "$variant"
    echo "-----------"
    $exe > /dev/null # discard one run: see bench/README.md on first-run cost
    output=$($exe)
    printf '%s\n' "$output"
    checksum=$(printf '%s\n' "$output" | awk '/^checksum:/ { print $2 }')
    if [ -z "$expected" ]; then
        expected=$checksum
    elif [ "$checksum" != "$expected" ]; then
        echo "checksum mismatch: got $checksum, expected $expected" >&2
        exit 1
    fi
done
