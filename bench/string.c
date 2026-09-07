#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void report(const char *name, uint64_t elapsed, uint64_t operations,
                   uint64_t checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed,
           (unsigned long long)operations, (unsigned long long)checksum);
}

__attribute__((noinline))
static int measured_compare(const void *a, const void *b, size_t size) {
    return memcmp(a, b, size);
}

__attribute__((noinline))
static void measured_copy(void *destination, const void *source, size_t size) {
    memcpy(destination, source, size);
}

__attribute__((noinline))
static const char *measured_find(const char *text, const char *needle) {
    return strstr(text, needle);
}

__attribute__((noinline))
static double measured_parse(const char *text) {
    return strtod(text, NULL);
}

__attribute__((noinline))
static int measured_format(char *out, size_t capacity, double value) {
    return snprintf(out, capacity, "%.17g", value);
}

static int (*volatile compare_call)(const void *, const void *, size_t) = measured_compare;
static void (*volatile copy_call)(void *, const void *, size_t) = measured_copy;
static const char *(*volatile find_call)(const char *, const char *) = measured_find;
static double (*volatile parse_call)(const char *) = measured_parse;
static int (*volatile format_call)(char *, size_t, double) = measured_format;

int main(void) {
    const uint64_t iterations = 1000000;
    unsigned char small_a[64], small_b[64], large_a[4097], large_b[4096];
    memset(small_a, 'a', sizeof small_a);
    memset(small_b, 'a', sizeof small_b);
    memset(large_a, 'a', 4096);
    memset(large_b, 'a', sizeof large_b);
    large_a[4096] = 0;
    small_b[63] = 'b';
    large_b[4095] = 'b';
    uint64_t checksum = 0;
    uint64_t started = now_ns();

    for (uint64_t i = 0; i < iterations; i++)
        checksum += (compare_call(small_a, small_b, 64) < 0 ? 0 : 2) + i % 2;
    report("compare-64", now_ns() - started, iterations, checksum);
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations / 16; i++)
        checksum += (compare_call(large_a, large_b, 4096) < 0 ? 0 : 2) + i % 2;
    report("compare-4096", now_ns() - started, iterations / 16, checksum);

    unsigned char copy_small[64], copy_large[4096];
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations; i++) {
        copy_call(copy_small, small_a, 64);
        checksum += copy_small[i % 64];
    }
    report("copy-64", now_ns() - started, iterations, checksum);
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations / 16; i++) {
        copy_call(copy_large, large_a, 4096);
        checksum += copy_large[i % 4096];
    }
    report("copy-4096", now_ns() - started, iterations / 16, checksum);

    large_a[4093] = 'x'; large_a[4094] = 'y'; large_a[4095] = 'z';
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations / 32; i++) {
        const char *found = find_call((const char *)large_a, "xyz");
        if (found) checksum += (uint64_t)(found - (const char *)large_a) + i % 2;
    }
    report("find-4096", now_ns() - started, iterations / 32, checksum);

    static const char *inputs[8] = {
        "0.1", "1.5", "3.141592653589793", "1e-308",
        "5e-324", "1.7976931348623157e308", "-2.5e100", "6.02214076e23"
    };
    const uint64_t float_iterations = 20000;
    uint64_t raw_sum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < float_iterations; i++) {
        double value = parse_call(inputs[i % 8]);
        uint64_t raw;
        memcpy(&raw, &value, sizeof raw);
        raw_sum ^= raw + i;
    }
    report("parse-f64", now_ns() - started, float_iterations, raw_sum);

    static const double values[8] = {
        0.1, 1.5, 3.141592653589793, 1e-308,
        5e-324, 1.7976931348623157e308, -2.5e100, 6.02214076e23
    };
    char out[64];
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations / 10; i++) {
        int count = format_call(out, sizeof out, values[i % 8]);
        checksum += (uint64_t)count + (unsigned char)out[0];
    }
    report("format-f64-17g", now_ns() - started, iterations / 10, checksum);
    return 0;
}
