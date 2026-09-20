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
- Deferred method calls on a struct receiver that find no method yet to the same
  end-of-file `Pending` fixup plain calls use (`check_method_call`, re-run with
  `final`), so methods on one type may call each other in any declaration order.
- Extended the narrow forward-reference fixup class to a bare slice exactly at
  an aggregate field or union variant root (`[]Later`), alongside the existing
  pointer-chain shape (`*Later`, `**Later`): a slice's fixed pointer-plus-count
  representation is equally independent of its element's size, so the same
  end-of-file placeholder-patch mechanism (`TypeFix`) now also records a fix
  slot for a slice's element type. The allowance does not extend through or
  past the slice itself — a slice reached via a preceding pointer, or nested
  inside another compound, stays ordinary declaration-before-use — while a
  pointer reached via a preceding root slice still forwards on its own
  existing rule. This lets two nominal types reference each other through a
  runtime-sized collection without boxing every element behind a pointer, e.g.
  `Json: union { none, object: []Pair }` followed by
  `Pair: struct { key: string, value: Json }`.
- Implemented release-mode inlining of one-expression scalar functions at
  emission: arguments evaluate once, in order, into temps that the callee's
  parameter identifiers read, so no call frame and no spill and reload of live
  temps around it. A math kernel calling one such helper seven times fell 24%
  (193 to 146 ns); checked builds and all other callees keep the call.
- Made register planning follow that inlining: a function whose calls all
  expand in place is planned as call-free through the one `inline_body`
  predicate emission also uses, counting the expanded body's temps. A call-free
  function promotes up to eight float locals into the caller-saved d24-d31,
  ahead of d8-d15 and never saved, unless it may reach the byte routines that
  clobber v24-v31; a routine reached with such locals live is an internal
  error, never wrong code. `math.log2_split` fell from 39 to 25 ns and
  `math.pow` from 145 to 119 ns; hot-loop, call, and CPU benchmarks did not
  move.

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
- Widened a fixed array type's length (`[N]T`) and an array literal's
  repetition count (`[value : count]`) from a bare integer literal to the
  existing restricted compile-time constant-expression surface already used
  by `@align(N)`: literals, `const` identifiers, `sizeof`/`alignof`/
  `offsetof`, and integer/bitwise arithmetic, resolved with the same `ut_const`
  evaluator. Both positions parse a general expression and defer constant
  resolution and positivity/exact-match checking to sema, which reports a
  non-constant or non-positive length or a mismatched repetition count as a
  compile-time error. Raised while carving `std/http`'s router, where a
  module-level `const usize` could not back a buffer or route-table size.

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
- Extended bare (type-omitting) struct-literal contextual typing to `return`
  statements and ordinary call arguments, matching the existing var-decl,
  assignment, field-default, static-map-entry, and union-payload contexts;
  applies equally to plain and generic struct targets. `coerce` now resolves
  a bare struct literal directly, and overload resolution defers a bare
  struct-literal argument's type to its matched parameter the same way it
  already deferred bare array literals.
- Audited aggregate-ABI classification (`classify_hfa`/`classify_ret`/
  `hfa_leaves`) across the cross-product of struct-in-struct, array-in-struct,
  struct-in-array-in-struct, tagged unions, multi-return tuples, and generic
  struct instances, at HFA, register-boundary, indirect, and
  register-exhausted positions, for ordinary Ace-to-Ace calls
  (`tests/emit/basics/abi_matrix.ace`) and, for the categories legal at the
  boundary, extended the clang-checked bidirectional C-ABI oracle
  (`tests/driver/abi.ace`/`abi.c`) with nested-struct and array-in-struct
  cases. No defect found; the existing recursive classification already
  covers every case correctly.
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
- Implemented Package System v1's manifest and dependency-graph resolution
  (`src/package.c`): a line-based `ace.pkg` format (`key: value` fields, `//`
  comments, one `[dependencies]` section of `name: relative/path` entries)
  and a depth-first resolver mirroring the existing module loader's
  canonical-path dedup and cycle diagnostics. Validates required fields,
  exact `major.minor.patch`/`major.minor` version formats, `app`/`lib` kind
  (only `lib` may be a dependency), a `root` file confined to its package
  directory, dependency aliases that exactly match the target's declared
  name, one source per declared package name, a shared `ace` language
  version across the graph, and no collision with standard-library module
  names. The `--pkg-graph=DIR` flag exposes this stage for testing, printing
  the resolved dependency-first order with no codegen involvement. Local-path
  dependencies only; no git, lockfile, or registry. Cross-package import
  resolution and the `ace pkg` command set remain separate next steps.
- Wired Package System v1 into `module.c`'s import loader: each loaded module
  now carries its owning `Package*` (propagated from the compiled root, or
  NULL for a manifest-free compile), and a bare/aliased/selective import
  checks the importing module's declared dependencies before falling back to
  stdlib resolution. Every relative import from a package-owned module must
  stay within that package's own canonical directory, which both keeps a
  dependency's non-root files from being deep-imported and blocks a
  package's own relative imports from escaping it; a dependency's further
  dependencies stay invisible to consumers unless redeclared (no automatic
  transitivity). The `--pkg-check=DIR` flag exposes this stage: full
  semantic check of a package's root file with cross-package imports
  resolved, no codegen. The real `ace pkg` command set remains a separate
  next step.
- Implemented the real `ace pkg init`/`build`/`run`/`check` CLI as a distinct
  argument-parsing mode in `main.c`, alongside (not replacing) the
  `--pkg-graph`/`--pkg-check` debug flags. `build`/`run`/`check` find the
  nearest enclosing `ace.pkg` by walking up from the working directory
  rather than taking a directory argument. `check` promotes `--pkg-check`'s
  logic verbatim. `build` resolves and fully checks the graph; a `lib`
  package stops there plus ensuring `build/` exists (v1 libraries are
  source-only, with no reusable archive), while an `app` package additionally
  compiles and links into `build/<package-name>`, reusing the direct
  compiler's TLS-recipe and linker machinery and accepting the same
  `--checked`/`--release`/`--target=`/`--tls-backend=`/`--report` flags.
  `run` is app-only and executes `build`'s linked binary in place,
  propagating its exit code. `init` creates a minimal `ace.pkg` plus root
  file (an app gets `main(): int { return 0 }`; a lib gets an empty root
  module) without overwriting an existing manifest or root file, deriving
  the package name from the target directory's basename. Added
  `tests/package_cli/` fixtures covering app/lib builds, cross-package builds,
  `run`'s app-only restriction, and nested-directory `check` discovery.
