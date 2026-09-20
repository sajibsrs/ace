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

### Wide byte-write checkpoint

After `mem.copy`, `move`, `fill`, and `zero` moved from byte loops to shared
NEON routines with a 32-byte-aligned forward loop, five runs on the reference
Apple-arm64 machine produced these median copy results:

- 64-byte copy: Ace 7.64 ns, C 3.36 ns (2.28x)
- 4096-byte copy: Ace 81.59 ns, C 74.08 ns (1.10x)

The remaining 64-byte gap is mostly the benchmark loop itself: its `i % 64`
index still lowers to `udiv`, and loop state stays in stack slots.

After `mem.compare` and `mem.equal` moved to one shared NEON scan, with helper
loop heads aligned to 16 bytes, twelve runs of one release binary produced
median compare results of 8.1 ns for 64 bytes (C 3.84 ns, 2.1x) and 107 ns for
4096 bytes (C 159 ns, 0.67x). Before loop alignment, the same copy loop
straddled a cache line and measured 135 ns instead of 82 ns; placement matters
as much as instruction choice at this scale.
The same holds for ordinary emitted code: `find-4096` measured 16.6 us with
`str.index_from` 12 bytes into a 64-byte line and 19.5 us at 44 bytes, with
byte-identical instructions. Compare string results across builds only when
that placement is controlled.

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
measurement-ordered backlog; the canonical 6.8x result above remains the
baseline until another carve lands.

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

### Constant-divisor checkpoint

After constant divisors lowered to reciprocal multiplies, five runs produced
medians of 43.60 ms checked, 42.45 ms release, 43.40 ms C `-O0`, and 34.22 ms
C `-O2`, all with checksum `1019507998`. Release now edges past unoptimized C;
the remaining gap to `-O2` is loop state that still round-trips through stack
slots every iteration.

### Code-placement checkpoint

After function entries and source loop heads were aligned to 16 bytes, three
runs each of both benchmarks matched the previous checkpoints within noise;
32- and 64-byte loop alignment measured the same. `str.index_from`'s inner
loop body is 152 bytes across three cache lines, so head alignment cannot
recover its earlier 16.6 us best placement until loop bodies shrink.

### Repeat-initializer checkpoint

After `[value : count]` moved to whole-array fill and doubling copies, the
string benchmark's `__text` fell from 358 KB to 106 KB and `main` from about
66,000 to 3,400 instructions. Its repeats run outside the timed loops, so the
timed workloads are unchanged.

### Direct-slot checkpoint

After scalar locals moved to direct `ldur`/`stur` slot access and call spill
slots became per-temp, three runs each produced CPU medians of 39.53 ms
release versus 33.91 ms C `-O2` (1.17x, from 1.29x), with the loop body at 37
instructions. String medians: `find-4096` 9.2x, `parse-f64` 6.1x,
`format-f64-shortest` 0.60x; `compare`/`copy` are unchanged within noise.
`state` still round-trips through its slot every iteration.

### Loop-register checkpoint

After compiler loop state moved to callee-saved registers, three runs each
produced string medians of `find-4096` 5.77x C (from 9.15x), `compare-4096`
0.70x (from 0.85x), `copy-64` about 1.9x (from 2.2x), and
`format-f64-shortest` 0.57x. The CPU recurrence regressed from 36.1 to
41.7 ms release in an alternating same-machine A/B, although its loop fell
from 37 to 30 instructions. A hand-written assembly harness isolates the
cause: once `state`'s store and next-iteration load are the loop's only memory
traffic, store forwarding adds about 3.7 cycles per iteration; one unrelated
`ldur`/`stur` pair per iteration, or keeping `state` in a register, restores
23.0 cycles. Loop alignment of 16, 32, or 64 bytes made no difference.

### Register-local checkpoint

After non-escaping integer-class locals moved to callee-saved registers,
three runs produced CPU medians of 36.0 ms release versus 34.2 ms C `-O2`
(1.05x), recovering the loop-register regression: `state` no longer
round-trips through memory. String medians: `find-4096` 5.5x, `copy-64`
1.9x, `parse-f64` 5.2x, `format-f64-shortest` 0.49x. `compare` alternates
between two placements (64-byte 1.8x or 3.1x, 4096-byte 0.67x or 0.86x);
an alternating A/B against the previous compiler measured identical times.

### Frame-access checkpoint

After loads landed in their destination and local aggregate fields,
descriptors, and literal members moved to direct `x29` access, five
alternating A/B runs produced string medians of `find-4096` 9,877 ns (from
10,551), `format-f64-shortest` 373 ns (from 391), and `parse-f64` 144 ns
(from 149); CPU was unchanged at 37.0 ms. `compare-64` moved to its slower
placement (7.2 ns from 6.7) with an instruction-identical loop.

### Register-operand checkpoint

After promoted integer locals became direct operands, five alternating A/B
runs produced CPU medians of 36.0 ms release (from 37.0) and string medians
of `format-f64-shortest` 366 ns (from 374) and `parse-f64` 141 ns (from 144).
`find-4096` stayed at 9,855 ns although its inner loop fell from 16 to 13
instructions: the removed register moves were not on its load-latency path.

