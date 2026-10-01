/* C reference for math.ace: the platform libm on identical inputs. */

#include "../awake.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void report(const char *name, uint64_t elapsed, uint64_t operations, uint64_t checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed, (unsigned long long)operations,
           (unsigned long long)checksum);
}

static uint64_t bits64(double value) {
    uint64_t raw;
    memcpy(&raw, &value, sizeof raw);
    return raw;
}

static uint64_t bits32(float value) {
    uint32_t raw;
    memcpy(&raw, &value, sizeof raw);
    return raw;
}

int main(void) {
    const size_t n = 2000000;
    double wide[256], positive[256], unit[256], classify[64];
    float wide32[256], positive32[256];

    for (size_t i = 0; i < 256; i++) {
        wide[i] = (double)i * 0.078125 - 10.0;
        positive[i] = (double)(i + 1) * 3.90625;
        unit[i] = (double)i * 0.01171875 - 1.5;
        wide32[i] = (float)wide[i];
        positive32[i] = (float)positive[i];
    }
    for (size_t i = 0; i < 64; i++) classify[i] = (double)i * 1.5 - 20.0;
    classify[3] = NAN;
    classify[9] = INFINITY;
    classify[17] = -INFINITY;

    /* Keep the inputs opaque so libm calls are not folded or hoisted. */
    __asm__ volatile("" : : "r"(wide), "r"(positive), "r"(unit), "r"(classify), "r"(wide32), "r"(positive32)
                     : "memory");

    uint64_t count = 0;
    uint64_t started = awake();
    for (size_t i = 0; i < n; i++) {
        double x = classify[i % 64];
        if (isnan(x)) count += 1;
        if (isinf(x)) count += 2;
        if (isfinite(x)) count += 4;
    }
    report("classify", awake() - started, n, count);

    double total = 0.0;
    started = awake();
    for (size_t i = 0; i < n; i++) total += exp(wide[i % 256]);
    report("exp-f64", awake() - started, n, bits64(total));
    total = 0.0;
    started = awake();
    for (size_t i = 0; i < n; i++) total += log(positive[i % 256]);
    report("log-f64", awake() - started, n, bits64(total));
    total = 0.0;
    started = awake();
    for (size_t i = 0; i < n; i++) total += pow(positive[i % 256], unit[(i * 7) % 256]);
    report("pow-f64", awake() - started, n, bits64(total));
    total = 0.0;
    started = awake();
    for (size_t i = 0; i < n; i++) total += sin(wide[i % 256]);
    report("sin-f64", awake() - started, n, bits64(total));
    total = 0.0;
    started = awake();
    for (size_t i = 0; i < n; i++) total += cos(wide[i % 256]);
    report("cos-f64", awake() - started, n, bits64(total));
    total = 0.0;
    started = awake();
    for (size_t i = 0; i < n / 10; i++) total += sin((double)(i + 1) * 1.0e7);
    report("sin-large", awake() - started, n / 10, bits64(total));

    float total32 = 0.0f;
    started = awake();
    for (size_t i = 0; i < n; i++) total32 += expf(wide32[i % 256] * 0.25f);
    report("exp-f32", awake() - started, n, bits32(total32));
    total32 = 0.0f;
    started = awake();
    for (size_t i = 0; i < n; i++) total32 += logf(positive32[i % 256]);
    report("log-f32", awake() - started, n, bits32(total32));
    total32 = 0.0f;
    started = awake();
    for (size_t i = 0; i < n; i++) total32 += sinf(wide32[i % 256]);
    report("sin-f32", awake() - started, n, bits32(total32));
    total32 = 0.0f;
    started = awake();
    for (size_t i = 0; i < n; i++) total32 += cosf(wide32[i % 256]);
    report("cos-f32", awake() - started, n, bits32(total32));
    return 0;
}