- Added `author`, `license`, and `repository` manifest fields alongside the
  existing `description` — all free-form single-line optional metadata with
  no format validation and no effect on identity, resolution, or
  compilation. `ace pkg init` now writes all four fields present but empty;
  an empty value for any of the four is treated as absent rather than the
  usual malformed-`key: value` parse error, so a freshly initialized manifest
  still builds unedited. Added `tests/package/metadata_blank` and
  `tests/package/metadata_dup_author` covering the blank-is-absent carve-out
  and that a real duplicate value is still rejected.
- Implemented native link-input declarations (SPEC.md Package System #14,
  `GRAMMAR.md` §7): a repeatable `[[link]]` manifest section, one record per
  occurrence admitting up to four optional keys once each — `object`/
  `search` (relative to the declaring package, no `..` traversal, `object`
  ending in `.o` or `.a`) and bare-name `library`/`framework` — parsed in
  `package.c` alongside the existing `[dependencies]` section (`path_is_
  contained` factored out of `path_is_local_ace` and reused for both).
  `object`/`search` are existence-checked and canonicalized at manifest-load
  time the same way `root` already is; validated entries populate a new
  `Package.links` list (`ace.h`) in file order. `ace pkg build`/`run` (`main.c`)
  collect every package's `links` across `package_resolve`'s dependency-first
  `order` into the direct driver's own flag vocabulary (`object` positional,
  `-L`/`-l`/`-framework`), de-duplicating exact matches and diagnosing a
  same-identity-different-value conflict (basename for `object`, the name
  itself for the other three kinds) as a resolution-time error naming both
  declaring packages — mirroring the existing one-version-per-package-name
  conflict handling rather than merging or renaming. A `lib` package's
  declared inputs stay dormant until some `app` in the graph reaches the
  link step. Added `tests/package/link_*` (parsing and per-package
  validation) and `tests/package_cli/link_framework`/`link_conflict`
  (end-to-end transitive linking through a real `cc` invocation, and the
  conflict diagnostic); `tests/run.sh` gained the `// expect-build-err:`
  marker since checking alone never reaches the link step.
- Implemented recursive nested modules: a subdirectory with its own `mod.ace`
  is itself addressable via a `/`-separated path (bare, aliased, or quoted
  relative without a `.ace` suffix). The parser accepts `ident (/ ident)*` for
  a bare/alias-target module reference, threading the full segment chain
  through the import node (`N_IMPORT.c`) while `str`/`b` keep naming the leaf
  segment, the sole bound qualifier. `module.c`'s import resolution walks
  every non-leaf segment requiring `<dir>/mod.ace` (erroring by name if
  absent — a subdirectory without `mod.ace` stays private, not a nested-module
  segment) before resolving the leaf exactly like today's flat-vs-directory
  stdname lookup; the same walk covers std, package-dependency, and quoted
  relative paths. The existing std directory-module privacy check now finds
  the *nearest* enclosing `mod.ace` rather than assuming one level, so a
  nested module's own implementation files stay private to it rather than to
  whichever outer directory module contains it.
- Moved the blocking TCP composition module from the flat `std/tcp.ace` into
  `std/net/tcp/mod.ace`, its first real use as a nested module: `import
  net/tcp` replaces `import tcp` everywhere it is used.

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
- Zero-initialized every deferred-call record, which read uninitialized arena
  memory under the sanitizer build.
- Rejected `fmt.printf` value arguments whose type is still unresolved when the
  plan is built (calls deferred to end of file), which previously matched the
  first helper specialization and silently dropped output.
- Added `%b` binary and `%o` octal `fmt.printf` conversions for `uint` and
  `usize`, generalizing hexadecimal's private helper into one fixed-buffer
  unsigned-radix path. Prefixes, width, and zero padding remain explicit
  additive work.
- Indexed compile-time name lookup: scopes past 32 names build an
  open-addressing hash over their value and type lists, and emit interns symbols
  through a hash table, removing the quadratic scans that made 20,000 generated
  functions take 12.9 s (now 0.4 s). Added `bench/scaling-corpus.sh`, the
  20,000-function point, and growth columns to `bench/run-compile.sh`.
- Removed the remaining per-function quadratic scans behind long-function
  compile time: a `PtrMap` (open-addressing pointer hash in `lexer.c`) indexes
  sema's definite-initialization variables and emit's frame slots and promotion
  candidates, and select lowering looks a name up among the few selections
  before it searches the locals. A 20,000-local function compiles in 92 ms
  (was 2.35 s) with identical machine code.
- Materialized a float constant with `fmov Vd, #imm8` when it is one of the 256
  encodable values (0.125 to 31.0 in sixteenths of a power of two, either sign),
  and folded the scaled index into a scalar array or slice load as
  `ldr Rt, [base, index, lsl #n]` when the element is 1, 2, 4, or 8 bytes wide
  (`index_addr`; stores and other strides keep the separate add). On a `saxpy`
  loop over `f64` arrays, 1.25 ns fell to 0.97; a branchy `f32` collision loop
  fell 19%, a `u8` blend 9%, and a dot product 5%.
- Corrected the float load and store fallbacks for an offset the scaled `imm12`
  cannot reach (over 32 KiB for `f64`, 16 KiB for `f32`). Both materialized the
  byte offset in `x16` and then emitted the unsigned-offset opcode, where the
  `Rm` field lands inside `imm12`: the load ignored `x16` and read a wrong
  constant displacement instead. They now emit the register-offset form,
  `ldr`/`str Vt, [base, x16]`, matching the sibling integer paths. Reachable
  through a scalar tuple member past that distance.
- Read a global integer `const` with a compile-time initializer as an immediate
  instead of a memory load, emit `and`/`orr`/`eor` with an encodable constant
  operand as one bitmask-immediate instruction, and let call-free functions keep
  every used local and parameter in x9-x14 even when read once, removing the
  argument spill and reload from scalar leaf calls (`call-1` 5.7 to 3.2 ns).
- Returned struct literals and tuples whose leaves are call-free scalars, one
  whole 8-byte integer or float per result register (`gen_return_regs`),
  evaluate straight into `x0`-`x1` or `d0`-`d3` instead of a frame temp that is
  then reloaded; leaf w is computed with temp w free so earlier leaves stay live.
  Other shapes (packed sub-8-byte fields, missing fields, call-bearing leaves,
  sret) keep the temp path. `struct-8` 9.3 to 7.6, `struct-16` 9.8 to 7.5,
  `struct-hfa` 13.7 to 12.3, `tuple-return` 16.4 to 12.9 ns.
