// C++ twin of mandelbrot.ace, ported from mandelbrot.c as written.
#include "../awake.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

constexpr std::size_t SIZE = 4000;
constexpr int ITERATIONS = 50;
constexpr std::uint64_t MODULUS = 1000000007;

int main() {
    std::vector<std::uint8_t> row(SIZE / 8);
    std::uint64_t checksum = 0;

    std::uint64_t started = awake();
    for (std::size_t y = 0; y < SIZE; y++) {
        double ci = 2.0 * (double)y / (double)SIZE - 1.0;
        for (std::size_t x = 0; x < SIZE; x += 8) {
            std::uint8_t bits = 0;
            for (std::size_t dx = 0; dx < 8; dx++) {
                double cr = 2.0 * (double)(x + dx) / (double)SIZE - 1.5;
                double zr = 0.0, zi = 0.0, tr = 0.0, ti = 0.0;
                for (int i = 0; i < ITERATIONS && tr + ti <= 4.0; i++) {
                    zi = 2.0 * zr * zi + ci;
                    zr = tr - ti + cr;
                    tr = zr * zr;
                    ti = zi * zi;
                }
                bits = (std::uint8_t)(bits << 1);
                if (tr + ti <= 4.0) bits = bits | 1;
            }
            row[x / 8] = bits;
        }
        for (std::uint8_t b : row) checksum = (checksum * 31 + b) % MODULUS;
    }
    std::uint64_t elapsed = awake() - started;

    std::printf("size: %zu\nelapsed ns: %llu\nchecksum: %llu\n", SIZE, (unsigned long long)elapsed,
                (unsigned long long)checksum);
    return 0;
}
