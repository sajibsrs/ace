// Rust twin of mandelbrot.ace and mandelbrot.c.
use std::time::Instant;

const SIZE: usize = 4000;
const ITERATIONS: i32 = 50;
const MODULUS: u64 = 1000000007;

fn main() {
    let mut row = vec![0u8; SIZE / 8];
    let mut checksum: u64 = 0;

    let started = Instant::now();
    for y in 0..SIZE {
        let ci = 2.0 * y as f64 / SIZE as f64 - 1.0;
        for x in (0..SIZE).step_by(8) {
            let mut bits: u8 = 0;
            for dx in 0..8 {
                let cr = 2.0 * (x + dx) as f64 / SIZE as f64 - 1.5;
                let (mut zr, mut zi, mut tr, mut ti) = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
                let mut i = 0;
                while i < ITERATIONS && tr + ti <= 4.0 {
                    zi = 2.0 * zr * zi + ci;
                    zr = tr - ti + cr;
                    tr = zr * zr;
                    ti = zi * zi;
                    i += 1;
                }
                bits <<= 1;
                if tr + ti <= 4.0 {
                    bits |= 1;
                }
            }
            row[x / 8] = bits;
        }
        for &b in &row {
            checksum = (checksum * 31 + b as u64) % MODULUS;
        }
    }
    let elapsed = started.elapsed().as_nanos();

    println!("size: {SIZE}\nelapsed ns: {elapsed}\nchecksum: {checksum}");
}
