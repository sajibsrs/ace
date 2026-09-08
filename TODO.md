# TODO — future completion list

Candidate catalogue of missing features and capabilities, core language first. This file is a survey, not a ledger: **no item here is a decision**. Anything promoted to work must be carved through the Decision Queue in `SPEC.md` (rule + rationale recorded there), implemented chunk by chunk, and logged in `CHANGELOG.md`. Ordering notes are recommendations to be resolved by the owner under the priority stack (`PHILOSOPHY.md`): performance > simplicity > modularity > compile time > safety.

Status tags:

- **[queued]** — already listed for later work in `SPEC.md`
- **[open]** — already open in the Decision Queue or normative spec
- **[additive]** — explicitly deferred as an additive extension by a carved decision
- **[new]** — absent from every existing deferred list; observed here first
- **[unspecified]** — neither decided nor rejected in `SPEC.md`; support must be verified, then carved either way

Territory rejected by design is excluded entirely: manual memory through one fixed strategy (no GC, ownership, or allocator values), no exceptions or propagation, no inference, no optional type operator, no COW, no iterator contracts, no comptime or reflection, no block-scoped defer, no hidden-capture closures (explicit contract values are the designated mechanism). Items appear only where the existing design leaves the question open.

The priority bands below are recommendations, not decisions. The active
Decision Queue in `SPEC.md` always wins; dependencies within a band are listed
before the work they unblock.

## 1. Core type-system completion

Finish the existing primitive, sequential, and aggregate model before adding
new execution or library layers. These are narrow completion tasks, not a
reopening of Ace's fixed explicit-type, copy, mutation, or allocation rules.

### Sequential and tuple completeness

- **Sequential/aggregate composition audit** [new] — verify nested arrays and
  slices of structs, enums, unions, distincts, and explicit generic instances
  across initialization, copying, mutation, parameters, returns, and checked
  bounds. Add missing positive and negative coverage before widening the model.

Read-only `string` → `[]u8`, wider array/string bridges, Unicode decoding, and
owned growable sequences remain additive library or view conveniences; they are
not blockers for the current sequential core.

### Aggregate and generic construction

- **Generic struct literals** [new] — explicitly instantiated generic structs
  exist, but their direct literal construction is not implemented.
- **Aggregate ABI cross-product audit** [new] — extend the existing ABI oracle
  across nested arrays, structs, unions, tuples, homogeneous floating
  aggregates, indirect returns, register exhaustion, and explicit generic
  instances. Treat failures as implementation defects, not new semantics.

### Representation and layout controls

- **Enum explicit discriminants and backing width** [new] — opcodes, hardware
  registers, and ABI structs need explicit values and storage width.
- **`packed` / `align` attributes** [queued] — required for exact wire, hardware,
  and C-compatible aggregate layouts.
- **`volatile` access** [queued] — required for MMIO and other storage whose
  reads and writes are externally observable; keep its semantics distinct from
  aggregate layout attributes.

### Compile-time layout introspection

- **`alignof(T)`** [new] — raw `mem.alloc(size, align)` callers must currently
  guess struct alignment; typed allocation derives it privately.
- **`offsetof(T, field)`** [new] — C structs are hand-transcribed; offsets plus
  static assertions are how a transcription is proven.
- **Static assertions** [new] — no general constant-expression check context
  exists.
- **`sizeof` on expressions** [new, minor] — currently type-only.

### Other compile-time machine facilities

- **Source-location builtins** (`@file`/`@line`-class) [new] — bounded context
  for checked assertions and panic diagnostics.
- **Binary data embedding (`embed "file"`)** [new] — static read-only storage
  serving later game and asset milestones.

### Pointer-level machine operations

- **Pointer arithmetic** [unspecified] — the compiler currently provides none.
  Carve only operations that preserve Ace's explicit mutability and checked-mode
  boundaries, without introducing iterator or ownership machinery.
- **Pointer ↔ integer conversions** (`usize(p)`, `T(addr)`) [unspecified] — state
  the sanctioned machine-true form for descriptor packing and tagged pointers.
- **Over-16-byte alignment for raw `mem.alloc`** [additive].

### C boundary completion

- **C-ABI function pointers as callable values** [new] — the C boundary
  whitelist rejects ordinary Ace function values, and taking the address of an
  `export`ed function as a value is unspecified. Carve only the callback shapes
  proven by target facilities; general callback interoperability stays
  additive.
- **`bool` (`_Bool`) mapping** [additive].
- **C enums at the boundary** [additive].
- **C global variables in extern blocks** [unspecified] — data imports such as
  `stdout` and errno-style globals are unaddressed.

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

- **Peephole cleanup** [queued] — implement the bounded emitted-code pass from
  Compiler #6, targeting redundant loads/stores and address recomputation first.
- **Loop-carried register promotion** [queued] — reduce the current stack-slot
  traffic with the specified bounded per-loop scan.
- **Read-only parameter load reuse** [queued] — exploit the existing parameter
  facts without adding global alias analysis.
