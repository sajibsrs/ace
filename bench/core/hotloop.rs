// Hot-loop kernel benchmark paired with hotloop.ace and hotloop.c. Arrays are
// heap slices indexed as C indexes them (bounds checks stay unless LLVM proves
// them redundant). Rust never fuses a*b+c, so every checksum matches C's
// FP_CONTRACT OFF build bit for bit.
use std::time::Instant;

const N: usize = 4096;
const REPEATS: u64 = 20000;
const PARTICLE_REPEATS: u64 = 5000;
const CUBES: usize = 1024;
const FRAMES: usize = 2000;
const ANGLES: usize = 64;

#[derive(Clone, Copy, Default)]
struct Vec3 { x: f32, y: f32, z: f32 }

/// A position, a color, and a texture coordinate, laid out as SDL_Vertex.
#[allow(dead_code)] // only x and y feed the checksum, as in C
#[derive(Clone, Copy, Default)]
struct Vertex { x: f32, y: f32, r: f32, g: f32, b: f32, a: f32, u: f32, v: f32 }

// Corner i sits at x = bit 0, y = bit 1, z = bit 2 of i; a face is four
// corners sharing one bit, in order around it.
const FACES: [usize; 24] = [0, 2, 6, 4, 1, 3, 7, 5, 0, 1, 5, 4, 2, 3, 7, 6, 0, 1, 3, 2, 4, 5, 7, 6];
const FACE_COLORS: [u32; 6] = [0xF44336, 0x4CAF50, 0x00BCD4, 0x9C27B0, 0x2196F3, 0xFFEB3B];
const DISTANCE: f32 = 4.0;
const SIZE: f32 = 800.0;

fn report(name: &str, started: Instant, operations: u64, checksum: u64) {
    println!("{name} {} {operations} {checksum}", started.elapsed().as_nanos());
}

fn sum64(v: &[f64]) -> u64 {
    let mut acc = 0.0;
    for x in v { acc += x; }
    f64::to_bits(acc)
}

fn sum32(v: &[f32]) -> u64 {
    let mut acc = 0.0;
    for &x in v { acc += x as f64; }
    f64::to_bits(acc)
}

fn sum_particles(p: &[Vec3]) -> u64 {
    let mut acc = 0.0;
    for q in p { acc += q.x as f64 + q.y as f64 + q.z as f64; }
    f64::to_bits(acc)
}

/// Sines and cosines of evenly spaced angles, stepped by an exact recurrence.
fn angles() -> ([f32; ANGLES], [f32; ANGLES]) {
    let step_cos: f32 = 0.99518472;
    let step_sin: f32 = 0.09801714;
    let mut sines = [0.0f32; ANGLES];
    let mut cosines = [0.0f32; ANGLES];
    cosines[0] = 1.0;
    for i in 1..ANGLES {
        sines[i] = sines[i - 1] * step_cos + cosines[i - 1] * step_sin;
        cosines[i] = cosines[i - 1] * step_cos - sines[i - 1] * step_sin;
    }
    (sines, cosines)
}

fn channel(rgb: u32, shift: u32) -> f32 { ((rgb >> shift) & 0xFF) as f32 / 255.0 }

fn axis(i: usize, bit: usize) -> f32 { if (i >> bit) & 1 == 1 { 1.0 } else { -1.0 } }

struct Scene { sines: [f32; ANGLES], cosines: [f32; ANGLES], vertices: Vec<Vertex>, indices: Vec<i32> }

