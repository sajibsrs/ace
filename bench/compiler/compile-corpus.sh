#!/bin/sh
# Emit equivalent Ace and C sources with N call-chained functions into DIR
# (DIR/corpus.ace, DIR/corpus.c). Three function shapes alternate — a bounded
# loop with branches, by-value struct passing, and early-return branches — and
# main prints one checksum so the two programs can be checked for equivalence.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <function-count> <output-dir>" >&2
    exit 2
fi
count=$1
dir=$2
mkdir -p "$dir"

awk -v n="$count" -v dir="$dir" '
function call(k) { return k > 0 ? "fn_" (k - 1) "(s & 255, b)" : "b" }
function call2(k) { return k > 0 ? "fn_" (k - 1) "(t & 255, b + 1)" : "b" }
BEGIN {
    ace = dir "/corpus.ace"
    c = dir "/corpus.c"

    print "import fmt\n\nMASK: const usize = 0x7FFFFFFF\n" > ace
    print "Pair: struct {\n    a: usize,\n    b: usize,\n}\n" > ace
    print "step(p: Pair): Pair {\n    return Pair {a = (p.a * 48271 + p.b) & MASK, b = (p.b + 1) & MASK}\n}\n" > ace

    print "#include <stdint.h>\n#include <stdio.h>\n" > c
    print "typedef uint64_t usize;\n#define MASK 0x7FFFFFFFull\n" > c
    print "typedef struct { usize a, b; } Pair;\n" > c
    print "Pair step(Pair p) { return (Pair){(p.a * 48271 + p.b) & MASK, (p.b + 1) & MASK}; }\n" > c
    for (k = 0; k < n; k++) print "usize fn_" k "(usize a, usize b);" > c
    print "" > c

    for (k = 0; k < n; k++) {
        m = k % 3
        mul = k % 13 + 1
        if (m == 0) {
            printf "fn_%d(a: usize, b: usize): usize {\n    s: usize = a\n", k > ace
            printf "    for i: usize in 0 .. 8 {\n        if (s + i) %% 3 == 0 {\n", k > ace
            printf "            s = (s * 7 + b) & MASK\n        } else {\n", k > ace
            printf "            s = (s + i * %d) & MASK\n        }\n    }\n", mul > ace
            printf "    return (s + %s) & MASK\n}\n\n", call(k) > ace

            printf "usize fn_%d(usize a, usize b) {\n    usize s = a;\n", k > c
            printf "    for (usize i = 0; i < 8; i++) {\n        if ((s + i) %% 3 == 0) {\n", k > c
            printf "            s = (s * 7 + b) & MASK;\n        } else {\n", k > c
            printf "            s = (s + i * %d) & MASK;\n        }\n    }\n", mul > c
            printf "    return (s + %s) & MASK;\n}\n\n", call(k) > c
        } else if (m == 1) {
            printf "fn_%d(a: usize, b: usize): usize {\n", k > ace
            printf "    p: Pair = Pair {a = a, b = b}\n    p = step(p)\n    q: Pair = step(p)\n", k > ace
            printf "    s: usize = (p.a + q.b) & MASK\n", k > ace
            printf "    return (s + %s) & MASK\n}\n\n", call(k) > ace

            printf "usize fn_%d(usize a, usize b) {\n", k > c
            printf "    Pair p = {a, b};\n    p = step(p);\n    Pair q = step(p);\n", k > c
            printf "    usize s = (p.a + q.b) & MASK;\n", k > c
            printf "    return (s + %s) & MASK;\n}\n\n", call(k) > c
        } else {
            printf "fn_%d(a: usize, b: usize): usize {\n", k > ace
            printf "    t: usize = (a * %d + b) & MASK\n    if t < 1000 {\n", mul > ace
            printf "        return (t + %s) & MASK\n    }\n", call2(k) > ace
            printf "    if t < 100000 {\n        return ((t >> 3) + %s) & MASK\n    }\n", call2(k) > ace
            printf "    return (t >> 7) & MASK\n}\n\n" > ace

            printf "usize fn_%d(usize a, usize b) {\n", k > c
            printf "    usize t = (a * %d + b) & MASK;\n    if (t < 1000) {\n", mul > c
            printf "        return (t + %s) & MASK;\n    }\n", call2(k) > c
            printf "    if (t < 100000) {\n        return ((t >> 3) + %s) & MASK;\n    }\n", call2(k) > c
            printf "    return (t >> 7) & MASK;\n}\n\n" > c
        }
    }

    printf "main(): int {\n    result: usize = fn_%d(12345, 678)\n    fmt.printf(\"checksum: %%u\\n\", result)\n    return 0\n}\n", n - 1 > ace
    printf "int main(void) {\n    printf(\"checksum: %%llu\\n\", (unsigned long long)fn_%d(12345, 678));\n    return 0;\n}\n", n - 1 > c
}
'
