// C++ twin of fannkuch.ace, ported from fannkuch.c as written.
#include "../awake.h"
#include <cstdint>
#include <cstdio>

constexpr std::int32_t N = 10;

int main() {
    std::int32_t perm[N], perm1[N], count[N];
    for (std::int32_t i = 0; i < N; i++) {
        perm1[i] = i;
        count[i] = 0;
    }
    std::int32_t max_flips = 0, checksum = 0, permutations = 0, r = N;

    std::uint64_t started = awake();
    for (;;) {
        while (r != 1) {
            count[r - 1] = r;
            r -= 1;
        }

        for (std::int32_t i = 0; i < N; i++) perm[i] = perm1[i];
        std::int32_t flips = 0;
        std::int32_t k = perm[0];
        while (k != 0) {
            std::int32_t i = 0, j = k;
            while (i < j) {
                std::int32_t t = perm[i];
                perm[i] = perm[j];
                perm[j] = t;
                i += 1;
                j -= 1;
            }
            flips += 1;
            k = perm[0];
        }
        if (flips > max_flips) max_flips = flips;
        if (permutations % 2 == 0) checksum += flips;
        else checksum -= flips;

        for (;;) {
            if (r == N) goto done;
            std::int32_t perm0 = perm1[0];
            for (std::int32_t i = 0; i < r; i++) perm1[i] = perm1[i + 1];
            perm1[r] = perm0;
            count[r] -= 1;
            if (count[r] > 0) break;
            r += 1;
        }
        permutations += 1;
    }
done:;
    std::uint64_t elapsed = awake() - started;

    std::printf("max flips: %d\nelapsed ns: %llu\nchecksum: %d\n", max_flips, (unsigned long long)elapsed, checksum);
    return 0;
}
