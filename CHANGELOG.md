# Ace — Implementation Changelog

Historical record of the bootstrap compiler's implementation: what was built, changed, and fixed, in the order the work was recorded. Chunk labels are preserved from the original log. This file is history only: normative language rules and decision records (rule + rationale) live in `spec.md`, and current stage plus open work are summarized in `AGENTS.md`. It never overrides the spec.

## Front end (C11 bootstrap)

### Lexer (`src/lexer.c`)

Go-style newline rule (newline token only after ident/literal/`)`/`]`/`}`/`return`/`break`/`continue`), `//` and `/* */` comments, hex/binary/underscore int literals, float literals with mandatory `.` before exponents, char/string literals with `\xNN` escapes (UTF-8 pass-through), full operator set incl. compound assignment, `..`/`...`, friendly errors (multi-error recovery, no-semicolon message).

### Parser (`src/parser.c`)

Full carved surface → AST, dump via `--ast`: declarations (struct/contract/enum/alias/distinct/signs/extern incl. variadics, generics `[T]`), typed destructuring (`n: int, err: Error = read()`), compound assignment (accepted, desugars at typecheck), comparison chains as one node, C precedence incl. shifts/bitwise, labeled blocks, asm bindings (in/out/clobbers, immediates), struct-literal suppression at block heads (Go-style), bare comma lists restricted to one physical line and delimited lists supporting multiline trailing commas, and brace-headed unknown declaration types diagnosed at their type name.

### Typecheck (`src/sema.c`)

Single-pass semantic walk in declaration order: two-namespace scopes (types/values), declaration-before-use with forward top-level calls resolved at EOF (fixup semantics), full type computation + layout for every type (size/align; enum=i32, contract=16B), untyped literals with range checks, explicit casts only, exact free-function and type-owned method overload sets, separated mut discipline (parameter `mut` controls caller storage; `mut *T` controls one pointee level; `mut &value` constructs writable pointers), canonical multi-contract signings + explicit contract construction/dispatch, generic stamping per explicit instantiation (`bigger[int]` — body checked once per type tuple), destructuring vs multi-return, `sizeof` builtin, and definite initialization before local reads.

## Emitter groundwork

### Pre-emitter decisions

Recorded in spec.md: `main(): int`; Mach-O objects + system linker (`cc` driver); minimal DWARF line tables.

### Mach-O format pinning

Pinned against clang reference objects (otool dumps): one nameless segment holding all sections, `r_length=2` for BRANCH26/PAGE21/PAGEOFF12 pairs (PAGEOFF12 pcrel=0), UNSIGNED `r_length=3`, locals/extdefs/undefs symbol order, section addrs continuous from 0; full AArch64 encoder set verified against `as` disassembly.

### Sema annotations for codegen

`Node.ty`/`Node.sym` (SType*/Sym* via `src/sema.h`) — expression types on every node (literals materialize at coercion), resolved symbols on idents/call targets/binders, enum-member indices, `sizeof` constant, forward-call syms at EOF, `sema_stamp_body()` for per-stamp re-annotation, mut args must be lvalues. All existing tests green.

## Implementation log

The direct AArch64 emitter (`src/emit.c`) and the standard library (`mem`, `sys`, `fmt`, `math`, `str`, `simd`) are the implementation surface. Core language codegen and source-level debugging work; checked diagnostics and parts of the stdlib remain.

### Chunk 4 — Mach-O writer + initial codegen

One nameless `LC_SEGMENT_64`, `__TEXT,__text`, relocatable `__DATA_CONST,__const`, `__DATA,__data`/`__bss`, symbol/relocation tables, `_main`, constant loading, internal-call fixups, and `-c`/`-o` driver modes. **Landmark one: hello world compiled and run** (`tests/emit/hello_min.ace` → exit 42).

### Chunk 5 — Full carved-surface codegen

Integer/floating expressions, casts, comparisons and short-circuiting, calls and internal CC (mut pointers, register aggregates, sret), locals/globals, multi-return, arrays/structs/slices, range and sequence loops, switch, labeled exits, generic stamps, contract vtables/dispatch, extern calls, and initial asm (`nop`/`svc`/`brk`). Covered by focused `tests/emit` executables; the normal and ASan/UBSan suites pass.

### Chunk 6 — Minimal DWARF line tables

DWARF v4 `__debug_line` rows for function and statement PCs, plus the minimal compile-unit/function discovery metadata Darwin needs to retain them through linking. Local relocations track final text addresses; `dwarfdump`, `dsymutil`, and LLDB resolve `hello_min.ace` source. No variable/type DIEs or unwind data.

### Chunk 7 — First stdlib output slice (`fmt.print(string)`)

`std/fmt.ace` exports allocation-free `fmt.print(string): int`: exact bytes, no implicit newline, bytes-written/`-1` result (owner approved). Bare `import fmt` is resolved from beside the compiler and merged ahead of the root module under a private prefix; multi-file DWARF rows retain the correct source file. `tests/emit/welcome.ace` compiles and prints exactly `Welcome Ace!\n`. This exposed and fixed two-word aggregate call-address clobbering and reversed incoming aggregate frame layout.

### Chunk 8 — Exact free-function overloading

Same-name functions form an overload set when parameter count/types or `mut` positions differ; return types and `noalias` do not distinguish declarations. Calls select one exact candidate statically with no conversions/ranking and zero runtime dispatch; overload literals use `int`/`f64`, generic overloads remain explicitly instantiated, explicitly typed function values can select an overload, forward calls retain their lexical scope, and internal symbols include the parameter signature. C-boundary overloads are rejected. Type-owned method overloading and contract participation landed in chunk 12. Normal and sanitizer suites pass.

### Chunk 9 — Integer output overload (`fmt.print(int)`)

`fmt.print(int): int` formats decimal into a fixed 20-byte stack buffer, allocates nothing, adds no newline, retries partial writes, returns bytes written/non-positive OS results, and handles zero, negatives, `INT_MAX`, and `INT_MIN`. The narrow `fmt` loader now lowers the complete public `print` overload set. This slice exposed and fixed AArch64 `%`: quotient generation now preserves the original dividend for `msub`; focused division/remainder and output executables cover it. Normal and sanitizer suites pass.

### Chunk 10 — Memory-size output overload (`fmt.print(usize)`)

`fmt.print(usize): int` formats lengths, sizes, and indices as allocation-free unsigned decimal through a fixed 20-byte stack buffer. It adds no newline, retries partial writes, returns bytes written/non-positive OS results, and covers zero through `USIZE_MAX`; `message.len` now selects it directly. Focused executable coverage includes runtime string length and both unsigned boundaries. Normal and ASan/UBSan suites pass.

### Chunk 11 — General module loader

