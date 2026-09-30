// C twin of raytrace.ace.
#ifdef __linux__
#define _DEFAULT_SOURCE /* CLOCK_MONOTONIC_RAW */
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const uint32_t FACE_COLORS[6] = {0xF44336, 0x4CAF50, 0x00BCD4, 0x9C27B0, 0x2196F3, 0xFFA000};
#define BACKGROUND 0x101018u
#define FLOOR_COLOR 0x546E7Au
#define DISTANCE 4.0f
#define FLOOR_Y (-2.0f)
#define AMBIENT 0.25f
#define CELL 2
#define EPSILON 0.001f
#define FAR 1.0e30f
#define SIZE 640
#define FRAMES 200

typedef struct { float r, g, b, a; } Color;
typedef struct { float x, y; } Point;
typedef struct { Point position; Color color; Point uv; } Vertex;
typedef struct { float x, y, z; } Vec;
typedef struct { float sa, ca, sb, cb; } Spin;
typedef struct { float near, far; int32_t face; } Span;

static Color color_hex(uint32_t rgb) {
    return (Color){(float)((rgb >> 16) & 0xFF) / 255.0f, (float)((rgb >> 8) & 0xFF) / 255.0f,
                   (float)(rgb & 0xFF) / 255.0f, 1.0f};
}

static float vec_dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static Vec to_camera(Spin s, Vec v) {
    float z = v.z * s.ca - v.x * s.sa;
    return (Vec){v.x * s.ca + v.z * s.sa, v.y * s.cb - z * s.sb, v.y * s.sb + z * s.cb};
}

static Vec to_cube(Spin s, Vec v) {
    float z = v.z * s.cb - v.y * s.sb;
    return (Vec){v.x * s.ca - z * s.sa, v.y * s.cb + v.z * s.sb, v.x * s.sa + z * s.ca};
}

static int clip(Span *self, float o, float d, int32_t axis) {
    if (d == 0.0f) return o >= -1.0f && o <= 1.0f;
    float t0 = (-1.0f - o) / d, t1 = (1.0f - o) / d;
    int32_t side = 0;
    if (t0 > t1) { float t = t0; t0 = t1; t1 = t; side = 1; }
    if (t0 > self->near) { self->near = t0; self->face = axis * 2 + side; }
    if (t1 < self->far) self->far = t1;
    return self->near <= self->far;
}

static int32_t hit(Spin spin, Vec o, Vec d, float *t) {
    Vec lo = to_cube(spin, (Vec){o.x, o.y, o.z - DISTANCE});
    Vec ld = to_cube(spin, d);
    Span span = {0.0f, FAR, -1};
    *t = 0.0f;
    if (!clip(&span, lo.x, ld.x, 0)) return -1;
    if (!clip(&span, lo.y, ld.y, 1)) return -1;
    if (!clip(&span, lo.z, ld.z, 2)) return -1;
    if (span.face < 0 || span.near < EPSILON) return -1;
    *t = span.near;
    return span.face;
}

static Vec normal(int32_t face) {
    float s = -1.0f;
    if ((face & 1) == 1) s = 1.0f;
    if (face < 2) return (Vec){s, 0, 0};
    if (face < 4) return (Vec){0, s, 0};
    return (Vec){0, 0, s};
}

static Color shade(uint32_t rgb, float cosine) {
    Color base = color_hex(rgb);
    float k = AMBIENT;
    if (cosine > 0.0f) k += (1.0f - AMBIENT) * cosine;
    return (Color){base.r * k, base.g * k, base.b * k, 1.0f};
}

static Color trace(Spin spin, Vec light, Vec d) {
    float t;
    int32_t face = hit(spin, (Vec){0, 0, 0}, d, &t);
    if (face >= 0) return shade(FACE_COLORS[face], vec_dot(to_camera(spin, normal(face)), light));
    if (d.y >= 0.0f) return color_hex(BACKGROUND);
    t = FLOOR_Y / d.y;
    Vec p = {d.x * t, FLOOR_Y, d.z * t};
    float u;
    if (hit(spin, p, light, &u) >= 0) return shade(FLOOR_COLOR, 0.0f);
    return shade(FLOOR_COLOR, light.y);
}

/* The clock Ace's time.awake reads on each target. */
static uint64_t awake(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

int main(void) {
    float lx = -0.5f, ly = 1.0f, lz = -0.6f;
    float ll = sqrtf(lx * lx + ly * ly + lz * lz);
    Vec light = {lx / ll, ly / ll, lz / ll};
    int32_t w = SIZE, h = SIZE;
    float cx = (float)w * 0.5f, cy = (float)h * 0.5f, scale = cx * 1.1f;
    int32_t columns = (w + CELL - 1) / CELL, rows = (h + CELL - 1) / CELL;
    size_t cells = (size_t)columns * (size_t)rows;
    Vertex *vertices = calloc(cells * 4, sizeof(Vertex));
    if (!vertices) return 1;

    uint64_t checksum = 0, started = awake();
    for (int32_t frame = 0; frame < FRAMES; frame++) {
        float seconds = (float)frame / 60.0f;
        Spin spin = {sinf(seconds * 0.9f), cosf(seconds * 0.9f), sinf(seconds * 0.6f), cosf(seconds * 0.6f)};
        size_t nv = 0;
        for (int32_t row = 0; row < rows; row++) {
            float y0 = (float)(row * CELL), y1 = (float)(row * CELL + CELL);
            if (y1 > (float)h) y1 = (float)h;
            float dy = (cy - (y0 + y1) * 0.5f) / scale;
            for (int32_t column = 0; column < columns; column++) {
                float x0 = (float)(column * CELL), x1 = (float)(column * CELL + CELL);
                if (x1 > (float)w) x1 = (float)w;
                float dx = ((x0 + x1) * 0.5f - cx) / scale;
                Color color = trace(spin, light, (Vec){dx, dy, 1.0f});
                vertices[nv] = (Vertex){{x0, y0}, color, {0, 0}};
                vertices[nv + 1] = (Vertex){{x1, y0}, color, {0, 0}};
                vertices[nv + 2] = (Vertex){{x1, y1}, color, {0, 0}};
                vertices[nv + 3] = (Vertex){{x0, y1}, color, {0, 0}};
                nv += 4;
            }
        }
        Color c = vertices[((size_t)frame * 4099) % nv].color;
        checksum += (uint64_t)(c.r * 255.0f) + (uint64_t)(c.g * 255.0f) * 256 + (uint64_t)(c.b * 255.0f) * 65536;
    }
    uint64_t elapsed = awake() - started;
    printf("rays: %zu\nelapsed ns: %llu\nchecksum: %llu\n", cells * FRAMES, (unsigned long long)elapsed,
           (unsigned long long)checksum);
    free(vertices);
    return 0;
}
