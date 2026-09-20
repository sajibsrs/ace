# Benchmarks

These are manual measurements, not pass/fail performance thresholds. Close
other expensive workloads and run several times before comparing results.

## First-run cost

A process pays page-in and first-touch costs that the earliest timed workloads
in it absorb. Over six fresh builds of `bench/string.ace`, four runs each, a
binary's first run cost 64% more than its warm runs on `compare-64`, 19% on
`compare-4096`, and 5% on `copy-64`; every later workload was within noise. The
effect tracks position in the program, not the code: the same binary is fast on
its second run, and an alternating A/B of two compilers measures the same warm
times for both. Every runner therefore discards one run of each binary before
the timed run.

Checkpoints below this change may report a cold `compare-64`, `compare-4096`,
or `copy-64`, so those three rows are not comparable across checkpoints; the
rest are. This is what earlier entries describe as `compare` alternating
between two placements. True placement effects are separate and are measured
at controlled offsets on byte-identical instructions.

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

### Current baseline

Five consecutive `./bench/run-string.sh` runs on the reference Apple-arm64
machine at `bf3fc42` (warm binaries, Ace release against C `-O2`). Each cell is
the median; the range is the smallest to largest Ace/C over the five runs. Every
parse checksum matched.

| workload | Ace ns | C ns | Ace/C | range | C baseline |
|---|---|---|---|---|---|
| `compare-64` | 6.73 | 3.84 | 1.75x | 1.71–1.75x | `memcmp` |
| `compare-4096` | 105.57 | 158.82 | 0.66x | 0.66–0.67x | `memcmp` |
| `copy-64` | 6.08 | 3.38 | 1.80x | 1.65–1.83x | `memcpy` |
| `copy-4096` | 79.93 | 74.07 | 1.08x | 1.03–1.11x | `memcpy` |
| `find-4096` | 1,186.88 | 2,002.36 | 0.59x | 0.59x | `strstr` ¹ |
| `parse-f64-short` | 46.05 | 26.70 | 1.72x | 1.72–1.74x | `strtod` |
| `parse-f64-long` | 68.18 | 31.50 | 2.16x | 2.11–2.33x | `strtod` |
| `parse-f64` | 51.77 | 27.92 | 1.85x | 1.80–1.91x | `strtod` |
| `format-f64-shortest` | 224.83 | 801.82 | 0.28x | 0.28x | `snprintf("%.17g")` ² |

¹ `strstr` scans a NUL-terminated string and has no length, so the ratio shows
Ace's word-at-a-time search against a different contract, not parity with a
length-aware search.
² A different contract (see above): a reference cost, not a semantic comparison.

The historical tables below are checkpoints for tracing when a number moved.
Cells marked `≈` were not recorded then: an Ace time is the recorded ratio times
the current C median (`compare-64` 3.84, `compare-4096` 159, `copy-64` 3.4,
`copy-4096` 74, `find-4096` 2,002, `parse-f64` 27.9, `parse-f64-short` 26.7,
`parse-f64-long` 31.5, `format-f64-shortest` 802 ns), and a ratio is the
recorded Ace time over that C median. C's times held steady across every
checkpoint that recorded them, so these derived cells carry the rounding of the
recorded figure, not a new measurement. `compare-64`, `compare-4096`, and
`copy-64` rows from before `e35a350` may be cold-run values (First-run cost).

### Initial string baseline

Three consecutive runs on the reference Apple-arm64 development machine with
Apple Clang 21 produced these median release-mode results:

| workload | Ace ns | C ns | Ace/C |
|---|---|---|---|
| 64-byte compare | 54.17 | 5.82 | 9.31x |
| 4096-byte compare | 3,975.50 | 196.74 | 20.21x |
| 64-byte copy | 35.71 | 3.98 | 8.97x |
| 4096-byte copy | 2,002.20 | 74.95 | 26.71x |
| late 4096-byte substring search | 16,498.44 | 2,005.88 | 8.22x |
| `f64` parse | 540,161.91 | 27.19 | about 19,866x |
| shortest `f64` format | 202,440.32 | 743.81 | about 272x ¹ |

¹ C is `%.17g`: a reference workload only, because the contracts differ.

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

