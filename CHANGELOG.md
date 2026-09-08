# Ace — Implementation History

The sole historical record of implemented compiler and language work. Entries
state what landed and the essential implementation approach; `SPEC.md` contains
current normative rules, and Git preserves patch-level detail.

## Bootstrap compiler

- Implemented the C11 lexer, recursive-descent parser, semantic checker, and
  direct AArch64/Apple-arm64 emitter without third-party dependencies.
- Emission writes Mach-O objects directly, follows the platform ABI, resolves
  forward calls with narrow fixups, and emits minimal multi-file DWARF line
  tables. Inline recursive layouts are rejected; forward type names are allowed
  only through pointer-only paths whose fixed size is already known.
- Added `--tokens`, `--ast`, `--report`, object emission, and executable
  linking. The driver passes ordered objects, archives, library paths, libraries,
  and Apple frameworks to `cc` through an argument vector rather than a shell.
- Added mutually exclusive checked/release emission modes, checked by default,
  with statically elided runtime guards and aborting diagnostics for dynamic
  array, slice, and string indexing and slicing bounds; release objects retain
  no bounds failure machinery or metadata.
- Implemented `@when` for top-level compile-time declaration filtering with a
  closed target/boolean expression language, plus host-defaulted `--target`;
  inactive declarations remain syntax-checked but never reach imports, semantic
  checking, or emission.

## Core language

- Implemented explicit `name: type` declarations, local `:=` type elision,
  per-name declaration/assignment in multi-name `=`, constants, aggregates,
  distinct types, aliases, enums, tuples, functions, overloads, and simultaneous
  multi-return assignment.
- Implemented value-copy assignment and by-reference parameters. `mut` controls
  caller-visible mutation at signatures and call sites; pointer and view
  writability is tracked independently at each type level; `noalias` supplies a
  compile-time non-overlap fact.
- Fixed module-private name lowering to preserve lexical value and type scopes,
  so parameters and locals shadow same-named module functions without changing
  unshadowed overload, qualified-module, or method resolution.
- Implemented canonical all-bits-zero values and path-sensitive definite
  initialization for direct Ace storage. Pointer aliases, extern calls, and
  inline assembly remain explicit trust boundaries.
- Implemented operators, chained comparisons, `if`, exhaustive `switch`,
  contextual explicit `fallthrough`, labeled blocks, labeled break/continue,
  `for-in`, ranges and steps, and general condition/three-clause/infinite
  `for` forms. Loops lower directly to structured branches and index traversal.
- Implemented arrays, slices, strings, slicing, indexing, `len`, and mutable
  views. Unified `string`, `[]char`, and `[]u8` as three intent spellings of
  one type, with the same identity rule for writable forms; spelling-only
  overload variations are duplicate declarations. Sequential value equality
  is rejected; byte content comparison is the explicit compiler-backed
  `mem.equal`.
- Implemented call-only, function-scope `defer`. Calls are captured at the
  statement according to the specified two-tier model and emitted in LIFO order
  at every function exit.

## Types and abstraction

- Implemented structs, explicit one-level field embedding, type-owned methods,
  exact method overloads, contracts, canonical signings, explicit contract-value
  construction, and per-used-pair vtables. Promoted methods use fixed-offset
  receiver thunks; fields are never promoted.
- Implemented explicit generic instantiation and exact function specialization.
  Generic bodies are checked per concrete stamp; compiler-backed standard
  generics share the ordinary resolution path.
- Implemented closed tagged unions as an i32 declaration-order tag plus aligned
  maximum-size inline payload. Construction is qualified; switches are
  exhaustive and bind payload copies; no boxing, reflection, or hidden
  allocation is involved.
- Implemented opaque packed 64-bit `Error` values with compiler-assigned nominal
  domains and i32 codes. Errors remain ordinary values. The earlier static
  descriptor representation was removed.
- Implemented reserved `panic(message)` as best-effort direct stderr output
  followed by platform `abort()`; it cannot be caught or used as a value.
- Implemented the remaining checked-build diagnostics: exact integer arithmetic
  and shift guards, slice-range `noalias` and byte-operation length contracts,
  null dereference/call/dispatch checks, and an allocation-backed live registry
  for authoritative extent validation and normal-return leak reporting. Release
  objects retain none of this machinery. Added reserved checked-only
  `assert(bool)` with source-located failure and fully stripped release
  evaluation.

