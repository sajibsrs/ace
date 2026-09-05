# TODO — future completion list

Candidate catalogue of missing features and capabilities, core language first. This file is a survey, not a ledger: **no item here is a decision**. Anything promoted to work must be carved through the Decision Queue in `SPEC.md` (rule + rationale recorded there), implemented chunk by chunk, and logged in `CHANGELOG.md`. Ordering notes are recommendations to be resolved by the owner under the priority stack (`PHILOSOPHY.md`): performance > simplicity > modularity > compile time > safety.

Status tags:

- **[queued]** — already listed for later work in `SPEC.md`
- **[open]** — already open in the Decision Queue or the Current-status surface index
- **[additive]** — explicitly deferred as an additive extension by a carved decision
- **[new]** — absent from every existing deferred list; observed here first
- **[unspecified]** — neither decided nor rejected in `SPEC.md`; support must be verified, then carved either way

Territory rejected by design is excluded entirely: manual memory through one fixed strategy (no GC, ownership, or allocator values), no exceptions or propagation, no inference, no optional type operator, no COW, no iterator contracts, no comptime or reflection, no block-scoped defer, no hidden-capture closures (explicit contract values are the designated mechanism). Items appear only where the existing design leaves the question open.

## 1. Concurrency & parallelism

Nothing carved; the largest unaddressed capability area.