| workload | Ace ns | C ns | Ace/C |
|---|---|---|---|
| 64-byte copy | 7.64 | 3.36 | 2.28x |
| 4096-byte copy | 81.59 | 74.08 | 1.10x |

The remaining 64-byte gap is mostly the benchmark loop itself: its `i % 64`
index still lowers to `udiv`, and loop state stays in stack slots.

After `mem.compare` and `mem.equal` moved to one shared NEON scan, with helper
loop heads aligned to 16 bytes, twelve runs of one release binary produced
these median compare results:

| workload | Ace ns | C ns | Ace/C |
|---|---|---|---|
| 64-byte compare | 8.1 | 3.84 | 2.1x |
| 4096-byte compare | 107 | 159 | 0.67x |

Placement matters as much as instruction choice at this scale, on library and
ordinary emitted code alike. Each pair below is byte-identical instructions at
a different offset:

| loop | placement | time |
|---|---|---|
| 4096-byte copy | straddling a cache line | 135 ns |
| 4096-byte copy | not straddling | 82 ns |
| `find-4096` (`str.index_from`) | 12 bytes into a 64-byte line | 16.6 us |
| `find-4096` (`str.index_from`) | 44 bytes into a 64-byte line | 19.5 us |

Compare string results across builds only when that placement is controlled
and the binaries are warm.

### Native float-parser optimization checkpoint

After adding exact-small and Eisel-Lemire fixed-width tiers, then specializing
and fusing the generated common path, five runs on the reference Apple-arm64
machine produced these median parse results:

| workload | Ace ns/op | C `strtod` ns/op | Ace/C |
|---|---|---|---|
| `f64` parse | 191.04 | 26.58 | about 7.2x |

Generated-code component probes attributed these costs:

| component | ns/op |
|---|---|
| syntax scanning | about 71 |
| complete fixed-width conversion | about 59 |
| standalone software 64x64-to-128 multiply | about 22 |
| exact-small selection | about 19 |
| public wrapper | about 15 |
| direct cached-power lookup | about 4 |

These components overlap when composed and therefore do not sum to the
end-to-end time. Direct and volatile-indirect C dispatch differed by less than
0.1 ns/op at median. Parsing remains materially slower than optimized C, with
scanning, non-inlined calls, and spill-heavy direct emission the next measured
targets.

The subsequent SWAR scan carve measured:

| measurement | before | after | change |
|---|---|---|---|
| long digit-run scanning (isolated probe) | 228.7 ns/op | 126.9 ns/op | -44.5% |
| mixed public parse workload, Ace | 191.04 ns/op | 186.72 ns/op | -4.32 (2.3%) |

On that mixed workload, whose short inputs often remain scalar, C measured
26.75 ns/op, or about 7.0x.

The canonical `parse-f64` workload assumes 80% common/short inputs and 20%
long-mantissa or scientific inputs. This is an explicit working estimate rather
than a measured production distribution and may be revised when representative
usage data exists. Its fixed 40-input schedule reuses the two eight-input pools
without adding timed selection branches. Five release runs on the reference
Apple-arm64 machine, with the supporting diagnostics that remain separate:

| workload | Ace ns/op | C `strtod` ns/op | Ace/C |
|---|---|---|---|
| `parse-f64` (canonical) | 192.15 | 28.32 | 6.8x |
| `parse-f64-short` | 186.06 | 26.45 | 7.0x |
| `parse-f64-long` | 213.60 | 31.67 | 6.7x |

`parse-f64-short` retains the original mixed pool; `parse-f64-long` uses 15-19
significant digits in long integer, fractional, leading-zero, and scientific
forms, and avoids inputs that invoke the greater-than-19-digit bigint
conversion fallback. A temporary scalar-only A/B on those same long inputs:

| build | ns/op |
|---|---|
| scalar only | 250.11 |
| with SWAR | 212.46 |

That is a 37.65 ns/op (15.1%) saving when every call reached the eight-byte
path. Checked
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

| build | time |
|---|---|
| Ace checked | 48.44 ms |
| Ace release | 45.25 ms |
| C `-O0` | 43.15 ms |
| C `-O2` | 31.16 ms |