## Memory

- Implemented raw `mem.alloc`, `realloc`, and `release` over authoritative
  slice descriptors using the platform allocator directly. Release mode carries
  no headers or allocation registry; failures return canonical null, successful
  release nulls only the passed descriptor, and aliases are not tracked.
- Implemented explicit typed allocation overloads. The compiler checks
  `count * sizeof(T)`, derives alignment from `T`, calls the same allocator,
  and returns an uninitialized `mut []T` whose length is the element count.
- Implemented allocation-free `mem.copy`, `move`, `fill`, `zero`,
  `compare`, and `equal` through direct compiler-backed platform operations.
  Empty views avoid calls; release builds assume carved length/alias contracts.
- Fixed slice and string representation at two words (pointer and count), with
  arrays stored contiguously and strings carrying exact non-NUL-terminated bytes.

## Modules and C boundary

- Implemented canonical-path, depth-first module loading with whole-module and
  selective imports, aliases, visibility checks, cycle diagnostics, and
  declaration-before-use ordering. Modules merge into one compilation unit under
  private prefixed identities; canonical files load once.
- Added standard directory modules (`std/name/module.ace`) beside single-file
  modules, with ambiguity diagnostics. OS-layer files remain explicitly imported
  behind stable public facades.
- Represented compiler-reserved standard-library implementation as the
  canonical `std/@/` subtree and unquoted `@file.ace` import source, enforcing
  the existing boundary after path canonicalization while leaving direct
  reserved-source checks available.
- Implemented `extern "c"`, `export`, link names, and C variadics behind a
  positive ABI whitelist. Apple-arm64 lowering covers compatible scalars,
  pointers, distincts, recursive structs, nested arrays, register exhaustion,
  homogeneous floating aggregates, indirect aggregates, and structure returns;
  clang-compiled fixtures serve as the ABI oracle.
- Implemented statement-only AArch64 inline assembly with explicit
  input/output/clobber bindings and no constraint language or reordering.

## Standard library

- Standardized concise stdlib naming across public facades, selected OS layers,
  documentation, and fixtures. Subject words remain where a module can expose
  multiple categories (`net.SocketKind`, `sys.FileKind`); established
  abbreviations shorten `Addr` and `Err` without
  clipping ambiguous ordinary words; reversible networking properties use
  `reuse_addr`, `reuse_port`, and `nonblock`; and directory operations use the
  consistent `open_dir` / `read_dir` / `close_dir` family.
- Implemented allocation-free `fmt.print` and `println` overloads using small
  fixed stack buffers and retrying partial writes; portable formatting lives in
  its facade while libc primitives stay in its selected target layer.
- Implemented compiler-planned `fmt.printf` for static format strings. Semantic
  analysis validates exact argument types and builds a format plan; emission
  calls typed helpers directly, with no runtime parser, argument tags, reflection,
  allocation, or C variadic formatter.
- Added `%b` binary and `%o` octal `fmt.printf` conversions for `uint` and
  `usize`, generalizing hexadecimal's private helper into one fixed-buffer
  unsigned-radix path. Prefixes, width, and zero padding remain explicit
  additive work.
- Implemented `str` character/string operations and explicit
  `str.parse[T]` specializations in pure Ace where practical, with libc float
  conversion isolated in its selected target layer. `char` remains the
  transparent byte alias and strings remain UTF-8 byte sequences.
- Implemented `sys` file-descriptor I/O, descriptor constants, errno capture,
  command-line arguments, environment access, and separate elapsed/awake
  monotonic clocks through thin platform bindings.
- Added immediate `sys.exit(code)` through the OS-neutral `sys` facade and
  a private Darwin OS layer calling libSystem `_exit`; it runs neither defers
  nor C-runtime exit handlers and requires no compiler intrinsic.
- Implemented blocking IPv4 TCP `net`: socket creation, explicit options,
  bind/listen/accept/connect, partial send/receive, peer addresses, DNS
  resolution, and operation-specific error domains. The public directory module
  wraps a private Darwin OS layer and owns no hidden allocation.