- **C-ABI function pointers as callable values** [new] — the C boundary whitelist rejects ordinary Ace function values (C Interop #3), and taking the address of an `export`ed function as a value is unspecified. `pthread_create`, `qsort`, `bsearch`, `signal`, and kqueue user filters all require passing a function through C. Prerequisite for threads; carve first.
- **Threads** — explicit create/join/exit over libSystem pthreads; spawn/join as ordinary calls keeps "no hidden control flow" (philosophy #2). Hinted in the post-nonblocking-chat milestone direction; never carved.
- **Atomics** — load/store/exchange/compare-and-swap plus fences with a stated ordering model; NEON/LL-SC or inline-asm lowering. Nothing exists.
- **Thread-local storage** — absent entirely, not on any deferred list. Errno-style per-thread state patterns need a storage class.
- **Memory-model statement** — what `noalias` and read-only parameter facts mean across threads; cheap to carve, expensive to retrofit.
- **Mutex/condvar std module** — thin pthread wrapper; trivial once threads land.

## 2. Compile-time constants & layout introspection

Machine truth (Memory #4) is only half exposed: `sizeof` covers size; the rest of layout has no access path.

- **`align_of(T)`** [new] — raw `mem.alloc(size, align)` callers must guess struct alignment today; typed allocation derives it privately (Memory #23).
- **`offset_of(T, field)`** [new] — every C struct is hand-transcribed for extern blocks; offsets plus static assertions are how a transcription is proven.
- **Static assertions** [new] — no general constant-expression check context exists; the `[value : count]` literal-count rule (Syntax & Parsing #8) is the only shape.
- **Binary data embedding (`embed "file"`)** [new] — static read-only storage as a generalization of the string-literal rule (Memory #20); serves the game/asset milestones; cheap to emit.
- **Source-location builtins** (`@file`/`@line`-class) [new] — assert/panic context in checked builds; bounded and zero-cost.
- **`sizeof` on expressions** [new, minor] — currently type-only (Memory #14).

## 3. Representation & declaration controls

- **`volatile` / `packed` / `align` attributes** [queued] — listed under the Pre-Implementation Checklist; MMIO and wire-format work is blocked until landed.
- **Enum explicit discriminants and backing width** [new] — enums are i32 and sequential today (implementation-phase record); opcodes, hardware registers, and ABI structs need explicit values and a chosen width.
- **Pointer ↔ integer conversions** (`usize(p)`, `T(addr)`) [unspecified] — admission and spelling unstated anywhere; a machine-true language should state the sanctioned form for descriptor packing and tagged pointers.

## 4. C boundary completion

Whitelist gaps hit by real bindings:

- **`bool` (`_Bool`) mapping** [additive] — C Interop #3.
- **C enums at the boundary** [additive] — interim documentation of a distinct-of-i32 convention helps until carved.
- **C global variables in extern blocks** [unspecified] — C Interop #2 covers declarations; data imports (`stdout`, errno-style globals) are unaddressed.
- **Over-16-byte alignment for raw `mem.alloc`** [additive] — Memory #10.

## 5. Checked & release build modes

Specified, unimplemented — the largest promised capability (Safety & Checked Builds, [open]).

- Build-mode flags; bounds, overflow, and allocator diagnostics.
- **`assert`** [new] — checked-only, release-stripped; complements `panic` without adding an error channel.
- Call-boundary `noalias`/length diagnostics (Memory #16) and checked null-use diagnostics (Zero #2) — both specified, unimplemented.

## 6. Backend & emission capabilities

- **x86-64 second backend and multi-target facade selection** [open] — Decision Queue.
- **Shared-library (dylib/PIC) and static-archive emission** [new] — the driver emits objects and executables only (Compiler, driver entry); C parity eventually requires both.
- **Per-module objects and incremental rebuilds** [queued] — module-loader record, `CHANGELOG.md` chunk 11.
- **Richer DWARF** (variable/type DIEs, unwind tables) [additive].

## 7. Numeric & bit-level operations

- **libm transcendentals** (`sin`/`cos`/`pow`/`exp`/`log`/`floor`/`ceil`) [open] — `math` is minimal by admission; the 2D/3D demo milestones need them.
- **Bit intrinsics** — popcount, clz/ctz, byteswap, rotate [new] — direct ARM instructions, zero-cost; protocols and hashing.
- **Float classification** (`isnan`/`isinf`/`isfinite`) [new] — `fmt` implements these privately (Stdlib #12); expose or reuse.

## 8. Declaration & control-flow open questions

Small items, each needing an owner decision:

- **Methods on enums and unions** [unspecified] — type-owned methods are specified through struct examples only (Abstraction #3).
- **Associated functions** namespaced under a type (`Vec[T].new()`) [new] — parallel to `Type.member` enum constants.
- **`switch` on strings and range cases** [unspecified] — protocol dispatch would use both.
- **Operator overloading / custom indexing** — likely conflicts with "dispatch is an explicit value" (philosophy #5, Abstraction); free functions or a `vec` module may suffice. Only worth carving if vector-math ergonomics prove painful in the demo milestones.
- **Runtime global initializers / module-init hook** [unspecified] — globals are constant-fold-only today; the constraint is implicit and was never decided explicitly. Rejecting it permanently would also be a valid outcome; either way the rejection (or admission) should be explicit.

## 9. Stdlib capabilities gating real programs

Not core language; listed because each blocks practical use.

- **`sys.exit(code)`** [new] — early termination is currently expressible only as `panic`; returning from `main` is the only normal exit. Process sleep is separately deferred (Stdlib #11).
- **Filesystem surface beyond open/read/write/close** — stat, mkdir, readdir, rename, unlink [new].
- **Signals** [additive] — excluded from `poll` (Stdlib #14); servers need SIGINT handling.
- **`str.concat` / `str.join`** into caller storage [new] — `str` searches, splits, trims, parses, and formats, but never assembles (Stdlib #15).
- **Dynamic array and hash-map modules** [new] — composed from `mem` and explicit generics; none exist.
- **`rune` + UTF-8 decode** [additive] — Stdlib #15.
- **Random source** [new].

## Suggested carving order

Recommendations only; queue order is an owner decision.

1. **C-ABI function pointers** — small, and unblocks both threads and every real extern binding.
2. **Concurrency carve** (threads, atomics, TLS, memory model) — the biggest true gap; matches the existing post-nonblocking-chat milestone direction.
3. **Introspection carve** (`align_of`/`offset_of`/static assert, enum discriminants/width) — cheap and high-value; the same items keep surfacing in C-interop work meanwhile.
4. **Checked-build modes plus the representation attributes** (`volatile`/`packed`/`align`) as the next tooling chunk.