All twenty executions produced checksum `1019507998`. On this workload, release
was about 5% behind unoptimized C, checked guards added about 7% over release,
and optimized C was about 31% faster than release. The result points first to
local code-generation optimization and broader workload coverage; it does not
establish performance for other programs or machines.

### Constant-divisor checkpoint

After constant divisors lowered to reciprocal multiplies, five runs produced
these medians, all with checksum `1019507998`:

| build | time |
|---|---|
| Ace checked | 43.60 ms |
| Ace release | 42.45 ms |
| C `-O0` | 43.40 ms |
| C `-O2` | 34.22 ms |

Release now edges past unoptimized C;
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
string benchmark measured:

| measurement | before | after |
|---|---|---|
| `__text` | 358 KB | 106 KB |
| `main` | about 66,000 instructions | about 3,400 |

Its repeats run outside the timed loops, so the timed workloads are
unchanged.

### Direct-slot checkpoint

After scalar locals moved to direct `ldur`/`stur` slot access and call spill
slots became per-temp, three runs each produced these medians:

| workload | Ace | C `-O2` | Ace/C | was |
|---|---|---|---|---|
| CPU recurrence | 39.53 ms | 33.91 ms | 1.17x | 1.29x |
| `find-4096` | ≈18,420 ns | ≈2,002 ns | 9.2x | not recorded |
| `parse-f64` | ≈170 ns | ≈27.9 ns | 6.1x | not recorded |
| `format-f64-shortest` | ≈481 ns | ≈802 ns | 0.60x | not recorded |

The CPU loop body is at 37 instructions; `compare` and `copy` are unchanged
within noise. `state` still round-trips through its slot every iteration.

### Loop-register checkpoint

After compiler loop state moved to callee-saved registers, three runs each
produced these string medians against C:

| workload | Ace | Ace/C | was |
|---|---|---|---|
| `find-4096` | ≈11,550 ns | 5.77x | 9.15x |
| `compare-4096` | ≈111 ns | 0.70x | 0.85x ¹ |
| `copy-64` | ≈6.5 ns | about 1.9x | 2.2x |
| `format-f64-shortest` | ≈457 ns | 0.57x | not recorded |

¹ Probably a cold run: the Register-local checkpoint below measures 0.67x warm
and 0.86x cold for this row.

The CPU recurrence regressed in an alternating same-machine A/B, although its
loop fell from 37 to 30 instructions:

| measurement | before | after |
|---|---|---|
| CPU recurrence, release | 36.1 ms | 41.7 ms |
| loop body | 37 instructions | 30 |

A hand-written assembly harness isolates the cause: once `state`'s store and
next-iteration load are the loop's only memory traffic, store forwarding costs
per iteration:

| loop memory traffic | cycles per iteration |
|---|---|
| `state` store and reload only | +3.7 |
| plus one unrelated `ldur`/`stur` pair | 23.0 |
| `state` kept in a register | 23.0 |

Loop alignment of 16, 32, or 64 bytes made no difference.

### Register-local checkpoint

After non-escaping integer-class locals moved to callee-saved registers,
three runs produced these medians:

| workload | Ace | C `-O2` | Ace/C |
|---|---|---|---|
| CPU recurrence | 36.0 ms | 34.2 ms | 1.05x |
| `find-4096` | ≈11,010 ns | ≈2,002 ns | 5.5x |
| `copy-64` | ≈6.5 ns | ≈3.4 ns | 1.9x |
| `parse-f64` | ≈145 ns | ≈27.9 ns | 5.2x |
| `format-f64-shortest` | ≈393 ns | ≈802 ns | 0.49x |

That recovers the loop-register regression: `state` no longer round-trips
through memory. `compare` alternates between two levels, and an alternating
A/B against the previous compiler measured identical times:

| workload | warm | cold |
|---|---|---|
| `compare-64` | 1.8x | 3.1x |
| `compare-4096` | 0.67x | 0.86x |

The higher level of each pair is the first-run cost above, not a placement.

### Frame-access checkpoint

After loads landed in their destination and local aggregate fields,
descriptors, and literal members moved to direct `x29` access, five
alternating A/B runs produced these string medians:

| workload | Ace | was |
|---|---|---|
| `find-4096` | 9,877 ns | 10,551 ns |
| `format-f64-shortest` | 373 ns | 391 ns |
| `parse-f64` | 144 ns | 149 ns |
| `compare-64` | 7.2 ns | 6.7 ns |

