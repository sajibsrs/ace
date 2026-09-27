# TODO — future completion list

Candidate catalogue of missing features and capabilities, core language first. This file is a survey, not a ledger: **no item here is a decision**. Anything promoted to work must be carved through the Decision Queue in `SPEC.md` (rule + rationale recorded there), implemented chunk by chunk, and logged in `CHANGELOG.md`. Ordering notes are recommendations to be resolved by the owner under the priority stack (`PHILOSOPHY.md`): performance > simplicity > modularity > compile time > safety.

Status tags:

- **[defect]** — implemented behavior is incorrect and requires diagnosis or repair
- **[queued]** — already listed for later work in `SPEC.md`
- **[open]** — already open in the Decision Queue or normative spec
- **[additive]** — explicitly deferred as an additive extension by a carved decision
- **[new]** — absent from every existing deferred list; observed here first
- **[unspecified]** — neither decided nor rejected in `SPEC.md`; support must be verified, then carved either way

Territory rejected by design is excluded entirely: manual memory through one fixed strategy (no GC, ownership, or allocator values), no exceptions or propagation, no inference, no optional type operator, no COW, no iterator contracts, no comptime or reflection, no block-scoped defer, no `++`/`--`, no hidden-capture closures (explicit contract values are the designated mechanism). Items appear only where the existing design leaves the question open.

The priority bands below are recommendations, not decisions. The active
Decision Queue in `SPEC.md` always wins; dependencies within a band are listed
before the work they unblock.

## 1. Core type-system completion

Finish the existing primitive, sequential, and aggregate model before adding
new execution or library layers. These are narrow completion tasks, not a
reopening of Ace's fixed explicit-type, copy, mutation, or allocation rules.

Wider fixed-array/view bridges remain additive conveniences; they are not
blockers for the current sequential core.

### Findings from the `pkg/regex` stress test

Found by writing `pkg/regex` (parser, compiler, Pike VM, benchmark); explore
next, in this order.

