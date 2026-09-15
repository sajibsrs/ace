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
  array, slice, and string indexing and slicing bounds. Bounds diagnostics now
  identify the failing source file, line, and column; release objects retain
  no bounds failure machinery or metadata.
- Implemented `@when` for top-level compile-time declaration filtering with a
  closed target/boolean expression language, plus host-defaulted `--target`;
  inactive declarations remain syntax-checked but never reach imports, semantic
  checking, or emission.

## Core language

- Replaced `:` with `=` for named struct and bits literal field initialization,
  reserving `:` for declarations and type annotations and rejecting the former
  literal spelling in the parser.
- Removed local `:=` declaration elision so every storage binding states its
  type and initializer checking always has an explicit declared expectation.
- Implemented predeclared module-level `StaticMap[K]V` constants with
  compile-time entry checking and duplicate detection, read-only key/value and
  deterministic perfect-hash-plan emission, exact candidate confirmation, and
  canonical-zero misses; `std/http` now uses the facility for status phrases.
- Implemented per-field compile-time struct defaults and omission of defaulted
  fields in named literals, including empty all-default literals and explicit
  overrides, without changing canonical zero, implicit storage initialization,
  layout, or ABI.
- Split unary operators by role: `*` now explicitly dereferences pointers, `~`
  complements integers, and `!` negates only Booleans, with dereference lvalues
  carried through semantic checking, constant evaluation, and direct emission.
- Implemented typed pointer `+`, `-`, `+=`, and `-=` with `usize` element
  offsets, exact pointer-permission preservation, complete nonzero pointee
  validation, checked null/scaling/address diagnostics, and guard-free release
  lowering. Pointer indexing, difference, ordering, bounds, and provenance stay
  excluded.

- Added `float` as a predeclared transparent alias of `f64`; both spellings
  share type and overload identity, representation, conversions, and ABI
  behavior.
- Implemented explicit `name: type` declarations, per-name
  declaration/assignment in multi-name `=`, constants, aggregates,
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
- Implemented explicit enum discriminants and fixed integer backing types with
  zero-first, fit, and uniqueness validation. Explicitly backed enums use their
  selected layout throughout ordinary values and C-compatible boundaries;
  default enums retain their existing `i32` layout and remain C-ABI-rejected.
- Implemented nominal `bits u8`/`u16`/`u32`/`u64` types with exact LSB-first
  signed, unsigned, and anonymous fields. The compiler lowers whole values at
  backing width, emits sign-extending reads and preserving checked/release RMW
  writes, zero-fill literals, exact raw conversions, same-type equality,
  aggregate placement, whole-value volatile access, and C-boundary rejection.
- Implemented operators, chained comparisons, `if`, exhaustive `switch`,
  contextual explicit `fallthrough`, labeled blocks, labeled break/continue,
  `for-in`, ranges and steps, and general condition/three-clause/infinite
  `for` forms. Loops lower directly to structured branches and index traversal.
- Implemented arrays, slices, strings, slicing, indexing, the direct read-only
  `.len` structural property, and mutable
  views. Unified `string`, `[]char`, and `[]u8` as three intent spellings of
  one type, with the same identity rule for writable forms; spelling-only
  overload variations are duplicate declarations. Sequential value equality
  is rejected; byte content comparison is the explicit compiler-backed
  `mem.equal`.
- Audited sequential/aggregate composition with positive and negative coverage
  for nested arrays and slices of structs, enums, unions, distincts, and exact
  generic instances across initialization, copying, mutation, calls, returns,
  and checked bounds; the existing value and view model required no widening.
- Implemented call-only, function-scope `defer`. Calls are captured at the
  statement according to the specified two-tier model and emitted in LIFO order
  at every function exit.

## Types and abstraction

- Added the allocation-free `ascii` module with control-byte classification,
  lowercase conversion, and selectable case-sensitive comparison; HTTP now
  reuses it, exposes serialization and incremental response parsing as
  type-owned methods, uses `str.concat` for multi-part text assembly, and keeps
  `mem.copy` for single-range byte movement.
- Added allocation-free, all-or-nothing `str.concat` over a string-parts slice
  and caller-owned destination storage.
- Implemented `@packed` structs and compile-time `@align` on structs and fields,
  with declaration-order effective alignment, exact stored field offsets,
  layout introspection, 16-byte stack/static support, and pointer-only initial
  C interoperability for special-layout aggregates.
- Implemented structs, explicit one-level field embedding, type-owned methods,
  exact method overloads, contracts, canonical signings, explicit contract-value
  construction, and per-used-pair vtables. Promoted methods use fixed-offset
  receiver thunks; fields are never promoted.
