#include <stdint.h>

/* Explicit-layout C ABI oracle: clang owns these definitions; layout_abi.ace
 * mirrors them with @packed, @align, and @offset. Each case crosses both ways
 * as a parameter and result, and as a stack argument once registers run out.
 */

typedef struct __attribute__((packed)) { uint8_t c; int16_t s; } P3;
typedef struct __attribute__((packed)) { uint8_t c; int64_t l; } P9;
typedef struct __attribute__((packed)) { uint8_t c; int64_t l, m; } P17;
typedef struct { _Alignas(16) int64_t a; } A16;
typedef struct __attribute__((aligned(16))) { float a, b; } AF16;
typedef union { float f[4]; int32_t i[4]; struct { float depth; uint32_t stencil; } ds; } Clear;
typedef union { float f[4]; float g[4]; } UF;
typedef union { double a[4]; double b[4]; } UD4;
typedef union { float f; double d; } UFD;
typedef struct { float a; union { float f; float g; } u; } SUF;

#define CASES(X) X(P3, p3) X(P9, p9) X(P17, p17) X(A16, a16) X(AF16, af16) \
    X(Clear, clear) X(UF, uf) X(UD4, ud4) X(UFD, ufd) X(SUF, suf)

static P3 bump_p3(P3 v) { v.c += 1; v.s += 2; return v; }
static P9 bump_p9(P9 v) { v.c += 1; v.l += 2; return v; }
static P17 bump_p17(P17 v) { v.c += 1; v.l += 2; v.m += 3; return v; }
static A16 bump_a16(A16 v) { v.a += 1; return v; }
static AF16 bump_af16(AF16 v) { v.a += 1; v.b += 2; return v; }
static Clear bump_clear(Clear v) { for (int k = 0; k < 4; k++) v.i[k] += k + 1; return v; }
static UF bump_uf(UF v) { for (int k = 0; k < 4; k++) v.f[k] += 1; return v; }
static UD4 bump_ud4(UD4 v) { for (int k = 0; k < 4; k++) v.a[k] += 1; return v; }
static UFD bump_ufd(UFD v) { v.d += 1; return v; }
static SUF bump_suf(SUF v) { v.a += 1; v.u.f += 2; return v; }

static int same_p3(P3 a, P3 b) { return a.c == b.c && a.s == b.s; }
static int same_p9(P9 a, P9 b) { return a.c == b.c && a.l == b.l; }
static int same_p17(P17 a, P17 b) { return a.c == b.c && a.l == b.l && a.m == b.m; }
static int same_a16(A16 a, A16 b) { return a.a == b.a; }
static int same_af16(AF16 a, AF16 b) { return a.a == b.a && a.b == b.b; }
static int same_clear(Clear a, Clear b) { for (int k = 0; k < 4; k++) if (a.i[k] != b.i[k]) return 0; return 1; }
static int same_uf(UF a, UF b) { for (int k = 0; k < 4; k++) if (a.f[k] != b.f[k]) return 0; return 1; }
static int same_ud4(UD4 a, UD4 b) { for (int k = 0; k < 4; k++) if (a.a[k] != b.a[k]) return 0; return 1; }
static int same_ufd(UFD a, UFD b) { return a.d == b.d; }
static int same_suf(SUF a, SUF b) { return a.a == b.a && a.u.f == b.u.f; }

/* c_N(v): v bumped. c_stack_N: v after every x and v register is taken. */
#define DEFINE(T, n) \
    T c_##n(T v) { return bump_##n(v); } \
    T c_stack_##n(int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6, \
                  int64_t a7, double d0, double d1, double d2, double d3, double d4, double d5, double d6, \
                  double d7, uint8_t tag, T v) { \
        if (a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 != 36 || d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 != 36.0 || \
            tag != 0x5a) \
            return (T){ 0 }; \
        return bump_##n(v); \
    } \
    extern T ace_##n(T); \
    extern T ace_stack_##n(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, \
                           double, double, double, double, double, double, double, uint8_t, T);
CASES(DEFINE)

/* Partly filled banks: a 16-aligned pair after one x register (Apple: no
 * even-register rounding), a four-element HFA that no longer fits in v. */
int64_t c_after_a16(int64_t lead, A16 v) { return lead * 100 + v.a; }
float c_after_uf(double d0, double d1, double d2, double d3, double d4, UF v, float tail) {
    return (float)(d0 + d1 + d2 + d3 + d4) + v.f[0] + v.f[1] + v.f[2] + v.f[3] + tail;
}
extern int64_t ace_after_a16(int64_t, A16);
extern float ace_after_uf(double, double, double, double, double, UF, float);

static P3 make_p3(void) { return (P3){ 1, -300 }; }
static P9 make_p9(void) { return (P9){ 2, 0x123456789 }; }
static P17 make_p17(void) { return (P17){ 3, -5, 1L << 40 }; }
static A16 make_a16(void) { return (A16){ 41 }; }
static AF16 make_af16(void) { return (AF16){ 1.5f, -2.5f }; }
static Clear make_clear(void) { Clear v; v.ds.depth = 1.0f; v.ds.stencil = 7; v.i[2] = -9; v.i[3] = 1 << 30; return v; }
static UF make_uf(void) { return (UF){ { 0.5f, 1.5f, 2.5f, 3.5f } }; }
static UD4 make_ud4(void) { return (UD4){ { 0.25, 1.25, 2.25, 3.25 } }; }
static UFD make_ufd(void) { UFD v; v.d = 6.5; return v; }
static SUF make_suf(void) { SUF v; v.a = 4.0f; v.u.f = 8.0f; return v; }

/* Calls every Ace export; 0 when each returns what C computes itself. */
int c_run_layout(void) {
    int code = 1;
#define RUN(T, n) \
    if (!same_##n(ace_##n(make_##n()), bump_##n(make_##n()))) return code; \
    code++; \
    if (!same_##n(ace_stack_##n(1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 0x5a, make_##n()), \
                  bump_##n(make_##n()))) \
        return code; \
    code++;
    CASES(RUN)
    if (ace_after_a16(3, make_a16()) != 341) return code;
    code++;
    if (ace_after_uf(1, 2, 3, 4, 5, make_uf(), 0.25f) != 23.25f) return code;
    return 0;
}