### Word-at-a-time search checkpoint

After `str` search moved to an eight-byte first-and-last-byte filter, five
alternating runs produced `find-4096` medians of 1,454 ns for Ace versus
1,993 ns for C `strstr` (0.73x, from 5.0x); other workloads were unchanged.

### Straight-line promotion checkpoint

After loop-free locals used at least twice moved into callee-saved registers,
five alternating runs produced `parse-f64` 101 ns (from 141; 3.6x C
`strtod`, from 5.0x), `parse-f64-short` 92 ns (from 139), and
`format-f64-shortest` 313 ns (from 367). CPU, search, and copy were
unchanged. A sampled profile before the change put 38% of short-input parse
time in syntax scanning, 26% in the Eisel-Lemire step (a quarter of its
instructions were slot loads and stores), 12% each in the small-integer tier
and the wrapper, and 5% in the software leading-zero count.

### Exact-power parse checkpoint

After the float parser gained a Clinger tier and hardware bit counts, five
alternating runs produced `parse-f64` 87 ns (from 100; 3.2x C `strtod`, from
3.6x), `parse-f64-short` 77 ns (from 92), and `parse-f64-long` 112 ns (from
118); format was unchanged. The exact-power tier takes four of the eight
short inputs. A sampled profile of short inputs now puts 51% of the time in
syntax scanning, 15% in the Eisel-Lemire step, 14% in the wrapper, 9% in the
small-integer tier, and 4% in the exact-power tier.

### Leaf-register checkpoint

After call-free functions gained x9-x14 for promoted locals, five alternating
runs produced `parse-f64` 61 ns (from 87; 2.2x C `strtod`, from 3.2x),
`parse-f64-short` 57 ns (from 76), `parse-f64-long` 79 ns (from 112), and
`format-f64-shortest` 303 ns (from 314). CPU, search, compare, and copy were
unchanged, and the string benchmark's text fell 1.3%. A sampled profile of
short inputs now puts 47% of parse time in syntax scanning, 20% in the
wrapper, 10% in the small-integer tier, 9% in the Eisel-Lemire step, and 4%
in the exact-power tier.

### Immediate-operand checkpoint

After small constants became add/sub/compare immediates and promoted integer
locals took assignment results in their own registers, five alternating runs
produced `find-4096` 1,300 ns (from 1,456; 0.65x C `strstr`),
`parse-f64-long` 75 ns (from 79), `parse-f64` 59 ns (from 60), and
`format-f64-shortest` 297 ns (from 303). CPU, compare, and copy were
unchanged. Parse retired 7% fewer instructions in the earlier prototype with
no cycle change, so its remaining cost is not instruction count.

### Full-comparison checkpoint

On the same compiler, three string runs and one CPU run produced these
medians against C `-O2`: `compare-64` 11.5 ns (3.0x; 8.9–11.9 across runs,
placement-dependent), `compare-4096` 0.85x, `copy-64` 1.8x, `copy-4096`
1.08x, `find-4096` 0.65x, `parse-f64` 58.7 ns (2.1x), `parse-f64-short`
54.9 ns (2.1x), `parse-f64-long` 74.0 ns (2.4x), and `format-f64-shortest`
296 ns (0.37x of `%.17g`, reference only). CPU measured 37.7 ms release
versus 35.5 ms C `-O2` (1.06x) and 41.4 ms C `-O0`, with 35.5 ms checked;
all checksums matched.

### Argument-placement checkpoint

After call arguments already in their argument registers skipped the spill
and reload, and scalar results moved by register instead of a slot, five
alternating runs produced `parse-f64` 52.4 ns (from 58.8; 1.9x C `strtod`,
from 2.1x), `parse-f64-short` 48.3 ns (from 54.0), `parse-f64-long` 71.0 ns
(from 74.3), and `format-f64-shortest` 262 ns (from 296). CPU, search, compare,
and copy were unchanged, and the string benchmark's text fell 5.3%. Gating the
`nan`/`inf` checks behind the first byte alone measured no change. Constant
unguarded indexes and direct local-struct field stores landed in the same
build and saved instructions without measurable time on their own.

### Post-`time`/`mem.alloc_size` comparison checkpoint

Five alternating runs of the string and CPU benchmarks, Ace release against C
`-O2`, medians: `compare-4096` 0.67x, `compare-64` 6.7 ns (1.75x),
`copy-4096` 1.08x, `copy-64` 1.73x, `find-4096` 0.65x, `parse-f64` 52.5 ns
(1.85x `strtod`), `parse-f64-short` 47.8 ns (1.81x), `parse-f64-long` 71.9 ns
(2.25x), and `format-f64-shortest` 261 ns (0.27x of `%.17g`, reference only).
CPU measured 36.0 ms release (1.07x C `-O2` at 33.6 ms), 37.1 ms checked, and
40.9 ms C `-O0`; all checksums matched. The remaining gaps are small-buffer
compare and copy and float parsing; the goal is to close them and surpass C
where possible.

