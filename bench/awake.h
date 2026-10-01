/* The clock Ace's time.awake reads on each target, for the C twins. Include it
 * before any system header: the feature macro must precede them.
 */
#ifndef BENCH_AWAKE_H
#define BENCH_AWAKE_H

#ifdef __linux__
#define _DEFAULT_SOURCE /* CLOCK_MONOTONIC_RAW */
#endif

#include <stdint.h>
#include <time.h>

static uint64_t awake(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

#endif