- Extended receiver-marked mutating method calls to every expression position,
  so their return values initialize and assign ordinary values while retaining
  the same visible `mut` receiver mark.
- Implemented explicit generic instantiation and exact function specialization.
  Generic bodies are checked per concrete stamp; compiler-backed standard
  generics share the ordinary resolution path.
- Implemented transparent generic type aliases by stamping and memoizing each
  explicit argument tuple to its fully substituted target type.
- Implemented standalone tuple-literal initialization and whole assignment;
  member expressions resolve independently before exact positional checking.
- Implemented closed tagged unions as an i32 declaration-order tag plus aligned
  maximum-size inline payload. Construction is qualified; switches are
  exhaustive and bind payload copies; no boxing, reflection, or hidden
  allocation is involved.
- Implemented opaque packed 64-bit `Error` values with compiler-assigned nominal
  domains and i32 codes. Errors remain ordinary values. The earlier static
  descriptor representation was removed.
- Added structured error-domain entries and allocation-free `Error.name` /
  `Error.message` lookup, emitting static description data only when used while
  retaining the packed 64-bit representation and arbitrary-code constructors.
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

- Implemented exact data-pointer/address conversions and explicit
  pointer-plus-count slice construction. The compiler preserves written
  permission, lowers the address and two-word descriptor directly, rejects
  locally provable alignment/null/extent violations, and emits only local
  alignment, null, scaling, and end-address guards in checked output.
- Implemented access-site volatile scalar reads and writes with once-only
  address/value evaluation and one exact-width AArch64 access. Packed and
  volatile fields share typed-place offsets and scalar load/store lowering;
  volatility adds no permission, atomicity, synchronization, or barrier.
- Implemented compile-time `alignof(T)` over the full type grammar, exposing the
  compiler's target-native required alignment as `usize` for raw allocation and
  layout checks.
- Implemented nested inline-struct `offsetof(T, field.path)` and zero-code
  module/function `static_assert` over the bounded constant-expression set.
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

- Made `mod.ace` the sole external facade of a standard-library directory
  module, with recursive implementation files importable only from within that
  directory boundary; facade-owned `pub` declarations, including transparent
  type aliases, explicitly define the external surface. Renamed existing
  directory facades from `module.ace` and shortened compiler-reserved imports
  from `@name.ace` to `@name`.
- Added explicit callable C function values with the distinct
  `extern "c" (parameters): return` type spelling, including zero-parameter and
  no-value shapes, contextual references to exact `export`/`extern` functions,
  positive ABI checking, null values with checked-call diagnostics, and
  C-convention indirect lowering. A clang fixture verifies the POSIX thread-entry
  shape in both C-to-Ace and Ace-to-C directions.
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

- Folded secure HTTP composition into the shared transport module and marked
  its redirecting client helper with the TLS external component, preserving
  standalone plaintext objects without a separate HTTP TLS file. Plaintext and
  TLS redirect entry points are now private overloads over shared redirect
  state preparation and advancement.
- Reorganized `std/http` as a directory module with focused parsing,
  formatting, client, redirect, transport, and TLS implementation files, and
  replaced six `get` overloads with plaintext `Context` and
  TLS-capable composed `Client { context, tls }` entry points. Redirect and TLS
  choices now live on those reusable values without multiplying overloads.
- Added opt-in bounded HTTP redirects to the owning client through explicit
  `RedirectPolicy` overloads, including relative URL resolution, HTTP-to-HTTPS
  upgrades, downgrade rejection, deterministic limits, and reusable owned URL
  workspace; existing GET overloads retain one-response behavior.
- Implemented the first blocking secure-client slice: a private TLS adapter uses
  system trust by default or decodes one explicit DER override, enforces
  hostname verification and SNI, and exposes partial record/application I/O to
  backend-neutral `tls` storage
  over `tcp`. Consolidated HTTPS into an exact allocation-free `http.get`
  overload with backend-neutral `TlsConfig`, default-null TLS storage, and one
  generic HTTP exchange path. Added compiler-owned conditional external components:
  TLS use automatically links the bundled, selectable BearSSL recipe, while the
  allocation-free plaintext HTTP path emits none of it. BearSSL engine failures retain
  their raw backend code behind a private offset.
- Moved secure entropy into allocation-free `sys.random`, with Darwin
  `getentropy` chunking, `EINTR` retry, and explicit errno failures; collapsed
  the target-neutral TLS facade to `std/tls.ace`; and renamed the compiler-owned
  conditional marker and internal requirement terminology from `@native` to
  `@extern`.