CPU was unchanged at 37.0 ms. `compare-64`'s loop is instruction-identical
across the pair, and both numbers sit within its cold-to-warm range.

### Register-operand checkpoint

After promoted integer locals became direct operands, five alternating A/B
runs produced these medians:

| workload | Ace | was |
|---|---|---|
| CPU recurrence | 36.0 ms | 37.0 ms |
| `format-f64-shortest` | 366 ns | 374 ns |
| `parse-f64` | 141 ns | 144 ns |
| `find-4096` | 9,855 ns | unchanged |

`find-4096` did not move although its inner loop fell from 16 to 13
instructions: the removed register moves were not on its load-latency path.

### Word-at-a-time search checkpoint

After `str` search moved to an eight-byte first-and-last-byte filter, five
alternating runs produced these medians; other workloads were unchanged:

| workload | Ace | C `strstr` | Ace/C | was |
|---|---|---|---|---|
| `find-4096` | 1,454 ns | 1,993 ns | 0.73x | 5.0x |

### Straight-line promotion checkpoint

After loop-free locals used at least twice moved into callee-saved registers,
five alternating runs produced these medians; CPU, search, and copy were
unchanged:

| workload | Ace | was | Ace/C | was |
|---|---|---|---|---|
| `parse-f64` | 101 ns | 141 ns | 3.6x | 5.0x |
| `parse-f64-short` | 92 ns | 139 ns | ≈3.4x | ≈5.2x |
| `format-f64-shortest` | 313 ns | 367 ns | ≈0.39x | ≈0.46x |

A sampled profile of short-input parse time before the change:

| component | share |
|---|---|
| syntax scanning | 38% |
| Eisel-Lemire step ¹ | 26% |
| small-integer tier | 12% |
| public wrapper | 12% |
| software leading-zero count | 5% |

¹ a quarter of its instructions were slot loads and stores.

### Exact-power parse checkpoint

After the float parser gained a Clinger tier and hardware bit counts, five
alternating runs produced these medians; format was unchanged:

| workload | Ace | was | Ace/C | was |
|---|---|---|---|---|
| `parse-f64` | 87 ns | 100 ns | 3.2x | 3.6x |
| `parse-f64-short` | 77 ns | 92 ns | ≈2.9x | ≈3.4x |
| `parse-f64-long` | 112 ns | 118 ns | ≈3.6x | ≈3.7x |

The exact-power tier takes four of the eight short inputs. A sampled profile
of short inputs now reads:

| component | share |
|---|---|
| syntax scanning | 51% |
| Eisel-Lemire step | 15% |
| public wrapper | 14% |
| small-integer tier | 9% |
| exact-power tier | 4% |

### Leaf-register checkpoint

After call-free functions gained x9-x14 for promoted locals, five alternating
runs produced these medians; CPU, search, compare, and copy were unchanged,
and the string benchmark's text fell 1.3%:

| workload | Ace | was | Ace/C | was |
|---|---|---|---|---|
| `parse-f64` | 61 ns | 87 ns | 2.2x | 3.2x |
| `parse-f64-short` | 57 ns | 76 ns | ≈2.1x | ≈2.8x |
| `parse-f64-long` | 79 ns | 112 ns | ≈2.5x | ≈3.6x |
| `format-f64-shortest` | 303 ns | 314 ns | ≈0.38x | ≈0.39x |

A sampled profile of short inputs now reads:

| component | share |
|---|---|
| syntax scanning | 47% |
| public wrapper | 20% |
| small-integer tier | 10% |
| Eisel-Lemire step | 9% |
| exact-power tier | 4% |

### Immediate-operand checkpoint

After small constants became add/sub/compare immediates and promoted integer
locals took assignment results in their own registers, five alternating runs
produced these medians; CPU, compare, and copy were unchanged:

| workload | Ace | was | Ace/C |
|---|---|---|---|
| `find-4096` | 1,300 ns | 1,456 ns | 0.65x `strstr` |
| `parse-f64-long` | 75 ns | 79 ns | ≈2.4x |
| `parse-f64` | 59 ns | 60 ns | ≈2.1x |
| `format-f64-shortest` | 297 ns | 303 ns | ≈0.37x |

