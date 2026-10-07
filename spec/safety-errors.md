## Safety & Debug Builds
Decision: checks are a development tool; release trusts the developer.
1. The compiler has two explicit emission modes: `--debug` and `--release`.
   They are mutually exclusive, and emitted programs default to `--release`.
   Check-only, `--tokens`, and `--ast` invocations accept either flag but emit
   no program, so the choice has no effect on their result.
2. Debug builds diagnose dynamic contract violations and then abort; bounds
   diagnostics include the source file, line, and column of the failing access.
   The diagnostic is best-effort on standard error and is not a recoverable error
   channel. Array, slice, and string bounds and integer overflow are on. A conversion to an enum, `E(n)`, checks that `n` is a member's discriminant (one range compare for a dense enum), so an enum value is always a member and indexing `[E]T` needs no check. A constant taking an enum type, explicitly (`E(7)`) or implicitly (`e: E = 7`, an `E` argument), must name a member at compile time.
   Allocator tracking diagnoses invalid/double release and leaks; arbitrary
   use-after-free access requires separate memory-access instrumentation and is
   not implied by allocation tracking.
3. Release builds strip the guards and tracking completely; the programmer and
   the tests own correctness. Violating a release-assumed contract has C-level
   unchecked behavior. The mode adds no word to any value and changes no
   language ABI or representation.
4. A violation provable from compile-time constants is a compile-time error in
   both modes. Mode selection controls only checks that depend on runtime
   values.
5. Safety is intentionally minimal and local: type checking and definite initialization improve on C at zero runtime cost; debug builds add bounds, overflow, and allocator diagnostics. Ace rejects Rust-style ownership, lifetime, borrow, and whole-program safety analysis under the priority stack.
6. Integer overflow checking covers the value-producing arithmetic operations
   `+`, `-`, `*`, and unary `-` on every fixed-width signed and unsigned integer
   type, including their compound-assignment forms. Debug builds diagnose a
   result outside the destination type before it is stored. They also diagnose
   division or remainder by zero and the signed `MIN / -1` and `MIN % -1`
   cases. Release builds emit the target's ordinary fixed-width operations with
   no guards; overflowing arithmetic is unchecked. Bitwise operations and
   explicit numeric casts are not overflow operations. A shift count must be
   nonnegative and less than the left operand's bit width; debug builds
   diagnose a false runtime precondition and release assumes it. Left shift is
   otherwise a bit operation: discarded high bits are not an overflow
   diagnostic. As elsewhere, a violation provable from constants is rejected
   in every mode.
7. Checked `noalias` enforcement is deliberately local. At a call boundary,
   every slice argument corresponding to a `noalias` parameter is compared
   against the byte range of every other slice argument whose parameter may
   reach writable storage; nonempty ranges must be disjoint. Computing a byte
   range that is not representable is itself a violation. The compiler performs
   no pointee-graph walk, ownership inference, or general pointer provenance
   analysis; promises outside directly represented slice ranges remain the
   programmer's responsibility. Separately stated call contracts such as the
   equal lengths required by `mem.copy` and `mem.move` are checked at the same
   boundary. Release emits none of these comparisons.
8. Checked null-use diagnostics cover explicit or automatic dereference of a
   zero pointer, a call through a zero function value, and dispatch through a
   zero contract value. Merely storing, copying, comparing, or passing a zero
   value remains valid. Zero slices and strings keep their sequence rules, so an
   indexed access fails the bounds contract rather than adding a second null
   rule. Release emits no null-use guard.
9. The checked allocator registry records each live allocation's base address,
   byte size, and required alignment. `realloc` and `release` require the exact
   live base and complete authoritative extent; an interior, truncated,
   extended, already released, or otherwise unknown descriptor is diagnosed
   before the platform allocator is called. A successful reallocation replaces
   the record, a failed reallocation leaves it unchanged, and a successful
   release removes it. Any records remaining when `main` returns normally are
   reported as leaks before process completion. `panic`, `sys.exit`, signals,
   and other abnormal or immediate termination perform no hidden cleanup or
   leak pass. The registry is diagnostic metadata only: it does not prevent or
   detect arbitrary access through dangling aliases, and release builds contain
   no registry, headers, or checks.
