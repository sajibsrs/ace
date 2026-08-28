# Ace — Language Specification

Concrete rules and syntax. The why behind every rule lives in philosophy.md (closed). The formal grammar for this surface lives in grammar.md. This file is carved topic by topic.

## Source
1. File extension: **.ace** (decided — `.ac` rejected: autoconf collision).

## Syntax & Parsing
1. Fast, simple parsing is the goal — single-lookahead LL(1) is the guideline, not a requirement; small bounded lookahead is acceptable where it buys readability.
2. Recursive descent top-down parsing implementation.
3. Storage declarations are `name: type` everywhere (variables, params, returns). The only type-leading top-level forms are explicitly qualified methods (`Circle.area`) and the concrete type's canonical interface contract (`Circle impl Shape`); both are bounded-lookahead declarations, never statements. No `var`/`let` keywords or C typedef lexer hack is needed.
4. Function declarations are keywordless with trailing returns (`fu(a: int): int`); distinguishable from calls by bounded lookahead.
5. Function values are typed by signature — `(int, int): int` — C pointer-to-function syntax never enters the language.
6. Tuple expressions are comma-built at the top level: `return words, null` ≡ `return (words, null)` — parentheses are grouping, required only for nesting (`return (a, b), err`) or clarity. Same rule on assignment sides.
7. Bare comma-separated lists stay on one physical line. A list that crosses a newline must be enclosed by its construct's `()`, `[]`, or `{}` delimiters. Where delimiters are otherwise optional, parentheses are the multiline form: `Circle impl Shape, Color` on one line, or `Circle impl (` followed by the interface list and `)` on later lines. A single-line parenthesized contract is rejected so the surface has one canonical spelling per layout.

