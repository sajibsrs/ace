#!/bin/sh
# Emit equivalent Ace and C sources into DIR (DIR/corpus.ace, DIR/corpus.c)
# that scale one compiler dimension other than the function count:
#   long   one function with N locals, each reading the previous local and an
#          earlier one, so a single scope grows to N names
#   types  N struct types, each with a function taking it by value, chained
# main prints one checksum so the two programs can be checked for equivalence.
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <long|types> <count> <output-dir>" >&2
    exit 2
fi
shape=$1
count=$2
dir=$3
mkdir -p "$dir"

awk -v shape="$shape" -v n="$count" -v dir="$dir" '
BEGIN {
    ace = dir "/corpus.ace"
    c = dir "/corpus.c"
    print "import fmt\n\nMASK: const usize = 0x7FFFFFFF\n" > ace
    print "#include <stdint.h>\n#include <stdio.h>\n" > c
    print "typedef uint64_t usize;\n#define MASK 0x7FFFFFFFull\n" > c

    if (shape == "long") {
        print "work(a: usize): usize {" > ace
        print "usize work(usize a) {" > c
        for (k = 0; k < n; k++) {
            prev = k > 0 ? "v" (k - 1) : "a"
            mid = "v" int(k / 2)
            if (k == 0) mid = "a"
            printf "    v%d: usize = (%s * 3 + %s + %d) & MASK\n", k, prev, mid, k % 7 > ace
            printf "    usize v%d = (%s * 3 + %s + %d) & MASK;\n", k, prev, mid, k % 7 > c
        }
        printf "    return v%d\n}\n\n", n - 1 > ace
        printf "    return v%d;\n}\n\n", n - 1 > c
        printf "main(): int {\n    fmt.printf(\"checksum: %%u\\n\", work(usize(12345)))\n    return 0\n}\n" > ace
        printf "int main(void) {\n    printf(\"checksum: %%llu\\n\", (unsigned long long)work(12345));\n    return 0;\n}\n" > c
    } else if (shape == "types") {
        for (k = 0; k < n; k++) {
            printf "T%d: struct {\n    a: usize,\n    b: usize,\n}\n\n", k > ace
            printf "g%d(x: usize): usize {\n    p: T%d = T%d {a = x, b = %d}\n", k, k, k, k % 11 + 1 > ace
            printf "    return ((p.a * 3 + p.b) + %s) & MASK\n}\n\n", (k > 0 ? "g" (k - 1) "(x & 255)" : "0") > ace

            printf "typedef struct { usize a, b; } T%d;\n", k > c
            printf "usize g%d(usize x) {\n    T%d p = {x, %d};\n", k, k, k % 11 + 1 > c
            printf "    return ((p.a * 3 + p.b) + %s) & MASK;\n}\n\n", (k > 0 ? "g" (k - 1) "(x & 255)" : "0") > c
        }
        printf "main(): int {\n    fmt.printf(\"checksum: %%u\\n\", g%d(usize(12345)))\n    return 0\n}\n", n - 1 > ace
        printf "int main(void) {\n    printf(\"checksum: %%llu\\n\", (unsigned long long)g%d(12345));\n    return 0;\n}\n", n - 1 > c
    } else {
        print "unknown shape: " shape > "/dev/stderr"
        exit 2
    }
}
'
