/* Call-overhead benchmark paired with calls.ace. Functions have external
 * linkage and, unless ALLOW_INLINE is defined, `noinline`, so -O2 keeps each
 * call and cannot specialise it on constant arguments. Defining ALLOW_INLINE
 * gives the inlined reference, which Ace has no way to match.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef ALLOW_INLINE
#define NOINLINE
#else
#define NOINLINE __attribute__((noinline))
#endif

#define MASK 0x7FFFFFFFull
#define ITERATIONS 10000000ull
#define FIB_INPUT 32ull
#define FIB_CALLS 7049155ull

typedef uint64_t usize;

typedef struct { usize v; } Small;
typedef struct { usize a, b; } Pair;
typedef struct { usize a, b, c; } Triple;
typedef struct { usize a, b, c, d, e, f, g, h; } Octet;
typedef struct { double x, y, z; } Vec3;
typedef struct { usize value; } Counter;
typedef struct { usize first, second; } Divmod;

NOINLINE usize call1(usize a) { return (a * 48271 + 1) & MASK; }

NOINLINE usize call4(usize a, usize b, usize c, usize d) {
    return (a * 3 + b * 5 + c * 7 + d) & MASK;
}

NOINLINE usize call8(usize a, usize b, usize c, usize d, usize e, usize f,
                     usize g, usize h) {
    return (a + b * 2 + c * 3 + d * 5 + e * 7 + f * 11 + g * 13 + h * 17) & MASK;
}

NOINLINE usize fib(usize n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

NOINLINE Small small_step(Small s) {
    return (Small){(s.v * 48271 + 1) & MASK};
}

NOINLINE Pair pair_step(Pair p) {
    return (Pair){(p.a * 48271 + p.b) & MASK, (p.b + 1) & MASK};
}

NOINLINE Triple triple_step(Triple t) {
    return (Triple){(t.a * 48271 + t.b) & MASK, (t.b + t.c) & MASK,
                    (t.c + 1) & MASK};
}

NOINLINE Octet octet_step(Octet o) {
    return (Octet){(o.a * 48271 + o.h) & MASK, (o.b + o.a) & MASK,
                   (o.c + o.b) & MASK, (o.d + o.c) & MASK, (o.e + o.d) & MASK,
                   (o.f + o.e) & MASK, (o.g + o.f) & MASK, (o.h + 1) & MASK};
}

NOINLINE Vec3 vec_add(Vec3 a, Vec3 b) {
    return (Vec3){a.x + b.x, a.y + b.y, a.z + b.z};
}

NOINLINE Divmod divmod(usize a, usize b) { return (Divmod){a / b, a % b}; }

NOINLINE void counter_add(Counter* self, usize amount) {
    self->value = (self->value + amount) & MASK;
}

static usize (*volatile callback_slot)(usize) = call1;

static void report(const char* name, uint64_t elapsed, usize operations,
                   usize checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed,
           (unsigned long long)operations, (unsigned long long)checksum);
}

static uint64_t now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

int main(void) {
    usize state = 1;
    uint64_t started = now();
    for (usize i = 0; i < ITERATIONS; i++) state = call1(state + i);
    report("call-1", now() - started, ITERATIONS, state);

    state = 1;
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) state = call4(state, i, state >> 3, 1);
    report("call-4", now() - started, ITERATIONS, state);

    state = 1;
    started = now();
    for (usize i = 0; i < ITERATIONS; i++)
        state = call8(state, i, 1, 2, 3, 4, 5, 6);
    report("call-8", now() - started, ITERATIONS, state);

    started = now();
    state = fib(FIB_INPUT);
    report("fib-recursive", now() - started, FIB_CALLS, state);

    Small small = {1};
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) small = small_step(small);
    report("struct-8", now() - started, ITERATIONS, small.v);

    Pair pair = {1, 1};
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) pair = pair_step(pair);
    report("struct-16", now() - started, ITERATIONS, (pair.a + pair.b) & MASK);

    Triple triple = {1, 1, 1};
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) triple = triple_step(triple);
    report("struct-24", now() - started, ITERATIONS,
           (triple.a + triple.b + triple.c) & MASK);

    Octet octet = {1, 1, 1, 1, 1, 1, 1, 1};
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) octet = octet_step(octet);
    report("struct-64", now() - started, ITERATIONS,
           (octet.a + octet.b + octet.c + octet.d + octet.e + octet.f + octet.g +
            octet.h) & MASK);

    Vec3 acc = {0.0, 0.0, 0.0};
    Vec3 step = {1.0, 2.0, 3.0};
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) acc = vec_add(acc, step);
    report("struct-hfa", now() - started, ITERATIONS,
           (usize)(acc.x + acc.y + acc.z));

    state = 1;
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) {
        Divmod result = divmod(state + i + 1, 97);
        state = (result.first * 3 + result.second) & MASK;
    }
    report("tuple-return", now() - started, ITERATIONS, state);

    Counter counter = {1};
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) counter_add(&counter, i);
    report("method", now() - started, ITERATIONS, counter.value);

    usize (*callback)(usize) = callback_slot;
    state = 1;
    started = now();
    for (usize i = 0; i < ITERATIONS; i++) state = callback(state + i);
    report("function-value", now() - started, ITERATIONS, state);
    return 0;
}
