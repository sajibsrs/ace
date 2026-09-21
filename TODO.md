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

- **Constant arithmetic in global initializers** [new] — a global initializer
  built from constant arithmetic (`G: f64 = 0.0 - 1.0`, `G: int = 2 * 3`) or an
  integer literal cast to float (`G: f32 = f32(-3)`) fails with "global
  initializer must be a compile-time constant"; the folder in `emit.c` accepts
  only literals, negated literals, and same-family casts. The same expressions
  work in local declarations.

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
- **Richer DWARF** [additive] — variable/type DIEs and unwind tables.

### Deferred optimization work

Optimization follows the feature TODO rather than interrupting it. The first
manual baseline is deliberately narrow: across five 5,000,000-iteration bounded
integer runs on the reference Apple-arm64 development machine, median times were
48.44 ms for Ace checked, 45.25 ms for Ace release, 43.15 ms for C `-O0`, and
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
- **Caller-saved promotion across calls** [new] — non-leaf functions still
  keep every promoted local in x19-x28; locals not live across any call could
  use x9-x14 given per-call liveness.
- **Live-range-aware register promotion** [new] — promotion assigns a register
  to a declaration for the whole function body (`promo_take` ranks candidates
  by use-count weight and nothing else), so two locals with disjoint live
  ranges can never share one. In a function that already saturates its
  registers this inverts the usual advice: naming a repeated subexpression in
  `pow` is bit-identical and saves five instructions, yet costs 5% throughput
  and 15% dependent-chain latency, because the new name evicts a hotter value
  for the whole body even when it is live for four instructions at the end.
  The same accounting makes recomputing a cheap register-only expression, and
  nesting a single-use value rather than naming it, the idioms that win in
  `std/math`. Where it pays is the code that is actually starved: `pow` itself
  saturates its sixteen float registers but loses only two candidates, both at
  the minimum weight, while `math`'s `reduce_large` loses fifteen integer
  locals and `float_shortest`'s `binary64` and `binary32` lose sixteen and
  eighteen. Candidate: live ranges, or at minimum letting disjoint locals share
  a register; it needs the same per-call liveness the caller-saved item above
  wants.
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
  argument that is not a plain frame-resident local (a global, an rvalue, a
  member behind a pointer) or that overflows the argument registers still
  travels through an address temp. A returned literal or tuple with packed
  sub-8-byte fields, a `(f64, bool)` tuple, or a call in a leaf is still built
  in a frame temp and reloaded.
  A returned struct whose size is not a multiple of 8, a tuple with a
  non-8-byte member, a call assigned to a non-local destination (a pointer
  target, an index, a global), and `q, r = f()` into existing variables still
  stage the result in a temp and copy it. `bool` locals and a string
  parameter's pointer and length stay in stack slots inside loops.
  `bench/calls.ace` measures it: the by-value 8- and 16-byte structs, the
  three-`f64` aggregate, and the tuple return run 1.0x to 3.4x noinline C
  `-O2`; what remains is bound by store-to-load forwards on the loop-carried
  slot, so a further step needs its gain measured first (for example reading
  HFA leaves in the incoming registers themselves, which measured 0.25 ns
  faster on `dot` and `length2` but 10% slower on a `Vec3`-returning call for
  a cause not yet found, or the caller side of `struct-8`, still 1.6 ns above
  C). Float `fmov` between registers costs about 1 ns on a loop-carried chain,
  so a promotion scheme that adds moves there loses most of its gain.
- **Unoptimized debug build mode** [new] — only the checked and release modes
  exist, and both emit the same optimized code; the modes differ in runtime
  guards alone. Local register promotion, immediates, and register returns are
  always on, promoted locals have no frame slot, and objects carry line tables
  but no variable locations, so a debugger cannot show a promoted local by
  name. Candidate: a third mode (or a modifier on checked) that disables
  promotion so every local keeps a slot, weighed against the cost of testing
  another mode.
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
  every access and float constants are rebuilt at every use, and hoisting both
  out of the loop measured 0.97 to 0.48 ns; a field of an indexed struct
  element costs `add`, `add`, then a load with no offset (`ps[i].x` about
  twelve times per particle); indexed stores still add the scaled index
  separately; a 12-byte element strides by `mov`/`mul`; and a leaf call taking
  by-value `Vec3`s copies every argument register and sets up a frame it does
  not use. A bottom-tested loop measured 7.7% on `saxpy` and 31% on a short
  branchy loop, but 8% slower on `for cond {}`; it is deferred until it can be
  weighed on top of the address work. `bench/run-hotloop.sh` tracks the saxpy,
  dot, and `Vec3` kernels. `std/simd` is specified and
  unimplemented.
- **Tail-call optimization** [new] — a `return f(args)` still emits `bl` plus a
  full epilogue, so tail recursion grows the stack linearly and overflows at
  roughly 100M frames in checked and release builds alike. Candidate: lower a
  self-recursive tail call to a jump past the prologue when no `defer` is
  pending, all arguments and the result ride registers, and the function
  creates no pointer or slice into its own frame; sibling tail calls to other
  functions need callee-saved restoration and frame teardown first. Whether the
  form becomes a normative guarantee or stays an unspecified optimization is
  undecided.