- A call whose result returns in registers as whole 8-byte chunks (an HFA, or a
  struct or tuple whose size is a multiple of 8) stores them straight into a
  frame-resident local destination instead of a staged temp plus copy: `x = f()`
  and `x: T = f()` into a plain local slot or a field of one, and typed tuple
  destructuring into whole 8-byte scalar binders (`gen_call_to_slot`,
  `gen_call_to_binders`). A one-shot `call_sink` armed by the statement is
  consumed by `emit_call` only for the matching call node and return type, so
  nested calls in arguments never take it. `struct-8` 7.6 to 6.1, `struct-16`
  7.5 to 5.5, `struct-hfa` 12.3 to 9.8, `tuple-return` 13.0 to 11.6 ns.
- A by-value aggregate argument that is a plain frame-resident local (or a
  field of one) and rides registers is loaded straight from its slot at
  marshal time (`ldur x<n>` / `ldur d<n>`) instead of through an address temp
  that was spilled and reloaded; the calling-convention units are now computed
  before the arguments are evaluated. Slice arguments keep the address temp in
  checked builds for the `noalias` range check. Text of `bench/calls.ace`
  fell 33,984 to 33,712 bytes; the call benchmarks moved within layout noise.
- A call-free function's read-only by-value HFA parameter whose every use is a
  constant read of one float leaf (`p.x`, `r.max.y`) keeps only the leaves it
  reads in v23 downward, copied once from the incoming argument registers, with
  no frame slot (`plan_param_homes`, `param_home`). Any whole-value use, `&`,
  runtime index, or call in the body, or more than eight homed leaves within
  the fp temps the body leaves free (`vtf_cap`), keeps the slot. Float `+ - * /`
  reads a homed operand in place, as `src_reg` does for integers. v24-v31 are
  the byte helpers' scratch, so homes sit at the top of the fp temp pool.
  `struct-hfa` 9.7 to 4.9 ns.
- The same read-only leaf-read rule homes whole 8-byte integer-class leaves of a
  by-value struct of at most 16 bytes that arrives in `x` registers: each read
  leaf is copied once into the x14-down registers leaf promotion leaves unused,
  within the int temps (`vti_cap`), and `src_reg` reads it in place. Packed or
  mixed-class structs and narrower leaves keep the slot. `struct-8` 6.5 to 3.9,
  `struct-16` 5.4 to 2.4 ns.
- Float `+ - * /` reads a promoted float local or a register-homed parameter
  leaf in its own register (`fsrc_reg`, the float twin of `src_reg`; the HFA
  fast path in `gen_binop` now goes through it), and an assignment to a promoted
  float local (`x = a op b`, `x op= y`) emits the final operation straight into
  its register. The old `fmov` copy in and out cost about 1 ns each on a
  loop-carried chain. Call-free sum 3.4 to 1.2, multiply-add 3.4 to 1.3,
  `x = x * 0.5 + 1.0` 4.9 to 2.7 ns; the call-carried `float-acc` workload
  4.8 to 2.4 ns.
- Fixed a multi-register return with a pending `defer` returning a corrupted
  second register: `emit_defers` addressed save-area unit w above the slot
  (`retsave_off + w * 8`), which fell below `sp`, instead of within it
  (`retsave_off - w * 8`), so the deferred call's frame overwrote it.
- Implemented `str` character/string operations and explicit
  `str.parse[T]` specializations in pure Ace where practical, with libc float
  conversion isolated in its selected target layer. `char` remains the
  transparent byte alias and strings remain UTF-8 byte sequences.
- Implemented `sys` file-descriptor I/O, descriptor constants, errno capture,
  command-line arguments, and environment access through thin platform
  bindings.
- Implemented `mem.alloc_size` as a compiler-backed raw `mem` name lowered to
  the platform allocator's size query; the benchmarks use it and `time.awake`
  in place of their private externs.
- Implemented `time.elapsed` and `time.awake` as separate monotonic clocks in
  the `time` module over a private Darwin binding.
- Renamed `json.writer_new` to `json.writer`: a constructor carries the `_new`
  suffix exactly when a matching `_wipe` releases what it returns, and the
  writer is a plain value over a caller-owned buffer.
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
- Added `math.sqrt`, `math.floor`, and `math.ceil` for `f32` and `f64` as
  compiler-provided intrinsics (`BI_MATH_*`, marked in `module.c`, signature-
  checked in `sema.c`) lowering to in-place `fsqrt`, `frintm`, and `frintp` on
  the operand's FP temp, with the same instruction in the callable symbol. The
  intrinsic whitelist grew from `bits` to include these `math` overloads.
- Added pure-Ace `math.exp` and `math.log` for `f32` and `f64`, within 1 ULP over
  the whole finite domain. They use the fdlibm scheme: `exp` reduces by
  `k * ln2` (split high/low) and evaluates a rational polynomial, scaling by
  exact exponent-bit construction in two steps for subnormal results; `log`
  splits the exponent, normalizes the significand into `[sqrt(2)/2, sqrt(2))`,
  and evaluates a polynomial in `s = f / (2 + f)`. The `f32` overloads compute at
  `f64` and round once. Verified against platform libm on 2.2M inputs with none
  beyond 1 ULP; libm is a test reference only.
- Added pure-Ace `math.sin` and `math.cos` for `f32` and `f64`, within 1 ULP
  over the whole finite domain. Small arguments use the fdlibm sine and cosine
  kernels; arguments below about 1.6e6 use three-stage Cody-Waite reduction by
  pi/2; larger arguments use Payne-Hanek reduction: the 53-bit significand
  times eight 32-bit limbs of a 1280-bit 2/pi table (generated with big-integer
  arithmetic) forms a 256-bit fixed-point sum whose truncation error is 2^-170,
  from which the quadrant and two exact 53-bit fraction pieces are taken, then
  multiplied by pi/2 as a double-double (Dekker product, no fused multiply-add).
  Verified against 960K libm comparisons and, independent of libm, against
  exact big-integer arithmetic: 22K random arguments (worst 0.80 ULP) and 2.5K
  continued-fraction worst cases whose reduced fraction reaches 2^-62 (worst
  0.50 ULP).
- Added pure-Ace `math.pow` for `f32` and `f64`, within 1 ULP over the whole
  finite domain and exact for every representable integer power. Composing the
  existing `exp` and `log` reaches hundreds of ULP, so `pow` carries a private
  extended-precision `log2` in the fdlibm scheme: the significand is normalized
  into one of two subintervals, around 1.0 and 1.5, and every step is kept as a
  high part with its low 32 bits cleared plus a tail, so `y * log2(x)` is formed
  from exact products; the `2^p` kernel then reuses `exp`'s polynomial
  coefficients. C99 Annex F special cases are dispatched before the kernels.
  Verified against platform libm on 966K comparisons (main path, negative bases
  with integer exponents, bases within 2^-20 of 1 with exponents past 2^31,
  subnormal results, and 200K `f32` cases, the last bit-identical throughout),
  with every Annex F case and all 736 representable integer powers exact; libm
  is a test reference only. It runs at about 119 ns per call against libm's
  9 ns, where the same algorithm is 33 ns compiled by clang at -O2 and 293 ns
  at -O0.