General merged-unit loader: arbitrary bare/aliased std imports beside the compiler, explicit relative local-path imports resolved from the importing source, canonical-path identity loading each file exactly once, depth-first dependency-order loading, cycle diagnostics with the full chain, qualified functions/overloads/types/globals/enum constants lowered to module-prefixed private identities (`name$`), strict per-name `pub` enforcement at the use site, extern/`export` C spellings preserved across prefixing, and multi-file DWARF preserved. Merged compilation unit only — separate per-module objects and incremental rebuilds stay deferred. `std/math.ace` (minimal pure-Ace `min[T]`/`max[T]`/`abs[T]`) is the second std module; `tests/modules/` covers local/aliased/transitive/diamond/imports of types+globals, private-name collisions, private access, cycles, and missing/invalid/duplicate imports. Normal and sanitizer suites pass.

Known limits (current): a local shadowing a whole-module import binding cannot use `.member` syntax (rejected as an unknown prefixed member; qualified lowering happens before scopes exist); selectively imported values do preserve lexical shadowing. There is no user-configurable std search path, directory/wildcard import, or per-module object yet.

### Chunk 28 — Tagged unions

Closed inline sums with an `i32` declaration-order tag and aligned maximum-sized payload; first variant is canonical zero; qualified payloadless/payload construction; exhaustive no-fallthrough union switches with explicitly typed copy binders; active tag/payload equality when all variants are comparable; plain copies and whole-value reassignment; definite initialization, explicit generic stamping, module visibility, const/global emission including relocation-backed strings, and initial `extern`/`export` rejection. No `any`, reflection, boxing, allocation, numeric tag access, or niche optimization. Focused parser/sema/runtime/module/docs coverage includes three-byte payload copies, large aggregate returns, public/private cross-module use, and const primitive/aggregate/string variants. This work also fixed latent emitter issues in FP load/store scaling, cross-class cast temporaries, and integer literals materialized in float expressions; follow-up testing fixed short-circuit result normalization after those temp pools were separated. Normal and ASan/UBSan suites pass.

### Chunk 29 — `fmt.printf`

One compiler-recognized direct-call standard-module name with literal/static-const formats and runtime arguments captured once before output; `%d`/`%u`/`%x`/`%X`/`%f`/`%s`/byte `%c`/`%%` with exact types; fixed `%f` default 6 and `%.Nf` through 324 with ties-to-even, signed rounded zero, and stable specials; no width/flags/`*`/positions; the uniform `print`/`println` result contract. Sema builds a checked format plan with targeted diagnostics, and the emitter calls direct private typed helpers. Fixed floats use Darwin `fcvt`, temporarily select/restore `FE_TONEAREST` so ambient rounding cannot change output, handle precision-zero nearest/even explicitly, and use one declared 640-byte buffer; decimal-point insertion is an in-buffer `memmove`. There is no runtime parser, tag, pack, `any`, reflection, Ace allocation, C variadic call, or C formatter. Coverage includes changing runtime values, exact mixed bytes/counts, integer/hex boundaries, f32/f64 ties/specials/signed zero and ambient-rounding independence, both precision-324 f64 magnitude boundaries, embedded NUL/high byte, static/selective imports, user-name noninterference, direct-call rejection, evaluate-before-failure behavior, diagnostic text, and undefined-symbol inspection; implemented docs and compiler-checked examples landed. Normal and ASan/UBSan suites pass.

### Chunk 1A — Driver linker controls

`ace` accepts ordered extra `.o`/`.a` inputs, `-L <dir>`, `-l <name>`, and `-framework <name>` beside the single root `.ace` source; the system `cc` is launched through an argument vector (`posix_spawnp`, no shell), preserving the written order after Ace's own object, and linking still requires `-o`. Validation rejects multiple root sources, link inputs under `-c`, link inputs without `-o`, missing option values, unknown options, unrecognized positionals, and output/intermediate paths that would overwrite the source or a linker input (including existing symlink/hard-link aliases) with exact `ace:` diagnostics; linker exit status and signals are reported precisely with cc's own diagnostics passed through. `--report` still counts only Ace's object and excludes linker time. No raw linker escape (`-Wl,...`, arbitrary flags, source-embedded directives, dependency discovery) exists; each stays additive later. Driver fixtures (`tests/driver/`) cover object/archive/framework linking, spaces and shell metacharacters in paths, input-collision protection, every diagnostic, and failure propagation. Normal and sanitizer suites pass.

### Chunk 1B — C ABI compatibility whitelist

`extern "c"` and `export` signatures now pass a positive, position-aware compatibility predicate: fixed-width and platform-word integers, `usize`, `f32`/`f64`, recursively compatible pointers/distinct types/structs, and nested arrays are allowed; direct arrays and every unverified Ace-specific shape are rejected. Exported `mut` parameters are rejected; writable C storage is spelled explicitly as `mut *T`. The Apple-arm64 C calling convention now handles homogeneous floating aggregates, mixed/small aggregates, large indirect values with caller-owned copies, result registers/sret, and exhausted integer/FP register banks. A clang-compiled bidirectional fixture is the ABI oracle. Normal and sanitizer suites pass.

### Initial IPv4 TCP networking

`std/net.ace` exposes the closed allocation-free blocking surface over private Darwin bindings in the same file: `Socket` aliases `sys.Fd`; `Address4` keeps text-order octets and a host-order port; socket creation, explicit reuse/nonblocking options, bind/listen, one-attempt accept/connect, and partial-count receive/send are implemented with operation-specific public error domains, `sys.errno()`, `sys.close`, and per-send `MSG_NOSIGNAL`. Apple-arm64 extern variadic extras now use 8-byte outgoing stack slots, enabling `fcntl(F_SETFL, flags)` without a C shim. Loopback runtime coverage proves failure values/domains, option toggles, peer-address conversion, both send overloads, receive, and cleanup; selective-import and compiler-checked docs coverage landed. Normal and ASan/UBSan suites pass.

### Std directory modules and backend-neutral readiness queues

Bare standard-library imports now accept either the existing `std/name.ace` single-file form or `std/name/module.ace` for a multi-file module, diagnose both definitions as ambiguous, and name both attempted paths when neither exists; explicit local `.ace` imports remain unchanged. `std/net/module.ace` is the stable socket facade over its grouped Darwin backend, while the separate `std/poll/module.ace` owns readiness for general `sys.Fd` values and explicitly imports its own Darwin backend. Darwin/Apple arm64 remains the only supported target, with multi-target selection deferred until a second real target lands. Private kqueue implements allocation-free `Queue`, combined level-triggered `Event` results, paired `watch`/`unwatch`, caller-owned bounded wait storage, nanosecond poll/finite/infinite timeouts, deterministic local errno, and caller-visible interruption. Runtime pipe coverage proves creation, interest replacement, read/write/hangup readiness, deterministic invalid-interest/empty-wait/missing-unwatch errors, cleanup, and the public error domains; directory-module resolution, ambiguity/missing diagnostics, selective imports, and compiler-checked documentation accompany it.

