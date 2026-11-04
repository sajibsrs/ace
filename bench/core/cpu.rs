// CPU benchmark paired with cpu.ace and cpu.c: the same LCG, step for step.
use std::time::Instant; // CLOCK_UPTIME_RAW on darwin, as Ace's time.awake

fn main() {
    let iterations: u64 = 5000000;
    let mut state: u64 = 1;

    let started = Instant::now();
    for i in 0..iterations {
        state = (state * 48271 + (i % 97)) % 2147483647;
    }
    let elapsed = started.elapsed().as_nanos();

    println!("iterations: {iterations}\nelapsed ns: {elapsed}\nchecksum: {state}");
}