- Added pure-Ace blocking IPv4 `tcp` composition over `net`: listener and
  connection constructors with failure cleanup, blocking accept, exact receive,
  all-byte send, EINTR retry with retained progress, and explicit wrapper close.
  Lower-layer operation errors remain unchanged; only premature EOF and
  successful zero-progress send introduce TCP-level domains. A fresh blocking
  echo fixture establishes the Stage 2 carving baseline without preselecting a
  nonblocking event model or public reactor state shape.
- Implemented the selected `tcp` nonblocking connection-progress model with
  caller-owned per-call buffers, explicit tagged events, managed poll interest,
  partial-send offsets, backpressure, and nonblocking connect completion.
  Added the one-operation `net.pending_error` prerequisite and named Darwin
  errno constants without adding OS code or externs to `tcp`.
- Reshaped the public `tcp` vocabulary around its module scope: `Conn`,
  `Interest`, and `Event` keep type names compact; blocking and nonblocking
  construction remain the distinct `connect` and `connect_nonblock` operations;
  `interest` exposes readiness state; and whole-buffer receive/send use the
  paired `receive_all` / `send_all` names with `ReceiveAllErr` / `SendAllErr`.
- Implemented general descriptor `poll` with allocation-free queues, paired
  watch/unwatch, level-triggered read/write interest, error/hangup reporting,
  caller-owned event storage, and finite/infinite nanosecond waits. Darwin kqueue
  is private to the OS layer.
- Proved nonblocking TCP server composition over `poll` with a forked loopback
  echo fixture: the listener and accepted peer are configured explicitly,
  readiness drives one-attempt accept/receive/send operations, would-block stays
  caller-visible, partial progress is retained, and all descriptors are closed
  explicitly. It exposed the need to query an OS-selected local endpoint.
- Extended the proof into a deterministic multi-client chat fixture with one
  single-threaded event loop, fixed caller-owned client and buffer storage, and
  forked child test clients. It covers multiple accepts, readiness-driven
  fragmented reads and writes, per-recipient broadcast progress, orderly
  disconnects, and reuse of a vacated client slot without adding API surface,
  allocation, threads, atomics, or retry policy.
- Added allocation-free `sys.Process`, `fork`, and `wait` wrappers with decoded
  exit/signal status, keeping process-control externs in the private Darwin OS
  layer. Added `net.local` for querying an ephemeral bound endpoint; networking
  fixtures now bind port zero instead of guessing ports from process IDs.
- Removed generated programs' `strlen` dependency: `sys.arg` and `sys.env` now
  scan their OS-provided NUL-terminated entry bytes directly in emitted code.
- Removed generated programs' `memcmp` dependency: `mem.compare`, `mem.equal`,
  and byte-sequence equality now use direct emitted unsigned-byte loops.
- Removed generated programs' `memset` dependency: `mem.fill` and `mem.zero`
  now use direct emitted byte-store loops.
- Removed generated programs' `memcpy` and `memmove` dependencies: `mem.copy`
  emits a forward byte loop and `mem.move` selects forward or backward traversal
  from the source and destination addresses.
- Added the public `bits` standard module with exact compiler-backed
  `from_float` and `to_float` overloads for `f32`/`u32` and `f64`/`u64`; all
  four preserve complete IEEE-754 representations through register moves with
  no runtime call. Added the restricted trailing `intrinsic` declaration form;
  admitted declarations receive real callable symbols while direct calls may
  inline the same instruction sequence.
- Extended `bits` with exact `u8`/`u16`/`u32`/`u64` population count,
  leading/trailing-zero count, byte-swap, and rotate overloads. Counts return
  `uint`, zero-count operations return the operand width, and rotations reduce
  every `uint` count modulo that width; direct and indirect calls use native
  Apple-arm64 scalar/SIMD instruction sequences with no runtime dependency.
- Shortened the public zero-count families to `bits.lead_zeros` and
  `bits.trail_zeros`, retaining their exact overloads and lowering.
- Added pure-Ace `math.isnan`, `math.isinf`, and `math.isfinite` overloads for
  `f32` and `f64`, using `bits` to classify every IEEE-754 encoding without
  allocation or a libm dependency.
- Replaced generated float formatting's `ecvt`/`fcvt`, parse-back, `floor`,
  floating-environment, and memory-helper dependencies with a shared native
  exact binary-to-decimal engine under `std/@`. Fixed-capacity base-2^32
  arithmetic produces shortest round-trip `f32`/`f64` text and fixed `%f`
  rounding through precision 324 without allocation or platform conversion.