## Calls

Run the call-overhead benchmark on Apple arm64:

```sh
./bench/run-calls.sh
```

The runner builds `bench/calls.ace` in release mode and `bench/calls.c` at
`-O2` twice, checks that every checksum matches, and prints ns per operation.
Each workload feeds its result into the next call: one-, four-, and
eight-argument scalar calls, recursive `fib(32)`, by-value struct arguments and
returns of 8, 16, 24, and 64 bytes, a three-`f64` homogeneous float aggregate, a
two-value tuple return, a `mut self` method, and a call through a function
value. The C functions have external linkage and `noinline`, so `-O2` keeps
every call and cannot specialise it on constant arguments. The third column is
the same C with inlining allowed: a reference Ace cannot match without an
inliner, not the comparison target. The C function-value slot is `volatile` so
the indirect call survives.

Medians of three runs, Ace release against noinline C `-O2` (ns/op, ratio):
`call-1` 5.80 vs 2.88 (2.01x), `call-4` 5.59 vs 3.40 (1.64x), `call-8` 4.92 vs
4.33 (1.14x), `fib-recursive` 2.04 vs 1.21 (1.69x), `struct-8` 10.87 vs 2.40
(4.53x), `struct-16` 8.97 vs 2.40 (3.74x), `struct-24` 7.61 vs 10.82 (0.70x),
`struct-64` 16.18 vs 14.24 (1.14x), `struct-hfa` 13.75 vs 1.44 (9.55x),
`tuple-return` 15.10 vs 7.19 (2.10x), `method` 1.92 vs 3.84 (0.50x), and
`function-value` 6.00 vs 2.88 (2.08x). The largest gaps are small by-value
aggregates and the float aggregate; the method and the 24-byte struct already
beat noinline C.

## Compile time

Run the compile-time benchmark on Apple arm64:

```sh
./bench/run-compile.sh
```

`bench/compile-corpus.sh <functions> <dir>` emits an equivalent Ace and C pair:
N call-chained functions cycling through a bounded loop with branches, by-value
struct passing, and early-return branches, plus a `main` that prints one
checksum. `bench/scaling-corpus.sh <long|types> <count> <dir>` scales other
dimensions: `long` is one function with N locals in a single scope, `types` is
N structs each with a function taking one by value. Both pairs were verified to
print the same checksum. The runner compiles each size to an object, timing
whole-process wall clock (median of three runs) for `ace -c` in checked and
release modes against `cc -O0 -c` and `cc -O2 -c`, prints a growth column (time
over the previous size's time; linear code growth is 10x for 100 to 1,000 and
4x for the later steps shown), then reports machine-code bytes and times a few
real programs.

The small-size comparison against `cc` flatters Ace: `cc` pays process startup
and header parsing that a 100-function corpus barely amortizes, so the growth
column, not the ratio at small sizes, is the real signal.

Function-count corpus, medians (Ace checked / `cc -O0` / `cc -O2`, ms): 100
functions (1,053 lines) 10.2 / 54.7 / 206.4; 1,000 (10,353 lines) 29.0 / 215.6 /
1,575; 5,000 (51,684 lines) 111 / 937 / 7,736; 20,000 (206,684 lines) 409 /
3,626 / 31,014. Growth is 3.8x for 5x the code and 3.7x for 4x, so this shape is
now linear. Before scope symbols were indexed the same corpus took 813 ms at
5,000 functions and 12.9 s at 20,000; a profile put about 78% in `find_in`,
`lookup_val`, and `declare_func`, and emit's symbol interning scanned every
symbol per call. Scopes past 32 names now use a hash index and emit interns
through a hash table.

Other shapes, Ace checked ms at 1,000 / 5,000 / 20,000: `types` 22 / 76 / 287
(3.5x and 3.8x growth, linear); `long` 20 / 161 / 2,347 (8.1x for 5x and 14.6x
for 4x, still superlinear; `cc -O0` is also superlinear there at 15x and 13x).
A sample of the 20,000-local case shows sema's definite-initialization tracking
(`init_var`) and emit's `promo_scan` and `load_var` as the remaining
hot spots. Machine code for 5,000 functions is 1.60 MB release and 2.07 MB
checked against 0.98 MB for `cc -O0` and 1.10 MB for `cc -O2`. Real programs
compile in 9 to 16 ms for the 19- to 194-line benchmarks (mostly process start
and standard-library loading) and 127 ms for the 2,150-line HTTP server demo
including its link step.

## Memory

Run the v1 raw-memory benchmark on Apple arm64:

```sh
./bench/run-memory.sh
```

The runner builds `bench/memory.ace` in checked and release modes with compiler
reports enabled, then runs each under macOS `/usr/bin/time -l`. The Ace program checks allocation,
reallocation payload preservation, and release nulling. It reports operation
counts, nanoseconds per operation, requested live bytes, allocator-usable live
bytes from `mem.alloc_size`, allocator overhead, a checksum, and failures.

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
