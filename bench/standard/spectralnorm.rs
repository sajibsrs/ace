// Rust twin of spectralnorm.ace and spectralnorm.c.
use std::time::Instant;

const N: usize = 4000;

fn eval_a(i: usize, j: usize) -> f64 {
    1.0 / ((i + j) * (i + j + 1) / 2 + i + 1) as f64
}

fn times(out: &mut [f64], v: &[f64]) {
    for i in 0..out.len() {
        let mut sum = 0.0;
        for j in 0..v.len() {
            sum += eval_a(i, j) * v[j];
        }
        out[i] = sum;
    }
}

fn times_transpose(out: &mut [f64], v: &[f64]) {
    for i in 0..out.len() {
        let mut sum = 0.0;
        for j in 0..v.len() {
            sum += eval_a(j, i) * v[j];
        }
        out[i] = sum;
    }
}

fn times_ata(out: &mut [f64], v: &[f64], scratch: &mut [f64]) {
    times(scratch, v);
    times_transpose(out, scratch);
}

fn main() {
    let mut u = vec![1.0; N];
    let mut v = vec![0.0; N];
    let mut scratch = vec![0.0; N];

    let started = Instant::now();
    for _ in 0..10 {
        times_ata(&mut v, &u, &mut scratch);
        times_ata(&mut u, &v, &mut scratch);
    }
    let (mut vbv, mut vv) = (0.0, 0.0);
    for i in 0..N {
        vbv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    let norm = (vbv / vv).sqrt();
    let elapsed = started.elapsed().as_nanos();

    println!("n: {N}\nelapsed ns: {elapsed}\nchecksum: {norm:.9}");
}
