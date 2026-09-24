#include <stdint.h>
#include <stdio.h>
#include <time.h>

int main(void) {
    const uint64_t iterations = 5000000;
    uint64_t state = 1;

    const uint64_t started = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (uint64_t i = 0; i < iterations; i++)
        state = (state * 48271 + (i % 97)) % 2147483647;
    const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - started;

    printf("iterations: %llu\nelapsed ns: %llu\nchecksum: %llu\n",
           (unsigned long long)iterations,
           (unsigned long long)elapsed,
           (unsigned long long)state);
    return 0;
}
