// CPU benchmark paired with cpu.ace and cpu.c: the same LCG, step for step.
#include "../awake.h"
#include <cstdint>
#include <cstdio>

int main() {
    constexpr std::uint64_t iterations = 5000000;
    std::uint64_t state = 1;

    const std::uint64_t started = awake();
    for (std::uint64_t i = 0; i < iterations; i++)
        state = (state * 48271 + (i % 97)) % 2147483647;
    const std::uint64_t elapsed = awake() - started;

    std::printf("iterations: %llu\nelapsed ns: %llu\nchecksum: %llu\n",
                (unsigned long long)iterations,
                (unsigned long long)elapsed,
                (unsigned long long)state);
    return 0;
}