- Generalized compiler-managed external components to ordered recipes keyed by
  component, target, and selected backend. Migrated distributed target-native
  TLS artifacts to `external/` and BearSSL source, headers, rebuild guidance,
  and staging files to `vendor/`; the Darwin BearSSL recipe now carries its
  object, archive, Security, and CoreFoundation inputs.
- Added Darwin system-trust HTTPS as the default behind the stable private TLS
  adapter ABI, while preserving a nonempty DER anchor as an explicit override.
  The adapter feeds BearSSL's delivered chain to the Security framework with an
  SSL hostname policy; SNI remains mandatory. The owning `http.Client` now
  accepts both URL schemes and lazily manages TLS workspace, while the
  caller-storage HTTPS path remains allocation-free.
- Fixed owning HTTPS clients to allocate DNS address storage and grow chunked
  response bodies to the active chunk size instead of retrying unchanged
  capacity indefinitely.
- Pinned BearSSL 0.6 as an experimental TLS-client proof backend and added a
  private adapter/fixture proving direct archive linkage, opaque caller-owned
  client/X.509 storage, supported alignment, half-duplex record-buffer sizing,
  enforced TLS-1.2-only policy, chunked Darwin `getentropy` acquisition through
  `sys.random`,
  and explicit caller-supplied BearSSL seeding without expanding Ace's C ABI or
  expanding Ace's C ABI.
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
- Added the Darwin Apple-arm64 `sys.ioctl` escape hatch with the native
  `unsigned long` request width, variadic-call ABI, caller-owned byte arguments,
  immediate `IoctlErr` capture, and SDK-verified BPF constants/layouts; the
  allocation-free BPF demo manually walks aligned native headers and bounded
  Ethernet/IPv4 bytes, with deterministic decoding and ABI fixtures.
- Added immediate `sys.exit(code)` through the OS-neutral `sys` facade and
  a private Darwin OS layer calling libSystem `_exit`; it runs neither defers
  nor C-runtime exit handlers and requires no compiler intrinsic.
- Implemented blocking IPv4 TCP `net`: socket creation, explicit options,
  bind/listen/accept/connect, partial send/receive, peer addresses, DNS
  resolution, and operation-specific error domains. The public directory module
  wraps a private Darwin OS layer and owns no hidden allocation.
- Implemented the first allocation-free HTTP slice: strict IPv4 endpoint and
  HTTP URL parsing, all-or-nothing caller-storage request serialization, and
  incremental response parsing for bodyless, fixed-length, chunked,
  connection-close, and informational framing with caller-owned views/storage.
- Completed the symmetric HTTP message surface with `RequestParser` and
  `RequestResult`, renamed response state parsing to `ResponseParser.parse`,
  and exposed all-or-nothing `Request.format` and `Response.format` methods.
- Made both HTTP parsers genuinely incremental by preserving phase, line scan,
  framing, header, and body progress across calls instead of reparsing the
  complete occupied input prefix.
- Added HTTP message and parser-progress field defaults, named method and
  status constants, and conventional response reason selection for a null
  reason while preserving explicit and unknown-status empty phrases.
- Renamed the HTTP formatting error domain to `FormatErr`, matching the public
  `Request.format` and `Response.format` operations.
- Added named, allocation-free descriptions to the existing HTTP URL,
  formatting, and client error codes without changing their domains or numeric
  values; HTTP now constructs those failures through their named entries.
- Exposed the existing allocation-free HTTP status text lookup as
  `http.status(code)`, returning conventional phrases for known codes and an
  empty string for unknown codes without changing `Response`; removed the
  redundant public numeric status constants in favor of ordinary status codes.
- Added named descriptions for standard-library-defined TCP terminal conditions,
  network and poll validation failures, resolver statuses, and deterministic
  system-facade validation failures while preserving raw OS error passthrough.
- Added allocation-free `http.get` over neutral caller-owned `Storage` and an
  owning heap-backed `Client` whose workspace buffers are lazily allocated and
  automatically resized, with the conventional explicit `client_new` and
  `client_wipe` lifecycle operations.
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

- Extended global constant emission to carry pointer relocations recursively
  through arrays, structs, repeated elements, aliases, and union payloads, so
  string-bearing aggregates remain static data without runtime initialization.
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
- Fixed homing of a trailing stack-passed scalar parameter to copy only its own
  width instead of the ABI-padded stack-slot width (Darwin pads scalar slots to
  4 bytes), which had let a narrow trailing argument (e.g. `bool`) clobber the
  low bytes of the previously homed adjacent stack argument. This was the root
  cause of the owning HTTPS client's response-dependent heap corruption during
  deferred `client_wipe`.