impl Scene {
    /// Rotates, projects, and culls every cube for one frame, filling vertices
    /// and indices for the visible faces; returns the vertex count.
    fn cubes(&mut self, frame: usize) -> usize {
        let (mut xs, mut ys, mut zs, mut sx, mut sy) = ([0.0f32; 8], [0.0f32; 8], [0.0f32; 8], [0.0f32; 8], [0.0f32; 8]);
        let cell = SIZE / 32.0;
        let scale = cell * 0.55;
        let (mut nv, mut ni) = (0, 0);
        for n in 0..CUBES {
            let cx = ((n % 32) as f32 + 0.5) * cell;
            let cy = ((n / 32) as f32 + 0.5) * cell;
            let ai = (n * 7 + frame) % ANGLES;
            let bi = (n * 5 + frame * 3) % ANGLES;
            let (sa, ca) = (self.sines[ai], self.cosines[ai]);
            let (sb, cb) = (self.sines[bi], self.cosines[bi]);
            for i in 0..8 {
                let (x, y, z) = (axis(i, 0), axis(i, 1), axis(i, 2));
                let rx = x * ca + z * sa;
                let mut rz = z * ca - x * sa;
                let ry = y * cb - rz * sb;
                rz = y * sb + rz * cb + DISTANCE;
                xs[i] = rx;
                ys[i] = ry;
                zs[i] = rz;
                sx[i] = cx + rx * scale / rz;
                sy[i] = cy - ry * scale / rz;
            }
            for f in 0..6 {
                let a = FACES[f * 4];
                let c = FACES[f * 4 + 2];
                let mx = (xs[a] + xs[c]) * 0.5;
                let my = (ys[a] + ys[c]) * 0.5;
                let mz = (zs[a] + zs[c]) * 0.5;
                if mx * mx + my * my + (mz - DISTANCE) * mz >= 0.0 { continue; }
                let rgb = FACE_COLORS[f];
                let (r, g, b) = (channel(rgb, 16), channel(rgb, 8), channel(rgb, 0));
                for k in 0..4 {
                    let corner = FACES[f * 4 + k];
                    self.vertices[nv + k] = Vertex { x: sx[corner], y: sy[corner], r, g, b, a: 1.0, u: 0.0, v: 0.0 };
                }
                let base = nv as i32;
                self.indices[ni..ni + 6].copy_from_slice(&[base, base + 1, base + 2, base, base + 2, base + 3]);
                nv += 4;
                ni += 6;
            }
        }
        nv
    }

    fn sum_vertices(&self, count: usize) -> u64 {
        let mut acc = 0.0;
        for i in 0..count {
            acc += self.vertices[i].x as f64 + self.vertices[i].y as f64 * 3.0 + self.indices[i] as f64;
        }
        f64::to_bits(acc)
    }
}

fn main() {
    let saxpy_ops = N as u64 * REPEATS;
    let particle_ops = N as u64 * PARTICLE_REPEATS;

    let mut xs64 = vec![0.0f64; N];
    let mut ys64 = vec![0.0f64; N];
    let mut xs32 = vec![0.0f32; N];
    let mut ys32 = vec![0.0f32; N];
    let mut positions = vec![Vec3::default(); N];
    let mut velocities = vec![Vec3::default(); N];
    for i in 0..N {
        xs64[i] = (i % 17) as f64 * 0.25;
        xs32[i] = (i % 17) as f32 * 0.25;
        positions[i] = Vec3 { x: (i % 11) as f32 * 0.5, y: (i % 13) as f32 * 0.5, z: (i % 7) as f32 * 0.5 };
        velocities[i] = Vec3 { x: (i % 3) as f32 * 0.25, y: (i % 5) as f32 * 0.25, z: (i % 9) as f32 * 0.25 };
    }

    let started = Instant::now();
    for _ in 0..REPEATS {
        for i in 0..N { ys64[i] = 0.5 * xs64[i] + ys64[i]; }
    }
    report("saxpy-f64", started, saxpy_ops, sum64(&ys64));

    let started = Instant::now();
    for _ in 0..REPEATS {
        for i in 0..N { ys32[i] = 0.5 * xs32[i] + ys32[i]; }
    }
    report("saxpy-f32", started, saxpy_ops, sum32(&ys32));

    let mut acc = 0.0;
    let started = Instant::now();
    for _ in 0..REPEATS {
        for i in 0..N { acc += xs64[i] * ys64[i]; }
    }
    report("dot-f64", started, saxpy_ops, f64::to_bits(acc));

    let started = Instant::now();
    for _ in 0..PARTICLE_REPEATS {
        for i in 0..N {
            positions[i].x += velocities[i].x * 0.5;
            positions[i].y += velocities[i].y * 0.5;
            positions[i].z += velocities[i].z * 0.5;
        }
    }
    report("particle-vec3", started, particle_ops, sum_particles(&positions));

    let mut length = 0.0;
    let started = Instant::now();
    for _ in 0..PARTICLE_REPEATS {
        for v in &velocities {
            length += (v.x * v.x + v.y * v.y + v.z * v.z).sqrt() as f64;
        }
    }
    report("length-vec3", started, particle_ops, f64::to_bits(length));

    let (sines, cosines) = angles();
    let mut scene = Scene { sines, cosines, vertices: vec![Vertex::default(); CUBES * 24], indices: vec![0; CUBES * 36] };
    let mut count = 0;
    let started = Instant::now();
    for frame in 0..FRAMES { count = scene.cubes(frame); }
    report("cube-transform", started, (CUBES * FRAMES) as u64, scene.sum_vertices(count));
}