- **Float constant materialization** [new] — every float constant is rebuilt
  with `mov`/`movk` plus `fmov` at each use (19 sequences in a 226-instruction
  `exp`), where C loads it from a literal pool or hoists it. A pure-Ace `exp`
  runs 6x the same algorithm compiled by clang. Candidate: pool float
  constants per function or module and load them with one PC-relative `ldr`;
  measure on the transcendental prototypes and `bench/run-hotloop.sh`.
- **Signing-check scaling** [new] — the check that walks all module values once
  per signing is O(signings x declarations); not yet measured on a corpus with
  many signings. Candidate: index values by signing; measure with
  `bench/run-compile.sh`.
- **Machine-code size** [new] — 5,000 generated functions emit 1.60 MB release
  and 2.07 MB checked against 1.10 MB for `cc -O2`.
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
  making optimizer-wide claims; `bench/calls.ace` covers calls and aggregate
  passing, and `bench/run-compile.sh` covers compile time and code size. `noalias`-driven
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

- **Dynamic array and runtime hash-map modules** [new] — composed from `mem`
  and explicit generics; separate from the compiler-built static lookup map.
- **Fixed-width integer formatting** [additive] — width, zero padding, and base
  prefixes for `fmt.printf`, especially fixed-width `%b`/`%o`/`%x` register
  displays; carve consistently across integer conversions from a hardware
  fixture rather than adding a binary-only modifier.
- **Transcendental math functions** [open] — `exp`, `log`, `pow`, `sin`, and
  `cos` are implemented, and `exp`, `log`, and `pow` are table-driven over the
  two 128-entry tables in `std/@/pow_tables.ace`. Figures below come from one
  harness: release build, 4096 arguments, 32 repeats, best of seven rounds,
  glue-subtracted through a non-inlined identity, first run of each binary
  discarded. Throughput: `exp` 5.8 ns, `log` 7.8 ns, `pow` 23 ns (`f32`: 12
  ns), `sin` 11.4 ns and `cos` 12.2 ns in the Cody-Waite band and 4.0 and 4.7
  ns below pi/4, against libm's 3-4 ns for the first four and 9.5 ns for
  `pow`. Dependent-chain latency: `exp` 15.2 ns, `log` 23.8 ns, `sin` 50.7 ns
  and `cos` 51.8 ns in the Cody-Waite band, 11.7 and 24.6 ns below pi/4.
  Arguments beyond 1.6e6 take about 590 ns because the Payne-Hanek fraction is
  extracted bit by bit; a word-at-a-time extraction would cut that. Chain
  latency runs between two and four times throughput throughout, so every one
  of these kernels is latency-bound and only chain-shortening changes pay.
  `sin` and `cos` now hold no call on either hot path, so what is left in
  their chains is the Cody-Waite reduction itself, about seventy per cent of
  it: a table-driven kernel shrinks the polynomial and needs a finer reduction
  to do it, so it addresses the smaller half and is not the next step for
  them. The `pow` gap to libm is backend quality: a local that wins no
  register is reached through an address computed per access, and each
  constant load takes three instructions. `pow`'s hot path holds no call and
  is at the register ceiling, so its source-level lever is spent; what is left
  for it is the promotion and constant-materialization items in the compiler
  band above. Separately, `reduce_large` loses fifteen integer locals to the
  promotion limit and extracts its fraction one bit at a time, which is what
  makes arguments beyond 1.6e6 cost about 590 ns.
- **Random source** [new].

## 5. Language questions and conveniences

Each item requires an owner decision and should wait for a motivating use case.

### Future primitive extensions

- **`f16`** [new] — IEEE binary16 for graphics, shaders, inference workloads,
  and targets with native GPU texture or render-target support.
- **`bf16`** [new] — a 16-bit float retaining the broad exponent range of
  `f32`, useful for ML training-style workloads and straightforward conversion
  to and from `f32`.

- **Methods on enums and unions** [unspecified].
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

## 6. Later networking and protocol layers

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

## 7. Completed checked-build checkpoint

The explicit `--checked`/`--release` modes and strict sequence bounds are
implemented: emitted programs default to checked mode, while release strips
their dynamic guards, failure paths, and diagnostic metadata. This completed
checkpoint remains here as a compact reference rather than occupying the active
priority list.

- **[implemented]** Build-mode plumbing and array, slice, and string bounds diagnostics.
- **[implemented]** Checked integer arithmetic diagnostics (Safety & Checked Builds #6).
- **[implemented]** Call-boundary `noalias`/length diagnostics (Safety & Checked Builds #7; Memory #16).
- **[implemented]** Checked null-use diagnostics (Safety & Checked Builds #8; Zero #2).
- **[implemented]** Allocator diagnostics (Safety & Checked Builds #9).
- **[implemented]** Language-level `assert(bool)`, checked-only and release-stripped, for invariants and simple checked-mode tests (Safety & Checked Builds #10).