10. `assert(condition)` is a reserved language builtin for checked invariants.
    It accepts exactly one `bool` expression, is not a first-class value, and
    cannot be declared, shadowed, or redefined. A debug build evaluates the
    condition exactly once; false produces a best-effort assertion diagnostic
    with the source location (`file:line:col`, paths per Errors #16) and then aborts as a broken invariant. A release
    build emits neither the condition nor its evaluation, so a program must not
    rely on assertion side effects. A condition known false at compile time is
    rejected in every mode under #4; one known true emits nothing. There is no
    message overload: use `panic(message)` for an unconditional contextual
    abort. The same builtin is suitable for simple tests run in debug mode;
    richer expected/actual reporting and test discovery belong to a library or
    runner rather than the language primitive.

## Errors
Decision: errors are ordinary values; checking is always explicit.
1. Functions return multiple values (e.g. value, memory size, error).
2. Errors are checked with branches the programmer writes — no hidden control flow, no exceptions, no propagate-operator that secretly returns.
3. Panic exists only as unrecoverable abort for bugs — never a catchable error channel.
4. Accepted cost: error checks add visible lines; nesting relief comes from early returns and explicit cleanup, ordered at function exits by `defer` (see: Deferred calls) — no error sugar.
5. `Error` is a predeclared opaque 64-bit value. Zero is success/null. Bits 63..32 hold a compiler-assigned nonzero 32-bit domain ID; bits 31..0 hold the raw bits of the occurrence's `i32` code. It is not a contract value and carries no pointer, allocation, registry, or metadata.
6. Convention: the error is the LAST return, as `Error`; zero/null means success and `if error != null` is the explicit check. The one-word zero sentinel keeps `(usize, Error)` a two-word aggregate and normally register-returnable on 64-bit targets.
7. `pub OpenErr: error` declares an empty nominal error domain. The optional structured form is `pub OpenErr: error {` followed by comma-separated entries such as `0: unknown = "unknown failure"` and a closing `}`; the trailing comma is optional, as in every brace-delimited list. Each code is an exact integer literal representable by `i32`, including zero and negative values; names and codes are each unique within the domain, and every message is a string literal. Domain visibility controls every entry; entries have no separate `pub`. Each independent domain declaration is unique. `OpenErr()` constructs `Error(domain, 0)` and `OpenErr(code)` accepts exactly one `i32`, preserving its raw bits, including codes absent from the declaration. The expression type is universal `Error`, so function signatures return `Error`, never the domain name.
8. Error-domain names are usable with `typeid`. Transparent aliases and re-exports preserve the originating declaration's identity; independent same-spelled declarations remain different. A resolved source file is canonicalized before module caching, so equivalent import paths reach one declaration and one domain ID.
9. `Error` exposes exactly four direct read-only fields: `error.domain: typeid`, `error.code: i32`, `error.name: string`, and `error.message: string`. `name` is the unqualified spelling on the originating domain declaration; aliases and re-exports preserve it. `message` matches the complete domain/code pair and returns that entry's declared literal. Null returns `""` for both description fields, and a known domain with an unlisted code returns `""` for `message`. No domain-specific entry becomes a dynamic `Error` field.
10. `Domain.entry` is an `Error` value equivalent to `Domain(code)`. `Error == Error` compares the complete packed value, so equal entry spellings or codes in independent domains remain unequal.
11. Compiler-assigned domain IDs are collision-free only within one linked program, unstable across builds and processes, nonserializable, and not suitable as external identifiers.
12. Multi-return consumption is typed destructuring — `n: int, error: Error = read()` — the `name: type` rule applied to multiple bindings; plain reassignment `n, error = read()` afterwards. A bare `_` in either list drops that value: it takes no type, binds nothing, may repeat, and is never read (`_, error: Error = read()`); `_ = f()` drops a single result. `_` must drop a typed value, not an untyped literal, and takes only plain `=`. No declaration may be named `_`; bit-field padding `_: N` is separate.
13. `Error`, error-domain names, and `typeid` are Ace-internal shapes and are rejected in `extern "c"` signatures. C-facing wrappers translate explicitly to stable `i32` codes or another declared C-compatible shape; imported C failures are mapped to an Ace domain and occurrence code.
14. Error descriptions are opt-in static metadata: names and messages are read-only string data, and lookup data/code is emitted only when `.name` or `.message` is used. Lookup is allocation-free and safely returns empty strings for unknown domains/codes. There is no runtime registry or initialization, metadata carried inside `Error`, descriptor, pointer, vtable, general reflection/type-name facility, dynamic entry lookup, or `fmt.print(Error)` support. Occurrence-specific context is returned separately: by convention, a function that can explain a failure takes a caller-owned `diag: mut fmt.Buffer` (storage-only, `fmt.Buffer.init(storage)`) and, on its failure path only, appends one line of its own context before returning the `Error` unchanged, so the buffer reads innermost-first. A caller wanting no context passes `fmt.Buffer.init(null)`. Code that continues past failures and reports each one instead takes a caller-owned `errors: mut array.Array[R]`, where `R` is a domain record holding an `err: Error` plus its own position or payload, and pushes one record per failure; there is no shared std failure-record type.
15. Panic remains semantically separate from recoverable errors: it terminates the process for broken invariants and is never catchable, recoverable, or convertible into `Error`. Surface (decided): `panic` is a reserved builtin — no declaration may bind, shadow, or redefine the name in any scope — and its only use is the call `panic(message: string)`: exactly one message argument, any string expression, static or runtime. The call never returns and `panic` is not a first-class value. Termination (decided): the message is delivered best-effort to standard error (file descriptor 2) exactly as supplied — no prefix, no added newline, a failed or partial write never blocks termination — then the process aborts via `abort()` (`SIGABRT`), as undefined externals resolved by the system linker. Debug and release builds behave identically. Users trigger it; the language owns everything that happens next.
16. Source-location builtins: `@file` and `@line` are reserved expression forms naming where they are written, never where the caller is. `@file` is a compile-time `string` constant naming the containing source file; its bytes are static read-only data. `@line` is an untyped integer constant holding the 1-based source line, typed by context exactly like an integer literal. A helper reporting its caller's location receives these values as ordinary arguments: there is no implicit caller-location parameter, `@column`, `@function`, or location struct, and any other `@name` in an expression is an error.
    - Reported paths are build-relative with `/` separators, never absolute. A standard-library file is `std/<path within std>`; every other file is relative to the build root — the directory holding the entry file for a direct compile, or the root package's directory under `ace pkg` — using `..` segments when outside it. The `assert` diagnostic (Safety & Debug Builds #10) reports the same paths.