- Moved `http`'s implementation files (`client.ace`, `format.ace`, `parse.ace`,
  `redirect.ace`, `server.ace`, `transport.ace`) under `std/http/inc/` by
  convention, keeping `std/http/mod.ace` as the sole external surface. Each of
  `client.ace` and `server.ace` now declares its own plain `Context` struct and
  `context_new`/`context_wipe` pair, since directory-module privacy already
  keeps the two from colliding internally; `mod.ace` aliases them as public
  `ClientContext`/`ServerContext` and re-exposes disambiguated constructors
  `client_context_new`/`client_context_wipe` alongside the existing
  `server_context_new`/`server_context_wipe`, keeping the facade uniform.
- Added `tcp.listen_nonblock` and `tcp.accept_nonblock`, mirroring
  `connect_nonblock`, so a listener and its accepted connections can join the
  same nonblocking, `poll`-driven composition as outbound connections.
- Implemented `http.serve`: a caller-driven, nonblocking HTTP/1.1 server core
  (`std/http/server.ace`) built directly on the existing incremental
  `RequestParser` and `Response.format`, following `tcp`'s externally-driven
  connection-progress model rather than owning a blocking loop. `ServerContext`
  binds a nonblocking listener and poll queue and allocates a caller-sized
  array of fixed-capacity `Session` slots (separate input/header/body/output
  buffers per session, no internal resizing); `serve` performs at most one
  `poll.wait` per exhausted batch and drives exactly one ready listener or
  session per call, returning one `Event`
  (`accepted`/`request`/`sent`/`disconnected`/`failed`) — the caller's own loop
  supplies the bound on how long to run. `respond` formats a handler's
  `Response` into the session's output, synthesizing exactly one
  `Connection: close` header and rejecting a handler-supplied one; each
  session closes and recycles once its response is fully sent. A forked-client
  loopback fixture (`tests/emit/http/serve.ace`) proves accept, parse,
  respond, close, and slot reuse across two sequential connections.
- Renamed the server context's per-buffer parameters from `*_cap` to `*_size`
  (`input_size`, `header_size`, `body_size`, `output_size`), matching the
  `_size` naming already used elsewhere (`mem.realloc`'s `new_size`,
  `tls.context_size`/`record_size`) instead of introducing a one-off `_cap`
  spelling.
- Added `server_context_new(endpoint): (ServerContext, Error)`, a low-effort
  overload beside the fully-parameterized form, for a caller with no capacity
  preference. Unlike the client's `Storage`, session capacities are
  deliberately fixed for the context's lifetime rather than grown on demand
  (so one session overflowing never disturbs another live session's buffer
  mid-poll), so the low-effort form supplies fixed built-in defaults instead —
  512 backlog, 128 sessions, 8 KiB input/output, 64 headers, and 256 KiB body
  per session — rather than growing storage. The defaults live in
  underscore-prefixed module constants (`_backlog`, `_session_count`, …)
  rather than uppercase names, following the project's established lowercase
  `const` style (e.g. `tls`'s `closed`, `send_record`); a same-named local
  parameter in the explicit overload ordinarily shadows a same-named constant
  without conflict, so no artificial `default_`-style prefix was needed to
  avoid a collision that was never actually possible.
- Added a complete HTTP server proof of concept
  (`demo/http-complete-server/main.ace`) built on the low-effort
  `server_context_new(endpoint)` overload, and a small `http.get` usage
  example to `docs/standard-library.md`.
- Added a blocking `serve(endpoint, handle): Error` overload alongside the
  nonblocking core: no `ServerContext`, poll queue, session table, or
  caller-visible backlog/capacity tuning, just an endpoint and a
  `(Request): Response` handler. It binds, then loops forever, serially —
  accept, read one request, call `handle`, write the response with a
  synthesized `Connection: close`, close, repeat — using fixed internal
  buffers sized like the low-effort `server_context_new(endpoint)`
  defaults. A per-connection failure just drops that connection; `serve`
  itself only returns if the listener cannot be created. Added a matching
  minimal demo (`demo/http-simple-server/main.ace`).
- Fixed `SPEC.md`'s stale claim that `parse_url` rejects `https://`: the
  parser has accepted both schemes since the TLS-capable client overload was
  added, since `get(url, mut client)` needs `https://` to parse and both
  `get` overloads share one parser (`get(url, mut context)` still rejects
  `https://` before connecting, but through its own scheme-forced
  `parse_url_for(url, false)` call, not through `parse_url` itself). Updated
  the stale test assertion in `tests/emit/http/main.ace` to match.

## Current frontier

The checked-build queue is complete. Additive networking, concurrency, fuller
math/SIMD, and a second
backend remain open; their status belongs in the `SPEC.md` Decision Queue and
`TODO.md`, not in this history.