### Explicit switch `fallthrough`

Contextual only inside a switch case, final statement only, transfers to the immediately following case, takes no target or condition, and is rejected in the final case. There is still no implicit fallthrough.

### Recursive-layout diagnostics and pointer-only type fixups

Reject direct or indirect all-inline value cycles instead of assigning placeholder sizes; keep self-pointers valid; permit an unknown same-module type only behind one or more pointer levels, resolve it later in the file, and reject unresolved or layout-dependent early uses. Simultaneous heterogeneous multi-assignment is already implemented; its RHS-before-target ordering is now explicit in `spec.md` and `docs/types.md`.

### Chunk 12 — Type-owned methods and canonical signings

`Circle.area()` / `pub mut Circle.scale`, implicit read-only-or-mut `self`, exact overloads, direct qualified calls, no method values, one `Circle signs Shape, Color` declaration per struct (parenthesized when multiline), explicit `Shape(&c)` construction, overload-aware EOF conformance, const-backing protection, defining-module coherence, public/private cross-module signings, and per-used-pair generic vtables. Normal and sanitizer suites pass.

### Chunk 13 — Compile report

`--report` writes compact successful-build statistics to stderr: unique source files, aggregate source bytes/tokens, emitted `__text` bytes, complete object bytes, and Ace compile time excluding the external linker. Check/token/AST-only runs report zero emission sizes. Imported modules are included once by canonical identity.

### Chunk 14 — Contract terminology

Source declarations use `Shape: contract`, and concrete types use one canonical `Circle signs Shape, Metric` signing. Compiler tokens, AST/type kinds, diagnostics, docs, and fixtures use the contract/signing model. Semantics and ABI are unchanged. Normal and sanitizer suites pass.

### Chunk 15 — Minimal Error layer

Predeclared one-word `Error` handle and 32-byte const-static `ErrorDescriptor` (`code: i32`, `domain: typeid`, `message: string`); compile-time `typeid(T)` builtin with per-exact-type nonzero identity, alias sharing, distinct/instantiation separation, and `==`/`!=` only; zero `Error`/`typeid` sentinels; `Error(&const_global_descriptor)` construction validated as a named literal with compile-time code, null/typeid domain, and static message backing (locals, mutable globals, and arbitrary casts rejected in all build modes); direct read-only `code`/`domain`/`message` inspection; exact descriptor-address identity; module-exported handles with private descriptors; strict `extern`/`export` rejection for `Error`/`ErrorDescriptor`/`typeid`. Panic was deliberately excluded pending its separate decision. Coverage: `tests/emit/errors.ace`, `tests/modules/error_export/`, five `tests/sema/error_*.err.ace` diagnostics, and `docs/errors.md` mirrored by `tests/sema/docs_errors.ok.ace`. The slice also fixed two latent emitter bugs the new tests exposed: short-circuit `||` computed NOR (result arms swapped), and multi-register aggregate call receives stored unit words below their frame slot (`off + w*8` instead of `off - w*8`); plus return-side loads deferred when the destination register aliases the slot base. Normal and sanitizer suites pass.

### Chunk 16 — Panic builtin

`panic` is a reserved name in every scope: sema rejects binding it as a local, global, param, destructuring binder, loop variable, import binding, free-function name, or type name (`panic` stays a plain identifier to the lexer; type-owned methods keep their qualified binding), and the loader rejects module-level `panic` declarations before `name$` prefixing, which would otherwise shadow the builtin for that module's own calls. The only use is the checked call `panic(message: string)` (typeid-style name case): exactly one argument coerced to `string` (static literal or runtime expression), never returns (poison-typed; no never-type work while sema has no return-completeness enforcement), not a first-class value (bare reference has its own diagnostic). Codegen evaluates the string (ptr/len in two registers) and emits a best-effort `write(2, ptr, len)` with the exact message bytes — no prefix, no added newline, result ignored — then `abort()`, both undefined externs through the existing BRANCH26 machinery; checked and release are identical; a failed or partial write never blocks the abort. Coverage: eight `tests/sema/panic_*.err.ace` diagnostics, `tests/emit/panic.ace` (`// expect-exit: 134`, runtime `panic(error.message)` bridge plus a not-taken static-literal path), and the `docs/errors.md` Panic section mirrored by `tests/sema/docs_errors.ok.ace`. Normal and sanitizer suites pass.

### Chunk 17 — Global raw allocation machinery

The process allocator is used directly with no Ace allocation header or registry; size/alignment requests are validated, failure returns a zero slice, reallocation preserves the original slice on failure, and release zeros only its passed slice lvalue. Arrays expose compile-time `.len`; slice `.len` remains a runtime element count and string `.len` a runtime byte count. Slice assignment is a two-word copy with no ownership transfer or alias search, so aliases may dangle after release. The implementation also corrected AArch64 unsigned byte/halfword extension encodings exposed by allocation-backed byte access. Checked-build allocation tracking remains a separate follow-up because the compiler does not yet expose checked/release build modes. The original predeclared public spellings were superseded by chunk 18.

### Chunk 18 — Selective imports and `mem` surface

`import one, two from module` and the parenthesized multiline form work for standard or relative-path modules. Selected public functions carry their complete overload set; types and values retain separate namespaces; same-namespace imports/declarations collide instead of merging; nested values and generic type parameters shadow lexically. `std/mem.ace` now owns the public `alloc`/`realloc`/`release` declarations, compiler-backed to the same direct platform lowering with no extra call; no unqualified allocation name exists without an import. Parser/module/sema/runtime/docs fixtures cover qualified and selective memory use, transitive selection, type/value twin names, overloads, shadowing, collisions, privacy, and missing members. Normal and sanitizer suites pass.

### Writable address-of and per-level pointer permissions

`mut &value` constructs a zero-cost `mut *T` from writable lvalue storage; `mut p: *T` retargets caller storage without granting pointee writes, `p: mut *T` writes the pointee without retargeting the caller, and the combined form grants both. Nested pointers retain independent permissions (`mut **T`, `*mut *T`, `mut *mut *T`); automatic and explicit dereference check only the pointer level performing each write. Parser/sema/runtime/diagnostic/docs fixtures cover direct call expressions, const rejection, retargeting, and multi-level access.

### Chunk 19 — `mem` byte operations

