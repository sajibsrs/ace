# Benchmarks

These are manual measurements, not pass/fail performance thresholds. Close
other expensive workloads and run several times before comparing results.

## Strings and byte operations

Run equivalent deterministic byte/string workloads through Ace release output
and optimized C/libSystem:

```sh
./bench/run-string.sh
```

The report includes nanoseconds per operation and the Ace/C ratio for 64- and
4096-byte comparison and copying, a late-match substring search, exact `f64`
parsing, and float formatting. Parsing uses identical inputs and requires equal
Ace/C bit checksums. C formatting uses `snprintf("%.17g")` only as a performance
reference: it does not implement Ace's exact shortest-format contract, so its
checksum and output lengths are not semantic comparisons. Timings are manual
measurements, not test-suite thresholds; run several times on an idle machine.

### Initial string baseline

Three consecutive runs on the reference Apple-arm64 development machine with
Apple Clang 21 produced these median release-mode results:

- 64-byte compare: Ace 54.17 ns, C 5.82 ns (9.31x)
- 4096-byte compare: Ace 3975.50 ns, C 196.74 ns (20.21x)
- 64-byte copy: Ace 35.71 ns, C 3.98 ns (8.97x)
- 4096-byte copy: Ace 2002.20 ns, C 74.95 ns (26.71x)
- late 4096-byte substring search: Ace 16498.44 ns, C 2005.88 ns (8.22x)
- `f64` parse: Ace 540161.91 ns, C 27.19 ns (about 19,866x)
- shortest `f64` format: Ace 202440.32 ns, C `%.17g` 743.81 ns
  (about 272x; reference workload only because the contracts differ)

The byte loops expose the expected gap between scalar direct emission and
libSystem's tuned implementations. Exact float parsing is the dominant issue:
the initial correctness-first converter performs a bigint-backed binary search
across the IEEE value space. These measurements justify a separate optimization
carve; they do not weaken the native exact semantics or reopen convenience
runtime dependencies.

### Native float-parser optimization checkpoint

After adding exact-small and Eisel-Lemire fixed-width tiers, then specializing
and fusing the generated common path, five runs on the reference Apple-arm64
machine produced these median parse results:

- Ace `f64` parse: 191.04 ns/op
- C `strtod`: 26.58 ns/op
- Ace/C: about 7.2x

Generated-code component probes attributed about 71 ns/op to syntax scanning,
19 ns/op to exact-small selection, 4 ns/op to direct cached-power lookup,
22 ns/op to the standalone software 64x64-to-128 multiply, 59 ns/op to the
complete fixed-width conversion, and 15 ns/op to the public wrapper. These
components overlap when composed and therefore do not sum to the end-to-end
time. Direct and volatile-indirect C dispatch differed by less than 0.1 ns/op
at median. Parsing remains materially slower than optimized C, with scanning,
non-inlined calls, and spill-heavy direct emission the next measured targets.

The subsequent SWAR scan carve reduced long digit-run scanning from about
228.7 ns/op to 126.9 ns/op in an isolated probe (44.5%). On the mixed public
parse workload, whose short inputs often remain scalar, five runs produced
median results of 186.72 ns/op for Ace and 26.75 ns/op for C, or about 7.0x.
That is a 4.32 ns/op (2.3%) reduction from the 191.04 ns/op checkpoint.

The canonical `parse-f64` workload assumes 80% common/short inputs and 20%
long-mantissa or scientific inputs. This is an explicit working estimate rather
than a measured production distribution and may be revised when representative
usage data exists. Its fixed 40-input schedule reuses the two eight-input pools
without adding timed selection branches. On the reference Apple-arm64 machine,
five release runs produced medians of 192.15 ns/op for Ace and 28.32 ns/op for C
`strtod`, making the canonical comparison 6.8x.

Supporting diagnostics remain separate: `parse-f64-short` retains the original
mixed pool and measured 186.06 ns/op for Ace versus 26.45 ns/op for C (7.0x),
while `parse-f64-long` uses 15–19 significant digits in long integer,
fractional, leading-zero, and scientific forms and measured 213.60 ns/op versus
31.67 ns/op (6.7x). The long cohort avoids inputs that invoke the greater-than-
19-digit bigint conversion fallback. A temporary scalar-only A/B on those same
long inputs measured 250.11 ns/op versus 212.46 ns/op with SWAR, a 37.65 ns/op
(15.1%) saving when every call reached the eight-byte path. Checked
instrumentation counted 27,500 SWAR chunks and 120,000 scalar steps over 20,000
long-cohort calls; all calls reached SWAR, and the chunks covered 64.7% of the
digit bytes counted by those paths.

Further parser optimization is intentionally deferred. `TODO.md` records the
measurement-ordered backlog and an aspirational next target of roughly 2x C;
the canonical 6.8x result above remains the baseline until another carve lands.

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