- Replaced shortest `f32`/`f64` formatting's exact bigint search with an
  Apache-2.0-attributed Ace adaptation of Ulf Adams' Ryu algorithm. Fixed-width
  products and cached powers now select the shortest ties-to-even coefficient;
  fixed `%f` retains the bigint path through precision 324.
- Replaced `str.parse[f32/f64]` platform parsing with native exact
  decimal-to-IEEE conversion. Fixed-capacity bigint comparisons select adjacent
  values and round exact midpoints to even, including subnormal and overflow
  boundaries, without allocation, locale state, `strtod`, or `strtof`.
- Added a three-tier decimal-to-float parser: exact small integers, a
  fast_float-attributed Eisel-Lemire fixed-width path for ordinary decimals,
  and the existing bigint comparison path for long exact fallback cases. The
  tier-selecting scan also performs the complete syntax validation once.
  Specialized the f32/f64 fast converters after generated-AArch64 profiling:
  fixed-step leading-zero normalization, scalar out-parameters, integrated
  special-value classification, and fused cached-power multiplication avoid
  the generic call chain and its aggregate spill/copy overhead.
- Added a compiler-shipped internal native-endian unaligned word load and used
  it to scan finite float digit runs eight bytes at a time. A branchless SWAR
  digit mask, fixed-step trailing-zero count, packed eight-digit accumulation,
  and scalar short-tail path preserve the shared three-tier parser validation.
- Added short and long-mantissa float-parse benchmark cohorts as supporting
  diagnostics and made an explicit 80%-short/20%-long blend the canonical
  comparison, keeping scan-path performance visible without conflating it with
  the greater-than-19-digit bigint conversion fallback.
- Added `sys.pipe` so descriptor-polling fixtures also use the public OS facade
  instead of declaring a test-local C binding.
- Exposed the target-defined `sys.EAGAIN` and `sys.EWOULDBLOCK` errno constants
  so nonblocking callers do not embed platform numbers.
- Standardized OS-specific stdlib organization as a public `module.ace` facade
  selecting `os/<target>.ace`; `fmt`, `net`, `poll`, and `sys` use the layout,
  and each target file implements its facade operations directly. Flattened
  `str` back to `std/str.ace` after its final OS dependency was removed.
- `math` currently contains only its minimal implemented integer/generic
  helpers; floating libm and SIMD remain open work.

## Significant implementation corrections

- Repaired the manual memory benchmark for the current zero-valid slice model,
  made checked/release costs explicit, and added a checksum-matched integer CPU
  comparison against equivalent C `-O0` and `-O2` builds.
- Corrected AArch64 aggregate argument/return layout, deep-frame loads, floating
  load/store scaling, integer-to-floating temporary separation, remainder
  lowering, short-circuit normalization, variadic stack slots, and large
  subobject copies as their feature fixtures exposed them.
- Widened constant folding to constructor-shaped same-family primitive, alias,
  distinct, enum, and qualified casts while retaining semantic fit and
  cross-family checks.
- Canonicalized equivalent module paths before assigning nominal Error identities,
  preventing duplicate module loads and domain identities.
- Derived private symbol prefixes for directory-module entries from their
  directory names rather than the repeated `module.ace` filename.

- Expanded `sys` with allocation-free filesystem metadata and mutation
  (`stat`, `mkdir`, `rename`, and `unlink`) plus caller-owned buffered directory
  iteration (`open_dir`, `read_dir`, and `close_dir`). The Darwin layer parses
  private `dirent64` batches into exact caller-provided name buffers, keeps
  undersized entries retryable, and exposes only stable file kind and size.
  The portable `mkdir(path)` form delegates effective permissions to the
  process umask, while explicit POSIX creation modes use named `S_I*` bits.
- Allowed global constant initializers to forward recursively evaluable
  constants from imported modules. The `sys` facade now re-exports its
  target-selected `O_*` and `S_I*` values without duplicating their numbers.

## Current frontier

The checked-build queue is complete. Additive networking, concurrency, fuller
math/SIMD, and a second
backend remain open; their status belongs in the `SPEC.md` Decision Queue and
`TODO.md`, not in this history.