- Fixed compound untyped constants under a float cast (`f32(-1.0)`,
  `f32(0.0 - 1.0)`, `f32(-3)`, `f32(2 * 3)`), which stored a wrong value or hit
  "untyped operand reached codegen": the cast check now settles the operand
  through `coerce` (float math at the target type, integer math as `i64`).
- Fixed global initializers with a negated float literal (`G: f32 = -2.5`,
  `f32(-2.5)`, struct and array elements), which were rejected as not
  compile-time constants: the constant-initializer folder now folds a negated
  float literal and admits it under a same-family float cast.
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
- Fixed cross-module identifier renaming (`module.c`'s `rename_type_refs`) to
  walk a fixed array type's length expression. It previously recursed only
  into an array type's element (`t->a`), so a module-private `const` used to
  back a `[N]T` array length inside that same imported module's own function
  bodies, struct fields, or parameter/return types kept its bare name instead
  of gaining the module's private prefix, producing a spurious "unknown name".
  Latent since array lengths were always integer literals; surfaced once
  Core language widened array lengths to constant expressions (above).

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
- Removed the caller-driven nonblocking HTTP server core
  (`ServerContext`/`Session`/`Event`, `server_context_new`,
  `server_context_wipe`, `serve(mut context, timeout_ns)`, `respond`) and its
  `demo/http-complete-server` proof of concept: it had been implemented
  directly, without going through the Decision Queue carve process, and the
  owner does not want to keep that design. `std/http` now exposes exactly one
  server convenience, the blocking `serve(endpoint, handle): Error` overload,
  with `demo/http-simple-server` as its sole demo. Trimmed the matching
  fixtures (`tests/emit/http/serve.ace` removed; `tests/emit/http/main.ace`
  and `tests/sema/stdlib/http_ok.ace` no longer exercise the removed surface).
  A fresh carve of how much server mechanism belongs in the language versus a
  framework built on top is tracked in `TODO.md` before anything like the
  nonblocking core returns.
- Implemented `http.Router` (`std/http/inc/router.ace`), the fresh carve for
  the nonblocking-server item above's routing half: a growable route table
  (`transport.mem_adjust`/`mem.release`, the same pattern `Client`'s
  `Context`/`Storage` already uses) with `router_new`/`router_wipe`,
  `mut Router.handle(method, path, handler)`, and `pub Router.dispatch`
  matching on exact method plus segment-boundary longest path-prefix
  (`/home` matches `/home` and `/home/about` but not `/homework`; any
  `?query` on the request target is stripped before matching). Ace has no
  closures or first-class method values, so a stateful `Router` cannot be
  handed to `serve` as a bare `(Request): Response`; instead `serve` gained a
  second overload, `serve(endpoint: string, router: Router): Error`, that
  dispatches through the router directly. `server.ace`'s single-shot
  `serve_one` was split into shared `read_request`/`respond_close` helpers
  reused by both the handler-based and router-based per-connection paths.
  Added `tests/emit/http/router.ace` covering registration, prefix/segment
  matching, method mismatch, query stripping, and the built-in 404 fallback.
- Moved the grow-or-allocate helper formerly named `transport.mem_adjust`
  into `std/mem.ace` as `mem.adjust[T]`: `router.ace` and `client.ace` both
  called it through `transport` for a purely generic slice-growth operation,
  not anything HTTP-specific. `transport.ace`, `router.ace`, and `client.ace`
  now call `mem.adjust` directly; `router.ace` no longer imports `transport`.
- Renamed module-level `const` declarations from a leading-underscore
  lowercase spelling to all uppercase (`_backlog` → `BACKLOG`, `_input_size` →
  `INPUT_SIZE`, and the rest of `server.ace`'s session defaults), replacing
  the project's earlier lowercase `const` convention.
- Collapsed `server.ace`'s two `serve` overloads back to one,
  `serve(endpoint: string, mut context: Context, router: Router): Error`,
  removing the bare-`handle` overload now that `Router` is the sole
  dispatch surface. Added `server.Context` (`input`/`headers`/`body`/`output`,
  each starting empty, plus a `backlog` field defaulting to 512) with
  `context_new`/`context_wipe`, replacing `server.ace`'s fixed
  `BACKLOG`/`INPUT_SIZE`/`MAX_HEADERS`/`BODY_SIZE`/`OUTPUT_SIZE` module
  constants; `read_request` and `respond_close` now grow each buffer through
  `mem.adjust` off the same capacity signals the parser and formatter
  already reported (`RequestResult.capacity`, `FormatErr.capacity`, and the
  `input` buffer filling before a request completes), with no size cap.
  Growing `input` resets and reparses the retained bytes, mirroring
  `transport.exchange`'s existing handling of a moving reallocation, since
  the parser's header/target views point into it. `backlog` is the one
  non-growing field, read once by `listen_endpoint` when `serve` binds and
  never touched by `mem.adjust`. `demo/http-simple-server` and `http.mod.ace`
  (`ServerContext`, `server_context_new`/`server_context_wipe`) were updated
  to match.
- Renamed `std/http/inc`'s `format.ace`, `parse.ace`, and `redirect.ace` to
  `message.ace`, `parsing.ace`, and `redirection.ace` so every file in the
  directory is a noun, matching its own import alias exactly; `parsing` and
  `message` were already used at every import site, and the `redirect` alias
  at its one call site in `client.ace` was renamed to `redirection` to match.
- Added a minimal `http.get` client demo (`demo/http-simple-client/main.ace`):
  one `ClientContext`-backed GET against a fixed URL, and the response
  status/body printed with no redirects or TLS.
- Removed TLS entirely: `std/tls.ace`, `http`'s HTTPS overloads
  (`get(url, mut client)`, `get_redirecting`'s TLS overload) and its `Client`/
  `TlsConfig` types, the Darwin BearSSL adapter and vendored sources
  (`vendor/bearssl/`), the distributed `external/` link artifacts, and every
  TLS-only test and driver fixture. `http` is now plaintext-only; `Storage`
  no longer carries `tls_context`/`tls_records`. Removed the compiler-owned
  `@extern("component")` attribute and its supporting mechanism along with
  it, since TLS was its only consumer: the `Node.extern_component` field, its
  parsing (`@extern("tls")`), the sema reachability walk
  (`extern_requirements`) that conditionally omitted component-carrying
  functions from emission, the emit-time gating, the `--tls-backend` CLI flag
  and pkg-build equivalent, and the `(component, target, backend)` link-recipe
  table in `main.c` (`ExternRecipe`, `TLS_BEARSSL_DARWIN_ARM64`) that
  resolved and appended bundled TLS link inputs. `-c`/`-o` and `ace pkg
  build`/`run` no longer have any external-component linking to perform.
  This was a deliberate reset, not a deprecation: the prior TLS approach is
  discarded outright rather than migrated, and its Decision Queue entry and
  `TODO.md` survey now record a fresh carve, unconstrained by the removed
  backend, adapter shape, or `@extern` mechanism.
- Removed `client.ace`'s `redirect_next` `secure_transport` parameter and
  `current_secure`/`RedirectErr.downgrade` branch, dead since the TLS removal
  above left the plaintext client as `redirect_next`'s only caller, always
  passing a constant `secure_transport = false`: the current URL can never be
  HTTPS, so the downgrade branch could no longer trigger. The remaining
  HTTPS-redirect-target rejection (`UrlErr.invalid`) is now unconditional.
  Removed `RedirectErr.downgrade` from `redirection.ace`'s error domain, now
  unreachable.
- Relocated `http` out of `std` to the standalone package `pkg/http`
  (`ace.pkg`: `kind: lib`, `root: mod.ace`), independent of the separate
  TLS/HTTPS std-vs-package question, since TLS's removal above left `http`
  plaintext-only and pure Ace with no compiler-owned mechanism tying it to
  the `std` tree. `pkg/http` is Package System v1's first real payload,
  proving its local-path dependency mechanism end to end. `demo/http-simple-
  client` and `demo/http-simple-server` became `app` packages (`ace.pkg`
  each declaring `http: ../../pkg/http`) since a bare `import http` no longer
  resolves under plain manifest-free compilation; `docs/std/http.md` moved to
  `docs/http.md`.
- Gave `pkg/http` its own behavioral test suite, decoupled from the core
  compiler suite: `pkg/http/tests/` holds 7 cases (`framing`, `client`,
  `redirect`, `request`, `router`, `plaintext_only`, `api_surface`), one
  directory per case, each an `app` package depending on the sibling `http`
  package (`http: ../..`), matching `tests/package_cli`'s convention —
  `// expect-exit: N`/`// expect-check-ok` markers as `ace.pkg` comments, run
  by the new `pkg/http/tests/run.sh`. `make test-packages` runs every
  `pkg/*/tests/run.sh`, kept separate from `make test`'s `tests/run.sh`:
  package content tests shouldn't gate core compiler iteration. Content
  carried over unchanged from the fixtures the relocation above left broken
  (`tests/emit/http`'s 5 fixtures, `tests/sema/stdlib/http_ok.ace`,
  `tests/driver/http_plain_autolink.ace`), which are removed from the core
  suite now that they're superseded; `tests/run.sh`'s corresponding
  `http_plain_autolink.ace` driver block is removed. `http_plain_autolink`'s
  duplicate "standalone object" invocation (recompiling the same file with
  `-c` alone) is dropped rather than ported: it existed to check that a
  plaintext build didn't drag in TLS link inputs at the object level, and
  with `@extern(component)` removed entirely there is nothing left to
  isolate from; the case's behavioral half (https scheme rejection, DNS-
  failure propagation) is preserved as `plaintext_only`.
- Removed `ace pkg build`/`run`'s stale `--tls-backend=NAME` mention from
  `package_usage()`'s printed help: the flag itself was already removed with
  `@extern(component)`, so the argument parser has rejected it as unknown
  since then, but the help text kept advertising it.
- Implemented `ace pkg add NAME PATH`, `ace pkg remove NAME`, and
  `ace pkg list` (SPEC.md Decision Queue, "Package system v1"), completing
  v1's local-path-only manifest-editing surface. `add`/`remove` are line-
  oriented text surgery in `package.c` (`package_manifest_add_dep`/
  `package_manifest_remove_dep`) mirroring `parse_manifest`'s own comment/
  section/key scan: `add` inserts a `NAME: PATH` line right after the
  `[dependencies]` header (creating the section, preceded by exactly one
  blank line normalized from however many trailing blank lines the file
  already had, if absent); `remove` deletes the one matching entry line and,
  if it was the section's last entry, the header line too, along with a
  blank separator line immediately before it — keeping repeated add/remove
  cycles byte-for-byte idempotent. Neither function validates the graph
  itself: `main.c`'s `package_cmd_add`/`package_cmd_remove` back up the
  manifest's original bytes first, perform the edit, then re-run the same
  `package_resolve` the other subcommands already use, restoring the backup
  verbatim and reporting the failing package_resolve diagnostic if the edit
  produced an invalid graph (duplicate name, alias not matching the target's
  declared name, stdlib-name collision, cycle, mismatched `ace` version) — no
  separate validation logic, and a failed `add`/`remove` never touches the
  file. `list` is read-only, resolving and printing the graph's name/kind/
  direct-or-transitive/directory. Added `tests/package_cli/pkg_add_cross_dep`
  (`add` then `pkg build`/`run` prove a real cross-package import),
  `pkg_add_err_rollback` (mismatched alias, manifest unchanged),
  `pkg_remove_dep` (add then remove round-trips to the original bytes),
  `pkg_remove_err_missing`, and `pkg_list_transitive` (three-package chain
  distinguishing direct from transitive); `tests/run.sh` gained the
  `pkg-add-args`/`expect-add-err`/`expect-remove-ok`/`expect-remove-err`/
  `expect-list-has` markers, with `pkg-add-args` cases auto-cleaned by a
  trailing `pkg remove` so the checked-in fixture manifest stays unmutated
  across repeated runs.
- Fixed `switch` dispatch (`emit_union_switch` and the scalar `N_SWITCH` case,
  both in `emit.c`) falling through into whichever case was emitted first when
  the scrutinee matched no explicit pattern and `else` was not the last case,
  instead of running `else`. For a union switch this ran the wrong variant's
  case body against a mismatched payload, misreading raw bytes (e.g. an `f64`)
  as a slice descriptor or other pointer-bearing binder; this was the actual
  cause of both the growable-array-of-unions corruption and the non-tail
  recursive-call-with-union-parameter crash recorded in `TODO.md`, which
  turned out to be a single defect rather than two. Both switch forms now
  branch unconditionally to `else`'s label (or past the switch with no
  `else`) after the pattern comparisons, never relying on fallthrough. Added
  `tests/emit/control_flow/switch_else_position.ace` and
  `tests/emit/unions/else_not_last.ace`.

- Fixed a root import alias (`import name: "./path.ace"`) being declared into
  the single flat scope semantic checking used for the entire merged
  compilation unit, so it collided with any same-named local anywhere else in
  the unit, including inside `std` modules the importing package never
  directly names. Root import aliases now declare into their own `Scope`,
  parented to that shared scope, spliced in only while checking the root
  file's own function bodies, global-variable initializers, and top-level
  statements; declarations of every kind still land in the one shared scope
  as before, so cross-file name resolution is unaffected. Added
  `tests/modules/import_alias_scope`.

- Generic function stamping now checks each body under the scope it was
  declared in: `Sym.decl_scope` records the root file's import-alias scope for
  root-declared generics and the shared file scope for every other generic, and
  both `instantiate_generic_func_args` and `sema_stamp_body` use it instead of
  the caller's scope or the bare shared scope. Added
  `tests/modules/import_alias_generic`.

- Added the standalone package `pkg/json` (`kind: lib`, `root: mod.ace`): a
  whole-document RFC 8259 parser. `json.parse(text): (Json, Error)` builds a
  `Json` union (`none`, `bool`, `number: f64`, `string`, `array: []Json`,
  `object: []Pair`) with heap strings and backing storage, reports failures
  through the `JsonErr` domain, and enforces a limit of 512 nested
  containers (`MAX_DEPTH`). Leading-zero numbers, unpaired `\u` surrogates, unescaped
  control bytes, and trailing data are rejected. `json.wipe` recursively releases every
  allocation, keys included. The parser walk is `Parser` methods, with
  `parse_value` and `parse_object`/`parse_array` calling each other; only
  helpers that ignore parser state stay free functions. `\uXXXX` decoding uses
  a private `encode_utf8`. `pkg/json/tests/basics` runs under
  `make test-packages`, `demo/json-parse-file` parses a file and prints every
  key and value, and `docs/json.md` documents the package.
- Renamed the `ace.pkg` manifest field `kind` to `type` and its value `app` to
  `exe` (`lib` unchanged). The parser reads keys as plain text, so `type` is
  not affected by the reserved keyword. `kind:` and `type: app` are now
  errors with no alias; `ace pkg init`, `list`, and `--pkg-graph` use `exe`.
- Renamed `sys.FileKind` and `net.SocketKind` to `sys.FileType` and
  `net.SocketType`, matching POSIX file/socket-type vocabulary. Because `type`
  is a keyword, the `FileInfo` field, the `socket` parameter, and the
  `read_dir` local are `file_type`/`socket_type`, and the `ResolveErr` member
  `socket_kind` is `socket_type`. The remaining Ace-side `kind` names follow:
  the private float `decimal.Kind` is `decimal.Type` with `number_type` fields,
  and the `pkg/http` capacity parameter is `capacity_type`. The C compiler
  keeps its internal `kind` names, since `Sym` already has a `type` member.
- Renamed the `switch` catch-all case from `else` to `default` and required it
  to be the last case. `default` is a new keyword and `F_ELSE` is now
  `F_DEFAULT`. One sema check (`check_default_last`) reports a case after
  `default`, which also covers a repeated `default`, replacing the old
  duplicate-`else` check. `else` as a case is a parse error naming `default`.
  The two dispatch regression tests now put `default` last and live as
  `switch_default_dispatch.ace` and `default_dispatch.ace`; new error fixtures
  cover both diagnostics. Every switch arm in `std`, `pkg`, `docs`, tests, and
  the VS Code grammar was migrated.
- The `pkg/http` request and response parsers now validate a header's framing
  (`Content-Length`, `Transfer-Encoding`) before checking header-slot capacity,
  so a framing conflict reports `framing` even on a full header buffer. State
  changes follow the capacity check, so a `capacity(headers)` retry is clean.
- Added a streaming writer to `pkg/json` (`inc/writing.ace`): `json.Writer`
  emits compact RFC 8259 text into caller storage with no allocation or tree.
  `object`/`array`/`end` open and close containers (`end` picks `}` or `]`
  from a one-bit-per-level kind set sized to the parser's 512-level limit),
  `key` plus `value`/`none` or the `field` shorthand write members, and
  `value(Json)` walks a parsed tree, which `json.format(value, output)`
  wraps. The first failure sticks and later calls are no-ops; `finish`
  returns the byte count or a `json.FormatErr` (`capacity`, `depth`,
  non-finite `number`, `structure`). Numbers reuse `str.format`; strings
  escape `"`, `\` and control bytes and pass other bytes through.
  `pkg/json/tests/writing` covers output, round trip, and every error.
- Added the allocation-free `utf8` module (`decode`, `encode`, `width`,
  `is_valid`) over plain `u32` code points with strict RFC 3629 validation;
  a failed decode returns width 1 so loops always advance. `pkg/json` now
  uses `utf8.encode` for `\uXXXX` escapes instead of a private encoder.
  Dropped the planned `rune` and `yarn` types: a transparent `u32` alias adds
  no behavior, and a code-point sequence costs 4x storage for indexing that
  code points do not make meaningful.
- Comparing two untyped constants (`3 == 3`, `2 + 3 > 4`) now yields `bool`;
  both operands materialize at their natural type (`int` or `f64`) through
  `coerce`. A comparison chain unifies on its first typed operand and also
  materializes untyped float operands, so `x < 2.0` checks for `x: f64`.
  `bool_const` evaluates chains pairwise, so `static_assert` accepts ordering
  comparisons. Fixed the `f64` `fcmp` encoding, which carried a stray `Rm` bit
  and read the wrong register when the right operand sat in an even temp ≥ 2.
- Added `@file` and `@line` (Errors #16): the parser lowers them to a string
  literal and an untyped integer literal for the location where they are
  written. Paths are build-relative (`std/...`, or relative to the entry
  file's directory or root package, with `..` outside it) via one
  `build_path` helper, which `assert` diagnostics now use too, replacing the
  absolute paths they embedded for imported modules. Closed the package v1
  Decision Queue item, whose slice is fully implemented.
- Float `<`, `<=`, `>`, and `>=` are now false when either operand is NaN:
  `tok_cond` gives floats `LO`/`LS`/`GT`/`GE`, which all fail on an
  unordered `fcmp`, instead of the unsigned `HI`/`HS` that held there.
- The loader's module-prefix walk (`rename_refs`) now visits struct field
  defaults and field/struct `@align` expressions, and selected-import lowering
  visits the `@align` slots it skipped. Previously such an expression naming
  its own module's enum or `const` stayed unprefixed, so the struct failed to
  resolve once imported.
- Brought `pkg/http`, `pkg/json`, their tests, and demos under the
  `defer`-after-acquisition discipline. `transport.get_with_storage` defers
  the connection close instead of `exchange` closing a passed `raw` handle on
  every exit; `server.serve_one` now accepts and defers its own connection
  close, and `serve` defers the listener close.
- `tcp.Listener` and `tcp.Conn` gained an `active` flag and canonical-zero
  field defaults, making `Listener {}`/`Conn {}` the empty handle rather than
  a live-looking descriptor zero; `close` on an inactive handle is a
  syscall-free no-op returning null, so `defer tcp.close` precedes the error
  check like `mem.release`. `pkg/http` now spells its empty handles that way.
- Range `for` loops test their 64-bit remaining trip count with a 64-bit
  `cbz`; the 32-bit test skipped or truncated ranges whose count reached 2^32.
- `mem.copy`, `move`, `fill`, and `zero` call shared per-object routines
  (`__ace$mem_copy`/`move`/`fill`) instead of emitting byte loops. A private
  convention (x15 destination, x16 source or byte, x17 count; clobbers only
  x8, x15-x17, x30, v24-v31) keeps call sites spill-free. Ranges up to 64
  bytes use overlapping NEON chunks loaded before any store, which also makes
  them overlap-safe; larger ranges use 32-byte loops with the far-end chunk
  preloaded, and forward runs align the destination to 32 bytes. Release
  4096-byte copy fell from 1995 to 82 ns (1.10x libSystem).
- `mem.compare` and `mem.equal` share `__ace$mem_compare` (x15/x16 operands,
  x17 common length in, -1/0/1 out) instead of byte loops. NEON `eor`/`umaxp`
  finds a differing 16- or 32-byte chunk, and byte-reversed 64-bit words order
  its first differing byte; lengths below 16 load overlapping or first/middle/
  last chunks into vector lanes. `__text` is now 64-byte aligned and helper
  loop heads are padded to 16 bytes, since an unaligned copy loop measured
  135 ns instead of 82. Release 4096-byte compare fell from 2976 to 107 ns
  (0.67x libSystem `memcmp`).
- Integer `/`, `%`, `/=`, and `%=` by a literal or named-constant divisor
  lower without `udiv`/`sdiv` (`emit_const_divide`): powers of two become
  shifts, masks, or a biased signed shift, and other divisors a
  `umulh`/`smulh` (64-bit) or `umull`/`smull` (narrower) reciprocal multiply
  with libdivide's Granlund-Montgomery magic generator, computed in portable
  C. Constant nonzero divisors other than signed -1 need no checked guards.
  The generator was validated against hardware division on 30.9M cases. The
  CPU recurrence fell from about 45.5 to 42.5 ms in release (C `-O2` 34 ms).
- Function entries and source `for` loop heads are padded with `nop` to
  16-byte boundaries (`align_code`), so identical code no longer changes speed
  with its offset in a fetch block. Entry padding never executes; loop-head
  padding runs once per loop entry. Current benchmarks are unchanged within
  noise, since loop bodies are still too large for head placement to
  dominate; `__text` grows about 0.4%.
- Runtime repeat initializers `[value : count]` no longer emit one copy per
  element. A constant whose encoding repeats one byte, or a runtime one-byte
  integer, fills the array through `__ace$mem_fill`; other values store element
  0 and double the filled prefix through disjoint `__ace$mem_copy` calls, so
  code grows with log2(count). `bench/string.ace` `__text` fell from 358 KB to
  106 KB and its `main` from about 66,000 to 3,400 instructions.
- Aggregate copies above 32 bytes call `__ace$mem_copy` instead of an 8-byte
  word loop, with operand moves ordered so no source register is overwritten
  first; up to 32 bytes stays inline. `StaticMap` string-key checks call
  `__ace$mem_compare` instead of a byte loop. Release copies of 48-byte
  sequence elements fell about 22%, 256-byte array assignment about 50%, and
  48-byte struct returns about 8%.
- Scalar locals are read and written directly in their frame slots: one
  `ldur`/`stur` (including `ldursb`/`ldursh` and FP `s`/`d` forms) within
  x29 - 256 instead of an address temp, load, and `mov`; `mut` slots load the
  pointer and then the value into one register. Register parameters are homed
  the same way. Call spill slots are allocated per temp index on first use
  rather than as a fixed 240-byte area, so later locals and range-loop state
  stay within direct reach. The CPU recurrence loop fell from 50 to 37
  instructions per iteration and 42.5 to 39.5 ms in release (C `-O2` 33.9 ms).
- Float negation and float register moves encode their source in `Rn`
  (bits 5-9) rather than `Rm`. `fneg` previously always negated v0, so
  `a * -b` on floats miscompiled whenever `-b` sat in a later temp; `fmov`'s
  sole caller only ever moved v0 to itself, so it never misbehaved.
- Untyped constant expressions beside a typed operand (`b != 1 + 2`,
  `b + (1 << 3)`, `-(1 + 2)`) now type every node through `coerce`, which
  also recurses into unary `-`/`~`; previously only the top node was typed and
  codegen rejected its untyped children. Unary `-` accepts untyped floats, so
  `-0.5` is valid; a negated literal keeps the plain-literal path, so its
  magnitude is judged through the negation (`-9223372036854775808` for i64).
  A compound untyped integer beside a float operand is rejected with the
  declaration's explicit-cast error rather than computed in float.
- Compiler loop state lives in callee-saved registers x19-x28: a range loop's
  cursor, count, and step, and a sequence loop's cursor and count (plus its
  index binder). A statement-only pre-pass sizes each loop nest and grants
  registers innermost-first while the nest fits the ten-register pool; outer
  loops that do not fit keep frame slots. Used registers are saved right after
  the prologue and restored in the epilogue. Functions containing `asm` get no
  loop registers. String `find-4096` fell from 9.2x to 5.8x C and
  `compare-4096` from 0.85x to 0.70x. The CPU recurrence regressed from 36.1
  to 41.7 ms although its loop shrank from 37 to 30 instructions: with loop
  state gone, `state`'s store-to-next-load forwarding is the loop's only
  memory traffic and costs about 3.7 cycles per iteration on Apple cores;
  promoting `state` itself removes it.
- Integer-class scalar locals and by-value parameters whose address never
  escapes live in the callee-saved registers loop state leaves free. The
  loop pre-pass also walks expressions, weights each use 8^loop-depth
  (straight-line uses weigh nothing, so loop-free functions save no extra
  registers), and marks escapes: `&x`, volatile access, `mut` arguments,
  field/index/slice bases, tuple assignment targets, and sequence iterables.
  Writes normalize narrow values exactly as a slot store and reload would;
  taking a promoted local's address is an internal compiler error. The CPU
  recurrence fell from 41.7 to 36.0 ms (C `-O2` 34.2 ms).
- Float locals and by-value float parameters are promoted the same way into
  d8-d15, a separate eight-register pool ranked by the same weights; `f32`
  values live in the low half. The pool has its own save area, saved with
  `stp`/`ldp d` pairs. Compiler runtime routines use only v24-v31, so the
  registers survive their calls. No current benchmark has a hot float loop;
  string timings are unchanged.
- Loop binders are promotion candidates: a range binder, a sequence index
  binder, and a sequence value binder (not an `in mut` alias) take each
  iteration's value in their own register — a width-normalizing move from
  the cursor, or a direct element load — instead of a slot store and reload
  per use. They stay per-iteration copies, so assigning one never moves the
  cursor. The CPU loop fell from 31 to 30 instructions with no memory
  traffic (time unchanged: `i` is off the recurrence); `find-4096` improved
  about 1.5%.
- `if`, general `for`, and checked `assert` conditions branch on the flags:
  `==`/`!=` and ordering chains on scalars emit `cmp`/`fcmp` + `b.cond`
  (condition-code inversion is exact on unordered floats), `!` swaps the
  branch sense, and `&&`/`||` chain branches without materializing a bool;
  other conditions still test a bool with `cbz`/`cbnz`. The string
  benchmark's code fell from 21,951 to 20,963 instructions (`cset` 436 to
  37) and `find-4096` about 3%.
- Loads land in their destination: a scalar load through an address temp
  names that temp as its own destination instead of loading a new temp and
  moving it back. Scalar fields (through struct-field chains), `.len`, and the
  indexed data pointer of a plain local slot load directly at `x29 - depth`,
  and scalar members of tuple, struct, and array literals store there; deep
  slots keep the computed-address fallback, and by-reference slots and
  pointer bases keep their address loads. The string benchmark's code fell
  from 20,963 to 20,211 instructions and `str.index_from`'s inner loop from
  20 to 16.
- Promoted integer locals are operands in their own registers: arithmetic,
  compound assignment, comparisons (including chains and bool condition
  tests), and 64-bit indexes read x19-x28 directly through `src_reg` instead
  of copying into a temp, and write results to a temp, so a promoted local
  never changes by accident. Only those audited read-only consumers borrow;
  narrower indexes, which widen in place, and every other consumer still
  copy. The arithmetic emitters take physical registers so the destination
  can differ from a borrowed source. Static copies in the string benchmark
  fell from 715 to 156 and its code from 20,211 to 19,623 instructions;
  the CPU recurrence fell from 37.0 to 36.0 ms.
- `str` searches reject a start position past the last possible match before
  iterating: a range whose first endpoint exceeds its second runs downward,
  so `split` with a multi-byte separator ending the text read past the end
  (a checked bounds abort).
- `str` search is word-at-a-time: byte search tests eight bytes per
  `@byte_load` load with the exact carry-free zero-byte test
  `~(((x & 0x7F…) + 0x7F…) | x | 0x7F…)` and `bits.trail_zeros`; substring
  search loads the words at each start and at start + len - 1, keeps
  positions matching both the first and last needle byte, and confirms each
  candidate with `mem.equal`. `find-4096` fell from 9,860 to 1,450 ns
  (0.73x C `strstr`).
- Straight-line uses count toward promotion: each weighs 1 (loop uses keep
  8^depth), and a local needs a total weight of at least 2, so a single
  straight-line use, which costs about what a register save and restore
  would, keeps its slot. Loop-free arithmetic such as the float parser's
  Eisel-Lemire step no longer stores and reloads every local. `parse-f64`
  fell from 141 to 101 ns and `format-f64-shortest` from 367 to 313 ns;
  weighting a single use as enough measured 301 ns on format with no parse
  difference, but saved registers in one-use functions.
- Float parsing tries a Clinger tier first: a coefficient of at most 2^53
  with |exponent10| <= 22 (`f64`), or 2^24 with <= 10 (`f32`), is one
  multiply or divide by an exact power-of-ten table at the requested width,
  assuming round-to-nearest. The scanner and Eisel-Lemire step use
  `bits.trail_zeros`/`lead_zeros` instead of software cascades. `parse-f64`
  fell from 100 to 87 ns (3.2x C `strtod`).
- Call-free functions promote into caller-saved x14 down to x9 before
  x19-x28, needing no save. The promotion scan marks any call that is not an
  inline form (cast, constructor, compile-time builtin, assert/panic, `bits`,
  `@byte_load`, or `mem` byte operation, whose helpers clobber only x8 and
  x15-x17); `emit_call` rejects a call in such a function. An operand-count
  temp estimate plus a margin of 3 (largest shortfall seen: 2) caps the temps
  below the promoted registers, with `tmp_int`'s cap as backstop. Union
  payload binders no longer use x14 as scratch. `parse-f64` fell from 87 to
  61 ns (2.2x C `strtod`), `format-f64-shortest` from 314 to 303 ns, and
  string-bench code by 1.3%; promoting `text.len` measured no gain.
- The exact-small float tier declines exponents past 19, where any nonzero
  coefficient overflows u64, instead of multiplying until overflow; zero goes
  to the Eisel-Lemire tier's zero check. Parsing `0e9999` fell from 14,429 to
  41 ns and `parse-f64` from 61 to 60 ns.
- Integer constants 0..4095 become add/sub/compare immediates in binary
  expressions, branches, the last operand of a comparison chain, and compound
  assignment; checked 32/64-bit forms keep the flag-based overflow branch,
  while checked narrow `+`/`-` keep the register path and its range check.
  A promoted 32/64-bit integer local takes compound results (except `/`, `%`),
  constants (masked to its width), and same-width `a op b` results directly
  in its register instead of through a temp. `const_int` no longer folds an
  error-domain constructor, which packs its domain id above the code.
  `find-4096` fell from 1,456 to 1,300 ns, `parse-f64-long` from 79 to 75 ns,
  and `format-f64-shortest` from 303 to 297 ns; CPU was unchanged.
- `emit_call` places a scalar or by-address argument whose temp already is its
  argument register without a spill or reload, and spills only argument temps
  not placed that way (temps below the call's entry depth were spilled at
  entry). A scalar result moves from x0/v0 into its temp's register before
  `reload_temps` instead of round-tripping a slot. An unguarded constant index
  (at most 4095 bytes of offset) is one add or none, and a plain store to a
  scalar field of a frame-resident local struct is a direct `stur`.
  `parse-f64` fell from 59 to 52 ns (1.9x C `strtod`), `format-f64-shortest`
  from 296 to 262 ns, and string-bench code by 5.3%; CPU was unchanged.

## Current frontier

The checked-build queue is complete. Additive networking, concurrency, fuller
math/SIMD, and a second
backend remain open; their status belongs in the `SPEC.md` Decision Queue and
`TODO.md`, not in this history.
