#!/bin/sh
# Emit one program with N functions written idiomatically in Ace, C, C++ and
# Rust into DIR (corpus.ace, .c, .cpp, .rs). Four shapes alternate, each using
# the language's own abstraction: a generic container (Array[T], VEC_OF(T),
# std::vector<T>, Vec<T>), generic functions (explicit generics, macros,
# templates, bounded generics), polymorphism (contracts, function-pointer
# tables, virtual, dyn traits) and derived equality (hand-written in Ace and C,
# defaulted operator== in C++, #[derive] in Rust). Element types cycle through
# i64, i32, u32 and u64. main prints one checksum to check equivalence.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <function-count> <output-dir>" >&2
    exit 2
fi
mkdir -p "$2"

awk -v n="$1" -v dir="$2" '
BEGIN {
    split("i64 i32 u32 u64", AT, " ")
    split("int64_t int32_t uint32_t uint64_t", CT, " ")
    split("i64 i32 u32 u64", RT, " ")
    ace = dir "/corpus.ace"; c = dir "/corpus.c"; cpp = dir "/corpus.cpp"; rs = dir "/corpus.rs"

    # Ace prelude: shared generics and the contract.
    print "import fmt\nimport array\n" > ace
    print "sum[T](xs: []T): T {\n    s: T = 0\n    for i: usize in 0 .. xs.len { s = s + xs[i] }\n    return s\n}\n" > ace
    print "scale[T](xs: []T, m: T) {\n    for i: usize in 0 .. xs.len { xs[i] = xs[i] * m }\n}\n" > ace
    print "find[T](xs: []T, v: T): usize {\n    for i: usize in 0 .. xs.len { if xs[i] == v { return i } }\n    return xs.len\n}\n" > ace
    print "Shape: contract {\n    area(self): u64\n    perim(self): u64\n}\n" > ace

    # C prelude: macro-generated vectors and generic functions, vtables.
    print "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n" > c
    print "#define VEC_OF(T) \\\n    typedef struct { T *data; size_t len, cap; } vec_##T; \\\n    static int vec_##T##_push(vec_##T *v, T x) { \\\n        if (v->len == v->cap) { \\\n            size_t cap = v->cap ? v->cap * 2 : 8; \\\n            T *d = realloc(v->data, cap * sizeof(T)); \\\n            if (!d) return -1; \\\n            v->data = d; v->cap = cap; \\\n        } \\\n        v->data[v->len++] = x; return 0; \\\n    } \\\n    static void vec_##T##_free(vec_##T *v) { free(v->data); }" > c
    print "#define GENERICS(T) \\\n    static T sum_##T(const T *xs, size_t n) { T s = 0; for (size_t i = 0; i < n; i++) s += xs[i]; return s; } \\\n    static void scale_##T(T *xs, size_t n, T m) { for (size_t i = 0; i < n; i++) xs[i] *= m; } \\\n    static size_t find_##T(const T *xs, size_t n, T v) { for (size_t i = 0; i < n; i++) if (xs[i] == v) return i; return n; }" > c
    for (t = 1; t <= 4; t++) print "VEC_OF(" CT[t] ")\nGENERICS(" CT[t] ")" > c
    print "\ntypedef struct { uint64_t (*area)(const void *); uint64_t (*perim)(const void *); } ShapeVt;" > c
    print "typedef struct { const ShapeVt *vt; const void *self; } Shape;\n" > c

    # C++ prelude.
    print "#include <cstdint>\n#include <cstdio>\n#include <vector>\n#include <numeric>\n#include <algorithm>\n#include <memory>\n" > cpp
    print "template <typename T> T sum(const std::vector<T> &xs) { return std::accumulate(xs.begin(), xs.end(), T{}); }" > cpp
    print "template <typename T> void scale(std::vector<T> &xs, T m) { std::transform(xs.begin(), xs.end(), xs.begin(), [m](T x) { return x * m; }); }" > cpp
    print "template <typename T> size_t find(const std::vector<T> &xs, T v) { return std::find(xs.begin(), xs.end(), v) - xs.begin(); }" > cpp
    print "struct Shape {\n    virtual ~Shape() = default;\n    virtual uint64_t area() const = 0;\n    virtual uint64_t perim() const = 0;\n};\n" > cpp

    # Rust prelude.
    print "use std::ops::{Add, Mul};\n" > rs
    print "fn sum<T: Copy + Default + Add<Output = T>>(xs: &[T]) -> T { xs.iter().fold(T::default(), |s, &x| s + x) }" > rs
    print "fn scale<T: Copy + Mul<Output = T>>(xs: &mut [T], m: T) { for x in xs.iter_mut() { *x = *x * m; } }" > rs
    print "fn find<T: PartialEq>(xs: &[T], v: T) -> usize { xs.iter().position(|x| *x == v).unwrap_or(xs.len()) }" > rs
    print "trait Shape {\n    fn area(&self) -> u64;\n    fn perim(&self) -> u64;\n}\n" > rs

    for (k = 0; k < n; k++) {
        t = int(k / 4) % 4 + 1
        a = AT[t]; ct = CT[t]; r = RT[t]
        mul = k % 13 + 1
        m = k % 4
        if (m == 0) {
            # Generic container: fill 16 elements, sum them back.
            printf "fn_%d(): u64 {\n    v: array.Array[%s], err: Error = array.Array[%s].new(0)\n    if err != null { return 0 }\n    defer v.wipe()\n", k, a, a > ace
            printf "    for i: usize in 0 .. 16 { if v.push(%s((i * %d + %d) %% 1000)) != null { return 0 } }\n", a, mul, k % 97 > ace
            printf "    s: u64 = 0\n    for i: usize in 0 .. v.len { s = s + u64(v.get(i)) }\n    return s\n}\n\n" > ace

            printf "static uint64_t fn_%d(void) {\n    vec_%s v = {0};\n", k, ct > c
            printf "    for (size_t i = 0; i < 16; i++) if (vec_%s_push(&v, (%s)((i * %d + %d) %% 1000))) { vec_%s_free(&v); return 0; }\n", ct, ct, mul, k % 97, ct > c
            printf "    uint64_t s = 0;\n    for (size_t i = 0; i < v.len; i++) s += (uint64_t)v.data[i];\n    vec_%s_free(&v);\n    return s;\n}\n\n", ct > c

            printf "static uint64_t fn_%d() {\n    std::vector<%s> v;\n", k, ct > cpp
            printf "    for (size_t i = 0; i < 16; i++) v.push_back(%s((i * %d + %d) %% 1000));\n", ct, mul, k % 97 > cpp
            printf "    uint64_t s = 0;\n    for (auto x : v) s += uint64_t(x);\n    return s;\n}\n\n" > cpp

            printf "fn fn_%d() -> u64 {\n    let mut v: Vec<%s> = Vec::new();\n", k, r > rs
            printf "    for i in 0..16usize { v.push(((i * %d + %d) %% 1000) as %s); }\n", mul, k % 97, r > rs
            printf "    v.iter().map(|&x| x as u64).sum()\n}\n\n" > rs
        } else if (m == 1) {
            # Generic functions: shared sum/scale/find plus a per-function
            # generic instantiated at two types.
            u = t % 4 + 1
            printf "mix_%d[T](x: T, y: T): T {\n    m: T = %d\n    return x * m + y\n}\n\n", k, k % 7 + 1 > ace
            printf "fn_%d(): u64 {\n    xs: [8]%s = [", k, a > ace
            for (i = 0; i < 8; i++) printf "%s%d", (i ? ", " : ""), (i * mul + k) % 100 > ace
            printf "]\n    scale[%s](xs[..], %d)\n", a, k % 5 + 1 > ace
            printf "    return u64(sum[%s](xs[..])) + u64(find[%s](xs[..], xs[%d])) + u64(mix_%d[%s](%s(%d), %s(3))) + u64(mix_%d[%s](%s(%d), %s(5)))\n}\n\n", a, a, k % 8, k, a, a, k % 50, a, k, AT[u], AT[u], k % 30, AT[u] > ace

            printf "#define MIX_%d(T) static T mix_%d_##T(T x, T y) { return x * (T)%d + y; }\nMIX_%d(%s)\n", k, k, k % 7 + 1, k, ct > c
            if (u != t) printf "MIX_%d(%s)\n", k, CT[u] > c
            printf "static uint64_t fn_%d(void) {\n    %s xs[8] = {", k, ct > c
            for (i = 0; i < 8; i++) printf "%s%d", (i ? ", " : ""), (i * mul + k) % 100 > c
            printf "};\n    scale_%s(xs, 8, %d);\n", ct, k % 5 + 1 > c
            printf "    return (uint64_t)sum_%s(xs, 8) + find_%s(xs, 8, xs[%d]) + (uint64_t)mix_%d_%s(%d, 3) + (uint64_t)mix_%d_%s(%d, 5);\n}\n\n", ct, ct, k % 8, k, ct, k % 50, k, CT[u], k % 30 > c

            printf "template <typename T> T mix_%d(T x, T y) { return x * T(%d) + y; }\n", k, k % 7 + 1 > cpp
            printf "static uint64_t fn_%d() {\n    std::vector<%s> xs{", k, ct > cpp
            for (i = 0; i < 8; i++) printf "%s%d", (i ? ", " : ""), (i * mul + k) % 100 > cpp
            printf "};\n    scale<%s>(xs, %d);\n", ct, k % 5 + 1 > cpp
            printf "    return uint64_t(sum(xs)) + find(xs, xs[%d]) + uint64_t(mix_%d<%s>(%d, 3)) + uint64_t(mix_%d<%s>(%d, 5));\n}\n\n", k % 8, k, ct, k % 50, k, CT[u], k % 30 > cpp

            printf "fn mix_%d<T: Copy + Add<Output = T> + Mul<Output = T> + From<u8>>(x: T, y: T) -> T { x * T::from(%d) + y }\n", k, k % 7 + 1 > rs
            printf "fn fn_%d() -> u64 {\n    let mut xs: [%s; 8] = [", k, r > rs
            for (i = 0; i < 8; i++) printf "%s%d", (i ? ", " : ""), (i * mul + k) % 100 > rs
            printf "];\n    scale(&mut xs, %d);\n", k % 5 + 1 > rs
            printf "    sum(&xs) as u64 + find(&xs, xs[%d]) as u64 + mix_%d::<%s>(%d, 3) as u64 + mix_%d::<%s>(%d, 5) as u64\n}\n\n", k % 8, k, r, k % 50, k, RT[u], k % 30 > rs
        } else if (m == 2) {
            # Polymorphism: a new shape type called through the interface.
            printf "S%d: struct {\n    r: u64,\n}\n\nS%d signs Shape\n\n", k, k > ace
            printf "S%d.area(self): u64 { return self.r * self.r * %d }\n\nS%d.perim(self): u64 { return self.r * %d }\n\n", k, mul, k, k % 7 + 2 > ace
            printf "fn_%d(): u64 {\n    s: S%d = S%d { r = %d }\n    sh: Shape = Shape(&s)\n    return sh.area() + sh.perim()\n}\n\n", k, k, k, k % 40 + 1 > ace

            printf "typedef struct { uint64_t r; } S%d;\n", k > c
            printf "static uint64_t s%d_area(const void *p) { const S%d *s = p; return s->r * s->r * %d; }\n", k, k, mul > c
            printf "static uint64_t s%d_perim(const void *p) { const S%d *s = p; return s->r * %d; }\n", k, k, k % 7 + 2 > c
            printf "static const ShapeVt s%d_vt = {s%d_area, s%d_perim};\n", k, k, k > c
            printf "static uint64_t fn_%d(void) {\n    S%d s = {%d};\n    Shape sh = {&s%d_vt, &s};\n    return sh.vt->area(sh.self) + sh.vt->perim(sh.self);\n}\n\n", k, k, k % 40 + 1, k > c

            printf "struct S%d final : Shape {\n    uint64_t r;\n    explicit S%d(uint64_t r) : r(r) {}\n", k, k > cpp
            printf "    uint64_t area() const override { return r * r * %d; }\n    uint64_t perim() const override { return r * %d; }\n};\n", mul, k % 7 + 2 > cpp
            printf "static uint64_t fn_%d() {\n    std::unique_ptr<Shape> sh = std::make_unique<S%d>(%d);\n    return sh->area() + sh->perim();\n}\n\n", k, k, k % 40 + 1 > cpp

            printf "struct S%d {\n    r: u64,\n}\n\nimpl Shape for S%d {\n", k, k > rs
            printf "    fn area(&self) -> u64 { self.r * self.r * %d }\n    fn perim(&self) -> u64 { self.r * %d }\n}\n", mul, k % 7 + 2 > rs
            printf "fn fn_%d() -> u64 {\n    let sh: Box<dyn Shape> = Box::new(S%d { r: %d });\n    sh.area() + sh.perim()\n}\n\n", k, k, k % 40 + 1 > rs
        } else {
            # Derived behavior: a record copied and compared for equality.
            printf "P%d: struct {\n    a: u64,\n    b: %s,\n}\n\n", k, a > ace
            printf "P%d.eq(const self, o: P%d): bool { return self.a == o.a && self.b == o.b }\n\n", k, k > ace
            printf "fn_%d(): u64 {\n    p: P%d = P%d { a = %d, b = %d }\n    q: P%d = p\n", k, k, k, k, k % 50, k > ace
            printf "    w: P%d = P%d { a = %d, b = %d }\n", k, k, k + 1, k % 50 > ace
            printf "    s: u64 = p.a + u64(p.b)\n    if p.eq(q) { s = s + 1 }\n    if p.eq(w) { s = s + 2 }\n    return s\n}\n\n" > ace

            printf "typedef struct { uint64_t a; %s b; } P%d;\n", ct, k > c
            printf "static int p%d_eq(P%d x, P%d y) { return x.a == y.a && x.b == y.b; }\n", k, k, k > c
            printf "static uint64_t fn_%d(void) {\n    P%d p = {%d, %d};\n    P%d q = p;\n    P%d w = {%d, %d};\n", k, k, k, k % 50, k, k, k + 1, k % 50 > c
            printf "    uint64_t s = p.a + (uint64_t)p.b;\n    if (p%d_eq(p, q)) s += 1;\n    if (p%d_eq(p, w)) s += 2;\n    return s;\n}\n\n", k, k > c

            printf "struct P%d {\n    uint64_t a;\n    %s b;\n    bool operator==(const P%d &) const = default;\n};\n", k, ct, k > cpp
            printf "static uint64_t fn_%d() {\n    P%d p{%d, %d};\n    P%d q = p;\n    P%d w{%d, %d};\n", k, k, k, k % 50, k, k, k + 1, k % 50 > cpp
            printf "    uint64_t s = p.a + uint64_t(p.b);\n    if (p == q) s += 1;\n    if (p == w) s += 2;\n    return s;\n}\n\n" > cpp

            printf "#[derive(Clone, PartialEq, Debug)]\nstruct P%d {\n    a: u64,\n    b: %s,\n}\n", k, r > rs
            printf "fn fn_%d() -> u64 {\n    let p = P%d { a: %d, b: %d };\n    let q = p.clone();\n    let w = P%d { a: %d, b: %d };\n", k, k, k, k % 50, k, k + 1, k % 50 > rs
            printf "    let mut s = p.a + p.b as u64;\n    if p == q { s += 1; }\n    if p == w { s += 2; }\n    s\n}\n\n" > rs
        }
    }

    print "main(): int {\n    s: u64 = 0" > ace
    print "int main(void) {\n    uint64_t s = 0;" > c
    print "int main() {\n    uint64_t s = 0;" > cpp
    print "fn main() {\n    let mut s: u64 = 0;" > rs
    for (k = 0; k < n; k++) {
        printf "    s = s + fn_%d()\n", k > ace
        printf "    s += fn_%d();\n", k > c
        printf "    s += fn_%d();\n", k > cpp
        printf "    s += fn_%d();\n", k > rs
    }
    print "    fmt.printf(\"checksum: %u\\n\", s)\n    return 0\n}" > ace
    print "    printf(\"checksum: %llu\\n\", (unsigned long long)s);\n    return 0;\n}" > c
    print "    std::printf(\"checksum: %llu\\n\", (unsigned long long)s);\n    return 0;\n}" > cpp
    print "    println!(\"checksum: {}\", s);\n}" > rs
}'
