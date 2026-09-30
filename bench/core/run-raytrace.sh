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

expected=
for variant in ace-debug ace-release c-o0 c-o2; do
    case "$variant" in
        ace-debug) exe="$bench_tmp/raytrace-debug" ;;
        ace-release) exe="$bench_tmp/raytrace-release" ;;
        c-o0) exe="$bench_tmp/raytrace-c-o0" ;;
        c-o2) exe="$bench_tmp/raytrace-c-o2" ;;
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