- **Failure position alongside `Error`** [unspecified] — `Error` carries only
  its domain, code, name, and message (Errors #9), so a parser that reports
  where it failed returns a separate value: `regex.Syntax::parse` returns
  `(Syntax, Error, usize)`, while `pkg/json` reports no position at all.
  Whether this becomes a standard-library convention (return shape, value on
  success, which layers report it) is open.
- **Typed element copy** [new] — the `mem` byte operations take only `[]u8`
  (Memory #15), so the Pike VM copies each `[]usize` capture row (2–10 words
  per thread step) with a hand loop where C uses `memcpy`. Measure a typed
  copy against the loop at those sizes before proposing one.
- **`pkg/regex` search prefilter** [new] — the Pike VM seeds a thread at
  every byte, while PCRE2 skips to candidate positions: `literal` runs at
  26.8 ns/B against PCRE2's 1.61 (`bench/README.md`), which bounds the gain.
  Candidate: skip ahead with `str`'s word-at-a-time search to the pattern's
  required first byte or first-byte class. Measure first.

### Other compile-time machine facilities

- **Binary data embedding (`embed "file"`)** [new] — static read-only storage
  serving later game and asset milestones.

### Pointer-level machine operations

- **Typed views over byte storage** [additive] — consider an explicit general
  operation that aliases an existing byte slice as a read-only or writable
  pointer to `T`, with size, alignment, permission, representation, and lifetime
  rules stated directly. This is a future convenience for native binary layouts,
  memory-mapped data, devices, and foreign buffers, not a prerequisite for such
  work: bounded byte-wise parsing remains the baseline and is sufficient for the
  BPF packet-sniffer proof of concept.
- **Over-16-byte alignment for raw `mem.alloc`** [additive].

### C boundary completion

- **`bool` (`_Bool`) mapping** [additive].
- **C enums at the boundary** [additive].
- **C global variables in extern blocks** [unspecified] — data imports such as
  `stdout` and errno-style globals are unaddressed.

### Low-level systems breadth

The Darwin BPF proof establishes allocation-free user-space kernel interaction,
native ABI calls, descriptor control, aligned record walking, and bounded
binary parsing. It does not by itself establish every low-level domain. Keep
the remaining proof points explicit:

- **Memory-mapped files and devices** [new] — carve mapping, protection,
  synchronization, unmapping, and device-memory rules from real target uses.
- **Broader event and descriptor control** [additive] — extend existing polling
  and raw Darwin `ioctl` only for proven terminal, device, or kernel customers,
  without implying unsupported portability.
- **Freestanding and privileged targets** [new] — kernels, drivers, bootloaders,
  and no-libSystem programs require separately carved runtime and target
  contracts; successful Darwin user-space BPF access does not establish them.
- **Low-level target breadth** [new] — validate assembly and SIMD ergonomics,
  device-scale volatile access, additional architectures and operating systems,
  and richer static/shared linking through concrete programs. Callback families
  beyond the implemented direct C-function-value boundary require their own
  concrete customers.

## 2. Compiler and backend completion

- **Canonical source formatter** [open] — deferred while language implementation
  remains the priority. Resume from the Decision Queue's `ace fmt` carving: one
  shared lexer scan and parser pass, a mode-gated lossless token/trivia stream
  and separate lossless syntax tree, compact compiler tokens/AST, normative
  `.acefmt` discovery, transactional rewrite/check/stdout modes, preserved
  comments and literal spellings, deterministic idempotent layout, and no
  semantic analysis or `std/fmt` coupling. The future VS Code formatter and LSP
  should consume this toolchain formatter rather than implement Ace parsing.
- **x86-64 backend and OS layer** [open] — the second target; facade selection
  already uses `@when`.
- **Shared-library (dylib/PIC) and static-archive emission** [new] — the driver
  currently emits objects and executables only.
- **Per-module objects and incremental rebuilds** [queued] — the merged-unit
  loader deliberately defers both.
- **Definite initialization inside the checker pass** [new] — the check is a
  separate whole-file walk after sema (`check_definite_initialization`).
  Skipping it entirely saves 1.0–1.9% of instructions on real programs (the
  regex and json package checks, `bench/core` and `bench/std` compiles) and
  3.3–3.8% on generated corpora. Folding it into the in-order check would save
  only the traversal part of that, while tracking initialization state across
  pending calls and the end-of-file re-check. Not worth it at that ceiling.
- **Local lookup in module lowering is quadratic** [new] — in an imported
  file, `lower_refs` checks each unshadowed name against the list of locals in
  scope (`local_name` in `module.c`), so one function of 8,000 locals takes
  1.44G instructions against 0.30G as a root file. Indexing locals past 32 live
  bindings, as sema's scopes do, removes that (-79%) but costs the regex and
  json package checks 0.2–0.3%; revisit if generated code reaches that size.
- **Richer DWARF** [additive] — variable/type DIEs and unwind tables.

### Deferred optimization work

Optimization follows the feature TODO rather than interrupting it. The first
manual baseline is deliberately narrow: across five 5,000,000-iteration bounded
integer runs on the reference Apple-arm64 development machine, median times were
48.44 ms for Ace debug, 45.25 ms for Ace release, 43.15 ms for C `-O0`, and
31.16 ms for C `-O2`; every variant produced the same checksum. This suggests
that current direct release emission is already near unoptimized C for that
workload, checked arithmetic has modest cost there, and optimization—not
language machinery—is the visible gap to optimized C. The allocator benchmark
separately confirms that checked registry bookkeeping is intentionally much
more expensive than the stripped release path, especially around `release`.
These are baseline observations, not performance promises.

- **Read-only parameter load reuse** [queued] — exploit the existing parameter
  facts without adding global alias analysis. Keeping a string parameter's
  length in a register measured no parse gain even with registers free.
- **Caller-saved promotion across calls** [new] — an integer local whose live
  interval crosses a call still takes x19-x28, or its slot below
  `PROMO_MIN_WEIGHT`; x9-x14 would need a save and reload around each call it
  spans.
- **Float promotion ignores live ranges** [new] — integer locals share
  registers by live interval, but float locals still hold d8-d15 or d24-d31
  for the whole body (`promo_take` ranks them by use-count weight alone), so
  two with disjoint live ranges can never share one. In a function that already
  saturates its float registers this inverts the usual advice: naming a
  repeated subexpression in `pow` is bit-identical and saves five
  instructions, yet costs 5% throughput and 15% dependent-chain latency,
  because the new name evicts a hotter value for the whole body even when it
  is live for four instructions at the end. The same accounting makes
  recomputing a cheap register-only expression, and nesting a single-use value
  rather than naming it, the idioms that win in `std/math`. `pow` saturates
  its sixteen float registers but loses only two candidates, both at the
  minimum weight. Candidate: extend the integer intervals to the float
  classes.
- **Stores through a by-reference pointer copy it first** [new] — a promoted
  pointer is copied into the address temp (`mov x0, x14` then
  `str x1, [x0]`), and the peephole folds only a load that consumes it.
  Candidate: address a scalar store through the pointer register directly.
  Removing copies alone saved no time before, so measure first.
- **Large read-only parameters reload their address** [new] — a read-only
  parameter over 16 bytes (not an HFA) also arrives by address but keeps it in
  a slot. Candidate: make it a by-reference promotion candidate like `mut`
  parameters and receivers.
- **Counted range loops** [new] — a range whose direction or step is not
  known at compile time (a signed `0 .. n`, a descending or explicit step)
  stores its bounds and step to frame slots, branches on direction, and
  divides for its trip count: about 27 setup instructions. Candidates, each
  measured on its own: compute the bounds and step in registers with no
  division for a step of one; then let a value binder the body never assigns
  share the cursor register (one copy per iteration).
- **An unused loop binder is stored every iteration** [new] — `for i: usize
  in 0 .. n` whose body never reads `i` still gives `i` a slot and stores the
  cursor to it each iteration. Skipping the store measured slower on the
  `calls` bench (`struct-16` 2.62 to 2.77 ns, `struct-hfa` 4.42 to 4.69 ns) at
  every placement, because the loop around the call moves the struct out of
  and back into the argument registers (see `bench/README.md`). Revisit it
  once that loop keeps the struct in the argument registers.
- **Wrapper calls cost a float spill** [new] — since promoted floats reach
  `d24-d31`, which are caller-saved, every branch-with-link stores and reloads
  them. A one-line dispatcher between two callers and two callees is therefore
  not free: deleting `math`'s `reduce` was worth 28% of `sin`. Candidate: spill
  only the promoted registers a call actually crosses, which needs the same
  per-call liveness; until then, prefer folding a thin wrapper into its callers
  in hot code.
- **Call and tuple-return plumbing** [new] — incoming aggregate parameters
  other than the read-only, leaf-read HFA and 8-byte-integer-leaf struct
  parameters of a call-free function (a parameter in a function with calls, a
  packed or mixed-class struct, a tuple) are spilled to slots and reloaded per
  use, and an aggregate
  argument that is not a plain frame-resident local or a small struct local
  in registers (a global, an rvalue, a member behind a pointer) or that
  overflows the argument registers still travels through an address temp. A returned literal or tuple with packed
  sub-8-byte fields, a `(f64, bool)` tuple, or a call in a leaf is still built
  in a frame temp and reloaded.
  A returned struct whose size is not a multiple of 8, a tuple with a
  non-8-byte member, a call assigned to a non-local destination (a pointer
  target, an index, a global), and `q, r = f()` into existing variables still
  stage the result in a temp and copy it. `bool` locals and a string
  parameter's pointer and length stay in stack slots inside loops.
  `bench/core/calls.ace` measures it: the by-value 8- and 16-byte structs, the
  three-`f64` aggregate, and the tuple return run 1.0x to 2.35x noinline C
  `-O2`. `vec_add` is now three `fadd`s on its argument registers, so the
  `struct-hfa` gap sits in the caller, which moves `acc` and `step` between
  d8-d15 and d0-d5 around every call where C leaves them in the argument
  registers. Float `fmov` between registers costs about 1 ns on a
  loop-carried chain, so a promotion scheme that adds moves there loses most
  of its gain. Integer struct leaves are still copied to x14 down; reading
  them in place measured 0.04 ns on `struct-16`, and dropping a leaf's frame
  measured nothing on `call-1` or `struct-hfa` (`bench/README.md`).
- **Remaining immediate and destination forms** [new] — declarations with an
  initializer still compute through a temp, and a constant on the left of a
  compare or in the middle of a chain still takes a register. Float compares
  still copy promoted operands into temps; comparing the promoted registers in
  place measured 1.63 to 1.32 ns per iteration (19%) on a loop with two
  compares. `x = x * 0.5 + 1.0` runs 2.7 ns against C's 1.7 because C fuses it
  into one `fmadd` (1.6 ns), not because of its constants. Ace does not contract
  a multiply and add, since that changes rounding; `math.fma` is the explicit
  spelling.
- **Hot-loop codegen gap** [new] — game and graphics kernels over global arrays
  run 1.1x (dot product) to 8x (by-value `Vec3` calls) scalar C `-O2` with
  vectorization off, and 2x to 30x C's NEON auto-vectorized output, which Ace
  leaves to explicit `simd`. What remains, measured on a `saxpy` loop (1.25 ns
  then, 0.97 ns now, C 0.48): an array base is rebuilt with `adrp`/`add` at
  every access, and hoisting it and the loop's float constants into registers
  measured 0.97 to 0.48 ns; a field of an indexed struct
  element costs `add`, `add`, then a load with no offset (`ps[i].x` about
  twelve times per particle); indexed stores still add the scaled index
  separately; and a leaf call taking
  by-value `Vec3`s copies every argument register and sets up a frame it does
  not use. A bottom-tested loop measured 7.7% on `saxpy` and 31% on a short
  branchy loop, but 8% slower on `for cond {}`; it is deferred until it can be
  weighed on top of the address work. `bench/core/run-hotloop.sh` tracks the saxpy,
  dot, and `Vec3` kernels. Latest ratios to scalar C `-O2`: saxpy 1.62x, particle
  `Vec3` 1.51x, `Vec3` length 0.89x, floating dot 0.79x. `std/simd` is
  specified and unimplemented. Candidate order: (1) scalar address work, meaning
  the scaled index and field offset folded into load and store addressing, measured
  against a saved `bench/core/run-hotloop.sh` baseline over several runs; loop-invariant
  hoisting into registers is the alternative but needs analysis the
  single-pass design avoids; (2) the bottom-tested loop and by-value `Vec3`
  leaf-call frame cost; (3) `std/simd` NEON intrinsics (`f32x4`, `i8x16`), the
  only route past auto-vectorized C.
- **Inline-expansion leftovers** [new] — an expansion the IR lowers has no
  result temp and no argument copies, but a `bool` result returned from
  several branches is joined as a phi of constants and branched on again
  instead of branching to the caller's targets, and a scalar local passed to a
  `mut` parameter binds by address, which keeps it in its frame slot for the
  whole caller: `str`'s integer-parse loop runs 20-25% slower when its
  accumulator is updated through a `mut` parameter than when the loop updates
  it after a check-only helper. Private inline functions keep an out-of-line
  body even when no call or function value uses it. Each needs its gain
  measured.
- **Loop-invariant hoisting** [new] — hoisting pure operations with invariant
  operands to a loop preheader gained nothing over value numbering and made the
  regex benchmark 5% to 10% slower: the backend fuses compares into branches,
  pointer offsets into addressing, and `mul` with `add`, and has no live-range
  splitting for a value held across a loop. Candidates: `div`, `rem`, `sqrt`,
  and float chains only; loads in a loop with no store, call, or volatile
  access (needs a design); and the array-base and float-constant hoisting the
  address-work entry above measured, which is a register-allocation decision.
  Each needs a benchmark that shows a gain first.
- **Float constants from bit patterns** [new] — `bits.cast(u64(0x7FF0000000000000))`
  still builds the integer with `mov`/`movk` and moves it with `fmov`, and a
  const float initialized by an expression rather than a literal loads from
  its `__const` slot with `adrp`/`add`/`ldr`. Float literals and
  literal-initialized const floats load from the function's constant pool.
  Candidate: route both through the pool; measure on `bench/std/run-math.sh`.
- **Signing-check scaling** [new] — the check that walks all module values once
  per signing is O(signings x declarations); not yet measured on a corpus with
  many signings. Candidate: index values by signing; measure with
  `bench/compiler/run-compile.sh`.
- **Machine-code size** [new] — 5,000 generated functions emit 1.60 MB release
  and 2.07 MB debug against 1.10 MB for `cc -O2`.
- **Float-parse optimization backlog** [new] — the canonical 80%-short /
  20%-long `parse-f64` workload is 1.9x `strtod` (52 ns); the goal is to close
  the gap to C and surpass it where possible. Measured next steps, in order:
  1. Reduce syntax-scanning cost (47% of short-input time before argument
     placement; re-profile first). The `nan`/`inf` check gate measured no gain.
  2. Consider dropping the exact-small tier (10%) for the Eisel-Lemire step,
     with differential tests of exact integers above 2^53.
  3. Consider a wide-multiply intrinsic (`umulh`) for the Eisel-Lemire step
     (9%).
  4. Consider power-table alignment and cache layout last. C is the
     comparison baseline, not a ceiling; any stronger claim requires a local
     checksum-matched measurement.
- **Broader performance corpus** [new] — add checksum-matched Ace/C fixtures for
  slices, branches, floating-point kernels, and cache-heavy loops before
  making optimizer-wide claims; `bench/core/calls.ace` covers calls and aggregate
  passing, and `bench/compiler/run-compile.sh` covers compile time and code size. `noalias`-driven
  auto-vectorization remains outside v1 as stated by Compiler #6.

## 3. Concurrency and process expansion

Begin this only after the core type, representation, pointer, and C-boundary
work above. The completed single-threaded chat proof removes networking as a
prerequisite, and process control already has a usable allocation-free baseline.
Shared-memory concurrency must still begin with its ABI and memory-model
foundations.

- **Memory-model statement** [new] — specify data races, synchronization, and
  what `noalias` and read-only parameter facts mean across threads before
  exposing shared-memory execution.
- **Atomics** [new] — load/store/exchange/compare-and-swap plus fences with
  explicit ordering; lower directly to the target's supported instructions.
- **Threads** [new] — explicit create/join/exit over libSystem pthreads, using
  the narrowly carved C-callable function values from section 1 and the stated
  memory model and atomics; create/join remain ordinary calls with no hidden
  control flow.
- **Thread-local storage** [new] — required for per-thread application state;
  distinct from the future TLS secure-transport module.
- **Mutex/condition-variable module** [new] — thin explicit synchronization
  over the thread and memory model, with no hidden allocation or ownership.
- **Signals and signal-safe execution** [new] — after the concurrency
  foundations, define process- and thread-directed delivery, handler ABI,
  masks, interruption, synchronous waiting, and the deliberately restricted
  operations valid in handlers. Graceful server shutdown is a required proof,
  not a reason to bypass these prerequisites or fold signals into `poll` early.
- **Process lifecycle completion** [new] — `fork`, exact-child `wait`, pipes,
  and immediate `exit` are implemented. Add `exec`/spawn composition, explicit
  argument/environment and descriptor wiring, and nonblocking waits only when
  a concrete subprocess fixture establishes their required shapes; keep this
  separate from shared-memory thread semantics.

## 4. Practical standard-library capabilities

- **Hash-map iteration and wider key types** [additive] — iteration (an
  explicit cursor, with no mutation while iterating) and enum, distinct, and
  composite keys, once specializations may live outside the generic's module
  (Generics #14). String keys hash a byte at a time; measure a word-at-a-time
  hash before adding one.
- **`map` throughput** [new] — with `probe`, `hash`, `mix`, and `same`
  `inline`, 16,384 cache-resident `int` keys take 40.2, 10.5, and 22.3 ns per
  `set`, `get`, and `unset` in release builds, against 30.7, 7.0, and 16.3 ns
  for the same algorithm, hash, and load factor in C `-O2`; the gap is not yet
  diagnosed. `unset` moves by about 30% with its code's alignment, so compare
  its layout before crediting a change. String keys and `--debug` are
  unmeasured. `bench/std/run-collections.sh` tracks the gap.
- **`Array` element access** [new] — `get` runs 2.9x a scalar C array and
  `push` 2.8x the same growth policy in C, while `set` is within 1.1x
  (`bench/std/run-collections.sh`); the cause is not yet diagnosed.
- **UTF-8 validation throughput** [new] — `utf8.valid` over 78 mixed bytes
  runs 6.3x a strict C validator with the same rules
  (`bench/std/run-string.sh`). It calls `decode`, which returns
  `(u32, usize, bool)` per sequence. Candidate: validate without decoding, or a
  scalar-result decode step; measure both.
- **Large-argument `sin`/`cos`** [new] — past the Cody-Waite cutoff, `sin`
  takes 366 ns against libm's 7.8 (47x, `bench/std/run-math.sh`). Profile the
  Payne-Hanek path before choosing a candidate.
- **`f32` transcendental functions** [new] — the `f32` `exp`, `log`, `sin`,
  and `cos` convert to `f64`, call the `f64` function, and round back, running
  3.3-5.3x libm's `expf`, `logf`, `sinf`, and `cosf` against 2.1-3.1x for the
  `f64` forms (`bench/std/run-math.sh`). Candidate: `f32` kernels with
  `f32`-sized polynomials that keep the 1-ULP bound (Stdlib #8).
- **Fixed-width integer formatting** [additive] — width, zero padding, and base
  prefixes for `fmt.printf`, especially fixed-width `%b`/`%o`/`%x` register
  displays; carve consistently across integer conversions from a hardware
  fixture rather than adding a binary-only modifier.
- **Random source** [new].

## 5. Packages

- **Remote package source** [open] — Package System #9 in `SPEC.md`: one
  central registry or resolver maps a name to its source and installs into
  `pkg/`. Compare against Go's model, where the source URL is itself the
  package name (`github.com/owner/json`), uniform without a central server.
- **`pkg/json` formatter throughput** [new] — with its eleven small
  `Formatter` helpers `inline`, a 158-byte document written with 28 calls
  takes 821 ns in release builds, against 366 ns for the same state machine
  in C `-O2` and 748 ns for hand-inlined Ace source. The hand-inlined source
  also splits `reserve` into an inline capacity check and an out-of-line
  `grow`, writes errors directly where `fail`'s null check is already known,
  and branches on conditions instead of `ok` flags. Candidate: the `reserve`
  split, measured against the inline-expansion leftovers first.

## 6. Language questions and conveniences

Each item requires an owner decision and should wait for a motivating use case.

### Future primitive extensions

- **`f16`** [new] — IEEE binary16 for graphics, shaders, inference workloads,
  and targets with native GPU texture or render-target support.
- **`bf16`** [new] — a 16-bit float retaining the broad exponent range of
  `f32`, useful for ML training-style workloads and straightforward conversion
  to and from `f32`.

- **Deferred cleanup-result handling** [unspecified] — `defer` currently
  executes its call at function exit and discards every returned value,
  including `Error`; no exception or propagation occurs. Revisit only when a
  concrete cleanup path must affect the enclosing function's result without an
  explicit pre-return cleanup call.
- **`switch` on strings and range cases** [unspecified].
- **Operator overloading / custom indexing** [unspecified] — likely conflicts
  with explicit dispatch; free functions may suffice.
- **Runtime global initializers / module-init hook** [unspecified] — globals are
  constant-fold-only today; either admission or permanent rejection should be
  stated explicitly.

## 7. Later networking and protocol layers

Resume after the higher-priority core work. Each layer keeps allocation,
buffering, capacities, retries, timeouts, and ownership explicit.

The first pure-Ace blocking IPv4 TCP composition (`net/tcp`) is implemented:
explicit listener/connection handles, setup cleanup, blocking accept, exact
receive, all-byte send, EINTR retry, and explicit close. The items below remain
additive later work; the nonblocking proofs continue to use `net` and `poll`.
The Stage 2 connection-progress layer is also implemented: caller-owned buffers,
explicit tagged events, poll-interest transitions, partial-send offsets, and
nonblocking connect completion remain visible without callbacks or allocation.

- **Additive socket capabilities** [additive] — IPv6, UDP, address queries,
  half-close, and extra TCP options, carved
  in the order demanded by concrete proofs. UDP's placement (nested under
  `net` directly, or as its own `net/udp`-style composition module) remains
  undecided.
- **TLS client transport needs a fresh carve** [open] — see the Package
  System future-work section (#14) in `SPEC.md`. `http` lives in `pkg/http`,
  independent of this item.
- **HTTP protocol and transport ergonomics** [partly implemented] — the strict IPv4 endpoint
  parser, zero-copy HTTP URL parser, caller-storage request and response
  formatters, and incremental HTTP/1.1 request and response parsers from
  Standard Library #17 are implemented, including field and response defaults,
  named method/status constants, conventional response reasons, and blocking
  plaintext `get` overloads over caller storage and opt-in bounded redirects
  for the owning client, plus the single blocking server convenience,
  `serve(endpoint, mut context, router): Error` over a growable `Context` and
  `Router` (see `SPEC.md`); the caller-driven nonblocking `ServerContext`/
  `Event` core has been removed (see the fresh-carve item below). Next complete
  the paired one-shot `post` overload over `Storage`. `http` is plaintext-only
  until the TLS client transport carve above lands.
- **Nonblocking HTTP server core needs a fresh carve** [open] — the
  caller-driven `ServerContext`/`Event`/`serve(mut ctx, timeout_ns)` core
  (formerly `std/http/inc/server.ace`, now `pkg/http/inc/server.ace`) and its
  `demo/http-complete-server` proof of concept had been implemented directly,
  without going through the Decision Queue carve process first. The owner
  does not want to keep that design and has removed it (code, tests, and
  docs); `pkg/http/inc/server.ace`
  now holds only the blocking `serve(endpoint, mut context, router)` overload
  over a growable `Context` (unrelated to the removed `ServerContext`/`Event`
  design beyond the name), and `demo/http-simple-server/main.ace` is the sole
  server demo. A fresh carve
  (philosophy, then semantics, then syntax, then implementation) deciding how
  much server mechanism belongs in the language versus a framework built on
  top must happen before anything like the nonblocking core returns.

## 8. Completed debug-build checkpoint

The explicit `--debug`/`--release` modes and strict sequence bounds are
implemented: emitted programs default to debug mode, while release strips
their dynamic guards, failure paths, and diagnostic metadata. This completed
checkpoint remains here as a compact reference rather than occupying the active
priority list.

- **[implemented]** Build-mode plumbing and array, slice, and string bounds diagnostics.
- **[implemented]** Checked integer arithmetic diagnostics (Safety & Debug Builds #6).
- **[implemented]** Call-boundary `noalias`/length diagnostics (Safety & Debug Builds #7; Memory #16).
- **[implemented]** Checked null-use diagnostics (Safety & Debug Builds #8; Zero #2).
- **[implemented]** Allocator diagnostics (Safety & Debug Builds #9).
- **[implemented]** Language-level `assert(bool)`, debug-only and release-stripped, for invariants and simple debug-mode tests (Safety & Debug Builds #10).
