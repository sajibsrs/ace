/* Hot-loop kernel benchmark paired with hotloop.ace. Arrays are globals so
 * every access pays the base-address computation the Ace emitter still
 * repeats. Timed results are consumed into checksums so the work stays
 * observable. Ace and C perform identical operations in an identical order,
 * so every checksum must match bit for bit.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define N 4096
#define REPEATS 20000
#define PARTICLE_REPEATS 5000

typedef struct {
    float x, y, z;
} Vec3;

static double xs64[N], ys64[N];
static float xs32[N], ys32[N];
static Vec3 positions[N], velocities[N];

static uint64_t now(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void report(const char* name, uint64_t elapsed, uint64_t operations,
                   uint64_t checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed,
           (unsigned long long)operations, (unsigned long long)checksum);
}

static uint64_t bits64(double v) {
    uint64_t r;
    memcpy(&r, &v, sizeof r);
    return r;
}

/* Whole-array reductions in double: a single element can be insensitive to
 * the kernel (some particles have zero velocity) and would check nothing. */
static uint64_t sum64(const double* v) {
    double acc = 0.0;
    for (int i = 0; i < N; i++) acc += v[i];
    return bits64(acc);
}

static uint64_t sum32(const float* v) {
    double acc = 0.0;
    for (int i = 0; i < N; i++) acc += (double)v[i];
    return bits64(acc);
}

static uint64_t sum_particles(void) {
    double acc = 0.0;
    for (int i = 0; i < N; i++)
        acc += (double)positions[i].x + (double)positions[i].y +
               (double)positions[i].z;
    return bits64(acc);
}

int main(void) {
    const uint64_t saxpy_ops = (uint64_t)N * REPEATS;
    const uint64_t particle_ops = (uint64_t)N * PARTICLE_REPEATS;
    uint64_t started;

    for (int i = 0; i < N; i++) {
        xs64[i] = (double)(i % 17) * 0.25;
        ys64[i] = 0.0;
        xs32[i] = (float)(i % 17) * 0.25f;
        ys32[i] = 0.0f;
        positions[i].x = (float)(i % 11) * 0.5f;
        positions[i].y = (float)(i % 13) * 0.5f;
        positions[i].z = (float)(i % 7) * 0.5f;
        velocities[i].x = (float)(i % 3) * 0.25f;
        velocities[i].y = (float)(i % 5) * 0.25f;
        velocities[i].z = (float)(i % 9) * 0.25f;
    }

    started = now();
    for (uint64_t r = 0; r < REPEATS; r++)
        for (int i = 0; i < N; i++)
            ys64[i] = 0.5 * xs64[i] + ys64[i];
    report("saxpy-f64", now() - started, saxpy_ops, sum64(ys64));

    started = now();
    for (uint64_t r = 0; r < REPEATS; r++)
        for (int i = 0; i < N; i++)
            ys32[i] = 0.5f * xs32[i] + ys32[i];
    report("saxpy-f32", now() - started, saxpy_ops, sum32(ys32));

    double acc = 0.0;
    started = now();
    for (uint64_t r = 0; r < REPEATS; r++)
        for (int i = 0; i < N; i++)
            acc += xs64[i] * ys64[i];
    report("dot-f64", now() - started, saxpy_ops, bits64(acc));

    started = now();
    for (uint64_t r = 0; r < PARTICLE_REPEATS; r++)
        for (int i = 0; i < N; i++) {
            positions[i].x += velocities[i].x * 0.5f;
            positions[i].y += velocities[i].y * 0.5f;
            positions[i].z += velocities[i].z * 0.5f;
        }
    report("particle-vec3", now() - started, particle_ops, sum_particles());

    double length = 0.0;
    started = now();
    for (uint64_t r = 0; r < PARTICLE_REPEATS; r++)
        for (int i = 0; i < N; i++)
            length += (double)sqrtf(velocities[i].x * velocities[i].x +
                                    velocities[i].y * velocities[i].y +
                                    velocities[i].z * velocities[i].z);
    report("length-vec3", now() - started, particle_ops, bits64(length));
    return 0;
}
