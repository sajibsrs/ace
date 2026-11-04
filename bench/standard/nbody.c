// C twin of nbody.ace.
#include "../awake.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define STEPS 10000000
#define PI 3.141592653589793
#define SOLAR_MASS (4.0 * PI * PI)
#define DAYS_PER_YEAR 365.24
#define BODIES 5

typedef struct { double x, y, z, vx, vy, vz, mass; } Body;

static void offset_momentum(Body *bodies) {
    double px = 0.0, py = 0.0, pz = 0.0;
    for (size_t i = 0; i < BODIES; i++) {
        px += bodies[i].vx * bodies[i].mass;
        py += bodies[i].vy * bodies[i].mass;
        pz += bodies[i].vz * bodies[i].mass;
    }
    bodies[0].vx = -px / SOLAR_MASS;
    bodies[0].vy = -py / SOLAR_MASS;
    bodies[0].vz = -pz / SOLAR_MASS;
}

static double energy(const Body *bodies) {
    double e = 0.0;
    for (size_t i = 0; i < BODIES; i++) {
        Body b = bodies[i];
        e += 0.5 * b.mass * (b.vx * b.vx + b.vy * b.vy + b.vz * b.vz);
        for (size_t j = i + 1; j < BODIES; j++) {
            Body o = bodies[j];
            double dx = b.x - o.x, dy = b.y - o.y, dz = b.z - o.z;
            e -= b.mass * o.mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void advance(Body *bodies, double dt) {
    for (size_t i = 0; i < BODIES; i++) {
        for (size_t j = i + 1; j < BODIES; j++) {
            double dx = bodies[i].x - bodies[j].x;
            double dy = bodies[i].y - bodies[j].y;
            double dz = bodies[i].z - bodies[j].z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * sqrt(d2));
            double mi = bodies[i].mass * mag;
            double mj = bodies[j].mass * mag;
            bodies[i].vx -= dx * mj;
            bodies[i].vy -= dy * mj;
            bodies[i].vz -= dz * mj;
            bodies[j].vx += dx * mi;
            bodies[j].vy += dy * mi;
            bodies[j].vz += dz * mi;
        }
    }
    for (size_t i = 0; i < BODIES; i++) {
        bodies[i].x += dt * bodies[i].vx;
        bodies[i].y += dt * bodies[i].vy;
        bodies[i].z += dt * bodies[i].vz;
    }
}

int main(void) {
    Body bodies[BODIES] = {
        /* sun */
        {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS},
        /* jupiter */
        {4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
         1.66007664274403694e-03 * DAYS_PER_YEAR, 7.69901118419740425e-03 * DAYS_PER_YEAR,
         -6.90460016972063023e-05 * DAYS_PER_YEAR, 9.54791938424326609e-04 * SOLAR_MASS},
        /* saturn */
        {8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
         -2.76742510726862411e-03 * DAYS_PER_YEAR, 4.99852801234917238e-03 * DAYS_PER_YEAR,
         2.30417297573763929e-05 * DAYS_PER_YEAR, 2.85885980666130812e-04 * SOLAR_MASS},
        /* uranus */
        {1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
         2.96460137564761618e-03 * DAYS_PER_YEAR, 2.37847173959480950e-03 * DAYS_PER_YEAR,
         -2.96589568540237556e-05 * DAYS_PER_YEAR, 4.36624404335156298e-05 * SOLAR_MASS},
        /* neptune */
        {1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
         2.68067772490389322e-03 * DAYS_PER_YEAR, 1.62824170038242295e-03 * DAYS_PER_YEAR,
         -9.51592254519715870e-05 * DAYS_PER_YEAR, 5.15138902046611451e-05 * SOLAR_MASS},
    };
    offset_momentum(bodies);
    double before = energy(bodies);

    uint64_t started = awake();
    for (int i = 0; i < STEPS; i++) advance(bodies, 0.01);
    uint64_t elapsed = awake() - started;

    printf("energy: %.9f\nelapsed ns: %llu\nchecksum: %.9f\n", before, (unsigned long long)elapsed, energy(bodies));
    return 0;
}
