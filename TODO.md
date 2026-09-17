# TODO — future completion list

Candidate catalogue of missing features and capabilities, core language first. This file is a survey, not a ledger: **no item here is a decision**. Anything promoted to work must be carved through the Decision Queue in `SPEC.md` (rule + rationale recorded there), implemented chunk by chunk, and logged in `CHANGELOG.md`. Ordering notes are recommendations to be resolved by the owner under the priority stack (`PHILOSOPHY.md`): performance > simplicity > modularity > compile time > safety.

Status tags:

- **[defect]** — implemented behavior is incorrect and requires diagnosis or repair
- **[queued]** — already listed for later work in `SPEC.md`
- **[open]** — already open in the Decision Queue or normative spec
- **[additive]** — explicitly deferred as an additive extension by a carved decision
- **[new]** — absent from every existing deferred list; observed here first
- **[unspecified]** — neither decided nor rejected in `SPEC.md`; support must be verified, then carved either way

Territory rejected by design is excluded entirely: manual memory through one fixed strategy (no GC, ownership, or allocator values), no exceptions or propagation, no inference, no optional type operator, no COW, no iterator contracts, no comptime or reflection, no block-scoped defer, no hidden-capture closures (explicit contract values are the designated mechanism). Items appear only where the existing design leaves the question open.

The priority bands below are recommendations, not decisions. The active
Decision Queue in `SPEC.md` always wins; dependencies within a band are listed
before the work they unblock.

## 0. Known defects

## 1. Core type-system completion

Finish the existing primitive, sequential, and aggregate model before adding
new execution or library layers. These are narrow completion tasks, not a
reopening of Ace's fixed explicit-type, copy, mutation, or allocation rules.

Wider fixed-array/view bridges remain additive conveniences; they are not
blockers for the current sequential core.

### Aggregate and generic construction

- **Generic struct literals** [new] — explicitly instantiated generic structs
  exist, but their direct literal construction is not implemented.
- **Aggregate ABI cross-product audit** [new] — extend the existing ABI oracle
  across nested arrays, structs, unions, tuples, homogeneous floating
  aggregates, indirect returns, register exhaustion, and explicit generic
  instances. Treat failures as implementation defects, not new semantics.

### Other compile-time machine facilities

- **Source-location builtins** (`@file`/`@line`-class) [new] — bounded context
  for checked assertions and panic diagnostics.
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
- **TLS client transport needs a fresh carve** [open] — the prior proof
  (pinned BearSSL 0.6, a private C adapter, the compiler-owned
  `@extern(component)`/link-recipe mechanism, `std/tls.ace`, and `http`'s
  HTTPS/`Client` overloads) has been removed entirely (code, tests, vendor
  sources, and docs); see `CHANGELOG.md` and the SPEC.md Decision Queue entry.
  Nothing about the removed approach — backend, adapter shape, or the
  `@extern` component mechanism — carries forward. A fresh carve (philosophy,
  then semantics, then syntax, then implementation) deciding the transport
  boundary and whether `tls`/HTTPS belongs in `std` at all versus launching
  directly as a package must happen before anything like it returns. `http`
  itself (independent of TLS/HTTPS) has already relocated out of `std` to
  the standalone package `pkg/http`; see `CHANGELOG.md`.
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
  until the TLS client transport carve above lands; `rune`,
  UTF-8 decoding, and `yarn` are not prerequisites because HTTP framing is
  ASCII and payloads remain byte views.
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
