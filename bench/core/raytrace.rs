// Rust twin of raytrace.ace and raytrace.c.
use std::time::Instant;

const FACE_COLORS: [u32; 6] = [0xF44336, 0x4CAF50, 0x00BCD4, 0x9C27B0, 0x2196F3, 0xFFA000];
const BACKGROUND: u32 = 0x101018;
const FLOOR_COLOR: u32 = 0x546E7A;
const DISTANCE: f32 = 4.0;
const FLOOR_Y: f32 = -2.0;
const AMBIENT: f32 = 0.25;
const CELL: i32 = 2;
const EPSILON: f32 = 0.001;
const FAR: f32 = 1.0e30;
const SIZE: i32 = 640;
const FRAMES: i32 = 200;

#[allow(dead_code)] // alpha is filled as for SDL, not read
#[derive(Clone, Copy, Default)]
struct Color { r: f32, g: f32, b: f32, a: f32 }
#[allow(dead_code)]
#[derive(Clone, Copy, Default)]
struct Point { x: f32, y: f32 }
#[allow(dead_code)] // position and uv are filled as for SDL, not read
#[derive(Clone, Copy, Default)]
struct Vertex { position: Point, color: Color, uv: Point }
#[derive(Clone, Copy)]
struct Vec3 { x: f32, y: f32, z: f32 }
#[derive(Clone, Copy)]
struct Spin { sa: f32, ca: f32, sb: f32, cb: f32 }
struct Span { near: f32, far: f32, face: i32 }

fn color_hex(rgb: u32) -> Color {
    Color {
        r: ((rgb >> 16) & 0xFF) as f32 / 255.0,
        g: ((rgb >> 8) & 0xFF) as f32 / 255.0,
        b: (rgb & 0xFF) as f32 / 255.0,
        a: 1.0,
    }
}

impl Vec3 {
    fn dot(self, b: Vec3) -> f32 { self.x * b.x + self.y * b.y + self.z * b.z }
}

impl Spin {
    fn to_camera(self, v: Vec3) -> Vec3 {
        let z = v.z * self.ca - v.x * self.sa;
        Vec3 { x: v.x * self.ca + v.z * self.sa, y: v.y * self.cb - z * self.sb, z: v.y * self.sb + z * self.cb }
    }

    fn to_cube(self, v: Vec3) -> Vec3 {
        let z = v.z * self.cb - v.y * self.sb;
        Vec3 { x: v.x * self.ca - z * self.sa, y: v.y * self.cb + v.z * self.sb, z: v.x * self.sa + z * self.ca }
    }
}

impl Span {
    fn clip(&mut self, o: f32, d: f32, axis: i32) -> bool {
        if d == 0.0 { return o >= -1.0 && o <= 1.0; }
        let (mut t0, mut t1) = ((-1.0 - o) / d, (1.0 - o) / d);
        let mut side = 0;
        if t0 > t1 { std::mem::swap(&mut t0, &mut t1); side = 1; }
        if t0 > self.near { self.near = t0; self.face = axis * 2 + side; }
        if t1 < self.far { self.far = t1; }
        self.near <= self.far
    }
}

/// The face hit and the distance to it, if the ray meets the cube.
fn hit(spin: Spin, o: Vec3, d: Vec3) -> Option<(i32, f32)> {
    let lo = spin.to_cube(Vec3 { x: o.x, y: o.y, z: o.z - DISTANCE });
    let ld = spin.to_cube(d);
    let mut span = Span { near: 0.0, far: FAR, face: -1 };
    if !span.clip(lo.x, ld.x, 0) || !span.clip(lo.y, ld.y, 1) || !span.clip(lo.z, ld.z, 2) { return None; }
    if span.face < 0 || span.near < EPSILON { return None; }
    Some((span.face, span.near))
}

fn normal(face: i32) -> Vec3 {
    let s = if face & 1 == 1 { 1.0 } else { -1.0 };
    if face < 2 { return Vec3 { x: s, y: 0.0, z: 0.0 }; }
    if face < 4 { return Vec3 { x: 0.0, y: s, z: 0.0 }; }
    Vec3 { x: 0.0, y: 0.0, z: s }
}

fn shade(rgb: u32, cosine: f32) -> Color {
    let base = color_hex(rgb);
    let mut k = AMBIENT;
    if cosine > 0.0 { k += (1.0 - AMBIENT) * cosine; }
    Color { r: base.r * k, g: base.g * k, b: base.b * k, a: 1.0 }
}

fn trace(spin: Spin, light: Vec3, d: Vec3) -> Color {
    if let Some((face, _)) = hit(spin, Vec3 { x: 0.0, y: 0.0, z: 0.0 }, d) {
        return shade(FACE_COLORS[face as usize], spin.to_camera(normal(face)).dot(light));
    }
    if d.y >= 0.0 { return color_hex(BACKGROUND); }
    let t = FLOOR_Y / d.y;
    let p = Vec3 { x: d.x * t, y: FLOOR_Y, z: d.z * t };
    if hit(spin, p, light).is_some() { return shade(FLOOR_COLOR, 0.0); }
    shade(FLOOR_COLOR, light.y)
}

fn main() {
    let (lx, ly, lz) = (-0.5f32, 1.0f32, -0.6f32);
    let ll = (lx * lx + ly * ly + lz * lz).sqrt();
    let light = Vec3 { x: lx / ll, y: ly / ll, z: lz / ll };
    let (w, h) = (SIZE, SIZE);
    let (cx, cy) = (w as f32 * 0.5, h as f32 * 0.5);
    let scale = cx * 1.1;
    let (columns, rows) = ((w + CELL - 1) / CELL, (h + CELL - 1) / CELL);
    let cells = columns as usize * rows as usize;
    let mut vertices = vec![Vertex::default(); cells * 4];

    let mut checksum: u64 = 0;
    let started = Instant::now();
    for frame in 0..FRAMES {
        let seconds = frame as f32 / 60.0;
        let spin = Spin { sa: (seconds * 0.9).sin(), ca: (seconds * 0.9).cos(), sb: (seconds * 0.6).sin(), cb: (seconds * 0.6).cos() };
        let mut nv = 0;
        for row in 0..rows {
            let y0 = (row * CELL) as f32;
            let y1 = ((row * CELL + CELL) as f32).min(h as f32);
            let dy = (cy - (y0 + y1) * 0.5) / scale;
            for column in 0..columns {
                let x0 = (column * CELL) as f32;
                let x1 = ((column * CELL + CELL) as f32).min(w as f32);
                let dx = ((x0 + x1) * 0.5 - cx) / scale;
                let color = trace(spin, light, Vec3 { x: dx, y: dy, z: 1.0 });
                let uv = Point { x: 0.0, y: 0.0 };
                vertices[nv] = Vertex { position: Point { x: x0, y: y0 }, color, uv };
                vertices[nv + 1] = Vertex { position: Point { x: x1, y: y0 }, color, uv };
                vertices[nv + 2] = Vertex { position: Point { x: x1, y: y1 }, color, uv };
                vertices[nv + 3] = Vertex { position: Point { x: x0, y: y1 }, color, uv };
                nv += 4;
            }
        }
        let c = vertices[(frame as usize * 4099) % nv].color;
        checksum += (c.r * 255.0) as u64 + (c.g * 255.0) as u64 * 256 + (c.b * 255.0) as u64 * 65536;
    }
    let elapsed = started.elapsed().as_nanos();
    println!("rays: {}\nelapsed ns: {elapsed}\nchecksum: {checksum}", cells * FRAMES as usize);
}
