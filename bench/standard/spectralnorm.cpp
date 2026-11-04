// C++ twin of spectralnorm.ace, ported from spectralnorm.c as written.
#include "../awake.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

constexpr std::size_t N = 4000;

static double eval_a(std::size_t i, std::size_t j) { return 1.0 / (double)((i + j) * (i + j + 1) / 2 + i + 1); }

static void times(std::vector<double> &out, const std::vector<double> &v) {
    for (std::size_t i = 0; i < out.size(); i++) {
        double sum = 0.0;
        for (std::size_t j = 0; j < v.size(); j++) sum += eval_a(i, j) * v[j];
        out[i] = sum;
    }
}

static void times_transpose(std::vector<double> &out, const std::vector<double> &v) {
    for (std::size_t i = 0; i < out.size(); i++) {
        double sum = 0.0;
        for (std::size_t j = 0; j < v.size(); j++) sum += eval_a(j, i) * v[j];
        out[i] = sum;
    }
}

static void times_ata(std::vector<double> &out, const std::vector<double> &v, std::vector<double> &scratch) {
    times(scratch, v);
    times_transpose(out, scratch);
}

int main() {
    std::vector<double> u(N, 1.0), v(N), scratch(N);

    std::uint64_t started = awake();
    for (int k = 0; k < 10; k++) {
        times_ata(v, u, scratch);
        times_ata(u, v, scratch);
    }
    double vbv = 0.0, vv = 0.0;
    for (std::size_t i = 0; i < N; i++) {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    double norm = std::sqrt(vbv / vv);
    std::uint64_t elapsed = awake() - started;

    std::printf("n: %zu\nelapsed ns: %llu\nchecksum: %.9f\n", N, (unsigned long long)elapsed, norm);
    return 0;
}