- **Float-parse optimization backlog** [new] — use the canonical 80%-short /
  20%-long benchmark's current 6.8x `strtod` ratio as the auditable baseline;
  roughly 2x is an aspirational next target, not a performance promise. Carve
  only after measuring each boundary, in this order:
  1. Disassemble the hot path and verify whether the specialized Eisel-Lemire
     step is inlined, whether scalar and multi-value results stay in ABI
     registers, and where calls introduce stack or out-parameter traffic.
  2. Inspect 64x64-to-128 multiplication lowering. Apple arm64 should use
     `mul`/`umulh` where applicable (and a future x86-64 backend its native wide
     multiply) rather than an avoidable software partial-product sequence.
  3. Instrument per-call tier selection on the canonical corpus: exact-small,
     Eisel-Lemire, and bigint fallback. Improve exact-small coverage for common
     inputs only where exactness and the existing explicit tier rules permit.
  4. Profile fixed per-call work: special-value prefix handling, validation,
     digit/exponent bookkeeping, spills, and repeated loads. Preserve the
     current single syntax-validation pass and exact rounding behavior.
  5. Audit arithmetic-heavy generated code for generally poor instruction
     selection before adding parser-specific workarounds; feed reusable fixes
     back into the backend.
  6. Consider power-table alignment and cache layout last, after instruction
     and call costs are reduced. Specialized parsers can outperform platform
     `strtod`, so C is the comparison baseline rather than a hard ceiling, but
     any stronger claim requires a local checksum-matched measurement.
- **Constant arithmetic lowering** [new] — measure and carve local strength
  reduction for constant division/remainder and similarly costly instruction
  sequences if broader benchmarks confirm the recurrence result.
- **Broader performance corpus** [new] — add checksum-matched Ace/C fixtures for
  calls, large aggregates, slices, branches, floating point, cache-heavy loops,
  and compile/object size before making optimizer-wide claims. `noalias`-driven
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
- **Threads** [new] — explicit create/join/exit over libSystem pthreads, using
  the narrowly carved C-callable function values from section 1; create/join
  remain ordinary calls with no hidden control flow.
- **Atomics** [new] — load/store/exchange/compare-and-swap plus fences with
  explicit ordering; lower directly to the target's supported instructions.
- **Thread-local storage** [new] — required for per-thread application state;
  distinct from the future TLS secure-transport module.
- **Mutex/condition-variable module** [new] — thin explicit synchronization
  over the thread and memory model, with no hidden allocation or ownership.
- **Process lifecycle completion** [new] — `fork`, exact-child `wait`, pipes,
  and immediate `exit` are implemented. Add `exec`/spawn composition, explicit
  argument/environment and descriptor wiring, and nonblocking waits only when
  a concrete subprocess fixture establishes their required shapes; keep this
  separate from shared-memory thread semantics.

## 4. Practical standard-library capabilities

- **Signals** [additive] — excluded from `poll`; servers need explicit shutdown
  handling.
- **Dynamic array and hash-map modules** [new] — composed from `mem` and
  explicit generics.
- **`str.concat` / `str.join`** into caller storage [new].
- **Fixed-width integer formatting** [additive] — width, zero padding, and base
  prefixes for `fmt.printf`, especially fixed-width `%b`/`%o`/`%x` register
  displays; carve consistently across integer conversions from a hardware
  fixture rather than adding a binary-only modifier.
- **`rune` + UTF-8 decode and `yarn`** [additive] — retain explicit decoding
  and ownership over the shared `string`/`[]char`/`[]u8` byte-view type.
- **libm transcendentals** [open] — `sin`, `cos`, `pow`, `exp`, `log`, `floor`,
  and `ceil` for the 2D/3D milestones.
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
- **Associated functions** (`Vec[T].new()`) [new].
- **`switch` on strings and range cases** [unspecified].
- **Operator overloading / custom indexing** [unspecified] — likely conflicts
  with explicit dispatch; free functions may suffice.
- **Runtime global initializers / module-init hook** [unspecified] — globals are
  constant-fold-only today; either admission or permanent rejection should be
  stated explicitly.

## 6. Later networking and protocol layers

Resume after the higher-priority core work. Each layer keeps allocation,
buffering, capacities, retries, timeouts, and ownership explicit.

The first pure-Ace blocking IPv4 TCP composition (`std/tcp.ace`) is implemented:
explicit listener/connection handles, setup cleanup, blocking accept, exact
receive, all-byte send, EINTR retry, and explicit close. The items below remain
additive later work; the nonblocking proofs continue to use `net` and `poll`.
The Stage 2 connection-progress layer is also implemented: caller-owned buffers,
explicit tagged events, poll-interest transitions, partial-send offsets, and
nonblocking connect completion remain visible without callbacks or allocation.

- **Additive socket capabilities** [additive] — IPv6, UDP, address queries,
  half-close, and extra TCP options, carved
  in the order demanded by concrete proofs.
- **TLS secure-transport module** [new] — explicit transport over `net`, with
  visible configuration, handshake progress, certificate verification, and
  caller-owned buffers; no hidden allocation or background execution.
- **HTTP client/server module** [new] — a protocol layer over plain `net` or
  explicit TLS transport with caller-owned connection, parser, body, and
  response storage. Address family belongs to transport values, so IPv6 does
  not duplicate the protocol API as `http6` or `https6`. HTTPS is HTTP composed
  with TLS; an `https` convenience remains deferred until real usage proves it
  can preserve explicit costs and state.

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