Parse retired 7% fewer instructions in the earlier prototype with no cycle
change, so its remaining cost is not instruction count.

### Full-comparison checkpoint

On the same compiler, three string runs and one CPU run produced these medians
against C `-O2`:

| workload | Ace | Ace/C |
|---|---|---|
| `compare-64` | 11.5 ns | 3.0x ¹ |
| `compare-4096` | ≈135 ns | 0.85x ¹ |
| `copy-64` | ≈6.1 ns | 1.8x |
| `copy-4096` | ≈80 ns | 1.08x |
| `find-4096` | ≈1,300 ns | 0.65x |
| `parse-f64` | 58.7 ns | 2.1x |
| `parse-f64-short` | 54.9 ns | 2.1x |
| `parse-f64-long` | 74.0 ns | 2.4x |
| `format-f64-shortest` | 296 ns | 0.37x ² |

¹ cold; `compare-64` spanned 8.9–11.9 ns across runs.
² of `%.17g`, reference only.

| CPU build | time | Ace/C |
|---|---|---|
| Ace release | 37.7 ms | 1.06x |
| Ace checked | 35.5 ms | 1.00x |
| C `-O2` | 35.5 ms | 1.00x |
| C `-O0` | 41.4 ms | 1.17x |

All checksums matched.

### Argument-placement checkpoint

After call arguments already in their argument registers skipped the spill
and reload, and scalar results moved by register instead of a slot, five
alternating runs produced these medians; CPU, search, compare, and copy were
unchanged, and the string benchmark's text fell 5.3%:

| workload | Ace | was | Ace/C | was |
|---|---|---|---|---|
| `parse-f64` | 52.4 ns | 58.8 ns | 1.9x | 2.1x |
| `parse-f64-short` | 48.3 ns | 54.0 ns | ≈1.8x | ≈2.0x |
| `parse-f64-long` | 71.0 ns | 74.3 ns | ≈2.3x | ≈2.4x |
| `format-f64-shortest` | 262 ns | 296 ns | ≈0.33x | ≈0.37x |

Gating the
`nan`/`inf` checks behind the first byte alone measured no change. Constant
unguarded indexes and direct local-struct field stores landed in the same
build and saved instructions without measurable time on their own.

### Post-`time`/`mem.alloc_size` comparison checkpoint

Five alternating runs of the string and CPU benchmarks, Ace release against C
`-O2`, medians:

| workload | Ace | Ace/C |
|---|---|---|
| `compare-64` | 6.7 ns | 1.75x |
| `compare-4096` | ≈107 ns | 0.67x |
| `copy-64` | ≈5.9 ns | 1.73x |
| `copy-4096` | ≈80 ns | 1.08x |
| `find-4096` | ≈1,301 ns | 0.65x |
| `parse-f64` | 52.5 ns | 1.85x `strtod` |
| `parse-f64-short` | 47.8 ns | 1.81x |
| `parse-f64-long` | 71.9 ns | 2.25x |
| `format-f64-shortest` | 261 ns | 0.27x ¹ |

¹ of `%.17g`, reference only.

| CPU build | time | Ace/C |
|---|---|---|
| Ace release | 36.0 ms | 1.07x |
| Ace checked | 37.1 ms | 1.10x |
| C `-O2` | 33.6 ms | 1.00x |
| C `-O0` | 40.9 ms | 1.22x |

All checksums matched. The remaining gaps are small-buffer
compare and copy (1.75x and 1.80x, mostly benchmark-loop overhead) and float
parsing (1.85x, 2.16x on long inputs); the goal is to close them and surpass C
where possible. The Current baseline table at the top of this section is the
comparison point for later changes.

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
float accumulator fed by a call whose arguments change every iteration, a
two-value tuple return, a `mut self` method, and a call through a function
value. The C functions have external linkage and `noinline`, so `-O2` keeps
every call and cannot specialise it on constant arguments. The third column is
the same C with inlining allowed: a reference Ace cannot match without an
inliner, not the comparison target. The C function-value slot is `volatile` so
the indirect call survives.

Ace release against noinline C `-O2` in ns/op, after constant, parameter, and
float-accumulator promotion and register returns:

