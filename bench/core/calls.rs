// Call-overhead benchmark paired with calls.ace and calls.c. As in C, every
// function is `#[inline(never)]` and exported (`#[no_mangle]`, the analogue
// of C's external linkage), so -C opt-level=2 keeps each call and cannot
// specialise it on constant arguments. They keep the Rust ABI.
use std::hint::black_box;
use std::time::Instant;

const MASK: u64 = 0x7FFFFFFF;
const ITERATIONS: u64 = 10000000;
const FIB_INPUT: u64 = 32;
const FIB_CALLS: u64 = 7049155;
const TAIL_DEPTH: u64 = 10000;
const TAIL_ROUNDS: u64 = 1000;

#[derive(Clone, Copy)]
pub struct Small { v: u64 }
#[derive(Clone, Copy)]
pub struct Pair { a: u64, b: u64 }
#[derive(Clone, Copy)]
pub struct Triple { a: u64, b: u64, c: u64 }
#[derive(Clone, Copy)]
pub struct Octet { a: u64, b: u64, c: u64, d: u64, e: u64, f: u64, g: u64, h: u64 }
#[derive(Clone, Copy)]
pub struct Vec3 { x: f64, y: f64, z: f64 }
pub struct Counter { value: u64 }

impl Counter {
    #[inline(never)]
    #[no_mangle]
    pub fn counter_add(&mut self, amount: u64) {
        self.value = (self.value + amount) & MASK;
    }
}

#[inline(never)]
#[no_mangle]
pub fn call1(a: u64) -> u64 { (a * 48271 + 1) & MASK }

#[inline(never)]
#[no_mangle]
pub fn call4(a: u64, b: u64, c: u64, d: u64) -> u64 { (a * 3 + b * 5 + c * 7 + d) & MASK }

#[inline(never)]
#[no_mangle]
pub fn call8(a: u64, b: u64, c: u64, d: u64, e: u64, f: u64, g: u64, h: u64) -> u64 {
    (a + b * 2 + c * 3 + d * 5 + e * 7 + f * 11 + g * 13 + h * 17) & MASK
}

#[inline(never)]
#[no_mangle]
pub fn fib(n: u64) -> u64 {
    if n < 2 { return n; }
    fib(n - 1) + fib(n - 2)
}

#[inline(never)]
#[no_mangle]
pub fn sum_tail(n: u64, acc: u64) -> u64 {
    if n == 0 { return acc; }
    sum_tail(n - 1, (acc + n) & MASK)
}

#[inline(never)]
#[no_mangle]
pub fn small_step(s: Small) -> Small { Small { v: (s.v * 48271 + 1) & MASK } }

#[inline(never)]
#[no_mangle]
pub fn pair_step(p: Pair) -> Pair { Pair { a: (p.a * 48271 + p.b) & MASK, b: (p.b + 1) & MASK } }

#[inline(never)]
#[no_mangle]
pub fn triple_step(t: Triple) -> Triple {
    Triple { a: (t.a * 48271 + t.b) & MASK, b: (t.b + t.c) & MASK, c: (t.c + 1) & MASK }
}

#[inline(never)]
#[no_mangle]
pub fn octet_step(o: Octet) -> Octet {
    Octet {
        a: (o.a * 48271 + o.h) & MASK, b: (o.b + o.a) & MASK, c: (o.c + o.b) & MASK,
        d: (o.d + o.c) & MASK, e: (o.e + o.d) & MASK, f: (o.f + o.e) & MASK,
        g: (o.g + o.f) & MASK, h: (o.h + 1) & MASK,
    }
}

#[inline(never)]
#[no_mangle]
pub fn vec_add(a: Vec3, b: Vec3) -> Vec3 { Vec3 { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z } }

#[inline(never)]
#[no_mangle]
pub fn dot3(a: Vec3, b: Vec3) -> f64 { a.x * b.x + a.y * b.y + a.z * b.z }