`copy(noalias mut dst: []u8, src: []u8)` and overlap-safe `move` copy complete equal-length views without changing their descriptors; callers slice larger storage explicitly. `fill` and `zero` write complete views; `compare` performs normalized `-1`/`0`/`1` unsigned-byte lexicographic ordering across arbitrary lengths. Empty views are valid, write operations return nothing, and every path is allocation-free. The bootstrap lowers the exact standard-module declarations directly to libSystem `memcpy`/`memmove`/`memset`/`memcmp`, skipping calls for zero-length views; current release behavior adds no length/overlap check, while the specified checked build will diagnose false contracts. Qualified/selective import, sema diagnostics, compiler-checked docs, and runtime fixtures cover the surface. This slice also fixed a latent deep-frame emitter bug: frame loads into x16/x17 now compute their address in the destination register, preventing the second load from clobbering the first. Normal and ASan/UBSan suites pass.

### Byte-sequence equality

`==`/`!=` compare complete contents for every pair of `[N]u8`, `[]u8`, and `string`, symmetrically and without conversion or allocation. Both operands are evaluated once in source order; unequal lengths read no element storage, empty equal values skip `memcmp`, and equal nonempty lengths compare exact bytes. The same content rule applies when a tagged union's active payload is a byte sequence. Other array/slice element types retain their existing rules. Focused semantic, runtime, union-payload, evaluation-order, embedded-NUL/high-byte, and compiler-checked docs coverage landed.

### Chunk 20 — Canonical zero and definite initialization

Every type has an all-bits-zero representation; plain pointers, slices, strings, function values, contracts, `Error`, and `typeid` accept `null`, and the optional-type syntax/layout is removed. Globals retain BSS zero initialization. Bare locals reserve uninitialized storage with no fill; a post-sema control-flow pass rejects direct reads before whole or per-subobject initialization, intersecting facts at branches and treating loops conservatively. Address-taking, pointer aliases, asm, and extern code are trust boundaries. Large-frame allocation now uses both AArch64 immediate tiers and correct x29-relative addressing. Normal, runtime, diagnostic, and sanitizer coverage is included.

### Chunk 21 — Endpoint-ordered ranges and indexed sequence iteration

`a .. b` captures both endpoints once and traverses `[min(a,b), max(a,b))` in their written direction, so `4 .. 0` yields `3,2,1,0` and equal endpoints are empty. Array/slice loops accept either one value binder or explicit `index: usize, value: T` binders; both are per-iteration copies independent of the internal cursor, and direct sequence iteration retains its entry-captured pointer/length. Focused runtime, semantic, parser, grammar, and compiler-checked documentation coverage is included. Explicit stepped ranges remain specified but unimplemented.

### Chunk 22 — Contextual range steps and labeled-block continuation

`step` remains an ordinary identifier except contextually after a range's second endpoint; its positive same-typed value/expression is evaluated once after both bounds, and descending setup aligns once so swapped endpoints exactly reverse the selected sequence. Static zero/negative steps are rejected; runtime validation awaits checked-build mode while the current release path assumes the precondition. `continue label` restarts an enclosing labeled block, plain `continue` still advances the nearest `for-in`, fallthrough exits normally, and `break label` remains the structured exit. Range bounds/steps now participate in definite-initialization reads. Focused runtime, diagnostic, grammar, and compiler-checked documentation coverage is included.

### Chunk 23 — `sys` file I/O

Pure-Ace `std/sys.ace` over `extern "c"` with the new optional extern link name (`c_open(...): i32 = "__open"`), added to keep raw C bindings privately named inside modules; the emitter already honored `Node.cname`. Link names are nonempty, NUL-free, and capped at the v1 emitter's 510-byte limit, with parser diagnostics for each invalid form. Surface as carved in spec.md Stdlib #10: `Fd: distinct i32` (`-1` failed open, zero is stdin), `read(fd, mut buf: []u8): (int, Error)` (0 = EOF), `write(fd, []u8)`/`write(fd, string)` (partial counts as-is, no retry; the string overload exists because no string→`[]u8` view exists yet), `close(fd): Error`, two `open` overloads with the fixed `[1024]u8` path buffer and `ENAMETOOLONG` passthrough, per-operation error domains with `errno(): i32` reading Darwin `__error()`, and raw Darwin flag constants with OS spellings. Two latent bugs surfaced and fixed: call marshaling read later arguments' temps from registers that earlier aggregate word-loads clobbered (flags arrived holding the path length — marshaling now spills all argument values and reads them exclusively from their spill slots), and Apple arm64 passes C variadic arguments on the caller's stack (libSystem `open` reads mode from the stack slot), so the module binds the fixed-arity `__open` export. General extern-varargs stack marshaling landed later with networking; `sys` keeps the fixed binding because its wrapper needs no variadic surface. `tests/emit/sys.ace` covers ENOENT/roundtrip/EOF/EBADF/EEXIST/long-path at runtime; parser fixtures cover the link-name grammar and validation. The test runner accepts an alternate compiler binary, and `make test-sanitize` repeats the full suite under ASan/UBSan. Normal and sanitizer suites pass.

### Chunk 24 — `sys` monotonic time

The owner-approved initial time surface in `std/sys.ace`: `pub time_mono(sleep: bool): u64` returns a monotonic clock reading in nanoseconds from an unspecified origin. `sleep == true` reads Darwin `CLOCK_MONOTONIC_RAW` (`mach_continuous_time`) and keeps advancing while the system is asleep; `sleep == false` reads `CLOCK_UPTIME_RAW` (`mach_absolute_time`) and pauses while the system is asleep but advances whenever the system is awake, including while this process is idle or blocked — never process CPU time. Both modes are immune to calendar-clock changes, readings subtract meaningfully only within the same `sleep` mode, and the call is allocation-free with no `Error` return; the clock-id constants stay private. The implementation stays pure Ace over a private link-named `clock_gettime_nsec_np` extern — the nanoseconds-returning entry point `bench/memory.ace` already binds directly — so no compiler intrinsic was needed. Calendar time, CPU time, duration conversion, and process-sleep APIs are deliberately excluded. Coverage: `tests/emit/sys_time.ace` (both modes nondecreasing, both advance across a short harness-only `usleep` with loaded-CI-tolerant bounds, selective-import call), `tests/sema/sys_time_selective.ok.ace` (selective import, exact `(bool): u64` signature typing), and the docs example mirrored in `tests/sema/docs_standard_library.ok.ace`. Normal and sanitizer suites pass.

### Chunk 25 — `fmt.print(uint)` platform-word output

