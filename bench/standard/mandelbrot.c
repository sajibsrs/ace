// C twin of mandelbrot.ace.
#include "../awake.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SIZE 4000
#define ITERATIONS 50
#define MODULUS 1000000007u

int main(void) {
    uint8_t *row = malloc(SIZE / 8);
    if (!row) return 1;
    uint64_t checksum = 0;

    uint64_t started = awake();
    for (size_t y = 0; y < SIZE; y++) {
        double ci = 2.0 * (double)y / (double)SIZE - 1.0;
        for (size_t x = 0; x < SIZE; x += 8) {
            uint8_t bits = 0;
            for (size_t dx = 0; dx < 8; dx++) {
                double cr = 2.0 * (double)(x + dx) / (double)SIZE - 1.5;
                double zr = 0.0, zi = 0.0, tr = 0.0, ti = 0.0;
                for (int i = 0; i < ITERATIONS && tr + ti <= 4.0; i++) {
                    zi = 2.0 * zr * zi + ci;
                    zr = tr - ti + cr;
                    tr = zr * zr;
                    ti = zi * zi;
                }
                bits = (uint8_t)(bits << 1);
                if (tr + ti <= 4.0) bits = bits | 1;
            }
            row[x / 8] = bits;
        }
        for (size_t i = 0; i < SIZE / 8; i++) checksum = (checksum * 31 + row[i]) % MODULUS;
    }
    uint64_t elapsed = awake() - started;

    printf("size: %d\nelapsed ns: %llu\nchecksum: %llu\n", SIZE, (unsigned long long)elapsed,
           (unsigned long long)checksum);
    free(row);
    return 0;
}
