#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bench_tmp=$(mktemp -d "${TMPDIR:-/tmp}/ace-poll.XXXXXX")
trap 'rm -rf "$bench_tmp"' EXIT HUP INT TERM

cd "$repo_dir"
make ace

./ace --release --report -o "$bench_tmp/poll-ace" bench/std/poll.ace

# Discard one run: see bench/README.md on first-run cost.
"$bench_tmp/poll-ace" > /dev/null
"$bench_tmp/poll-ace"
