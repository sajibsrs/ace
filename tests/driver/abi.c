#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

typedef struct { int32_t a; float b; } Mixed;
typedef struct { int64_t a; int32_t b; } Pair;
typedef struct { float a, b, c; } Hfa3;
typedef struct { double a, b, c, d; } Hfa4;
typedef struct { float v[3]; } Arr3;
typedef struct { int64_t a, b, c; } Large;

extern int64_t ace_scalars(int8_t, int16_t, int32_t, int64_t,
                           uint8_t, uint16_t, uint32_t, uint64_t, size_t);
extern double ace_floats(float, double);
extern float ace_f32(float);
extern void ace_bump(int32_t*);
extern int32_t ace_code(int32_t);
extern uint8_t ace_wire(uint8_t);
extern Mixed ace_mixed(Mixed);
extern Pair ace_pair(Pair);
extern Hfa3 ace_hfa3(Hfa3);
extern Hfa4 ace_hfa4(Hfa4);
extern Arr3 ace_arr3(Arr3);
extern Large ace_large(Large);
extern int64_t ace_stack(int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, Pair, int32_t);
extern double ace_fpstack(double, double, double, double, double, double,
                          double, Hfa3, double);
extern int64_t ace_stack_align(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int32_t, int64_t);
extern double ace_fp_align(double, double, double, double, double, double,
                           double, double, float, double);

int64_t c_scalars(int8_t a, int16_t b, int32_t c, int64_t d,
                  uint8_t e, uint16_t f, uint32_t g, uint64_t h, size_t i) {
    return a + b + c + d + e + f + g + (int64_t)h + (int64_t)i;
}

double c_floats(float a, double b) { return (double)a + b; }

float c_f32(float value) { return value + 0.5f; }

void c_bump(int32_t* value) { *value += 7; }

int32_t c_code(int32_t value) { return value + 1; }
uint8_t c_wire(uint8_t value) { return value == 7 ? 8 : 0; }

Mixed c_mixed(Mixed value) {
    value.a += 2;
    value.b += 3.0f;
    return value;
}

Pair c_pair(Pair value) {
    value.a += 3;
    value.b += 4;
    return value;
}

Hfa3 c_hfa3(Hfa3 value) {
    value.a += 1.0f;
    value.b += 2.0f;
    value.c += 3.0f;
    return value;
}

Hfa4 c_hfa4(Hfa4 value) {
    value.a += 1.0;
    value.b += 2.0;
    value.c += 3.0;
    value.d += 4.0;
    return value;
}

Arr3 c_arr3(Arr3 value) {
    value.v[0] += 2.0f;
    value.v[1] += 3.0f;
    value.v[2] += 4.0f;
    return value;
}

Large c_large(Large value) {
    value.a += 4;
    value.b += 5;
    value.c += 6;
    return value;
}

int64_t c_large_copy(Large value) {
    value.a = 99;
    return value.a + value.b + value.c;
}

int64_t c_stack(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                int64_t f, int64_t g, Pair pair, int32_t tail) {
    return a + b + c + d + e + f + g + pair.a + pair.b + tail;
}

int64_t c_stack_align(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e,
                      int64_t f, int64_t g, int64_t h, int32_t narrow,
                      int64_t wide) {
    return a + b + c + d + e + f + g + h + narrow + wide;
}

double c_fpstack(double a, double b, double c, double d, double e, double f,
                 double g, Hfa3 h, double tail) {
    return a + b + c + d + e + f + g + h.a + h.b + h.c + tail;
}

int32_t c_fpstack_check(double a, double b, double c, double d, double e,
                        double f, double g, Hfa3 h, double tail) {
    if (a != 1 || b != 2 || c != 3 || d != 4 || e != 5 || f != 6 || g != 7)
        return 1;
    if (h.a != 8 || h.b != 9 || h.c != 10) return 2;
    if (tail != 11) return 3;
    return 0;
}

double c_fp_align(double a, double b, double c, double d, double e, double f,
                  double g, double h, float narrow, double wide) {
    return a + b + c + d + e + f + g + h + narrow + wide;
}

int32_t c_varargs(int32_t fixed, ...) {
    va_list args;
    va_start(args, fixed);
    int32_t integer = va_arg(args, int32_t);
    double floating = va_arg(args, double);
    int32_t* pointer = va_arg(args, int32_t*);
    va_end(args);
    if (fixed != 7 || integer != 9 || floating != 1.25 || !pointer)
        return 1;
    return *pointer == 11 ? 0 : 2;
}

int32_t c_run_ace(void) {
    if (ace_scalars(-1, 2, 3, 4, 5, 6, 7, 8, 9) != 43) return 1;
    if (ace_floats(1.25f, 2.5) != 3.75) return 2;
    int32_t x = 10;
    ace_bump(&x);
    if (x != 17) return 3;
    if (ace_code(41) != 42) return 4;
    if (ace_wire(7) != 8) return 18;

    Mixed mixed = ace_mixed((Mixed){10, 1.5f});
    if (mixed.a != 12 || mixed.b != 4.5f) return 5;
    Pair pair = ace_pair((Pair){20, 3});
    if (pair.a != 23 || pair.b != 7) return 6;
    Hfa3 h3 = ace_hfa3((Hfa3){1.0f, 2.0f, 3.0f});
    if (h3.a != 2.0f || h3.b != 4.0f || h3.c != 6.0f) return 7;
    Hfa4 h4 = ace_hfa4((Hfa4){1.0, 2.0, 3.0, 4.0});
    if (h4.a != 2.0 || h4.b != 4.0 || h4.c != 6.0 || h4.d != 8.0) return 8;
    Arr3 arr = ace_arr3((Arr3){{1.0f, 2.0f, 3.0f}});
    if (arr.v[0] != 3.0f || arr.v[1] != 5.0f || arr.v[2] != 7.0f) return 9;
    Large large = ace_large((Large){1, 2, 3});
    if (large.a != 5 || large.b != 7 || large.c != 9) return 10;
    if (ace_stack(1, 2, 3, 4, 5, 6, 7, (Pair){8, 9}, 10) != 55) return 11;
    if (ace_fpstack(1, 2, 3, 4, 5, 6, 7,
                    (Hfa3){8, 9, 10}, 11) != 66.0) return 12;
    if (ace_f32(1.25f) != 2.0f) return 13;
    if (ace_stack_align(1, 2, 3, 4, 5, 6, 7, 8, 4, 7) != 47) return 14;
    if (ace_fp_align(1, 2, 3, 4, 5, 6, 7, 8, 4, 7) != 47) return 15;
    return 0;
}