## Compiler
1. Make memory size available to programmer. (see: Errors)
2. Single-pass compilation (two if needed) — decided by the compile-time priority.
3. Name mangling — internal symbols only; the C boundary is explicit and unmangled (see: C Interop).
4. Code generation: direct machine-code emission, single-pass (tcc-style) — fastest builds, full control; the optimization ceiling is accepted (see philosophy.md, Cautions #1). v1 target: AArch64 / Apple arm64 (reference machine is Apple Silicon); x86-64 is the second backend.
5. Declaration-before-use is the rule — names must be declared before use. Forward and mutual calls at top level are handled by fixups: emit a call to a placeholder, record the patch site, patch the real address at definition. No prototypes, no pre-pass.
6. Performance plan (v1, compile-time-cheap by design):
   - Peephole pass over emitted code — a pattern table rewriting dead store/load pairs, folding constants, removing redundant address computation. Near-free compile-time cost.
   - Loop-carried register promotion — after emitting a loop body, one linear backward scan promotes loop-carried values into registers. Bounded per loop; no global analysis.
   - Read-only parameters are alias facts — loads through them may be hoisted and cached freely by the emitter.
   - Vectorization path: `noalias` facts (Mutability #8) plus the `simd` stdlib module; auto-vectorization belongs only to a future release backend if ever added.

## Memory
Decision: manual management with explicit allocators (Zig/Odin school).
1. Explicit allocation/deallocation; no GC, no hidden runtime memory management.
2. Allocators are first-class and explicit — functions that allocate take an allocator parameter.
3. Safety comes from tooling, not language rules: checked/debug builds detect leaks, double-free, use-after-free.
4. Machine-true: sizes and layout stay visible to the programmer (see: Iterations, Errors).
5. A value lives on the stack unless explicitly allocated; allocation is the only path to heap memory, so stack vs heap is always statically known.
6. Pointer type is `*T` (`p: *Point`); field access auto-derefs (`p.x` ≡ `(~p).x`); full dereference is `~p`; `->` never exists. `*T?` is a nullable pointer — free (zero bits).
7. `~` is dereference, so NOT is `!` — logical on bools, bitwise on integers (Rust-style dual role). [provisional]
8. Address-of `&expr` exists for lvalues only (variables, `arr[i]`, `s.field`) and yields a read-only `*T` by default — unmarked reference = read-only, same rule as params; a write-capable form exists [spelling queued: `&mut q` vs `mut &q`]. Its C jobs are gone: out-params died to by-reference parameters and multi-return; call-site `&x` for mutation died to `mut` marks. What remains: aliasing stack values, interior/field pointers, and bridging stack values into `*T` signatures.
9. No pointer arithmetic on `*T` — use slices `[]T` (pointer + length) or an explicit raw-byte pointer type from the stdlib. [slice details carved with iteration]
10. Allocator is an explicit interface value: allocating functions take `a: Allocator`; no global default, no implicit allocation anywhere; arena/page/general-purpose are library types implementing the interface.
11. Allocation returns `*T?` — null is failure, checked by hand like every error. No abort-on-OOM.
12. Cleanup is explicit frees only — no defer (additive later per philosophy.md Considerations #1); the goto-for-cleanup question is parked under conditions & iteration.
13. `size_of(T)` builtin returns the static size — memory size visible in code (Compiler #1).

## Values & Passing
Decision: value semantics with explicit copies; references only at call boundaries.
1. Assignment copies — `q = p` gives `q` its own storage; mutating `q` never touches `p`.
2. Function/method parameters are passed by reference (unlike C) — no hidden copies at call boundaries; mutations are visible to the caller. To pass a protected copy, assign to a new variable first.
3. Plain declarations are stack storage; heap memory exists only via explicit allocation (see: Memory #5).
4. Copy-on-write is not a language semantic — assignment is a plain copy. COW, where wanted, is an explicit opt-in library type.
5. The compiler may elide copies when the result is observably identical (codegen freedom, not language semantics).

## Mutability
Decision: locals are mutable by default; mutation marking lives at reference boundaries.
1. Local variables are mutable by default; `const` is the rare opt-in. [keyword provisional]
2. Unmarked by-reference parameters are read-only borrows; `mut` on a parameter grants write access — visible in the signature, stated once. `mut` belongs to references (params, pointers), never to local declarations.
3. Per-parameter `const` does not exist — unmarked already means read-only. Function-level `const` does not exist — "writes nothing" is spelled "no `mut` in the signature." (A future opt-in purity annotation — no writes anywhere, globals included — remains possible; it would carry information the signature cannot.)
4. Rationale: local mutability is invisible to optimization (SSA); mutation through references is what blocks alias analysis. Syntax is spent only where the compiler and reader profit.
5. Const globals are the performance-relevant case: truly immutable data can live in read-only memory.
6. Mutating arguments carry one token at the call site (exact spelling decided with syntax) — mutation is greppable, and an unmarked call changes nothing you passed, so functions can be reasoned about without reading their bodies. Composes with method syntax: a mutating receiver is visible in the method signature.
7. Composition order of `mut` × `*T` × `T?` — each modifier answers one question, read left to right: `mut` = "writes are granted through this reference", `*` = "this is a reference", `?` = "the reference may be null". So: `*Node` read-only non-null; `mut *Node` writable non-null; `*Node?` read-only nullable; `mut *Node?` writable nullable.
8. `noalias` parameter marker: a signature-stated fact that memory reachable through one `noalias` parameter is not reachable through another (or through any `mut` parameter). The emitter may vectorize and reorder on it. Checked builds verify slice-range disjointness pairwise in O(1) at call boundaries — a false claim crashes in dev, never in prod. Release cost: zero. Signature facts are optimizer facts, same pattern as read-only.

## Null & Absence
Decision: nullability is tracked in the type; `null` is the universal literal. [Spelling `T?` provisional]
1. Plain `T` can never be null — the compiler and optimizer may assume non-null everywhere it appears.
2. `T?` admits exactly one extra value: `null`. One concept for pointers and value types alike.
3. `null` is the single spelling of absence across all optional types.
4. Zero runtime cost for non-nullable types. Pointer optionals are free (null = zero bits); value-type optionals carry a visible tag or a proven niche — the cost lives in the type, never hidden.

## Safety & Checked Builds
Decision: checks are a development tool; release trusts the developer.
1. Checked builds (debug): array bounds, integer overflow, allocator diagnostics (leaks, double-free, use-after-free) — all on.
2. Release builds: checks stripped; the programmer and the tests own correctness.
3. No Rust-style full safety: no borrow checker, no whole-program analysis — the compile-time priority forbids it.

## Errors
Decision: errors are ordinary values; checking is always explicit.
1. Functions return multiple values (e.g. value, memory size, error).
2. Errors are checked with branches the programmer writes — no hidden control flow, no exceptions, no propagate-operator that secretly returns.
3. Panic exists only as unrecoverable abort for bugs — never a catchable error channel.
4. Accepted cost: error checks add visible lines; nesting relief comes from early returns and explicit cleanup — no defer, no error sugar.
5. The error value is a rich explicit interface value (code, message, context — exact method set carved with stdlib shape). Any type implementing the interface can be an error payload.
6. Convention: error is the LAST return, as `Err?` — null means success; `if err != null` is the check. The `Err?` slot costs nothing on success (null = zero bits); the interface value materializes only on failure (cold path).
7. Constructing an error never allocates implicitly — payloads are static/stack data (codes, static strings) unless the programmer explicitly allocates. Keeps the no-hidden-allocation invariant.
8. Multi-return consumption is typed destructuring — `n: int, err: Err? = read()` — the `name: type` rule applied to multiple bindings; plain reassignment `n, err = read()` afterwards.

## Type System
Decision: fully explicit types; no inference.
1. Every declaration and signature states its type — locals included. No infer-and-declare operator.
2. Zero inference is the fastest to compile, the most greppable, and the most machine-true; verbosity is the accepted cost.
3. Local inference could be added later if ever wanted (see philosophy.md, Considerations #1).
4. Type aliases: `Name: type = Existing` — transparent, zero-cost notation; the alias IS the type, fully interchangeable. Generic aliases allowed: `Pair[T]: type = (T, T)`.
5. Distinct types: `Name: distinct Existing` — same layout, new nominal type. `Name(value)` constructs (constructor position is free via the namespace split); distinct types do not mix with their base or each other without explicit conversion. Zero runtime cost — pure compile-time discipline for units, handles, and IDs.

## Modules
Decision: module = file; explicit imports; explicit exports.
1. Each file is a module and lists exactly what it imports; import statements sit at the top of the file, before any declaration — reading order is stated up front for the single pass.
2. Members are private unless marked `pub` on the declaration line — `pub max(a: int): int`; the mark lives with the member and cannot drift; one grep finds every public member. Applies to types, functions, and globals alike.
3. Single-pass compiles imports in dependency order, reading only what is imported — nothing scanned, nothing implicit. **Import cycles are a compile error** (a cycle has no first file; structure, not policy).
4. Incremental rebuild granularity: the file.
5. Imports are whole-module and qualified: `import math`, then `math.sqrt(x)` — provenance visible at every use site; nothing enters the local namespace. Collisions solved by aliasing (`import m: math` [spelling provisional]); path form for nested files (`import math: "std/math"` [provisional]).

## C Interop
Decision: explicit extern/export boundary; Ace internals stay free.
1. Ace-internal symbols may be mangled (multi-return, mut params, modules) — mangling is private to the compiler.
2. `extern "c" { … }` blocks import C declarations — the ABI stated once per block; variadics (`…`) are legal only inside extern blocks (Functions & ABI #7). Export: an `export` modifier on the declaration produces a C-ABI function with an unmangled name. [spellings provisional]
3. C-facing functions must use C-compatible shapes; multi-return and mut parameters stay internal-only.
4. Ace never depends on C — interop is a declared boundary, not a foundation.

## Abstraction
Decision: composition over inheritance; dispatch is explicit.
1. Encapsulation = module privacy (see: Modules) — the file is the unit of hiding, not a class.
2. Abstraction + dynamic polymorphism = explicit interface values (vtable + data, constructed and visible). Signatures are contracts: explicit types, explicit exports.
3. Implementation reuse = struct embedding at a known offset + type-owned methods. A method declaration is `Circle.area()`; its implicit `self` receiver is read-only unless the declaration begins with `mut`. Visibility precedes mutation (`pub mut Circle.scale(k: i32)`).
4. Inheritance is rejected: subtyping is served by interfaces, implementation reuse by composition.

Carved (interface value layout):
5. An interface value is two words: a pointer to a const vtable (read-only memory) plus a data pointer. `Shape?` is valid and free — null vtable = null interface.
6. A vtable is a struct of function values typed by signature — no new type concept.
7. Each struct has at most one canonical interface contract, declared in the struct's defining module after all named interfaces in its list are declared: `Circle impl Shape, Color`. A second `Circle impl` declaration is an error, as is repeating an interface in the list. A multiline interface list uses parentheses. The contract is checked at end-of-file so its type-owned methods may follow it. Methods may exist without a contract.
8. A contract registers one exact concrete/interface pair per listed interface. Construction names the target interface explicitly: `s: Shape = Shape(&c)`. The compiler looks up the concrete type's one contract and emits one const vtable per exact used pair. There is no named impl value, alternate implementation, open-ended search, runtime lookup, or trait solving.
9. Interface methods are plain signatures with the receiver implied. Conformance matches method name, receiver `mut`, exact parameter types, and return type. Methods may overload by exact parameters and receiver mutation; return types must match a requirement but never distinguish or select overloads.
10. Calls are `c.area()` and `mut c.scale(2)`; direct disambiguated calls are `Circle.area(c)` and `Circle.scale(mut c, 2)`. `Circle.area` by itself is not a first-class method value. Dispatch through an interface calls the selected vtable slot with `s.data` and retains the same visible `mut` call-site rule.
11. `Shape(&c)` does not mutate `c`; a later `mut s.scale(2)` is the explicit mutation. Const remains absolute: an interface constructed over const backing storage may be read through, but mutating dispatch through it is a compile error. This property is compile-time-only and adds no runtime word or check.
12. Contracts and methods are private by default. `pub Circle impl Shape, Color` exports every conformance in that canonical list; `pub Circle.area()` exports a method for direct external calls. A public contract may use private methods internally.
13. Generic contracts ride the bracket rules: `Box[T] impl Value[T]`. Both sides are explicitly instantiated at use (`Value[int](&box)`), and each used exact pair gets its own stamped vtable. The one-contract rule applies to the nominal generic struct; per-instantiation/conditional contracts are rejected.

## Generics
Decision: explicit monomorphization.
1. Generic functions and types exist; instantiation is explicit — types named at the use site.
2. No trait solving, no comptime reflection, no implicit instantiation — each instantiation compiles as plain stamped code.
3. Runtime-perfect specialization; duplication solved; compile cost bounded by what is actually written.
4. Type parameters ride brackets: `max[T](a: T, b: T): T`; generic types likewise: `Vec[T]: struct { items: []T, cap: int }`.
5. Instantiation always names its types — `max[int](3, 9)`, `sort[Point](pts)` — no deduction from arguments, ever; the no-inference rule (Type System #1) covers type arguments too.
6. Instantiated function values type cleanly: `cmp: (int, int): int = max[int]`.
7. No constraint syntax exists — T is unbounded. Checking happens per instantiation: the body must typecheck for the stated types; errors surface at the instantiation site.
8. Each distinct type-argument tuple stamps a separate plain function/type — stamping is bounded by written instantiations.

## Functions & ABI
1. Multi-return — decided (see: Errors). Returns are positional: types only, `(int, Err?)`; names appear at destructuring, never in signatures.
2. Multi-returns are laid out as anonymous structs: small ones ride platform registers, larger ones use the hidden return pointer — plain platform struct-return rules.
3. `mut` parameters pass the caller's actual address (writes visible to the caller); read-only parameters pass addresses too, but small ones may travel by value — identical observable behavior, legal codegen freedom (same class as copy elision).
4. Internal calling convention: the platform convention (SysV, AArch64, …) — debuggers, profilers, tracers, and the C boundary work unchanged.
5. Parameter aliasing: parameters may alias unless proven otherwise; checked builds may test overlap; read-only marks are the optimizer's facts.
6. Export mapping: `mut p` → `T*`, read-only → `const T*`, single C-compatible return; multi-return stays internal-only (see: C Interop #3).
7. No user variadics in v1 — pass slices explicitly; C variadic functions (printf) import via extern blocks only. Slice-sugar variadics are additive later if ever wanted.

## Names & Scoping
1. Qualification is the namespacing mechanism — modules provide what C fakes with prefixes: `parser.init()` instead of `ace_parser_init`; import aliasing resolves module-name collisions.
2. Two namespaces, separated by grammar position: **types** (type positions: after `:`, in brackets, casts) and **values** (expression positions). A type and a value may share a name — `Point: struct` + `Point(...)` constructor coexist. Position decides; decidable single-pass, no inference.
3. The value namespace holds functions, variables, and imports together. Variables and imports still require a unique same-scope name; functions may form an overload set under one name. A function name cannot share its scope with a variable or import.
4. Free-function overload selection is exact and static: argument count, argument types, and `mut` parameter/call-site marks must match one candidate. No implicit conversions, conversion ranking, inheritance conversions, argument-dependent lookup, or return-type-directed call selection exist. Exactly one match is required; zero matches and multiple matches are compile errors.
5. Return types do not distinguish overload declarations. Two declarations with the same parameter types and `mut` positions are duplicates even when their return types differ. `noalias` does not distinguish them because it changes optimizer facts, not the callable shape.
6. Untyped constants remain context-materialized for an ordinary single-signature call. When a real overload set requires selection, an integer/character constant has the natural `int` candidate type and a floating constant has `f64`; an explicit cast selects a narrower or unsigned overload. String and boolean literals already have fixed types. No candidate receives a private expected type during selection.
7. Generic overloads remain explicitly instantiated. `f[int](...)` considers only generic declarations named `f` with the stated type-argument count, substitutes those written types, and then applies the same exact argument rules. A bare generic is never inferred.
8. An overload used as a function value is selected by the explicitly declared function-value type, including its return type; a bare overloaded name without that context is an error. This is contextual selection of a value, not return-type overloading at a call.
9. Overloading is compile-time only. The selected internal function has a parameter-signature-mangled symbol and calls it directly: no runtime tag, branch, vtable, boxing, allocation, or metadata is introduced. C-boundary `extern`/`export` names cannot overload because those names are explicitly unmangled.
10. Shadowing: nearest binding wins — locals may shadow top-level names and imports; checked builds may warn on import shadowing. A shadowed function miscalled yields a clear "not callable" error.
11. Type-owned method overloads use the same exact static rule as free functions, with receiver type and receiver `mut` included in the parameter signature. Ambiguous method names through embedding are compile errors and can be resolved with a direct type-qualified call. Methods are not first-class values in v1.
12. Rejected: bare-name fallback resolution (`use(sort)` picking whichever function/variable exists) — meaning would depend on the declaration set, the C++ lookup disease. Same-scope non-function value collisions are a rename.
13. Future valve, if ever needed: a dedicated function-reference sigil (`&sort` = code address) enabling a separate function namespace — additive, not v1.

## Inline Assembly
1. `asm "instruction text" { bindings }` — a statement, never an expression; values flow only through the bindings.
2. Bindings name real registers directly: `in: x0 = fd` wires an Ace value in, `out: x0 = ret` wires a register out to an Ace local, `clobbers: x16, x17` declares what breaks. Nothing implicit — the compiler emits plain moves around the block. No constraint language exists.
3. asm statements are never reordered or elided; clobbers are honored by surrounding codegen.
4. The ISA is implied by the compilation target (v1: AArch64).
5. Memory written through asm is the programmer's trust — checked builds do not trace it.

```c
ret: i64
asm "svc #0" {
    in:  x0 = fd, x16 = 3     // Apple syscall convention
    out: x0 = ret
    clobbers: x16, x17
}
```

## Stdlib
Decision: minimal, allocator-explicit, nothing ambient.
1. `Err` method set: `code(): int`, `message(): string` — static strings only (no implicit allocation); richer context is additive later.
2. Allocator interface (raw, on byte slices): `alloc(size: int, align: int): []u8?`, `free(data: []u8)`, `resize(data: []u8, new_size: int): []u8?` — resize may grow in place or move; the returned slice is authoritative.
3. Typed sugar — library code, not language: `create[T](a: Allocator): *T?`, `destroy[T](a: Allocator, p: *T)`, `make[T](a: Allocator, n: int): []T?`.
4. Allocator implementations are values you construct — `page` (OS-page-backed), `arena` (bump), `gpa` (debug, leak-tracking; powers checked-build diagnostics). No global default exists anywhere.
5. Import resolution: bare name = stdlib only (`import math`); local files always by path (`import util: "./util.ace"`). Shadowing is impossible by construction.
6. Platform access: through the OS system library via `extern "c"` (macOS: libSystem) — the language does not depend on C; this is the OS front door.
7. `string` is a distinct view over bytes (`[]u8`, UTF-8 by convention) — a slice, no new machinery.
8. v1 contents — six modules: `mem` (Allocator, page/arena/gpa, typed sugar), `sys` (OS calls), `fmt` (print/format), `math` (sqrt, `abs[T]`, `min[T]`/`max[T]`, …), `str` (byte/string operations), `simd` (`f32x4`, `i8x16`, arithmetic and shuffles emitting NEON directly — explicit vectorization, zero compiler analysis).
9. Output API begins as an exact overload set. `fmt.print(text: string): int` writes the supplied bytes exactly; `fmt.print(value: int): int` and `fmt.print(value: usize): int` write signed platform integers and memory-sized unsigned values in base-10 ASCII. None adds a newline or allocates; numeric conversion uses a fixed 20-byte stack buffer and covers each type's complete range. Every overload retries partial writes and returns bytes written, or the non-positive result from the current OS boundary. Other numeric overloads, `fmt.println`, and formatted `fmt.printf` are additive follow-ups. Richer `Err` integration comes with the error layer rather than blocking output.

## Pre-Implementation Checklist
Semantics are complete; these are the implementation-level defaults (derived from every sample written so far) and the few remaining `[?]` decisions. Resolve `[?]` items in the first compiler session, then: lexer → recursive-descent parser → direct AArch64 emitter.

Recorded defaults (from samples, now normative):
1. Source files are UTF-8; newline terminates statements — no semicolons; `//` line comments and `/* … */` block comments (non-nesting; a newline inside a block comment acts as a statement terminator). [comment forms decided in implementation]
2. Keyword inventory (v1): `import pub mut const if else for in switch return break continue asm extern export interface struct impl distinct type enum null true false` — plus builtin functions (`size_of`), not keywords. Primitive type names are predeclared identifiers, not keywords.
3. Primitive types: `bool`, `i8 i16 i32 i64`, `u8 u16 u32 u64`, `f32 f64`, `string`, `usize`, `typeid`, plus the platform words `int` (= i64 on arm64) and `uint` (= u64 on arm64); C-ABI aliases (`c_int` …) inside extern blocks only.
4. Casts are explicit and constructor-shaped: `i32(x)`, `Meters(x)` — no implicit numeric conversions; `null` assigns to any `T?`. Literals are untyped constants: an integer/float/char literal takes the expected type at each use site and must fit (range-checked); no other implicit conversions exist. [literal rule decided in implementation]
5. `if`/`switch`/`for` are statements, never expressions; conditions are `bool` only (no integer-as-bool).
6. Operator set follows C (`+ - * / % == != < <= > >= && || & | ^ << >>` unary `- ! ~ &`); precedence as C; no `++`/`--`.
7. Array literals `[1, 2, 3]`; struct literals named `{ x: 0.0 }` (positional when unambiguous). [provisional]
8. Struct layout: declaration order, natural alignment padding, no reordering.

Compiler-session decisions:
- [x] `int`/`uint`: **platform word** — `int` = i64, `uint` = u64 on arm64 (each target picks its word; explicit widths always available).
- [x] Compound assignment `+= -= *= /= %= &= |= ^= <<= >>=`: **included as pure sugar** — `x += y` ≡ `x = x + (y)`, left operand evaluated once.
- [x] Enum: **yes** — `Name: enum { a, b }` as distinct-typed int constants; `enum` is a keyword; no tagged unions in v1 (interface values cover sums).
- [x] `main` shape: **`main(): int`** — the source-level entry uses Ace's platform-word integer (`i64` on arm64), like ordinary application code. The platform entry ABI consumes the low 32 bits as the process exit status (`return 0` = success); on AArch64 both travel in `x0`, so this needs no extra instruction. Owner-approved revision from the earlier `i32` shape.
- [x] Linking: **emit Mach-O objects, link via the system toolchain** (`ld`/`clang` driver) for v1. libSystem externs work day one; direct executable emission stays additive later.
- [x] Debug info: **yes, minimal DWARF line tables in v1** — `.debug_line` provides file + line per PC range; only the structural compile-unit/function DIEs required for Darwin linker discovery accompany it. No variable/type DIEs, no unwind tables. lldb/atos resolve Ace source from day one.
- [ ] Queued language additions (not v1-blocking): `volatile`/`packed`/`align` attributes for OS work.

## Conditions
1. Chained comparisons: `lo <= x <= hi` — each operand evaluated once; no boolean-coercion trap (C's `a < b < c` meaning `(a<b)<c` is dead); lowers to two compares and a branch.
2. Multi-way branch: a `switch`-style construct — one value, many comparisons, no fallthrough. [spelling provisional]
3. Flow narrowing: inside `if p != null` (or `== null`), `p` is usable as `T` — flow facts from checks the programmer wrote; declared types never change, so this is checking, not inference. Same for `*T?` and `Err?`.
4. Cleanup jumps: blocks can be labeled, `break label` exits any enclosing block — structured single-exit cleanup on error paths. **goto does not exist.**

## Iterations
1. Memory-size-based iteration is realized as slices: `for b: u8 in buf` walks exactly `buf.len` bytes — pointer + length, nothing else.
2. `for-in` is the only loop form; it lowers to a plain index loop — no iterator interface, no hidden machinery, zero cost.
3. Element types are stated explicitly: `for i: int in 0 .. n` (ranges), `for b: u8 in buf` (slices/arrays).
4. Ranges: `a .. b` exclusive by default; reverse and step forms available. [spelling provisional]
5. Arrays `[N]T` are inline values; slices `[]T` are pointer + length; slicing expressions like `arr[1..n]` produce slices. [expression spelling provisional]
6. `break`/`continue` as usual; labeled `break` exits outer loops — the structured early-exit path.
7. A `for-in` loop fixes its pointer and length at loop entry — mutations to the slice variable inside the body do not affect the running loop (element mutation through the loop is fine). Consequence: the compiler provably controls every index, so checked builds need no per-iteration bounds checks inside `for-in`.
8. Implementation guidance: emit `for-in` as a pointer-bump loop (pointer + precomputed end, post-increment addressing) — the closed loop form is the optimization contract; straight loops should reach C-parity in v1.

## Decision Queue
Closed (philosophy phase):
- [x] Why Ace exists; priority stack: performance > compile time > safety > simplicity.
- [x] Memory management: manual + explicit allocators; safety via checked builds, not language rules.
- [x] Value semantics: assignment copies; parameters by reference; plain declarations are stack; COW is library-only.
- [x] Null: tracked in the type (`T?`), `null` is the universal literal, non-nullable `T` costs nothing.
- [x] Mutability: locals mutable by default (`const` opt-in); params read-only by default (`mut` grants write — visible in signature and call site).
- [x] Safety: checked builds on in debug (bounds, overflow, allocator diagnostics), stripped in release; no borrow checker, no whole-program analysis.
- [x] Errors: values + explicit manual checks; no exceptions, no propagation sugar; panic = abort only; multi-return confirmed.
- [x] Type system: fully explicit — no inference anywhere; verbosity is the accepted cost.
- [x] Modules: module = file, explicit imports, explicit exports; single-pass reads only what is imported.
- [x] C interop: explicit extern/export boundary; mangling internal-only; Ace never depends on C.
- [x] Abstraction: composition over inheritance — module privacy, explicit interface values, embedding + method sugar; inheritance rejected.
- [x] Generics: explicit monomorphization — types named at use site; no trait solving, no comptime.
- [x] Backend: direct machine-code emission, single-pass; optimization ceiling accepted.
- [x] File extension: .ace.

Carving phase — closed so far:
- Declarations & signatures: `name: type` everywhere, keywordless, trailing returns, signature-typed function values, no pointer-to-function syntax.
- Memory & allocator API: `*T` pointers (`~` deref, auto-deref fields, `&` address-of), explicit allocator interface (no default), `*T?` on OOM, explicit frees (no defer), `size_of`.
- Error conventions: `Err?` last in multi-return (null = success); rich explicit interface value, no implicit allocation on failure; typed destructuring `n: int, err: Err? = read()`.
- Conditions & iteration: chained comparisons `lo <= x <= hi`; `switch` multi-way branch (no fallthrough); flow narrowing from explicit checks; labeled `break`, no goto; `for-in` only, lowering to index loops over pointer+length; arrays `[N]T`, slices `[]T`.
- Function ABI: multi-return = anonymous struct per platform struct-return rules; positional returns; platform calling convention; `mut` params pass real addresses, small read-only params may pass by value; no user variadics (extern C only).
- Generics syntax: brackets `max[T](a: T, b: T): T`; instantiation always explicit `max[int](3, 9)`, never deduced; no constraint syntax — unbounded T, checked per instantiation.
- Interface value layout and contracts: two words (const vtable pointer + data pointer); one canonical `Circle impl Shape, Color` list per struct (parenthesized when multiline); type-owned methods with implicit `self`; explicit `Shape(&c)` construction; exact overload-aware conformance; `mut` visible on declarations and calls; const backing cannot be mutated.
- Import/export: `pub` prefix on declarations; whole-module qualified imports (`import math` → `math.sqrt`); aliasing for collisions; `extern "c" { … }` blocks (variadics only there) and `export` modifier for the C boundary.
- Names & scoping: qualification as namespacing; type/value namespaces split by grammar position (type + constructor may share a name); variables/imports remain unique while free functions may form exact-signature overload sets; nearest-binding shadowing; ambiguous embedding sugar is an error.
- Inline assembly: `asm "…" { in/out/clobbers }` with registers named directly — no constraint language; statement only; never reordered; ISA implied by target (v1: AArch64/Apple arm64).
- Stdlib: Err = {code, static message}; raw allocator interface on byte slices + typed sugar `create[T]`/`destroy[T]`/`make[T]`; page/arena/gpa as constructed values; bare import = std, path import = local; OS via extern "c" system library; v1 = mem, sys, fmt, math, str.

**Carving phase: complete.** Open semantic items: none.

Implementation phase — closed so far:
- [x] Lexical/layout rules: `/* … */` block comments (non-nesting; a newline inside acts as a statement terminator); newline token emitted only after ident/literal/`)`/`]`/`}`/`?`/`return`/`break`/`continue` (Go-style, made explicit); bare comma lists are parser-restricted to one physical line, while delimited multiline lists use trailing commas; `else` stays on the `}` line.
- [x] `int`/`uint` = platform word (i64/u64 on arm64), kept alongside explicit widths.
- [x] Compound assignment operators included as pure sugar (`x += y` ≡ `x = x + (y)`, left operand evaluated once).
- [x] Enum `Name: enum { a, b }` — distinct type + distinct-typed int constants; `enum` keyword; no tagged unions in v1.
- [x] Semantic model (implementation): literals are untyped constants (fit-checked, never converting between kinds); enum layout = i32; interface values = 16 bytes (vtable + data); optionals free where a niche exists (pointer/slice/interface/string), value optionals carry a visible tail tag; mut-parameter write access extends through the reference even when the pointer type is unmarked.
- [x] `main(): int` — entry returns the platform-word integer; the target entry ABI takes its low 32 bits as the observable exit status. Rationale: ordinary Ace code uses the natural platform type, while the boundary mapping is localized and free on AArch64.
- [x] Linking: Mach-O objects + system linker for v1 — rationale: extern "c" to libSystem works immediately, compile-time stays fast, direct executable emission remains an additive later step.
- [x] DWARF: minimal `.debug_line` line tables in v1 — rationale: crash PCs resolve to Ace source from day one; everything richer (DIEs, unwind) is additive.
- [x] Free-function overloading: same-name functions may differ by exact parameter types/count and `mut` positions; returns and `noalias` do not distinguish declarations. Calls resolve statically with no conversions or ranking; overload literals default to `int`/`f64`, generic overloads still require explicit type arguments, contextual function-value types may select one overload, and C-boundary names cannot overload. Runtime cost is zero beyond the selected body. Method-sugar/interface participation remains separately queued.
- [x] Integer output: `fmt.print(int): int` is the allocation-free decimal overload beside `fmt.print(string)`. It formats through a 20-byte stack buffer, including `INT_MIN`, performs no implicit newline, retries partial writes, and preserves the existing bytes-written/non-positive-result contract.
- [x] Memory-size output: `fmt.print(usize): int` is the allocation-free unsigned decimal overload for lengths, sizes, and indices. It uses a 20-byte stack buffer, covers zero through `USIZE_MAX`, performs no implicit newline, retries partial writes, and preserves the existing result contract.
- [x] General module loader: bare and aliased imports resolve to any standard-library module beside the compiler; `import u: "./util.ace"` loads local files by a path relative to the importing source (relative-only, `.ace` required). Module identity is the canonical file path, so one file loads exactly once through any number of imports or aliases; dependencies load depth-first in declaration order and are merged ahead of their importers, preserving declaration-before-use in the single pass. Import cycles are compile errors showing the full chain. Qualified references lower to module-private prefixed names (`name$member`; `$` cannot appear in source identifiers) after a `pub` check, covering functions, overload sets, globals, types, casts, and `Type.member` enum constants. Type-owned methods derive their private identity from the renamed owner; method and contract visibility is enforced at cross-module use sites. Extern and `export` declarations inside modules keep their C-boundary linker spelling in the node while their Ace-side names stay module-private. Multi-file DWARF line tables keep each function's source file and rows. Separate per-module objects and incremental rebuilds remain deferred.
- [x] Type-owned methods and canonical interface contracts: implicit read-only/`mut self`, visibility-first modifiers, exact method overloads, direct qualified calls, no method values, one multi-interface contract per struct, compact single-line and parenthesized multiline contract lists, explicit interface construction, overload-aware EOF conformance, const-backing protection, cross-module visibility, and per-used-pair generic vtables. Old named impl values/mapping blocks are removed.

Provisional spellings (semantics fixed, spellings may be adjusted during implementation without a decision):
- `const` keyword (locals/globals opt-in); `!` dual-role NOT; `T?` optional postfix; `mut` call-site token form; slicing expressions (`arr[1..n]`); range forms (`a .. b`, reverse, step); switch-case syntax; import alias/path forms; struct literal form; Err/vtable member spellings (`s.vt`).

Syntax details surfaced by the first code sample (decide during related topics):
- Composition order of `mut` × `*T` × `T?` (`mut *Node?` — what exactly is mutable?).
- Bitwise NOT spelling (`!` dual-role) — provisional under Memory #7.
- Optional narrowing — decided: flow narrowing (Conditions #3).
- Array/slice spellings — settled: `[N]T`, `[]T` (Iterations #5); slicing expression spelling provisional.
- Multi-return destructuring — decided: typed destructuring `n: int, err: Err? = f()` (Errors #8).
- Generic instantiation spelling — decided: brackets `max[int](3, 9)` (Generics #4–5).
- Call-site `mut` × method sugar — decided: `mut c.scale(2.0)`; interface methods mark mutating receivers on the method (Abstraction #9).
