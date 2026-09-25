#!/bin/sh
# Differential check of definite initialization. Generates random functions
# that mix partial struct and array writes with every branch form (if/else if/
# else, switch with and without default, loops, labeled blocks, break label,
# early return) and requires two compilers to report byte-identical
# diagnostics. Run a change against a build of HEAD:
#   tests/definite-init-diff.sh <baseline-ace> <candidate-ace> [count] [seed]
# Every program must reach definite initialization: any other baseline output
# (another diagnostic, a crash) means the generator no longer matches the
# language or the baseline is broken, and the run fails.
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 <baseline-ace> <candidate-ace> [count] [seed]" >&2
    exit 2
fi
old=$1
new=$2
count=${3:-1000}
seed=${4:-1}
for ace in "$old" "$new"; do
    if [ ! -f "$ace" ] || [ ! -x "$ace" ]; then
        echo "$0: not an executable compiler: $ace" >&2
        exit 2
    fi
done
dir=$(mktemp -d "${TMPDIR:-/tmp}/ace-definite-init.XXXXXX")
trap 'rm -rf "$dir"' EXIT HUP INT TERM

generator='
function pick(n) { return int(rand() * n) }

function indent(depth,   s, i) {
    s = ""
    for (i = 0; i < depth; i++) s = s "    "
    return s
}

function block(depth, labels, pad,   i, n) {
    n = 1 + pick(4)
    for (i = 0; i < n; i++) stmt(depth, labels, pad)
}

function stmt(depth, labels, pad,   r, p, kind, name, i, n, open) {
    r = rand()
    p = indent(pad)
    if (depth <= 0 || r < 0.35) { print p writes[1 + pick(nwrites)]; return }
    if (r < 0.5) { print p reads[1 + pick(nreads)]; return }
    if (r < 0.55) {
        name = "t" locals++
        print p name ": int"
        if (rand() < 0.5) print p name " = c"
        print p "sink = " name
        return
    }
    if (r < 0.6 && labels != "") {
        n = split(labels, open, " ")
        print p "if c > " pick(6) " {"
        print p "    break " open[1 + pick(n)]
        print p "}"
        return
    }
    if (r < 0.63) {
        print p "if c > 7 {"
        print p "    return c"
        print p "}"
        return
    }
    kind = pick(7)
    if (kind == 0) {
        print p "if c > " pick(6) " {"
        block(depth - 1, labels, pad + 1)
        print p "}"
    } else if (kind <= 2) {
        print p "if c > " pick(6) " {"
        block(depth - 1, labels, pad + 1)
        if (rand() < 0.3) {
            print p "} else if c > 2 {"
            block(depth - 1, labels, pad + 1)
        }
        print p "} else {"
        block(depth - 1, labels, pad + 1)
        print p "}"
    } else if (kind <= 4) {
        print p "switch c {"
        n = 1 + pick(3)
        for (i = 0; i < n; i++) {
            print p "    " i " {"
            block(depth - 1, labels, pad + 2)
            print p "    }"
        }
        if (kind == 4) {
            print p "    default {"
            block(depth - 1, labels, pad + 2)
            print p "    }"
        }
        print p "}"
    } else if (kind == 5) {
        print p "for i: int in 0 .. c {"
        block(depth - 1, labels, pad + 1)
        print p "}"
    } else {
        name = "l" blocks++
        print p name ": {"
        block(depth - 1, labels == "" ? name : labels " " name, pad + 1)
        print p "}"
    }
}

BEGIN {
    srand(seed * 1000003 + n)
    nwrites = split("x = c|y = c + 1|p.a = c|p.b = 2|p = P{a = 1, b = 2}|arr[0] = 1|arr[1] = c|arr[2] = 3|arr = [0 : 3]|q.p.a = 1|q.p = P{a = 1, b = c}|q.n = 4", writes, "|")
    nreads = split("sink = x|sink = y|sink = p.a|sink = p.b|sink = arr[1]|sink = p.a + p.b|sink = q.p.a|sink = q.n|take(p)|take_arr(arr)|take_q(q)", reads, "|")
    print "P: struct { a: int, b: int }\n"
    print "Q: struct { p: P, n: int }\n"
    print "take(p: P) {}\n"
    print "take_arr(a: [3]int) {}\n"
    print "take_q(q: Q) {}\n"
    print "f(c: int): int {"
    print "    sink: int = 0"
    print "    x: int"
    print "    y: int"
    print "    p: P"
    print "    q: Q"
    print "    arr: [3]int"
    block(4, "", 1)
    print "    return sink"
    print "}\n"
    print "main(): int {"
    print "    return f(1) & 0"
    print "}"
}'

n=0
diagnosed=0
mismatched=0
while [ "$n" -lt "$count" ]; do
    awk -v seed="$seed" -v n="$n" "$generator" > "$dir/case.ace"
    old_rc=0
    old_out=$("$old" "$dir/case.ace" 2>&1) || old_rc=$?
    new_rc=0
    new_out=$("$new" "$dir/case.ace" 2>&1) || new_rc=$?
    # only initialization diagnostics and the compiler's error count
    other=$(printf '%s\n' "$old_out" |
        grep -v -e 'initialized before use$' -e ': [0-9][0-9]* errors*$' -e '^$' || true)
    if [ -n "$other" ]; then
        echo "FAIL: program $n did not reach definite initialization under $old:" >&2
        printf '%s\n' "$other" | head -3 >&2
        exit 1
    fi
    [ -n "$old_out" ] && diagnosed=$((diagnosed + 1))
    if [ "$old_rc" -ne "$new_rc" ] || [ "$old_out" != "$new_out" ]; then
        mismatched=$((mismatched + 1))
        if [ "$mismatched" -eq 1 ]; then
            keep=$(mktemp -d "${TMPDIR:-/tmp}/ace-definite-init-mismatch.XXXXXX")
        fi
        cp "$dir/case.ace" "$keep/case_$n.ace"
    fi
    n=$((n + 1))
done

echo "programs $count (seed $seed), with diagnostics $diagnosed, mismatches $mismatched"
if [ "$mismatched" -ne 0 ]; then
    echo "mismatching programs kept in $keep" >&2
    exit 1
fi
