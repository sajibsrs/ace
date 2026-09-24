#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

__attribute__((noinline))
static long long measured_parse_int(const char *text) {
    return strtoll(text, NULL, 10);
}

__attribute__((noinline))
static unsigned long long measured_parse_hex(const char *text) {
    return strtoull(text, NULL, 16);
}

/* The trimmed length over the same ASCII whitespace set as str.trim. */
static int space_byte(unsigned char c) {
    return (c >= '\t' && c <= '\r') || c == ' ';
}

__attribute__((noinline))
static size_t measured_trim(const char *text, size_t len) {
    size_t start = 0, end = len;
    while (start < end && space_byte((unsigned char)text[start])) start++;
    while (end > start && space_byte((unsigned char)text[end - 1])) end--;
    return end - start;
}

__attribute__((noinline))
static int measured_fold(const char *a, const char *b, size_t len) {
    return strncasecmp(a, b, len);
}

/* Strict RFC 3629 validation, the rules utf8.valid applies. */
__attribute__((noinline))
static int measured_utf8(const unsigned char *text, size_t len) {
    size_t at = 0;
    while (at < len) {
        unsigned char lead = text[at];
        if (lead < 0x80) { at++; continue; }
        size_t n;
        if (lead >= 0xC2 && lead <= 0xDF) n = 2;
        else if (lead >= 0xE0 && lead <= 0xEF) n = 3;
        else if (lead >= 0xF0 && lead <= 0xF4) n = 4;
        else return 0;
        if (len - at < n) return 0;
        unsigned char low = 0x80, high = 0xBF;
        if (lead == 0xE0) low = 0xA0;
        else if (lead == 0xED) high = 0x9F;
        else if (lead == 0xF0) low = 0x90;
        else if (lead == 0xF4) high = 0x8F;
        if (text[at + 1] < low || text[at + 1] > high) return 0;
        for (size_t i = 2; i < n; i++)
            if (text[at + i] < 0x80 || text[at + i] > 0xBF) return 0;
        at += n;
    }
    return 1;
}

static int (*volatile compare_call)(const void *, const void *, size_t) = measured_compare;
static void (*volatile copy_call)(void *, const void *, size_t) = measured_copy;
static const char *(*volatile find_call)(const char *, const char *) = measured_find;
static double (*volatile parse_call)(const char *) = measured_parse;
static int (*volatile format_call)(char *, size_t, double) = measured_format;
static long long (*volatile parse_int_call)(const char *) = measured_parse_int;
static unsigned long long (*volatile parse_hex_call)(const char *) = measured_parse_hex;
static size_t (*volatile trim_call)(const char *, size_t) = measured_trim;
static int (*volatile fold_call)(const char *, const char *, size_t) = measured_fold;
static int (*volatile utf8_call)(const unsigned char *, size_t) = measured_utf8;

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
    report("parse-f64-short", now_ns() - started, float_iterations, raw_sum);

    static const char *long_inputs[8] = {
        "1234567890123456", "1234567890123456789",
        "0.1234567890123456789", "1234567890123456.5",
        "1.234567890123456e+308", "-9876543210987654e-200",
        "0.000000000000000123456789012345", "3141592653589793238"
    };
    raw_sum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < float_iterations; i++) {
        double value = parse_call(long_inputs[i % 8]);
        uint64_t raw;
        memcpy(&raw, &value, sizeof raw);
        raw_sum ^= raw + i;
    }
    report("parse-f64-long", now_ns() - started, float_iterations, raw_sum);

    /* Canonical workload assumption: 80% common/short, 20% long/scientific. */
    const char *blended_inputs[40] = {
        inputs[0], inputs[1], inputs[2], inputs[3],
        inputs[4], inputs[5], inputs[6], inputs[7],
        inputs[0], inputs[1], inputs[2], inputs[3],
        inputs[4], inputs[5], inputs[6], inputs[7],
        inputs[0], inputs[1], inputs[2], inputs[3],
        inputs[4], inputs[5], inputs[6], inputs[7],
        inputs[0], inputs[1], inputs[2], inputs[3],
        inputs[4], inputs[5], inputs[6], inputs[7],
        long_inputs[0], long_inputs[1], long_inputs[2], long_inputs[3],
        long_inputs[4], long_inputs[5], long_inputs[6], long_inputs[7]
    };
    raw_sum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < float_iterations; i++) {
        double value = parse_call(blended_inputs[i % 40]);
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

    static const char *decimal_ints[8] = {
        "0", "7", "42", "-1234", "65535", "-2147483648", "9223372036854775807", "123456789012"
    };
    raw_sum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations; i++)
        raw_sum += (uint64_t)parse_int_call(decimal_ints[i % 8]);
    report("parse-int", now_ns() - started, iterations, raw_sum);

    static const char *hex_ints[8] = {
        "ff", "DEADbeef", "7fffffffffffffff", "10", "0", "abc123", "FFFFFFFFFFFFFFFF", "1234abcd"
    };
    raw_sum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations; i++)
        raw_sum += (uint64_t)parse_hex_call(hex_ints[i % 8]);
    report("parse-hex", now_ns() - started, iterations, raw_sum);

    static const char *padded[4] = {
        "   hello, world   ", "\t\r\nkey: value\n", "no-padding-at-all", "        \t  indented line"
    };
    size_t padded_len[4];
    for (int k = 0; k < 4; k++) padded_len[k] = strlen(padded[k]);
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations; i++)
        checksum += trim_call(padded[i % 4], padded_len[i % 4]);
    report("trim", now_ns() - started, iterations, checksum);

    static const char *header = "Content-Type: Application/JSON; Charset=UTF-8; X-Request-Id: A7f";
    static const char *headers[2] = {
        "content-type: application/json; charset=utf-8; x-request-id: a7F",
        "content-type: application/json; charset=utf-8; x-request-id: a7G",
    };
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations; i++)
        if (fold_call(header, headers[i % 2], 64) == 0) checksum += 1 + i % 3;
    report("ascii-fold-64", now_ns() - started, iterations, checksum);

    static const char *texts[2] = {
        "Gr\xC3\xBC\xC3\x9F" "e, \xE4\xB8\x96\xE7\x95\x8C! \xCE\xA9mega \xE2\x86\x92 \xF0\x9D\x84\x9E na\xC3\xAF"
        "ve caf\xC3\xA9 plain ascii tail text 0123456789",
        "Gr\xC3\xBC\xC3\x9F" "e, \xE4\xB8\x96\xE7\x95\x8C! \xCE\xA9mega \xE2\x86\x92 \xF0\x9D\x84\x9E na\xC3\xAF"
        "ve caf\xC3\xA9 plain ascii tail text 012345678\xC0",
    };
    size_t text_len[2] = { strlen(texts[0]), strlen(texts[1]) };
    checksum = 0;
    started = now_ns();
    for (uint64_t i = 0; i < iterations; i++)
        if (utf8_call((const unsigned char *)texts[i % 2], text_len[i % 2])) checksum += text_len[i % 2] + i % 3;
    report("utf8-valid", now_ns() - started, iterations, checksum);
    return 0;
}