First slice of the owner-decided overload policy (spec.md Stdlib #9): `pub print(value: uint): int` in `std/fmt.ace` formats unsigned platform-word decimal through the same fixed 20-byte stack buffer as the `usize` overload — zero through `UINT_MAX`, no implicit newline, partial-write retry, and the bytes-written/non-positive-result contract. `uint` is the `P_U64` platform-word primitive, so `u64`-typed values select the overload directly with no cast while `usize` remains a distinct exact overload. Narrower integer widths get no overload: callers convert explicitly (`fmt.print(int(my_i32))`, `fmt.print(uint(my_u16))`), and direct narrower prints diagnose `no matching overload`. Coverage: `tests/emit/print_uint.ace` (zero, `u64`-typed `UINT_MAX`, explicit `u16` cast; output and exit checked), `tests/sema/fmt_print_narrow.err.ace` (direct `u16`/`i32` prints rejected), and the four-overload docs example mirrored in `tests/sema/docs_standard_library.ok.ace`. Normal and sanitizer suites pass.

### Chunk 26 — `fmt.print` float overloads

Owner-decided representation (spec.md Stdlib #9): shortest round-trip decimal in pure-Ace `std/fmt.ace` over three fixed-arity libSystem externs — `ecvt` supplies correctly rounded digit strings (the variadic `snprintf` path stays unavailable), `strtod`/`strtof` verify the round trip (strtof parses f32 directly, no double-rounding detour), and `memcpy` immediately copies ecvt's internal buffer before a later call can overwrite it. The search takes the first correctly rounded digit count in 1..17 (f64) / 1..9 (f32) whose decimal form parses back to the exact value; notation is decimal for scientific exponents -6..20, otherwise scientific like `1e21`/`1.5e-7` (no `+`, no padding); specials are spelled `nan`, `inf`, `-inf`; negative zero keeps its sign (read through the reciprocal, since comparisons cannot see a zero's sign bit); no trailing `.0`. ecvt's digits are the integer form (value = digits x 10^(decpt-count)) — the probe exponent is decpt-count while the notation exponent is decpt-1. This slice exposed and fixed a latent emitter bug: an untyped literal under a float cast (`f32(0.1)`, `f64(3)`) loaded its f64 bit pattern and was viewed at the narrow width (low 32 bits — garbage); the cast path in `gen_call` now materializes untyped literals directly at the target float width. A pre-existing sema wrinkle surfaced, deliberately unfixed: a pending call with an untyped-literal argument (`fmt.print(id32(0.1))`) reads as poison in overload matching, which matches every overload and reports `ambiguous` — printing a local avoids it. Coverage: `tests/emit/print_float.ace` (zero, shortest digits `0.1`/`1/3`/pi, notation thresholds 1e-6/1e-7 and 1e20/1e21, denormals `5e-324`/`1e-45`, `DBL_MAX`, `FLT_MAX`, specials, signed zero, negative scientific, bytes-written count; output and exit checked) and the six-overload docs example mirrored in `tests/sema/docs_standard_library.ok.ace`. Normal and sanitizer suites pass.

### Chunk 27 — `fmt.println`

Owner-approved newline layer over the landed `print` overload set (spec.md Stdlib #9): six overloads in `std/fmt.ace` (`string`, `int`, `uint`, `usize`, `f32`, `f64`), each a one-line call through a private `finish_line` helper that orders the two writes. Semantics: exactly the corresponding `print` representation followed by one `\n`; allocation-free; returns the total bytes written including the newline. A non-positive result from a nonempty value write returns immediately with no newline attempt; `println("")` handles its successful empty value directly by writing the newline and returning `1`; a newline failure after a written value returns the newline's negative result unchanged. No zero-argument form (`fmt.print("\n")` covers it); narrower integer widths stay rejected per the overload policy. Coverage: `tests/emit/println.ace` (byte-count contract across all six overloads, empty string, `INT_MIN`, negative scientific; output bytes verified via `od`), `tests/sema/fmt_println_narrow.err.ace` (direct `u16`/`i32` printlns rejected), and the docs examples mirrored in `tests/sema/docs_standard_library.ok.ace`. Normal and sanitizer suites pass.

### Chunk 28 — `-o FILE.ace` in-place linking

Driver convenience over the existing `-o` pipeline: when the `-o` value ends in `.ace`, it names the root source itself — that file is compiled, and the executable and the intermediate object take the file's name minus the suffix (`./ace -o program.ace` behaves exactly like `./ace -o program program.ace`, producing `program` and `program.o`; a directory-qualified value keeps its path). A stemless value (`-o .ace`) is rejected, and the form combined with a different positional root source still diagnoses `only one root .ace source may be compiled`; spelling the same file in both places collapses to the in-place form. The derivation runs before the missing-source `usage()` exit, so `-o FILE.ace` alone works, and all existing collision checks apply to the derived executable and object names unchanged. Coverage: `tests/run.sh` driver cases (in-place compile of a copied fixture produces and runs the stem executable plus the stem object; the two-sources conflict and the stemless-value diagnostics). `docs/compiler.md` and the usage text document the form. Normal and sanitizer suites pass.

### Large aggregate copies at subobject offsets

The AArch64 emitter's downward `copy_mem` loop now keeps its byte counter in
`x17` and uses the non-temp `x8` as a separate effective-address scratch when
either source or destination has a nonzero offset. This fixes nonterminating,
out-of-bounds copies of aggregates larger than 16 bytes, including tagged-union
construction and payload binders. The corrected bare-base load/store encodings
also use `[x8]` rather than accidentally indexing the address by itself. Runtime
coverage exercises 17-byte and 24-byte local union payloads plus a 24-byte const
global payload and a large non-union tuple member at a nonzero source offset.

### Explicit field embedding and one-way contract promotion

`embed name: Base` marks an ordinary named inline struct field for one-level
method promotion. Direct outer methods win, same-level promoted matches are
ambiguous, fields do not promote, and non-struct embedded fields are rejected.
An embedded base that signs a contract may provide exact promoted methods for
the embedder's own explicit signing, never the reverse. Contract vtables use
allocation-free AArch64 thunks that adjust the receiver by the field's static
offset before calling the base method. Runtime and diagnostic fixtures cover
read-only and mutating dispatch, direct promotion, ambiguity, invalid embedded
types, and the rejected reverse-contract direction.

### Memory representation carved normative

No code change: the carving records the bootstrap's existing layouts as normative
language rules (spec.md Memory #17–21 and the Decision Queue's "Memory
representation carving"). Slices and strings are exactly two-word descriptors —
data pointer at offset 0, count at offset 8, size 16, alignment 8 — with no
capacity word or hidden fields ever; a string is a read-only view over bytes that
is never NUL-terminated; string literals live in static read-only storage as
exactly their written bytes, with the empty literal a valid non-null string;
arrays are contiguous index-order elements with the element's alignment and no
padding. Programmer code may rely on these facts (inline asm, descriptor
inspection, `sizeof` reasoning). Documentation mirrors the pinned facts in
`docs/types.md`, `docs/memory.md`, and `docs/primitives.md`.

### Sequence-overlap carving closed (implementation pending)

No code change: the carve fixes the cross-type rules for `[N]T`, `[]T`, and
`string` (spec.md new "Sequences" section, Decision Queue "Sequence-overlap
carving"). Slicing is uniform with half-open bounds and 0/len defaults; string
slicing will yield `string` and never widen permission; slice re-basing stays
inside the current window; empty views keep their computed pointer and are never
canonicalized to null; `string([]u8)` will be an explicit zero-cost view that
preserves the descriptor exactly while the original slice may still write the
shared bytes; no `string` → `[]u8` conversion and no implicit array/slice
conversion exist. Equality: content for byte sequences, a nullness test when
either operand is the literal `null` (previously implicit behavior, now the
stated rule), constant-time descriptor identity for same-type non-byte slices,
and no `==` for fixed non-byte arrays; `mem.compare` is byte-slices-only, so
non-byte content comparison is an explicit loop or a type-specific library
function. Strings will iterate as stored UTF-8 bytes. The string-side
operations — string slicing, the `string([]u8)` view conversion, string
iteration, and the constant-provable slice-bounds compile check — remain
unimplemented until a dedicated chunk. Documentation records only the
already-implemented facts (descriptor identity, null-literal nullness,
no implicit conversions, empty-view pointers) in `docs/types.md` and
`docs/memory.md`.

### Typed `mem` allocation

The owner-approved typed overloads land in `std/mem.ace` beside the unchanged
raw byte forms: `alloc[T](count: usize): []T`, `realloc[T](data: []T,
new_count: usize): []T`, and `release[T](mut data: []T)`, all explicitly
instantiated generic declarations that the loader marks compiler-backed like
their raw siblings. Sema checks them through the ordinary generic-call path —
the typed intrinsics are intercepted before stamping, so no fallback body is
ever checked or emitted — with exact-`[]T` release matching, `mut` call-site
enforcement, `usize` count coercion, and source-positioned diagnostics;
calling `mem.release(mut typed)` now points at the `mem.release[T](mut data)`
spelling instead of demanding a `[]u8`. The emitter lowers typed alloc/realloc
to the platform allocator with `count * sizeof(T)` computed as a constant
multiply guarded by an `UINT64_MAX / sizeof(T)` overflow compare (overflow and
zero byte size return null without calling the allocator), alignment derived
from `T`, and the element count — not bytes — stored as the returned
descriptor length; typed release shares the raw free-and-null lowering.
Slice/array type names now render exact element types (`[]usize`) instead of
the `[]T` placeholder, sharpening every mismatch diagnostic. The slice also
fixed a latent sema gap: expression-position type arguments such as
`f[Vec[int]](...)` or `f[T](...)` inside a generic body did not resolve
through `build_type` (N_IDENT lacked generic substitution and N_INDEX was
rejected), which makes generic struct literals and nested generic type
arguments work in call brackets. Coverage: `tests/emit/typed_allocation.ace`
(usized and 24-byte struct elements, exact lengths, aliasing, release nulling,
null no-op, zero-count and overflow nulls, realloc preserve/new-length/failed
paths, raw interop), `tests/sema/mem_typed*.ok.ace` (qualified + selective
imports, generic wrappers, string/`Pair[i32]` elements),
`tests/sema/mem_typed.err.ace` with run.sh diagnostic greps, a parser
fixture, `tests/modules/mem_typed_module/`, and compiler-checked docs
examples; `sandbox.ace` drops its little-endian byte packing for
`mem.alloc[usize]`/`mem.release[usize]`. Normal and sanitizer suites pass.

### Separate continuous and awake monotonic clocks

The boolean-selected `sys.time_mono(sleep)` surface is replaced by two
parameterless operations whose names state their distinct time domains:
`time_elapsed()` reads Darwin `CLOCK_MONOTONIC_RAW` and advances through
system sleep, while `time_awake()` reads `CLOCK_UPTIME_RAW` and pauses during
system sleep. Both retain the allocation-free `u64` nanosecond result, immunity
to calendar-clock changes, unspecified origin, no-error contract, and the rule
that only readings from the same operation subtract meaningfully. The runtime,
selective-import, compiler-checked documentation, human documentation, and
editor showcase coverage now use and type-check both parameterless functions.

### Character and string-library carving closed (implementation pending)

No code change: `char` is fixed as a predeclared transparent `u8` alias while
strings remain read-only UTF-8 byte views; decoded `rune` support stays an
explicit additive Unicode layer. The closed `str` surface uses ordinary free
functions rather than methods: byte-exact `find`/`has`/`compare`, caller-buffer
`copy`, zero-copy bounded `split`, exact-boundary and ASCII-whitespace `trim`,
explicitly instantiated `parse[T]`, and overloaded caller-buffer `stringify`.
Every operation is allocation-free, decoding-free, and reports capacity or
conversion failure explicitly. The carve also admits inline slice and array
types in `sizeof`, so the already normative descriptor layouts can be stated
directly. Implementation, tests, and implemented-only human documentation are
a later chunk; the full rules and rationale are in spec.md Stdlib #15 and the
Decision Queue.

### Character, string sequences, and the str module implemented

The carved surface is implemented end to end. `char` is the predeclared
transparent `u8` alias (shared identity, representation, and overload
behavior; character literals keep their untyped one-byte status and now
default to the byte type in overload calls), and `sizeof` accepts the full
inline type grammar (`sizeof([]u8)`, `sizeof([3]char)`). The sequence-overlap
carve landed: string slicing yields a `string` with the ordinary half-open
defaults, `string(bytes)` is a zero-copy descriptor conversion that accepts
exactly `[]u8` (fixed arrays must slice first, and no `string` → `[]u8`
direction exists), `for-in` walks string bytes with the ordinary one- and
two-binder rules, and constant-provable slice bounds that violate
`0 <= lo <= hi <= len(source)` are compile errors. `std/str.ace` implements
the closed free-function surface in pure Ace — `find`, `has`, `compare`,
`copy`, zero-copy `split`, exact-boundary and ASCII-whitespace `trim`, and
overloaded `stringify` whose float forms are digit-for-digit identical to
`fmt.print`. `str.parse[T]` follows the mem typed-allocation precedent: the
public generic declarations are compiler-backed, each instantiation is checked
against the carved type set with a precise diagnostic, and the emitter lowers
onto concrete private helpers (`strtod`/`strtof` only behind a strict
character-set validator that admits exactly the decimal/scientific spellings
plus `nan`, `inf`, and `-inf`). Along the way: multi-assignment targets are
now keyed for the emitter even when the right side is a still-pending overload
call, and untyped-constant negation avoids i64-min UB. Coverage spans parser,
sema, emit, runtime module fixtures, implemented-only documentation, and
sanitizer runs.

### Ace formatting conformance and module declaration order

The character/string chunk's Ace sources now follow the repository's normative
blank-line, indentation, and 120-column rules. `.acefmt` also records the
owner-decided module organization rule: after imports and any platform `extern`
boundary, public Ace declarations precede private implementation declarations,
with overloads kept together. `std/str.ace` is organized accordingly, and its
runtime fixture retains the long conversion cases through in-program checks
without an overlong expected-output directive.

### First-class generic function specialization

Generic specialization (Generics #9–14) lands as an ordinary user feature —
nothing migrates yet. The lexer gains the `specialized` keyword; the parser
gains no new bracket production (`try_tparams` already captures idents-only
brackets on `func_decl`), and `specialized` after the return clause sets a
no-body template flag (the lexer's newline rule treats the keyword as
statement-ending). Sema interprets the shared bracket production by type shape:
brackets whose identifiers resolve to declared types are specialization
arguments and must follow a declared generic of that name (clean error
otherwise, never a silent fresh parameter list); identifiers resolving to no
declared type start a fresh type-parameter list, so one name keeps the ability
to declare several generic overloads before any specialization (`str.parse`
does). A specialization registers as an ordinary independently checked
function — concrete substituted signature, non-generic symbol — bound to
exactly one generic overload by substituted-signature conformance (parameters,
`mut` positions, return; all type parameters positional); duplicates for one
exact tuple on one overload are declaration errors, as is `specialized` on a
specialization or a bracketless function. Bracketed calls exact-match a bound
specialization's type-argument tuple ahead of template stamping and lower as
ordinary direct calls; a chosen template marked `specialized` with no matching
tuple errors at the call site naming the missing type tuple and the
declaration, and end-of-file rejects a `specialized` generic with zero
specializations in the compiled program. The emitter routes specialization
declarations through ordinary function emission and tags their symbol mangling
with the exact type-argument tuple under an `s` marker (template stamps keep
`g`), so a specialization can never collide with a same-params overload or a
template stamp of the same tuple. Coverage: `tests/emit/spec_wins_over_template.ace`
and `tests/emit/specialized_template.ace` (runtime selection, template
fallback, no-default dispatch) plus `tests/emit/spec_stamp_before_spec.ace`
(a stamp created before its specialization is declared keeps the template
body; the `g`/`s` symbols coexist and later calls resolve to the spec),
`tests/sema/spec_*.err.ace` with run.sh
diagnostic greps (conformance, duplicate, missing-generic, unspecialized call,
zero specializations), a parser fixture, and compiler-checked documentation
examples. `docs/function.md` documents the feature, `docs/standard-library.md`
points at it from the compiler-backed `str.parse[T]` surface, and grammar
note 23 records the implemented bracket interpretation. Normal and sanitizer
suites pass.

### `str.parse[T]` migrated onto generic specialization

Both `std/str.ace` `parse[T]` overloads are now declared `specialized`, and the
private `parse_*_result` helpers became their complete callable surface: eight
ordinary `pub` specializations — `parse[int]`, `parse[uint]`, and
`parse[usize]` on the radix overload (the `usize` form delegating to the
`uint` body as before), the three default integer forms forwarding at decimal
radix 10, and the `f32`/`f64` forms — selected by exact type-argument match
with no compiler involvement beyond the generic machinery. The composed-lookup
path is deleted outright: `check_str_parse_call`, `parse_helper`, and the
per-type `SUFFIX` table with its call-site arm in sema, `mark_str_intrinsics`
in the module loader, `gen_str_parse_builtin` and its dispatch arm in the
emitter, and the `BI_STR_PARSE`/`BI_STR_PARSE_RADIX` markers. Caller-visible
parse behavior is unchanged — every `tests/emit/str_parse.ace` case passes
unmodified — and unsupported type arguments now reject through the Generics
#13 call-site diagnostic naming the missing tuple and the declaration
(`run.sh` greps updated to the new text). One selection refinement the
migration forced: the sole-type-tuple-match fallback at calls now requires the
candidate's arity to match the call, so a wrong-arity call on a matched tuple
falls through to the template path instead of short-circuiting Generics #13
with a bare arity error; the fallback's literal-coercion purpose (matching
arity, non-exact literal types) is preserved. `docs/standard-library.md` and
`docs/function.md` record that the parse surface is specialization-implemented.
Normal and sanitizer suites pass.

### `fmt.printf` dispatch migrated onto generic specialization

The nine private `printf_*` helpers in `std/fmt.ace` are now the complete
specialization surface of three private per-shape generics declared
`specialized` — `print_counted[T](value: T): int` (`string`, `u8`, `int`,
`uint`, `usize`), `print_precision[T](value: T, precision: usize): int`
(`f32`, `f64`), and `print_hex[T](value: T, uppercase: bool): int` (`uint`,
`usize`) — each an ordinary independently checked body, with the bodies
unchanged byte-for-byte apart from the internal calls that now go through
bracketed specializations (`print_counted[string]("nan")` and the `f32` →
`f64` precision delegation). Sema's `printf_helper` no longer splices helper
names off the canonical `fmt.printf` spelling: it composes only the module
prefix plus the generic name, then walks the overload chain for the
specialization whose type-argument tuple exactly equals the format
interpreter's concrete argument type (Generics #10), erroring if a shape's
specialization is absent. `check_printf_call`'s format interpreter is
untouched — the same parts, precision handling, counts, and diagnostics — and
`emit_printf_helper`'s call sequence is untouched; the one mechanical
adjustment is its `fn_mangle` invocation, which now passes the helper
symbol's `spec_args` so the call target spelling carries the same `s`-tagged
type-argument tuple as the emitted specialization definition (the identical
pattern the ordinary specialization call path already used). Caller-visible
printf behavior is unchanged: the full printf battery (`tests/emit/printf*.ace`,
`tests/sema/printf*.ace`, the docs fixture) passes unmodified, and object
inspection shows the helpers emitted under exact-tuple `s` symbols with no C
`printf`, variadic call, or allocator. `docs/standard-library.md` records the
specialization-implemented lowering. Normal and sanitizer suites pass.

### Standard descriptor constants (`sys.stdin`/`sys.stdout`/`sys.stderr`)

Owner-decided spelling (the suggested `sys.in` is impossible because `in` is a keyword; OS words win per the `sys` mirroring rule): three `pub const Fd` globals in `std/sys.ace` — `stdin` (0), `stdout` (1), and `stderr` (2) — valid from process start and consumed through the ordinary `read`/`write` operations, with the decision recorded in spec.md Stdlib #10 and the implementation-phase list. The initializers (`Fd(0)` and friends) exposed a genuine emitter gap: `eval_const` folded literals, unions, and `typeid` but not constructor-shaped casts, so `const` globals of distinct or enum type could not hold compile-time constructor values. The folder now folds a single-argument constructor-shaped cast whose callee resolves to a named distinct or enum type by evaluating the argument at the unwrapped target representation — byte-identical to the runtime cast, which literal fit-checks and castability already constrain at sema. Coverage: `tests/emit/sys_standard_descriptors.ace` (`dup2`s files onto fds 0, 1, and 2; verifies the value checks, a read, a write, and EOF through each constant), the docs example mirrored in `tests/sema/docs_standard_library.ok.ace`, and the implemented-only bullet in `docs/standard-library.md`. Normal and sanitizer suites pass.

### Widened constructor-constant folding

The descriptor-constant landing gated the fold on the callee's type symbol being a distinct or enum type — an implementation leak. Owner-approved widening: the folder now folds any constructor-shaped call that sema resolved as a cast (no resolved function symbol, one argument, callee an identifier carrying the cast's checked type) whenever the argument's family matches the target family of `callee->ty`, which aliases resolve to their terminal type at declaration. Integer, character, and boolean arguments — negated forms included — require an int-family target (integer primitives, enums, and distinct types over either); float arguments require a float-family target (float primitives and distinct types over one). The family gate is load-bearing because the folder writes the argument's own representation: `const f64 = f64(3)` would store raw int bits and stays rejected at emit (`global initializer must be a compile-time constant`), while `const i32 = i32(3.5)` is rejected at sema (float literals cannot cast to ints at all); same-family float casts (`f64(0.5)`) fold, byte-identical to the runtime cast. Bool targets, strings, contracts, unions, Error, pointers, and aggregates match no family and stay unfolded; distinct-of-pointer constructor constants remain sema-rejected; literal fit checks stay at sema (`H(300)` on a distinct u8). A same-named function with a matching signature still wins over the constructor shape (the initializer type-errors), a non-matching twin gives an arity error, and a local value may shadow a type name beside a folded global. This makes alias constants (`A: type = u8; a: const A = A(5)`) and primitive-cast constants (`x: const i32 = i32(7)`) work, including multi-level alias chains (folded at the terminal type, never an intermediate alias) and qualified constructors (`sys.Fd(0)`). Coverage: `tests/emit/const_constructor_folds.ace` (fold-equals-runtime-cast for `Color(1)`, narrowing `H(200)`, negative `S(-1)`, alias chains, the qualified constructor, and shadowing), `tests/sema/const_constructor_function_wins.err.ace`, `tests/sema/const_constructor_arity.err.ace`, `tests/sema/const_constructor_fit.err.ace` with run.sh diagnostic greps, and the new `tests/emit_errors/` fixtures (emit-level rejections, each documenting its diagnostic on an `// error:` line, driven through `-c` because the folder only runs when code is emitted). `docs/types.md` documents the constant-initializer rule with the example mirrored in `tests/sema/docs_type.ok.ace`. Normal and sanitizer suites pass.

### Composite-literal field stores keep their destination base

The element-wise struct/array/tuple literal path computed its destination temp
address into `x15` once before the field loop, but a nested aggregate literal in
field position (`Pair {ip: [127, 0, 0, 1], port: p}`) re-programs `x15` for its
own temp while evaluating, so every later field store landed at a stale base —
observed as the array field never stored and the following scalar field written
over the first field's bytes. Both loops (`N_STRUCTLIT`/`N_ARRAYLIT` and
`N_TUPLE_E`) now re-derive the destination address after each field expression,
stepping down to `x13`/`x14` when the evaluated value occupies the fallback
register, so no base can collide with a live value; the fill-style repeat
literal was already per-store and is unchanged. The const folder needed no
change (it writes a byte buffer with an explicit cursor). Coverage: new
`tests/emit/nested_literal.ace` (anonymous array fields in struct literals,
nested struct literals, struct-literal array elements, and a tuple carrying an
array through a multi-value return) and the inlined address literal in
`tests/emit/net.ace`, which also dropped its now-redundant per-byte content
checks and response arrays for byte-sequence equality and the `net.send`
string overload. Normal and sanitizer suites pass.

### Deferred calls (`defer`)

Owner-approved cleanup statement, landed front end first and then codegen,
recorded normatively as the Deferred calls section of `spec.md`. The lexer
reserves `defer`; the parser's `defer` statement takes one expression — or,
after an intervening `mut`, the receiver-marked call form
(`defer mut counter.add(1)`, setting `F_MUT` on the call) and rejecting `mut`
before any other expression. Sema admits a defer only as a direct statement of
the function's top-level body block (the innermost scope is the body scope
exactly when no nested block has pushed one), requires the operand to be a
call, and type-checks it exactly like an ordinary call statement, so a
discarded multi-return is allowed. The emitter expands each defer inline
instead of building a runtime list: the statement captures its call as a
synthetic call whose arguments read dedicated capture slots, and every exit
point — the five `N_RETURN` emit paths plus the fall-off epilogue — re-emits
the captured calls in reverse order through the ordinary call dispatch with
results discarded, under DWARF line marks that cite the defer statement's own
source line. Capture is two-tier: read-only arguments evaluate fully at the
statement (snapshot), while call-site-`mut` arguments and the `F_MUT` receiver
capture the lvalue's address, so the exit-time call reads final storage and
its writes outlive the function's return. A result-register save area
(`retsave_off`) parks a register-returned scalar/aggregate result across the
deferred calls (sret results already sit in the caller's buffer), and a
defensive emitter diagnostic re-checks the top-level-only invariant. The slice
also fixed a pre-existing fall-off bug (owner-approved): `bind_label` now
resets `ended_return`, because every bound label is a control join —
previously any `return` inside an `if` permanently suppressed the fall-off
epilogue, so falling off the end after a conditional return executed the next
function's code (verified segfault). Post-implementation review fixed one
capture defect: a deferred call through a function value now snapshots the
callee value itself at the `defer` statement, like a read-only argument, so
the exit-time call dispatches through the defer-time function instead of
re-reading the variable after later reassignment (`defer f()` with `f`
rebound called the new value); `mut` on an indirect callee marks nothing and
the callee position is always read-only-captured. The deferred
`mem.release(mut v)` null-out was audited against the same rule and already
writes canonical null through the captured address into the original lvalue
(probes: global, local, field, `mut` parameter, fall-off exit, live return
value, double-release no-op), so no codegen change was needed there.
Coverage: `tests/parser/defer_stmt.ok.ace`,
`tests/sema/defer_top_level.ok.ace` plus `defer_placement.err.ace` and
`defer_noncall.err.ace` with run.sh diagnostic-count checks, and ten runtime
fixtures (`defer_basic`, `defer_return_paths`, `defer_lifo`, `defer_fall_off`,
`defer_snapshot`, `defer_mut_capture`, `defer_panic_skip`,
`defer_break_skip`, `defer_release_null`, `defer_callee_snapshot`) covering
exit paths, LIFO order, both capture tiers, panic skipping, labeled-break
skipping, the release null-out landing on the original variable, and the
indirect-callee snapshot. Spec, grammar, and
implemented-only documentation landed in the same change; the full suite
passes.

## Still open after these chunks

Checked/release build modes, then checked allocation tracking; nonblocking echo and multi-client chat are the next networking proofs over the implemented `poll` readiness queue (see the `spec.md` Decision Queue).
