# Benchmarks

These are manual measurements, not pass/fail performance thresholds. Close
other expensive workloads and run several times before comparing results.

## CPU

Run the bounded integer recurrence in Ace checked and release modes and in
equivalent C built with `-O0` and `-O2`:

```sh
./bench/run-cpu.sh
```

The runner verifies that every executable produces the same checksum. The C
`-O0` result is a useful direct-code baseline; `-O2` shows the gap to an
optimizing compiler. One short workload is not a general language-performance
claim.

### Initial baseline

Five consecutive runs on the reference Apple-arm64 development machine produced
these median times for 5,000,000 iterations:

- Ace checked: 48.44 ms
- Ace release: 45.25 ms
- C `-O0`: 43.15 ms
- C `-O2`: 31.16 ms

All twenty executions produced checksum `1019507998`. On this workload, release
was about 5% behind unoptimized C, checked guards added about 7% over release,
and optimized C was about 31% faster than release. The result points first to
local code-generation optimization and broader workload coverage; it does not
establish performance for other programs or machines.

## Memory

Run the v1 raw-memory benchmark on Apple arm64:

```sh
./bench/run-memory.sh
```

The runner builds `bench/memory.ace` in checked and release modes with compiler
reports enabled, then runs each under macOS `/usr/bin/time -l`. The Ace program checks allocation,
reallocation payload preservation, and release nulling. It reports operation
counts, nanoseconds per operation, requested live bytes, allocator-usable live
bytes from `malloc_size`, allocator overhead, a checksum, and failures.

The external timing report adds process-level statistics such as peak resident
set size. Checked results include bounds, arithmetic, and allocator diagnostics;
release results measure the stripped runtime path. Timing and RSS vary by
machine and system load, so this is a manual v1 acceptance benchmark rather
than a performance threshold in `tests/run.sh`. The only pass/fail condition is
memory-operation correctness; the program exits nonzero on failure. The runner
validates that separately before invoking `/usr/bin/time -l`, because restricted
macOS environments may deny the process-statistics query even when the program
runs successfully.

Three consecutive reference runs produced stable per-operation medians: checked
versus release was 42/20 ns for allocation, 157/60 ns for reallocation, and
145/17 ns for release. The larger checked cost comes from the live-allocation
registry and is diagnostic-mode overhead, not part of release output.
