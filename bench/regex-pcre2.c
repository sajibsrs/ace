/* A reference column for bench/regex: the same workloads through PCRE2, a
 * backtracking engine, interpreted (`interp`) or JIT-compiled (`jit`). PCRE2
 * is leftmost-first like pkg/regex, so on these patterns it must report the
 * same spans and checksums. It is a reference, not a comparison target: its
 * algorithm differs, which the pathological workload exists to show.
 */

#define PCRE2_CODE_UNIT_WIDTH 8

#include <pcre2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIZE 1048576
#define CALLS 10000

static bool jit;

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void report(const char *name, uint64_t elapsed, uint64_t operations,
                   uint64_t checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed,
           (unsigned long long)operations, (unsigned long long)checksum);
}

static void fill(uint8_t *text, size_t size) {
    uint64_t x = 88172645463325252ull;

    for (size_t i = 0; i < size; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t r = x >> 33;
        uint64_t kind = r % 16;

        if (kind == 0) text[i] = ' ';
        else if (kind == 1) text[i] = '\n';
        else if (kind <= 4) text[i] = (uint8_t)(48 + (r >> 8) % 10);
        else if (kind == 5) text[i] = '-';
        else text[i] = (uint8_t)(97 + (r >> 8) % 26);
    }
}

static pcre2_code *compile(const char *pattern) {
    int error;
    PCRE2_SIZE offset;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                     0, &error, &offset, NULL);
    if (code == NULL) {
        fprintf(stderr, "pcre2_compile failed for %s\n", pattern);
        exit(1);
    }
    if (jit && pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) != 0) {
        fprintf(stderr, "pcre2_jit_compile failed for %s\n", pattern);
        exit(1);
    }
    return code;
}

/* Every match, stepping past empty ones; `groups` sums group 1's start and
 * group 2's end instead of the whole match, as the captures workload does.
 */
static void find_all(const char *name, const char *pattern, const uint8_t *text,
                     size_t len, bool groups) {
    pcre2_code *code = compile(pattern);
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, NULL);
    uint64_t checksum = 0;
    size_t at = 0;
    uint64_t started = now_ns();

    while (at <= len) {
        int rc = pcre2_match(code, text, len, at, 0, data, NULL);
        if (rc < 0) break;
        PCRE2_SIZE *v = pcre2_get_ovector_pointer(data);

        if (groups) {
            checksum += 1 + v[2] + v[5];
            at = v[1];
        } else {
            checksum += 1 + v[0] + v[1];
            at = v[1];
            if (v[1] == v[0]) at += 1;
        }
    }
    report(name, now_ns() - started, len, checksum);
    pcre2_match_data_free(data);
    pcre2_code_free(code);
}

static void pathological(void) {
    pcre2_code *code = compile("(a|aa)*c");
    pcre2_match_data *data = pcre2_match_data_create_from_pattern(code, NULL);
    const uint8_t *text = (const uint8_t *)"aaaaaaaaaaaaaabc";
    size_t len = strlen((const char *)text);
    uint64_t checksum = 0;
    uint64_t started = now_ns();

    for (size_t i = 0; i < CALLS; i++) {
        if (pcre2_match(code, text, len, 0, 0, data, NULL) >= 0) {
            PCRE2_SIZE *v = pcre2_get_ovector_pointer(data);
            checksum += 1 + v[0] + v[1];
        }
    }
    report("pathological", now_ns() - started, CALLS * len, checksum);
    pcre2_match_data_free(data);
    pcre2_code_free(code);
}

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "jit") != 0 && strcmp(argv[1], "interp") != 0)) {
        fprintf(stderr, "usage: %s jit|interp\n", argv[0]);
        return 2;
    }
    jit = strcmp(argv[1], "jit") == 0;
    uint8_t *text = malloc(SIZE);
    if (text == NULL) return 1;
    fill(text, SIZE);
    find_all("literal", "zqxj", text, SIZE, false);
    find_all("word-ing", "[a-z]+ing", text, SIZE, false);
    find_all("captures", "(\\d+)-(\\d+)", text, SIZE, true);
    pathological();
    free(text);
    return 0;
}