| workload | Ace | C noinline | Ace/C | Ace before promotion |
|---|---|---|---|---|
| `call-1` | 2.90 | 2.88 | 1.00x | 5.72 |
| `call-4` | 3.65 | 3.41 | 1.07x | 5.61 |
| `call-8` | 4.16 | 4.34 | 0.96x | 4.93 |
| `fib-recursive` | 2.05 | 1.23 | 1.66x | |
| `struct-8` | 3.85 | 2.40 | 1.60x | |
| `struct-16` | 2.42 | 2.40 | 1.00x | |
| `struct-24` | 6.92 | 10.71 | 0.65x | |
| `struct-64` | 15.52 | 14.37 | 1.08x | |
| `struct-hfa` | 4.92 | 1.44 | 3.41x | |
| `float-acc` | 2.40 | 1.44 | 1.67x | |
| `tuple-return` | 11.53 | 7.21 | 1.60x | |
| `method` | 2.08 | 3.85 | 0.54x | |
| `function-value` | 3.61 | 2.88 | 1.25x | 6.02 |

The largest remaining gaps are the float aggregate, the tuple return, and the
8-byte struct; the method and the 24-byte struct beat noinline C.

`call-1` and `fib` measure different things. `fib` is throughput-bound: its
calls overlap in the out-of-order core, so 2 ns per call is a throughput
figure. `call-1` feeds each result into the next call, so it measures the
length of a loop-carried dependency chain. A separate 100M-iteration run:

| configuration | ns/op |
|---|---|
| bare arithmetic chain, inline | 2.89 |
| `call1` in the chain | 5.78 |
| `call1` with independent arguments | 1.44 |
| `call1` in the chain, after both fixes | 2.94 |

The extra 2.9 ns in the chain came from the constant `MASK` being loaded from
memory (about 1.0 ns) and the callee spilling its single argument and reloading
it, a store-to-load forward on the critical path (the rest). Both are gone.

The aggregate and tuple workloads are bound by the same effect at more points:
the caller spills argument addresses to slots, and incoming HFA parameters are
spilled and reloaded per use. A callee that returns a struct literal or tuple
of call-free scalar leaves computes each leaf straight into its result
register, and a caller storing such a result into a frame-resident local or
typed binders writes the returned registers there directly. Measured over five
alternating runs against the previous compiler, with spreads under 0.1 ns
within each build:

| workload | before | after callee change | after caller change |
|---|---|---|---|
| `struct-8` | 9.3 | 7.6 | 6.1 |
| `struct-16` | 9.75 | 7.5 | 5.5 |
| `struct-hfa` | 13.7 | 12.3 | 9.8 |
| `tuple-return` | 16.4 | 12.95 | 11.6 |

Loading frame-resident aggregate arguments straight from their slots removed
the argument address spill and reload and shrank the object's text by 0.8%, but
measured neutral:

| workload | before | after |
|---|---|---|
| `struct-16` | 5.50 | 5.38 |
| `struct-hfa` | 9.80 | 9.72 |
| `struct-8` | 6.09 | 6.47 |
| `method` ¹ | 1.82 | 1.93 |

¹ its code did not change, so the differences are layout effects, not a gain.

These rows also moved by up to about 2 ns across unrelated compiler changes:

| workload | observed across builds | note |
|---|---|---|
| `struct-16` | 8.97, 7.15, 9.78 | three builds |
| `tuple-return` | 15.1 to 16.4 | loop strictly shorter, `divmod` byte-identical |

So single-digit-nanosecond changes on these rows are not evidence until the
memory round trips are removed. `divmod` also computes its quotient twice
because Ace has no common-subexpression elimination; that is left as is.

## Hot loops

Run float array kernels through Ace release output and equivalent C:

```sh
./bench/run-hotloop.sh
```

Four kernels over 4096-element global arrays: `saxpy-f64` and `saxpy-f32`
(`y[i] = a*x[i] + y[i]`), `dot-f64` (an accumulating reduction), and
`particle-vec3` (three `f32` fields of an indexed struct element, read and
written per iteration). The arrays are globals so every access pays the
base-address computation. The runner checks that Ace and both C builds produce
identical checksums, which are whole-array reductions rather than single
elements: some particles have zero velocity, so one element can be insensitive
to the kernel and check nothing.

