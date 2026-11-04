// Rust twin of fannkuch.ace and fannkuch.c.
use std::time::Instant;

const N: i32 = 10;

fn main() {
    let mut perm = [0i32; N as usize];
    let mut perm1 = [0i32; N as usize];
    let mut count = [0i32; N as usize];
    for i in 0..N {
        perm1[i as usize] = i;
    }
    let (mut max_flips, mut checksum, mut permutations, mut r) = (0i32, 0i32, 0i32, N);

    let started = Instant::now();
    'done: loop {
        while r != 1 {
            count[(r - 1) as usize] = r;
            r -= 1;
        }

        for i in 0..N as usize {
            perm[i] = perm1[i];
        }
        let mut flips = 0;
        let mut k = perm[0];
        while k != 0 {
            let (mut i, mut j) = (0usize, k as usize);
            while i < j {
                perm.swap(i, j);
                i += 1;
                j -= 1;
            }
            flips += 1;
            k = perm[0];
        }
        if flips > max_flips {
            max_flips = flips;
        }
        if permutations % 2 == 0 {
            checksum += flips;
        } else {
            checksum -= flips;
        }

        loop {
            if r == N {
                break 'done;
            }
            let perm0 = perm1[0];
            for i in 0..r as usize {
                perm1[i] = perm1[i + 1];
            }
            perm1[r as usize] = perm0;
            count[r as usize] -= 1;
            if count[r as usize] > 0 {
                break;
            }
            r += 1;
        }
        permutations += 1;
    }
    let elapsed = started.elapsed().as_nanos();

    println!("max flips: {max_flips}\nelapsed ns: {elapsed}\nchecksum: {checksum}");
}
