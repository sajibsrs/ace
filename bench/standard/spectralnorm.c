// C twin of spectralnorm.ace.
#include "../awake.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define N 4000

static double eval_a(size_t i, size_t j) { return 1.0 / (double)((i + j) * (i + j + 1) / 2 + i + 1); }

static void times(double *out, const double *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) sum += eval_a(i, j) * v[j];
        out[i] = sum;
    }
}

static void times_transpose(double *out, const double *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) sum += eval_a(j, i) * v[j];
        out[i] = sum;
    }
}

static void times_ata(double *out, const double *v, double *scratch, size_t n) {
    times(scratch, v, n);
    times_transpose(out, scratch, n);
}

int main(void) {
    double *u = malloc(N * sizeof(double));
    double *v = malloc(N * sizeof(double));
    double *scratch = malloc(N * sizeof(double));
    if (!u || !v || !scratch) return 1;
    for (size_t i = 0; i < N; i++) u[i] = 1.0;

    uint64_t started = awake();
    for (int k = 0; k < 10; k++) {
        times_ata(v, u, scratch, N);
        times_ata(u, v, scratch, N);
    }
    double vbv = 0.0, vv = 0.0;
    for (size_t i = 0; i < N; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    double norm = sqrt(vbv / vv);
    uint64_t elapsed = awake() - started;

    printf("n: %d\nelapsed ns: %llu\nchecksum: %.9f\n", N, (unsigned long long)elapsed, norm);
    free(u);
    free(v);
    free(scratch);
    return 0;
}