#[inline(never)]
#[no_mangle]
pub fn divmod(a: u64, b: u64) -> (u64, u64) { (a / b, a % b) }

fn report(name: &str, started: Instant, operations: u64, checksum: u64) {
    println!("{name} {} {operations} {checksum}", started.elapsed().as_nanos());
}

fn main() {
    let mut state = 1;
    let started = Instant::now();
    for i in 0..ITERATIONS { state = call1(state + i); }
    report("call-1", started, ITERATIONS, state);

    state = 1;
    let started = Instant::now();
    for i in 0..ITERATIONS { state = call4(state, i, state >> 3, 1); }
    report("call-4", started, ITERATIONS, state);

    state = 1;
    let started = Instant::now();
    for i in 0..ITERATIONS { state = call8(state, i, 1, 2, 3, 4, 5, 6); }
    report("call-8", started, ITERATIONS, state);

    let started = Instant::now();
    state = fib(FIB_INPUT);
    report("fib-recursive", started, FIB_CALLS, state);

    state = 1;
    let started = Instant::now();
    for _ in 0..TAIL_ROUNDS { state = sum_tail(TAIL_DEPTH, state); }
    report("tail-recursive", started, TAIL_ROUNDS * TAIL_DEPTH, state);

    let mut small = Small { v: 1 };
    let started = Instant::now();
    for _ in 0..ITERATIONS { small = small_step(small); }
    report("struct-8", started, ITERATIONS, small.v);

    let mut pair = Pair { a: 1, b: 1 };
    let started = Instant::now();
    for _ in 0..ITERATIONS { pair = pair_step(pair); }
    report("struct-16", started, ITERATIONS, (pair.a + pair.b) & MASK);

    let mut triple = Triple { a: 1, b: 1, c: 1 };
    let started = Instant::now();
    for _ in 0..ITERATIONS { triple = triple_step(triple); }
    report("struct-24", started, ITERATIONS, (triple.a + triple.b + triple.c) & MASK);

    let mut o = Octet { a: 1, b: 1, c: 1, d: 1, e: 1, f: 1, g: 1, h: 1 };
    let started = Instant::now();
    for _ in 0..ITERATIONS { o = octet_step(o); }
    report("struct-64", started, ITERATIONS,
           (o.a + o.b + o.c + o.d + o.e + o.f + o.g + o.h) & MASK);

    let mut acc = Vec3 { x: 0.0, y: 0.0, z: 0.0 };
    let step = Vec3 { x: 1.0, y: 2.0, z: 3.0 };
    let started = Instant::now();
    for _ in 0..ITERATIONS { acc = vec_add(acc, step); }
    report("struct-hfa", started, ITERATIONS, (acc.x + acc.y + acc.z) as u64);

    let mut total = 0.0;
    let mut probe = Vec3 { x: 0.0, y: 2.0, z: 3.0 };
    let weights = Vec3 { x: 4.0, y: 5.0, z: 6.0 };
    let started = Instant::now();
    for i in 0..ITERATIONS {
        probe.x = (i & 15) as f64;
        total += dot3(probe, weights);
    }
    report("float-acc", started, ITERATIONS, total as u64);

    state = 1;
    let started = Instant::now();
    for i in 0..ITERATIONS {
        let (first, second) = divmod(state + i + 1, 97);
        state = (first * 3 + second) & MASK;
    }
    report("tuple-return", started, ITERATIONS, state);

    let mut counter = Counter { value: 1 };
    let started = Instant::now();
    for i in 0..ITERATIONS { counter.counter_add(i); }
    report("method", started, ITERATIONS, counter.value);

    // black_box stands in for C's volatile slot: the target is opaque.
    let callback: fn(u64) -> u64 = black_box(call1);
    state = 1;
    let started = Instant::now();
    for i in 0..ITERATIONS { state = callback(state + i); }
    report("function-value", started, ITERATIONS, state);
}
