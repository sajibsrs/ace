/* Hot-loop kernel benchmark paired with hotloop.ace. Arrays are globals so
 * every access pays the base-address computation the Ace emitter still
 * repeats. Timed results are consumed into checksums so the work stays
 * observable. Ace and C perform identical operations in an identical order,
 * so every checksum must match bit for bit.
 */

#include "../awake.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define N 4096
#define REPEATS 20000
#define PARTICLE_REPEATS 5000
#define CUBES 1024
#define FRAMES 2000
#define ANGLES 64

typedef struct {
    float x, y, z;
} Vec3;

static double xs64[N], ys64[N];
static float xs32[N], ys32[N];
static Vec3 positions[N], velocities[N];

/* A position, a color, and a texture coordinate, laid out as SDL_Vertex. */
typedef struct {
    float x, y, r, g, b, a, u, v;
} Vertex;

/* cube-transform: the per-frame math of demo/sdl3-cube-rotation for many cubes.
 * Corner i sits at x = bit 0, y = bit 1, z = bit 2 of i; a face is four
 * corners sharing one bit, in order around it. */
static const int FACES[24] = {
    0, 2, 6, 4,  1, 3, 7, 5,
    0, 1, 5, 4,  2, 3, 7, 6,
    0, 1, 3, 2,  4, 5, 7, 6,
};
static const unsigned FACE_COLORS[6] = {0xF44336, 0x4CAF50, 0x00BCD4, 0x9C27B0, 0x2196F3, 0xFFEB3B};
#define DISTANCE 4.0f
#define SIZE 800.0f

/* Sines and cosines of evenly spaced angles, stepped by an exact recurrence
 * so Ace and C hold identical bits without sharing a sin/cos kernel. */
static float sines[ANGLES], cosines[ANGLES];
static Vertex vertices[CUBES * 24];
static int indices[CUBES * 36];

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

static void angles(void) {
#pragma STDC FP_CONTRACT OFF
    /* One step of 2*pi/64: cos and sin rounded to float. */
    float step_cos = 0.99518472f;
    float step_sin = 0.09801714f;
    sines[0] = 0.0f;
    cosines[0] = 1.0f;
    for (int i = 1; i < ANGLES; i++) {
        sines[i] = sines[i - 1] * step_cos + cosines[i - 1] * step_sin;
        cosines[i] = cosines[i - 1] * step_cos - sines[i - 1] * step_sin;
    }
}

static float channel(unsigned rgb, unsigned shift) {
    return (float)((rgb >> shift) & 0xFF) / 255.0f;
}

static float axis(int i, int bit) {
    if (((i >> bit) & 1) == 1) return 1.0f;
    return -1.0f;
}

/* Rotates, projects, and culls every cube for one frame, filling vertices
 * and indices for the visible faces; returns the vertex count. */
static size_t cubes(size_t frame) {
    /* Ace never fuses a*b+c into one rounding; neither may C here, or the
     * checksum would differ. */
#pragma STDC FP_CONTRACT OFF
    float xs[8] = {0}, ys[8] = {0}, zs[8] = {0}, sx[8] = {0}, sy[8] = {0};
    float cell = SIZE / 32.0f;
    float scale = cell * 0.55f;
    size_t nv = 0, ni = 0;
    for (size_t n = 0; n < CUBES; n++) {
        float cx = ((float)(n % 32) + 0.5f) * cell;
        float cy = ((float)(n / 32) + 0.5f) * cell;
        size_t ai = (n * 7 + frame) % ANGLES;
        size_t bi = (n * 5 + frame * 3) % ANGLES;
        float sa = sines[ai];
        float ca = cosines[ai];
        float sb = sines[bi];
        float cb = cosines[bi];
        for (int i = 0; i < 8; i++) {
            float x = axis(i, 0);
            float y = axis(i, 1);
            float z = axis(i, 2);
            float rx = x * ca + z * sa;
            float rz = z * ca - x * sa;
            float ry = y * cb - rz * sb;
            rz = y * sb + rz * cb + DISTANCE;
            xs[i] = rx;
            ys[i] = ry;
            zs[i] = rz;
            sx[i] = cx + rx * scale / rz;
            sy[i] = cy - ry * scale / rz;
        }
        for (size_t f = 0; f < 6; f++) {
            int a = FACES[f * 4];
            int c = FACES[f * 4 + 2];
            float mx = (xs[a] + xs[c]) * 0.5f;
            float my = (ys[a] + ys[c]) * 0.5f;
            float mz = (zs[a] + zs[c]) * 0.5f;
            if (mx * mx + my * my + (mz - DISTANCE) * mz >= 0.0f) continue;
            unsigned rgb = FACE_COLORS[f];
            float r = channel(rgb, 16);
            float g = channel(rgb, 8);
            float b = channel(rgb, 0);
            for (size_t k = 0; k < 4; k++) {
                int corner = FACES[f * 4 + k];
                vertices[nv + k] = (Vertex){sx[corner], sy[corner], r, g, b, 1.0f, 0.0f, 0.0f};
            }
            int base = (int)nv;
            indices[ni] = base;
            indices[ni + 1] = base + 1;
            indices[ni + 2] = base + 2;
            indices[ni + 3] = base;
            indices[ni + 4] = base + 2;
            indices[ni + 5] = base + 3;
            nv += 4;
            ni += 6;
        }
    }
    return nv;
}

static uint64_t sum_vertices(size_t count) {
    double acc = 0.0;
    for (size_t i = 0; i < count; i++)
        acc += (double)vertices[i].x + (double)vertices[i].y * 3.0 + (double)indices[i];
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

    started = awake();
    for (uint64_t r = 0; r < REPEATS; r++)
        for (int i = 0; i < N; i++)
            ys64[i] = 0.5 * xs64[i] + ys64[i];
    report("saxpy-f64", awake() - started, saxpy_ops, sum64(ys64));

    started = awake();
    for (uint64_t r = 0; r < REPEATS; r++)
        for (int i = 0; i < N; i++)
            ys32[i] = 0.5f * xs32[i] + ys32[i];
    report("saxpy-f32", awake() - started, saxpy_ops, sum32(ys32));

    double acc = 0.0;
    started = awake();
    for (uint64_t r = 0; r < REPEATS; r++)
        for (int i = 0; i < N; i++)
            acc += xs64[i] * ys64[i];
    report("dot-f64", awake() - started, saxpy_ops, bits64(acc));

    started = awake();
    for (uint64_t r = 0; r < PARTICLE_REPEATS; r++)
        for (int i = 0; i < N; i++) {
            positions[i].x += velocities[i].x * 0.5f;
            positions[i].y += velocities[i].y * 0.5f;
            positions[i].z += velocities[i].z * 0.5f;
        }
    report("particle-vec3", awake() - started, particle_ops, sum_particles());

    double length = 0.0;
    started = awake();
    for (uint64_t r = 0; r < PARTICLE_REPEATS; r++)
        for (int i = 0; i < N; i++)
            length += (double)sqrtf(velocities[i].x * velocities[i].x +
                                    velocities[i].y * velocities[i].y +
                                    velocities[i].z * velocities[i].z);
    report("length-vec3", awake() - started, particle_ops, bits64(length));

    angles();
    size_t count = 0;
    started = awake();
    for (size_t frame = 0; frame < FRAMES; frame++) count = cubes(frame);
    report("cube-transform", awake() - started, (uint64_t)CUBES * FRAMES, sum_vertices(count));
    return 0;
}