Ace leaves vectorization to explicit `simd`, so scalar C `-O2`
(`-fno-vectorize -fno-slp-vectorize`) is the comparison target. The
auto-vectorized `-O2` column is a reference, not a goal. Neither compiler
vectorizes `dot-f64`'s reduction, because clang will not reassociate floating
point without `-ffast-math`.

### Current hot-loop baseline

Five runs on the reference Apple-arm64 machine at `bf3fc42`, medians in ns per
element. `Ace/C` is against scalar C, the comparison target; `Ace/vector` is
against auto-vectorized C, the ceiling explicit `simd` would chase:

| workload | Ace | C scalar | Ace/C | C vector | Ace/vector |
|---|---|---|---|---|---|
| `saxpy-f64` | 0.962 | 0.483 | 1.99x | 0.181 | 5.31x |
| `saxpy-f32` | 0.962 | 0.483 | 1.99x | 0.092 | 10.46x |
| `dot-f64` | 1.277 | 1.642 | 0.78x | 1.068 | 1.20x |
| `particle-vec3` | 2.735 | 0.731 | 3.74x | 0.272 | 10.05x |

The first baseline (`283dc4f`: 0.964, 0.965, 1.280, and 2.744 ns for the four
rows in order) is within 0.01 ns of these on every row, which is run-to-run
noise, not a change. Spreads were under 0.005 ns within each build. `dot-f64` is the one kernel where
neither compiler can vectorize and both are bound by the accumulator's
dependency chain; Ace leads it. The `saxpy` pair reproduces the ad-hoc figures
`TODO.md` records for this gap. `particle-vec3` is the widest: binding
`velocities[i]` to a local `Vec3` once, instead of indexing it three times,
measured 2.551 ns against 2.744, so about 7% of that row is repeated address
computation and the rest is elsewhere.

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

Function-count corpus, medians in ms:

| functions | lines | Ace checked | `cc -O0` | `cc -O2` |
|---|---|---|---|---|
| 100 | 1,053 | 10.2 | 54.7 | 206.4 |
| 1,000 | 10,353 | 29.0 | 215.6 | 1,575 |
| 5,000 | 51,684 | 111 | 937 | 7,736 |
| 20,000 | 206,684 | 409 | 3,626 | 31,014 |

Growth is 3.8x for 5x the code and 3.7x for 4x, so this shape is now linear.
Before scope symbols were indexed:

| functions | Ace checked | was |
|---|---|---|
| 5,000 | 111 ms | 813 ms |
| 20,000 | 409 ms | 12,900 ms |

A profile then put about 78% in `find_in`, `lookup_val`, and `declare_func`,
and emit's symbol interning scanned every symbol per call. Scopes past 32 names
now use a hash index and emit interns through a hash table.

Other shapes, Ace checked ms, all linear:

| shape | 1,000 | 5,000 | 20,000 | growth for 5x code | for 4x code |
|---|---|---|---|---|---|
| `types` | 22 | 76 | 287 | 3.5x | 3.8x |
| `long` | 11 | 26 | 92 | 2.4x | 3.5x |
| `long`, before | 20 | 161 | 2,347 | | |

`long` improved once sema's definite-initialization variables, emit's frame
slots and promotion candidates, and select lowering stopped scanning
per-function lists. `cc -O0` is superlinear on that shape at 15x and 13x.
Machine code for 5,000 functions:

| build | machine code |
|---|---|
| Ace release | 1.60 MB |
| Ace checked | 2.07 MB |
| `cc -O0` | 0.98 MB |
| `cc -O2` | 1.10 MB |

Real programs:

| program | lines | compile time |
|---|---|---|
| benchmarks | 19 to 194 | 9 to 16 ms ¹ |
| HTTP server demo | 2,150 | 127 ms ² |

¹ mostly process start and standard-library loading.
² including its link step.

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

Three consecutive reference runs produced stable per-operation medians:

| operation | checked | release |
|---|---|---|
| allocation | 42 ns | 20 ns |
| reallocation | 157 ns | 60 ns |
| release | 145 ns | 17 ns |

The larger checked cost comes from the live-allocation registry and is
diagnostic-mode overhead, not part of release output.
