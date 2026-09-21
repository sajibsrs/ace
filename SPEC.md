# Ace — Language Specification

Concrete rules and syntax. The why behind every rule lives in PHILOSOPHY.md (closed). The formal grammar for this surface lives in GRAMMAR.md. This file is carved topic by topic and is the single ledger of language rules and decision records (rule + rationale); implementation history lives in CHANGELOG.md.

## Source
1. File extension: **.ace** (decided — `.ac` rejected: autoconf collision).
2. Source is UTF-8. Newlines terminate statements; semicolons appear only
   between clauses of a general `for` head. `//` comments end at newline and
   `/* ... */` comments do not nest.

## Syntax & Parsing
1. Fast, simple parsing is the goal — single-lookahead LL(1) is the guideline, not a requirement; small bounded lookahead is acceptable where it buys readability.
2. Recursive descent top-down parsing implementation.
3. Storage declarations state `name: type` everywhere (globals, locals, fields, parameters, returns, and multi-declarations). The only type-leading top-level forms are explicitly qualified methods (`Circle.area`) and a concrete type's canonical signing (`Circle signs Shape`); both are bounded-lookahead declarations, never statements. No `var`/`let` keywords or C typedef lexer hack is needed.
4. Function declarations are keywordless with trailing returns (`fu(a: int): int`); distinguishable from calls by bounded lookahead.
5. Function values are typed by signature — `(int, int): int` — C pointer-to-function syntax never enters the language.
6. Tuple expressions are comma-built at the top level: `return words, null` ≡ `return (words, null)` — parentheses are grouping, required only for nesting (`return (a, b), err`) or clarity. Same rule on assignment sides.
7. Bare comma-separated lists stay on one physical line. A list that crosses a newline must be enclosed by its construct's `()`, `[]`, or `{}` delimiters. Where delimiters are otherwise optional, parentheses are the multiline form: `Circle signs Shape, Color` on one line, or `Circle signs (` followed by the contract list and `)` on later lines. A single-line parenthesized contract is rejected so the surface has one canonical spelling per layout.
8. An array literal may repeat one value with `[value : count]`, as in `prefix: [12]u8 = [u8(0) : 12]`. The surrounding context still supplies the exact array type; `count` is a compile-time constant expression (Syntax & Parsing #15) and must evaluate to that array type's length. `value` is evaluated once and copied into every element in index order. The form allocates nothing and introduces no shared element storage.
9. Every storage declaration states its type explicitly. There is no declaration-elision spelling: a local initialized from an expression is `name: Type = expr`, and every name introduced by a multi-declaration states its own type. Rationale: one declaration rule works uniformly for literals, aggregates, null values, and single- or multi-return expressions while making the programmer's expected type locally visible and independently checkable against the initializer.
10. Numeric and character literals are untyped constants that take the expected
    type at use and must fit it. Casts are explicit and constructor-shaped
    (`i32(x)`, `Meters(x)`); there are no implicit numeric conversions. An
    untyped constant expression (`1 + 2`, `-0.5`, `1 << 3`) takes the expected
    type as a whole, beside a typed operand as in a declaration; an integer
    expression never becomes floating-point arithmetic (`x != 7 / 2` with `x: f64`
    needs a cast), while a lone integer literal may still meet a float operand.
    Under a float cast, an untyped floating constant expression takes the
    cast's target type (`f32(1.0 / 3.0)` computes at `f32`), and an untyped
    integer constant expression is evaluated as `int` and then converted.
11. A tuple expression may initialize or assign one explicitly stated tuple
    type of the same arity when every member expression independently resolves
    to the corresponding member type. Standalone integer, float, and character
    members therefore use their ordinary `int`, `f64`, and `u8` defaults before
    the tuple is compared with its destination; no tuple-wide contextual typing
    or inference occurs. Write casts where a different member type is required:
    `pair: (i32, i32) = (i32(1), i32(2))`. Construction uses the existing
    positional tuple layout, performs no allocation, and introduces no new
    runtime machinery.
12. `if`, `switch`, and `for` are statements, not expressions, and their
    conditions require `bool`. Compound assignments are pure sugar with the
    left operand evaluated once; `++` and `--` do not exist.
13. A struct field may opt into a compile-time default with `name: Type =
    expression`. A named-field struct literal supplies a field value with
    `name = expression`; `:` is reserved there for declarations and type
    annotations and is rejected as a field-initializer separator. A literal may
    omit exactly the fields with
    declared defaults; every field without one remains required, and `T {}` is
    valid only when every field of `T` has one. An explicit initializer wins.
    Unknown and duplicate fields remain errors, and there is no positional,
    spread, or update literal. Each default is independently checked against
    its declared field type and may use only the existing constant-expression
    surface; it cannot call a function or refer to `self` or another instance
    field. Defaults apply only when constructing a struct literal: they do not
    initialize bare locals, globals, allocations, arrays, or canonical-zero
    values and do not affect layout or ABI. Generic defaults neither infer type
    arguments nor instantiate a generic implicitly; when generic struct
    literals are implemented, substitution follows an explicitly written
    instantiation. Rationale: omission explicitly authorized by the type author
    removes repetitive construction without hidden work or weakening definite
    initialization, while per-field opt-in keeps required state visible. `=`
    consistently supplies a value, while `:` consistently associates a name
    with a type.
14. A structured error domain keeps one entry per physical line inside its
    braces, with no commas: `code: name = "message"`. The opening brace is
    followed by a newline unless the block is empty. Rationale: error entries
    are declarative records rather than an expression list; a line-oriented
    form stays easy to scan and parse without redundant separators.
15. A fixed array type's length `[N]T` is a compile-time constant expression,
    not only a bare integer literal: literals, `const` identifiers (including
    one reached through an imported module, Values & Passing #12),
    `sizeof`/`alignof`/`offsetof`, and integer/bitwise arithmetic over these —
    the same restricted surface `@align(N)` already uses (Type System,
    Carved). The evaluated length must be a positive `usize`; a runtime
    value, a non-positive result, or an expression the compiler cannot
    reduce is a compile-time error. An array literal's repetition count
    (Syntax & Parsing #8) uses the identical surface and must match the
    destination array type's length exactly. Rationale: a compile-time
    constant identifier is exactly as fixed and safe as a literal, so a
    literal-only grammar bought no safety — only friction for named sizes
    shared across declarations (`INPUT_SIZE: const usize = 8192` backing
    `input: [INPUT_SIZE]u8`).

## Compiler
1. Make memory size available to programmer. (see: Errors)
2. Single-pass compilation (two if needed) — the simplest modular compiler architecture that preserves fast builds and direct emission.
3. Name mangling — internal symbols only; the C boundary is explicit and unmangled (see: C Interop).
4. Code generation: direct machine-code emission, single-pass (tcc-style) — fastest builds, full control; the optimization ceiling is accepted (see PHILOSOPHY.md, Cautions #1). v1 target: AArch64 / Apple arm64 (reference machine is Apple Silicon); x86-64 is the second backend.
5. Declaration-before-use is the rule. Forward and mutual calls, including method calls on a struct, are handled by fixups: emit a call to a placeholder, record the patch site, and patch the real address at definition. A second narrow fixup class permits an as-yet undeclared type name only when the complete path from the field to that name consists of one or more pointer levels (`*Later`, `**Later`), or is a bare slice exactly at the field/variant root (`[]Later`) — a slice's representation is a fixed pointer-plus-count pair whose size and alignment, like a pointer's, do not depend on its element type. The slice allowance does not extend through or past the slice: a slice reached via a preceding pointer (`*[]Later`) or nested inside another compound stays ordinary declaration-before-use, while a pointer reached via a preceding root slice (`[]*Later`) still forwards, since it's the same pointer fixup applying once the slice has already handed it a root-started path. The exact nominal type must be declared later in the same module, and any field access, method use, inline containment, array element, union payload, alias expansion, or other operation requiring its layout remains declaration-before-use. Unresolved type placeholders are errors at end of file. No prototype or general pre-pass exists.
6. Performance plan (v1, compile-time-cheap by design):
   - Emission-time selection — the emitter picks fused forms where it already knows operand lifetimes: compare-and-branch, loads into their destination, frame-relative field access, direct use of promoted registers, add/sub/compare immediates, and results computed straight into a promoted destination. No separate pass over emitted code.
   - Register promotion — before emitting a function, one linear pass over its body selects scalar locals whose address never escapes, weighted by use count × loop depth, into callee-saved registers. A function that makes no clobbering call first uses caller-saved registers above its expressions' estimated temp need, which cost no save. Bounded per function; no global analysis.
   - Read-only parameters are alias facts — loads through them may be hoisted and cached freely by the emitter.
   - Vectorization path: `noalias` facts (Mutability #8) plus the `simd` stdlib module; auto-vectorization belongs only to a future release backend if ever added.
7. The source entry point is `main(): int`; on Apple arm64 the platform consumes
   its low 32 bits as the process exit status.
8. The compiler emits Mach-O objects and uses the system toolchain for executable
   linking. Minimal DWARF provides source line tables and only the structural
   records required for discovery; variable/type records and unwind data are not
   emitted.

## Memory
Decision: manual management through one fixed process-wide allocator strategy (C school, with explicit failure).
1. Explicit allocation/deallocation; no GC, no hidden runtime memory management.
2. The allocator is not a value, contract, parameter, or selectable policy. The public raw API is the standard-library module `mem`: `mem.alloc`, `mem.realloc`, `mem.release`, and `mem.alloc_size`. There is no `Allocator` abstraction, allocator argument, implicit allocation, or unqualified predeclared allocation function.
3. The release-mode implementation stays at C cost: it uses the platform allocation boundary and carries no Ace header, guards, registry, quarantine, or other per-allocation diagnostic metadata. Checked builds add a live-allocation registry for invalid/double release and leak diagnostics; detecting arbitrary reads or writes through a dangling alias requires separate access instrumentation and is not promised by the allocator alone.
4. Machine-true: sizes and layout stay visible to the programmer (see: Iterations, Errors).
5. A value lives on the stack unless explicitly allocated; allocation is the only path to heap memory, so stack vs heap is always statically known.
6. Pointer type is `*T` (`p: *Point`); unary `*p` explicitly dereferences a pointer, while infix `*` remains multiplication and `*T` remains the pointer-type spelling. Field access auto-derefs (`p.x` ≡ `(*p).x`); `->` never exists. A pointer's all-bits-zero value is `null`; there is no separate optional-pointer type.
7. Unary `!` is logical negation and accepts only `bool`; unary `~` is bitwise complement and accepts only integers. Rationale: distinct operators keep pointer access, Boolean logic, and integer bit manipulation visible and type-specific while retaining the familiar machine-level spellings.
8. Address-of exists only for lvalues (variables, `arr[i]`, `s.field`, and dereferences). `&expr` yields a read-only `*T`; `mut &expr` yields a writable `mut *T` and is rejected for const or otherwise read-only storage. Both emit the same machine address: `mut` is a compile-time permission with zero runtime representation or cost. Its C jobs are otherwise gone: out-params died to by-reference parameters and multi-return; a `mut` parameter supplies the outer reference needed to replace caller storage. What remains: aliasing stack values, interior/field pointers, bridging storage into pointer signatures, and explicit writable-pointer construction.
9. Typed pointer arithmetic is deliberately narrow. For `p: *T` or
   `p: mut *T` and `n: usize`, `p + n` and `p - n` offset the stored address by
   `n * sizeof(T)` bytes and preserve the exact pointer type, including its
   read/write permission. Untyped nonnegative integer literals materialize as
   `usize` in the offset position. The corresponding `+=` and `-=` forms are
   valid only when the pointer binding itself is assignable; they grant no
   additional pointee permission and remain subject to the ordinary outer
   `mut` parameter rules. `T` must have a complete, nonzero-sized layout.
   Checked builds diagnose a null starting pointer, overflow of the scaled byte
   offset, and overflow or underflow of the machine address; release emits the
   scaled machine add or subtract with no guard and assumes those preconditions.
   A pointer carries no extent, allocation identity, lifetime, or provenance,
   so neither mode promises bounds or same-object validation. Pointer-pointer
   difference, pointer ordering, `n + p`, and pointer indexing (`p[i]`) do not
   exist. Use slices for bounded traversal; an extent-free access remains
   explicit as `*(p + n)` or `*(p - n)`. Rationale: this exposes the required
   machine operation without making a pointer resemble a sequence, widening
   permission, adding metadata, or implying iterator, ownership, or provenance
   machinery.
   Pointer-integer conversion uses `usize` as the sole address integer; no
   separate `uintptr_t`-class type exists. `usize(p)` converts any data pointer,
   plain or writable and including `null`, to its exact target machine address.
   `*T(address)` and `mut *T(address)` convert exactly one `usize` value to the
   explicitly written read-only or writable data-pointer type. The latter is a
   programmer-trusted assertion of write authority; conversion never infers or
   implicitly widens permission. `T` must have a complete, nonzero-sized layout.
   Address zero produces canonical `null`. A statically known nonzero address
   not divisible by `alignof(T)` is rejected in both modes; checked builds
   diagnose dynamic nonzero misalignment, while release emits no guard. No mode
   validates mapping, lifetime, extent, allocation membership, provenance, or
   actual readability or writability. Pointer-to-integer conversion discards
   type and permission facts; converting an unchanged address back restores
   only the explicitly written pointer type and address. Function values,
   slices, strings, contracts, and other callable or multiword values do not
   participate. Rationale: kernels, embedded and memory-mapped access, tagged
   pointers, integer-address ABIs, custom runtimes, renderers, and address
   diagnostics require the exact machine facility. The explicit conversion is
   the trust boundary; Ace adds no tagged-pointer abstraction, hidden metadata,
   ownership rule, or runtime validation beyond the cheap local alignment
   diagnostic.
   A slice view may be constructed explicitly from an exact element pointer and
   element count: `[]T(pointer, count)` accepts `*T` or `mut *T`, while
   `mut []T(writable_pointer, count)` accepts only `mut *T`. The count is
   exactly `usize`; permission may shrink but never widen. Null plus zero is
   canonical null, while a non-null pointer plus zero remains a non-null empty
   view. Null plus a positive count is rejected when locally constant,
   diagnosed in checked output when dynamic, and is a release precondition.
   The same static/checked/release policy applies to `count * sizeof(T)` and
   end-address overflow. `T` must have a complete, nonzero-sized layout.
   Construction allocates nothing and asserts only the stated view extent: it
   establishes no allocation identity, ownership, lifetime, mapping, or actual
   range validity and never creates an authoritative `mem.realloc` or
   `mem.release` descriptor. There is no implicit pointer/slice conversion,
   allocator-metadata lookup, or reverse possibly-empty-slice-to-pointer
   conversion.
10. Raw surface: `mem.alloc(size: int, align: int): mut []u8`, `mem.realloc(data: []u8, new_size: int): mut []u8`, and `mem.release(mut data: []u8)`. Allocation and successful reallocation originate writable-content proof because they return fresh writable storage; failure returns the canonical null `mut []u8`, with the same representation as plain `[]u8`. The input to `realloc` remains plain because the operation requires the authoritative allocation descriptor, not element-write permission. `release` likewise takes plain content permission while its call-site `mut` grants replacement of the caller's descriptor with null; an intentionally permission-shrunk allocation handle therefore remains reallocatable and releasable. The module may be imported qualified or its members selected explicitly. Invalid requests and OOM return null, never abort. Sizes must be positive. v1 Apple arm64 alignment must be a positive power of two no greater than the platform heap's fundamental 16-byte alignment; larger over-aligned allocation is additive later. `mem.realloc` requires the complete authoritative non-null slice, may move, and requires a positive new size; on invalid size or OOM the original slice remains live and unchanged, and on success only the returned slice is authoritative.
11. `mem.release` accepts a slice so cleanup may be unconditional. A null empty slice is a no-op; otherwise it releases the storage and sets that caller variable to canonical zero (zero pointer and zero length). The `mut` signature and call-site mark make this mutation visible.
12. Slices are copyable views, not owners. Copying a slice copies only its pointer and length; all copies address the same storage. `mem.release(mut a)` nulls only `a`: copied slices remain non-null dangling aliases and must not be used or released. Ace performs no alias search and adds no ownership or move analysis.
13. Cleanup is explicit `mem.release` only — no destructor, finalizer, or other hidden teardown exists. The `defer` statement orders explicit cleanup calls at function exits (see: Deferred calls); the goto-for-cleanup question is settled by labeled blocks (Conditions #4).
14. `sizeof(T)` is always the compile-time `usize` storage size of one `T`; `alignof(T)` is always its compile-time `usize` required alignment in bytes. Both accept the full type grammar, require a complete value layout, and are unaffected by dynamic allocation or `mut`. Every built-in sequence exposes the direct read-only property `sequence.len` with type `usize`: `[N]T` yields the constant element count, `[]T` its stored runtime element count, and `string` its stored runtime byte count; writable forms behave identically. This is structural data, not a method, UFCS call, or user-extensible property mechanism. The former call spelling `len(sequence)` is not admitted. Structs, tuples, unions, contracts, and user-defined wrappers gain no built-in property; a declared struct field named `len` remains an ordinary field.
15. Allocation-free byte operations are complete-slice operations: `mem.copy(noalias dst: mut []u8, src: []u8)`, `mem.move(dst: mut []u8, src: []u8)`, `mem.fill(data: mut []u8, value: u8)`, `mem.zero(data: mut []u8)`, and `mem.compare(a: []u8, b: []u8): int`. `copy` and `move` require equal slice lengths and copy exactly that many bytes without changing either slice descriptor; callers explicitly slice larger storage to the intended range. Empty slices are valid. `copy` requires its complete source and destination ranges not to overlap, expressed by `noalias` on the writable destination; `move` permits overlap in either direction. `fill` writes one byte value throughout the complete slice and `zero` writes zero throughout it. All four writes return nothing. Write destinations carry type-position `mut` (Mutability #10); `mem.release(mut data: []u8)` keeps its call-site mark because it replaces the caller's descriptor, not bytes.
16. `mem.compare` compares complete slices lexicographically as unsigned bytes and returns exactly `-1`, `0`, or `1`; after an equal prefix, the shorter slice sorts first. It accepts unequal and empty slices and changes nothing. Every byte operation uses only caller-owned storage and performs no allocation. A `copy`/`move` length mismatch or a false `copy` no-alias promise is a programmer error: checked builds diagnose it at the call boundary, while release assumes the preconditions and pays no validation cost.

Carved (compile-time layout verification):
`offsetof(T, field.path)` is the compile-time `usize` byte offset of a direct or nested field within a complete struct `T`. Each path component traverses inline struct storage; pointers, arrays, and non-struct intermediates are rejected rather than implicitly dereferenced or indexed. `static_assert(condition)` is a reserved, zero-code statement at module or function scope. Its single Boolean condition must be compile-time provable from literals, const and enum values, integer and Boolean operations, comparisons, and layout builtins; false and non-constant conditions reject the program. Messages and broader constant evaluation remain additive.

Carved (memory representation):
17. Representation is normative, not implementation detail: the layout rules below may be relied on by programmer code, including inline-asm bindings, descriptor inspection through `&`, and `sizeof` reasoning (Memory #4 machine-true; zero runtime cost; later additions remain additive).
18. A slice `[]T` is exactly two words: the element pointer at offset 0 and the element count at offset 8. `sizeof([]T)` is 16 with alignment 8. There is no capacity word and no hidden field, ever; capacity, where wanted, belongs to an explicit user struct, never to the view.
19. `string`, `[]char`, and `[]u8` are three intent spellings of one type and therefore share identity, representation, conversions, and overload behavior; `char` is the transparent alias of `u8` specified by Standard Library #16. The shared view is exactly two words — byte pointer at offset 0 and byte count at offset 8 — with size 16 and alignment 8, and the language never appends a NUL terminator. `mut string`, `mut []char`, and `mut []u8` likewise name one writable-view type; `mut` never changes layout, size, canonical zero, or emitted code (Mutability #9). Plain spellings are read-only through their bytes. Rationale: use `string` for complete text, `[]char` for character-oriented parsing or framing, and `[]u8` for raw bytes; these are source-level intent conventions, not runtime or overload distinctions.
20. A string literal resides in static read-only storage holding exactly its written bytes — no appended NUL, nothing hidden. The empty literal `""` is a valid non-null string (byte pointer, count 0); only the all-bits-zero string is `null`. `mem.equal("", zero_string)` is true by content, while `"" == null` is false because nullness tests the complete descriptor (Values & Passing #11).
21. An array `[N]T` is `N` contiguous elements in index order with no metadata and no inter-element padding: `sizeof([N]T)` is `N * sizeof(T)`, and array alignment is the element's alignment. Padding and byte order inside one element are that element type's own platform facts.

Carved (volatile access):

- `volatile place` performs one volatile read and `volatile place = value`
  performs one volatile write. `volatile` marks an access, never storage or a
  type. The final addressable place must hold an integer, floating value,
  pointer, explicitly backed enum, or distinct form of an eligible type.
  Complete arrays, structs, tuples, tagged unions, default enums, slices,
  strings, contracts, function values, `Error`, and `typeid` are excluded.
- The complete addressing expression is evaluated exactly once under ordinary
  evaluation-order rules. A store retains ordinary assignment order and
  evaluates both address and value once. Ordinary permissions remain; writing
  through a pointer or view requires existing `mut` permission.
- An access transfers exactly `sizeof(T)` bytes for the final scalar `T`.
  Containment, field offset, `@packed`, and `@align` never widen, narrow, split,
  or merge it. A target unable to perform one access at that width and effective
  alignment rejects the program rather than synthesizing narrower accesses.
- Volatile accesses cannot be removed, combined, duplicated, cached, or moved
  across another volatile access. Volatile provides no atomicity,
  synchronization, memory barrier, or hardware-specific ordering. Tagged-union
  switch binders remain local payload copies, not addressable union views, and
  therefore introduce no volatile payload access.

Rationale: access-site marking exposes each observable operation without a
contagious qualifier system and remains independent of byte layout.

Implementation guidance: ordinary and volatile fields share one typed-place
path which computes the address once, takes width only from the final scalar
type, and selects volatile behavior at the exact-width load/store primitive.

Carved (typed allocation):
22. The `mem` module exports three explicitly instantiated generic overloads beside the raw byte forms: `mem.alloc[T](count: usize): mut []T`, `mem.realloc[T](data: []T, new_count: usize): mut []T`, and `mem.release[T](mut data: []T)`. Generic instantiation stays explicit (Generics #5): deallocation is spelled `mem.release[T](mut data)`, never inferred from the argument. As in the raw forms, allocation and successful reallocation originate writable-content proof in their results, while reallocation and release inputs require authoritative descriptors but not element-write permission. The raw and typed forms remain one overload set per name.
23. `mem.alloc[T](count)` requests storage for exactly `count` contiguous `T` elements. The byte size is `count * sizeof(T)`, computed safely before the platform allocator is called; the alignment is selected automatically from `T` and the caller supplies neither. The returned `mut []T` is the allocation pointer plus `count` as its element length — no capacity, allocator value, ownership marker, header, or other metadata (Memory #18). Storage uses the target's native representation, alignment, and byte order: typed native memory, not serialization.
24. Typed allocation is uninitialized exactly like the raw form: no value initialization, zeroing, constructor, destructor, or hidden loop. Heap contents remain an explicit programmer-trusted boundary under the existing definite-initialization model; the compiler adds no ownership, lifetime, per-element initialization, or alias analysis, and reading an element before the programmer has established a valid `T` representation is programmer error.
25. A multiplication overflow, an unsupported type alignment, a zero byte-size request — `sizeof(T) == 0` or `count == 0` — or allocation failure returns the canonical null `mut []T`; nothing aborts, and an overflowing or zero-size request is rejected without calling the allocator. The `mut` qualification changes no null representation and may shrink implicitly to plain `[]T`. Zero-sized types get no dangling-pointer or zero-sized-allocation machinery.
26. `mem.realloc[T]` requires the complete authoritative non-null typed slice returned by typed allocation or a prior successful typed reallocation of the same exact `T`; the input may have been permission-shrunk to plain `[]T`. It may move the storage. On success the `mut []T` result is the only authoritative slice and has length `new_count`; on overflow, invalid size/alignment, or failure it returns null while the original allocation remains live and unchanged. Newly added elements are not initialized and removed elements are not destroyed. Copied slices remain ordinary aliases and may dangle after a successful move, exactly as with the raw API (Memory #12); no ownership or alias search exists.
27. `mem.release[T](mut data)` releases the complete authoritative typed allocation. The `[T]` instantiation must match the argument's `[]T` element type exactly. A null typed slice is a no-op; otherwise release performs no element destruction or hidden iteration and sets only the passed caller variable to canonical zero — zero pointer and zero length. Copied slices are not found or cleared and become dangling aliases.
28. `mem.alloc_size(data: []u8): usize` reports the usable byte size of the live allocation whose base pointer `data` holds: at least the requested length, and allocator-defined beyond it. Release lowering is the platform allocator's own size query (Darwin `malloc_size`) with no Ace header or registry lookup. A null slice, a pointer the allocator does not own, and an interior pointer report 0. It is read-only: the argument takes no call-site `mut`, and typed `[]T` slices are not accepted (`[]T` does not convert to `[]u8`).

Rationale: typed allocation removes manual byte packing while changing nothing about the allocation model — one fixed process-wide strategy, machine-true sizes, explicit failure as the null slice, and zero release-mode cost (the lowering is the raw platform call plus a constant multiply). Reinterpretation (`[]u8` to `[]T`) and implicit conversions stay out (Sequences #7): the typed form is a separate allocation path, not a view cast.

## Values & Passing
Decision: value semantics with explicit copies; references only at call boundaries.
1. Assignment copies — `q = p` gives `q` its own value storage; mutating that copied value never changes `p`. For a slice this copies only its pointer and length: reassigning/reslicing `q` does not change `p`, while element writes through either view reach their shared backing storage (Memory #12).
2. Function/method parameters are passed by reference (unlike C) — no hidden copies at call boundaries; mutations are visible to the caller. To pass a protected copy, assign to a new variable first.
3. Plain declarations are stack storage; heap memory exists only via explicit allocation (see: Memory #5).
4. Copy-on-write is not a language semantic — assignment is a plain copy. COW, where wanted, is an explicit opt-in library type.
5. The compiler may elide copies when the result is observably identical (codegen freedom, not language semantics).
6. Every type has a canonical all-bits-zero representation. Numeric zero is `0`/`0.0`, boolean zero is `false`, pointer-like and handle-like zero is `null`, aggregates are recursively zero, and the first enum member is represented by zero.
7. Globals without initializers use canonical zero and reside in BSS. A local without an initializer reserves uninitialized storage and may not be read until definitely initialized. Whole assignment initializes the whole value; field and constant-index element assignments initialize those exact subobjects. A whole aggregate read requires every subobject to be initialized. Branch joins retain only facts true on every continuing path; a loop cannot establish a fact after itself because it may execute zero times.
8. Initialization checking follows direct Ace storage paths. Taking an address does not read the addressed value; pointer aliasing, inline assembly, and extern code are explicit programmer-trusted boundaries and receive no hidden alias or postcondition analysis.
9. Multi-assignment is simultaneous on its right side: `a, b = b, a` evaluates every right-hand expression exactly once in source order and preserves the complete result tuple before evaluating or writing any target. Targets are then evaluated and written from left to right, so if targets overlap, the later target's write is the final one. The number of values must equal the number of targets. Types are checked positionally, not uniformly: each right-hand value must be assignable to its corresponding target under the ordinary explicit-conversion rules, so heterogeneous assignment such as `number, message = 2, "new"` is valid. The compiler may keep the preserved values in registers or temporary stack storage; no user-visible temporary or allocation is created.
10. A multi-name `=` statement decides declaration or reassignment independently at each position. A typed name must not already exist and declares a new binding; a bare name must already exist and reassigns it: `written: int, error = write(...)`. Any order and mixture is valid, including the all-typed declaration `number: int, message: string = 1, "old"` and the all-bare assignment `number, message = 2, "new"`. Arity and types are checked positionally under Syntax & Parsing #9. Every name is resolved against the scope as it stood before the statement, so one position never makes another position valid; typed names are uninitialized while the right side is evaluated. The statement requires an initializer and stays on one physical line under Syntax & Parsing #7.
11. Sequential types — arrays, slices, and strings — are never compared with `==` or `!=`, regardless of element type. Byte-backed sequences (`[N]u8`, `[]u8`, and `string`, including plain and `mut`-qualified forms) compare content explicitly through `mem.equal(a, b)` in every pairwise combination, either operand order, and at any fixed-array lengths: length first, then exact stored bytes, with no allocation, conversion, decoding, or normalization. Length-only comparison is written `a.len == b.len`. Non-byte arrays and slices have no built-in content-equality helper; use an explicit loop. Where a content or length comparison is negated, the result is exactly the negation of the corresponding `mem.equal` or `.len` check; sequences themselves have no separate `!=` semantics. A comparison whose other operand is the literal `null` is instead the distinct nullness form specified by Sequences #8 and grants no sequence-to-sequence equality.
12. A global `const` initializer may name another initialized global `const`, including a public constant reached through an imported module. The referenced initializer is recursively evaluated at compile time and must itself be representable by the existing constant-expression rules; this introduces no runtime module initialization or storage read. The destination's stated type remains authoritative, and ordinary exact type checking still applies. Rationale: an initialized constant remains a constant across an explicit module boundary, allowing an OS-neutral facade to re-export target-selected values without duplicating their machine values.

## Mutability
Decision: locals are mutable by default; mutation marking lives at reference boundaries.
1. Local variables are mutable by default; `const` is the rare opt-in. [keyword provisional]
2. Unmarked by-reference parameters protect the caller's argument storage; `mut` on a parameter grants assignment to that caller storage — visible in the signature and repeated at the call site. Pointer reachability is a separate permission: `*T` reads its immediate pointee and `mut *T` may write it. Thus `mut p: *T` may replace the caller's pointer but may not write `T`; `p: mut *T` may write `T` but may not replace the caller's pointer; `mut p: mut *T` permits both. `mut` never marks local declarations, which remain mutable by default.
3. Per-parameter `const` does not exist — unmarked already means read-only. Function-level `const` does not exist — "writes nothing" is spelled "no `mut` in the signature." (A future opt-in purity annotation — no writes anywhere, globals included — remains possible; it would carry information the signature cannot.)
4. Rationale: local mutability is invisible to optimization (SSA); mutation through references is what blocks alias analysis. Syntax is spent only where the compiler and reader profit.
5. Const globals are the performance-relevant case: truly immutable data can live in read-only memory.
6. An argument whose own caller storage may be replaced carries one `mut` token at the call site — reassignment is greppable, and an unmarked call never changes the argument value itself. Writes through a `mut *T` need no argument-level `mut`: the pointer value is not replaced, and its pointee permission is already explicit in the pointer's type. Composes with method syntax: a mutating receiver is visible in the method signature and call.
7. Composition order of `mut` × `*T` is per pointer level: `mut` means writes are granted through that pointer level and `*` means this is a pointer. `*Node` is read-only and may be null; `mut *Node` is writable and may be null. Every `*` in a multi-level pointer has its own permission: `mut **T` may replace the inner `*T` but not write `T`; `*mut *T` may read the inner pointer and then write `T` but not replace the stored inner pointer; `mut *mut *T` permits both writes. Only the pointer level immediately used for a write needs `mut`.
8. `noalias` parameter marker: a signature-stated fact that memory reachable through one `noalias` parameter is not reachable through another or through any parameter that can write the same reachable storage (an outer `mut` reference or a writable pointer level). The emitter may vectorize and reorder on it. Checked builds verify slice-range disjointness pairwise in O(1) at call boundaries — a false claim crashes in dev, never in prod. Release cost: zero. Signature facts are optimizer facts, same pattern as read-only.
9. Type-position `mut` marks writable content at exactly one view level: `mut *T` grants a writable pointee and `mut []T` grants writable elements; `mut string` is the text-intent spelling of `mut []char`/`mut []u8`. Plain views are read-only through their content regardless of backing storage; `mut` is the only source of content writability. It never changes representation, size, layout, canonical zero, or emitted code — a compile-time permission with zero runtime cost (Memory #17, #19). A writable view participates wherever its plain form does and implicitly shrinks to the plain form at the same level; widening is never implicit. Writable and plain forms remain distinct types, while declarations differing only among `string`, `[]char`, and `[]u8` are duplicate declarations.
10. The two axes compose per level: call-site `mut name: T` grants replacing the caller's binding (the descriptor); type-position `mut` in the parameter's type grants content writes; the two are independent — `mut s: string` may rebind only, `s: mut string` may write bytes only, and `mut s: mut string` permits both. Content-write parameters are therefore spelled in the type (`dst: mut []u8`), while descriptor-replace parameters keep the call-site mark (`mem.release(mut data: []u8)`).
11. Content writability originates from proven writable storage or an explicit
    programmer-trusted integer-to-writable-pointer conversion and is thereafter
    preserved or shrunk by the explicit rules. `mut array[lo..hi]` establishes
    writability when the array's elements are owned writable storage. Slicing an
    already-`mut []T` or `mut string` preserves writability without restating
    `mut`; copying and function returns retain the value's stated type, and the
    exact conversions preserve proven permission between `mut []u8` and
    `mut string` (Sequences #4, #6). `mem.alloc` and successful `mem.realloc`
    originate `mut []T` results from fresh allocation (Memory #10, #22),
    `mut &lvalue` produces `mut *T` (#8), and `mut *T(address)` explicitly
    asserts writable authority for a `usize` machine address (Memory #9). In
    contrast, `mut plain_slice[lo..hi]` and `mut plain_string[lo..hi]` are always
    rejected, even when the descriptor variable is locally rebindable or passed
    through a call-site `mut name:` parameter: that axis permits replacing which
    descriptor value the variable holds and never manufactures content
    permission the current value lacks. Once a view's permission has shrunk to
    plain, ordinary view operations cannot grow it again. String literals,
    `const` globals, unmarked parameters, plain-returning calls, and every other
    read-only view likewise provide no writable-content proof. Rejection is
    compile-time only and changes no representation or emitted code.

## Zero, Null & Absence
Decision: every type has canonical zero; Ace has no optional type constructor.
1. `null` materializes the all-bits-zero value of pointers, function values, contracts, slices, strings, `Error`, and `typeid`, including their `mut`-qualified forms (same zero descriptor; Mutability #9). It is not a numeric, boolean, enum, struct, tuple, array, or distinct value.
2. Zero pointers may be stored, copied, compared, and replaced. Dereferencing one is invalid. Zero function and contract values likewise may be stored, copied, and compared; calling or dispatching through one is invalid. Checked builds diagnose these invalid operations; release inserts no null check and has C-level unchecked behavior.
3. A zero slice or string has zero pointer and zero length. Length queries, empty iteration, and zero-length operations are valid; indexing still requires an in-range element.
4. Zero `Error` means success/no error; its packed domain and code bits are both zero. Zero `typeid` means no type identity; every `typeid(T)` result remains nonzero.
5. When application data must distinguish absence from an ordinary zero value, it declares that distinction explicitly, normally as a struct containing a `present: bool` field and the value. There is no hidden tag or implicit optional ABI.

Historical design note (non-normative): `T?` optionals were explored, specified, and implemented, including niche and tagged representations. They were removed because canonical zero already covers machine-level sentinel values, while application-level presence is clearer as an explicit struct. Removing the operator also eliminates a second family of layouts, coercions, flow rules, and ABI cases.

## Safety & Checked Builds
Decision: checks are a development tool; release trusts the developer.
1. The compiler has two explicit emission modes: `--checked` and `--release`.
   They are mutually exclusive, and emitted programs default to `--checked`.
   Check-only, `--tokens`, and `--ast` invocations accept either flag but emit
   no program, so the choice has no effect on their result.
2. Checked builds diagnose dynamic contract violations and then abort; bounds
   diagnostics include the source file, line, and column of the failing access.
   The diagnostic is best-effort on standard error and is not a recoverable error
   channel. Array, slice, and string bounds and integer overflow are on.
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
5. Safety is intentionally minimal and local: type checking and definite initialization improve on C at zero runtime cost; checked builds add bounds, overflow, and allocator diagnostics. Ace rejects Rust-style ownership, lifetime, borrow, and whole-program safety analysis under the priority stack.
6. Integer overflow checking covers the value-producing arithmetic operations
   `+`, `-`, `*`, and unary `-` on every fixed-width signed and unsigned integer
   type, including their compound-assignment forms. Checked builds diagnose a
   result outside the destination type before it is stored. They also diagnose
   division or remainder by zero and the signed `MIN / -1` and `MIN % -1`
   cases. Release builds emit the target's ordinary fixed-width operations with
   no guards; overflowing arithmetic is unchecked. Bitwise operations and
   explicit numeric casts are not overflow operations. A shift count must be
   nonnegative and less than the left operand's bit width; checked builds
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
    cannot be declared, shadowed, or redefined. A checked build evaluates the
    condition exactly once; false produces a best-effort assertion diagnostic
    with the source location (`file:line:col`, paths per Errors #16) and then aborts as a broken invariant. A release
    build emits neither the condition nor its evaluation, so a program must not
    rely on assertion side effects. A condition known false at compile time is
    rejected in every mode under #4; one known true emits nothing. There is no
    message overload: use `panic(message)` for an unconditional contextual
    abort. The same builtin is suitable for simple tests run in checked mode;
    richer expected/actual reporting and test discovery belong to a library or
    runner rather than the language primitive.

Rationale: the compiler never guesses a mode from its own build, an output
name, or the surrounding environment; the selected artifact semantics remain
visible and reproducible. Checked-by-default follows the strict-first rule for
development, while explicit release preserves the priority stack's highest
requirement by removing every dynamic safety cost. Compile-time proofs remain
mode-independent because rejecting a known-invalid program costs nothing at
runtime and avoids giving one source two static meanings. The exact arithmetic
set checks operations that claim to produce a numeric value while keeping casts
and bit manipulation machine-direct. Call-boundary range comparisons, direct
null guards, and allocator bookkeeping are bounded local work rather than
whole-program safety analysis. Assertions add one minimal development primitive
and reuse abort semantics without weakening the visible distinction among
recoverable `Error` values, broken-invariant `panic`, and caller-chosen
`sys.exit` status.

## Errors
Decision: errors are ordinary values; checking is always explicit.
1. Functions return multiple values (e.g. value, memory size, error).
2. Errors are checked with branches the programmer writes — no hidden control flow, no exceptions, no propagate-operator that secretly returns.
3. Panic exists only as unrecoverable abort for bugs — never a catchable error channel.
4. Accepted cost: error checks add visible lines; nesting relief comes from early returns and explicit cleanup, ordered at function exits by `defer` (see: Deferred calls) — no error sugar.
5. `Error` is a predeclared opaque 64-bit value. Zero is success/null. Bits 63..32 hold a compiler-assigned nonzero 32-bit domain ID; bits 31..0 hold the raw bits of the occurrence's `i32` code. It is not a contract value and carries no pointer, allocation, registry, or metadata.
6. Convention: the error is the LAST return, as `Error`; zero/null means success and `if error != null` is the explicit check. The one-word zero sentinel keeps `(usize, Error)` a two-word aggregate and normally register-returnable on 64-bit targets.
7. `pub OpenErr: error` declares an empty nominal error domain. The optional structured form is `pub OpenErr: error {` followed by entries such as `0: unknown = "unknown failure"` on their own lines and a closing `}`. Each code is an exact integer literal representable by `i32`, including zero and negative values; names and codes are each unique within the domain, and every message is a string literal. Domain visibility controls every entry; entries have no separate `pub`. Each independent domain declaration is unique. `OpenErr()` constructs `Error(domain, 0)` and `OpenErr(code)` accepts exactly one `i32`, preserving its raw bits, including codes absent from the declaration. The expression type is universal `Error`, so function signatures return `Error`, never the domain name.
8. Error-domain names are usable with `typeid`. Transparent aliases and re-exports preserve the originating declaration's identity; independent same-spelled declarations remain different. A resolved source file is canonicalized before module caching, so equivalent import paths reach one declaration and one domain ID.
9. `Error` exposes exactly four direct read-only fields: `error.domain: typeid`, `error.code: i32`, `error.name: string`, and `error.message: string`. `name` is the unqualified spelling on the originating domain declaration; aliases and re-exports preserve it. `message` matches the complete domain/code pair and returns that entry's declared literal. Null returns `""` for both description fields, and a known domain with an unlisted code returns `""` for `message`. No domain-specific entry becomes a dynamic `Error` field.
10. `Domain.entry` is an `Error` value equivalent to `Domain(code)`. `Error == Error` compares the complete packed value, so equal entry spellings or codes in independent domains remain unequal.
11. Compiler-assigned domain IDs are collision-free only within one linked program, unstable across builds and processes, nonserializable, and not suitable as external identifiers.
12. Multi-return consumption is typed destructuring — `n: int, error: Error = read()` — the `name: type` rule applied to multiple bindings; plain reassignment `n, error = read()` afterwards.
13. `Error`, error-domain names, and `typeid` are Ace-internal shapes and are rejected in `extern`/`export` signatures. C-facing wrappers translate explicitly to stable `i32` codes or another declared C-compatible shape; imported C failures are mapped to an Ace domain and occurrence code.
14. Error descriptions are opt-in static metadata: names and messages are read-only string data, and lookup data/code is emitted only when `.name` or `.message` is used. Lookup is allocation-free and safely returns empty strings for unknown domains/codes. There is no runtime registry or initialization, metadata carried inside `Error`, descriptor, pointer, vtable, general reflection/type-name facility, dynamic entry lookup, or `fmt.print(Error)` support. Occurrence-specific context is returned separately. Rationale: static descriptions improve diagnostics at no per-value or unused-program cost while preserving the packed ABI, explicit errors, and machine-visible allocation model.
15. Panic remains semantically separate from recoverable errors: it terminates the process for broken invariants and is never catchable, recoverable, or convertible into `Error`. Surface (decided): `panic` is a reserved builtin — no declaration may bind, shadow, or redefine the name in any scope — and its only use is the call `panic(message: string)`: exactly one message argument, any string expression, static or runtime. The call never returns and `panic` is not a first-class value. Termination (decided): the message is delivered best-effort to standard error (file descriptor 2) exactly as supplied — no prefix, no added newline, a failed or partial write never blocks termination — then the process aborts via `abort()` (`SIGABRT`), as undefined externals resolved by the system linker. Checked and release builds behave identically. Users trigger it; the language owns everything that happens next.
16. Source-location builtins: `@file` and `@line` are reserved expression forms naming where they are written, never where the caller is. `@file` is a compile-time `string` constant naming the containing source file; its bytes are static read-only data. `@line` is an untyped integer constant holding the 1-based source line, typed by context exactly like an integer literal. A helper reporting its caller's location receives these values as ordinary arguments: there is no implicit caller-location parameter, `@column`, `@function`, or location struct, and any other `@name` in an expression is an error.
    - Reported paths are build-relative with `/` separators, never absolute. A standard-library file is `std/<path within std>`; every other file is relative to the build root — the directory holding the entry file for a direct compile, or the root package's directory under `ace pkg` — using `..` segments when outside it. The `assert` diagnostic (Safety & Checked Builds #10) reports the same paths.
    - Rationale: panic, log, and error-report text gains location without macros or hidden parameters, and build-relative paths keep binaries identical across machines without exposing the builder's directory layout.

## Type System
Decision: fully explicit types; no inference.
Predeclared types: `bool`, `i8`/`i16`/`i32`/`i64`,
`u8`/`u16`/`u32`/`u64`, `f32`/`f64`, `float`, `string`, `usize`,
`typeid`, and the platform words `int`/`uint` (i64/u64 on Apple arm64).
`float` is a transparent alias of `f64`: the spellings have the same type
identity, representation, conversions, ABI behavior, generic identity, and
overload signature. Consequently, overloads cannot distinguish `float` from
`f64`. Floating literals continue to default to this shared type.
`char` is the transparent `u8` alias specified by Stdlib #16.
1. Every signature, global, field, parameter, return, local, and multi-binding states its type. Generic instantiation is also explicit.
2. No type inference keeps compilation direct, types deterministic, declared intent locally visible, and generated code machine-true. Initializer expressions are checked against the type written on their bindings.
3. Any broader inference — contextual literal or aggregate typing, multi-binding deduction, generic deduction, or global type elision — remains absent and could only be added later as an opt-in relaxation (PHILOSOPHY.md, Considerations #1).
4. Type aliases: `Name: type = Existing` — transparent, zero-cost notation; the alias IS the type, fully interchangeable. Generic aliases allowed: `Pair[T]: type = (T, T)`.
5. Distinct types: `Name: distinct Existing` — same layout, new nominal type. `Name(value)` constructs (constructor position is free via the namespace split); distinct types do not mix with their base or each other without explicit conversion. Zero runtime cost — pure compile-time discipline for units, handles, and IDs.
6. `typeid(T)` is a compile-time builtin for an exact named type. The compiler assigns a nonzero 32-bit ID represented in the existing pointer-sized `typeid`, allowing an error's upper 32 bits to compare directly with `typeid(Domain)`. The same exact type compares equal throughout one linked program; different nominal types and exact generic instantiations compare unequal; transparent aliases share their target's identity. Values support storage, passing, `==`, and `!=`, but no ordering, arithmetic, reflection, integer conversion, runtime name, serialization, or cross-build/process stability. Zero/null means no type identity.
7. Every inline value type has a finite compile-time layout. A struct, union payload, array element, tuple member, distinct base, or alias expansion may not contain itself through an all-inline path, directly or indirectly; such a cycle is a compile error. `A: struct { a: A }` and the mutual `A: struct { b: B }` / `B: struct { a: A }` therefore have no layout and are rejected.
8. A pointer breaks a layout cycle because its size and alignment do not depend on its pointee. Self-pointers are valid, as are mutually recursive pointer-linked types: `Node: struct { next: *Node }` and `A: struct { b: *B }` followed by `B: struct { a: *A }`. Pointer mutability changes write permission, not this layout rule. A slice breaks a layout cycle the same way (fixed pointer-plus-count representation) and additionally may forward-reference an as-yet-undeclared type at a field/variant root, per Compiler #5: `Json: union { none, object: []Pair }` followed by `Pair: struct { key: string, value: Json }`.
9. Struct fields retain declaration order with natural alignment and padding; the
   compiler does not reorder them.

Carved (explicit struct layout):

- `@packed` and `@align(N)` occupy their own lines before a struct declaration;
  `@align(N)` may likewise precede a field. Each occurs at most once on its
  subject. With `@when`, order is `@when`, `@packed`, then `@align`. `N` is a
  compile-time `usize` expression and, initially, a positive power of two no
  greater than 16; larger alignment remains additive with over-aligned storage.
- An ordinary field's effective alignment is its type's natural alignment, and
  field `@align(N)` may only increase it. An ordinary struct's alignment is the
  maximum effective field alignment, raised by its own `@align(N)`; the
  struct-level attribute may not reduce that result.
- In an `@packed` struct, each unannotated field has effective alignment one. A
  field `@align(N)` raises only that field's packed baseline to exactly `N`, even
  below its type's natural alignment. Following unannotated fields retain
  alignment one. The struct alignment is the maximum effective field alignment,
  raised by its own `@align(N)`.
- Each field begins at the first offset satisfying its effective alignment and
  struct size is rounded up to struct alignment. Nested values retain their
  internal layouts; packing changes placement, not contents. Explicit byte
  arrays represent reserved or unnamed bytes. `sizeof`, `alignof`, and
  `offsetof` expose the result.
- Packed and explicitly aligned structs may be C-compatible behind pointers
  when all recursive rules hold and the layout matches the external definition.
  They are rejected as direct `extern`/`export` parameters and returns pending
  by-value ABI verification. Layout attributes do not determine byte order, bit
  fields, observability, or synchronization.

Rationale: explicit controls complete machine-true wire, hardware, and C
representations without changing ordinary struct layout.
10. Enums default to `i32` storage and declaration-order discriminants beginning
    at zero. `Name: enum u8 { none, read = 4, write }` explicitly selects one of
    `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, or `u64` as the backing type;
    aliases of those types share their identity. A member may provide a
    compile-time integer discriminant, while an omitted discriminant is zero for
    the first member and otherwise one greater than the preceding member. Every
    value must fit the backing type, duplicate values are rejected, and the first
    member must remain zero so the all-bits-zero representation is canonical.
    Enum identity remains nominal and integer conversion remains explicit.
    Size, alignment, storage, passing, and return behavior follow the backing
    type with no runtime metadata or validation.
11. An explicitly backed enum is C-ABI-compatible wherever its backing integer
    is compatible and crosses the boundary exactly as that integer. A default
    enum remains rejected at C boundaries because it states no external width
    promise. Rationale: explicit values and widths make opcodes, wire values,
    registers, and integer-backed ABI fields machine-true while the strict zero,
    fit, and uniqueness checks preserve Ace's local safety and leave aliases or
    other relaxations additive (PHILOSOPHY.md, Priority Stack and Considerations
    #1).

Carved (compile-time static lookup maps):

- `StaticMap[K]V` is a predeclared compiler type for immutable lookup tables.
  Its sole construction form is a module-level declaration initialized by a
  brace list of compile-time `key: value` pairs, for example
  `status: StaticMap[u16]string = { 200: "OK", 404: "Not Found" }`.
  `StaticMap` intrinsically makes the binding and every entry immutable;
  `const` is neither required nor permitted on the declaration. Construction
  in a function, mutation, insertion, removal, iteration, membership syntax,
  methods, `.find`, and other first-class map operations do not exist.
- Lookup is exactly `table[key]`. The key expression has the map's exact key
  type; an untyped literal may materialize as that type under the ordinary
  literal rules. A present key returns its stored value by value. A missing key
  returns the canonical all-bits-zero `V`, so absence and a present entry whose
  value is canonical zero are intentionally indistinguishable.
- Keys are exactly `bool`, `char`, the integer types, enums, `typeid`, `string`,
  and distinct types recursively backed by one of those types. String equality
  is exact stored-byte equality. Floats, pointers, functions, slices, arrays,
  structs, tuples, unions, contracts, and `Error` are not key types. Each key
  and value uses the existing global constant-expression rules; `V` may be any
  complete value type those rules can initialize. Equal keys are a duplicate
  declaration error under their key type's equality.
- The compiler emits keys, values, and lookup-plan data in read-only static
  storage. It deterministically searches a seeded perfect-hash family for a
  collision-free slot plan, then confirms the candidate key exactly so misses
  cannot alias a stored entry. A declaration performs no runtime construction
  or initialization. The representation is compiler-private and introduces no
  runtime map descriptor or hashing contract for user-defined types.

Rationale: sparse standard-library LUTs need direct declarative source without
hand-written ranges, holes, or switches. A module-only lookup surface supplies
that use while preserving recursive descent, single-pass compilation, direct
machine-code emission, explicit values, and no hidden allocation or general
compile-time execution.

## Scalar-backed Bit Fields
Decision: nominal, exact-width bit layouts lower:
1. `Flags: bits u32 { enabled: 1, mode: signed 3, _: 28 }` declares a nominal
   value backed by exactly `u8`, `u16`, `u32`, or `u64`. The backing integer
   determines size, alignment, representation, and byte order. Fields occupy
   contiguous bits from bit 0 upward in declaration order. Widths are positive
   and must exactly fill the backing integer; there is no implicit padding or
   cross-unit field.
2. An unmarked named field is unsigned and reads as the backing unsigned type.
   `signed N` reads as the corresponding signed type after exact two's-complement
   sign extension; `signed 1` represents exactly `-1` and `0`. `_` is an
   anonymous inaccessible range.
3. Named fields are non-addressable projections. They may be read or assigned,
   but cannot be addressed, passed as a `mut` argument, used with `offsetof`, or
   accessed individually through `volatile`. Assignment requires an ordinarily
   writable containing place and performs one preserving read-modify-write of
   the complete backing value. The containing place and assigned expression are
   each evaluated exactly once. Separate assignments remain separate updates.
4. A constant write outside the field's signed or unsigned range is rejected in
   every mode. A runtime violation aborts in checked mode; release keeps the low
   `N` bits. This applies equally to construction and later assignment.
5. `Flags { enabled = 1 }` starts from canonical all-bits-zero and may omit any
   named field; omitted and anonymous bits remain zero. `_` cannot be named.
   There is no spread or update literal.
6. `Flags(raw)` and `u32(flags)` are exact explicit conversions only between a
   bits type and its declared backing integer. Copies preserve the complete
   backing value. `==` and `!=` compare every backing bit only between the same
   exact nominal bits type.
7. Bits values are ordinary fixed-size values in aggregates, packed structs,
   arrays, parameters, and returns. Whole-value volatile access transfers
   exactly the backing width. Bits types are not C-ABI-compatible in `extern` or
   `export` signatures; interop uses the backing integer and explicit conversion.
   No C bit-field ABI equivalence, atomic field operation, volatile field RMW,
   reflection, wider backing unit, or cross-statement fusion is implied.

Rationale: deterministic machine-register and compact binary layouts are useful
without importing C's implementation-defined bit fields or weakening Ace's
explicit mutation, checked-build, layout, and access-site volatile rules.

## Modules
Decision: modules are single files or `mod.ace`-rooted directories; explicit imports; explicit exports.
1. A single-file module consists of one `.ace` file. A directory module consists of `mod.ace` and every implementation file recursively beneath its directory; `mod.ace` is its sole external import surface. Each file lists exactly what it imports; after compile-time-inactive declarations are removed, import statements precede every other declaration — reading order is stated up front for the single pass. A subdirectory with its own `mod.ace` is itself a nested module, addressable via a `/`-separated path from any enclosing directory: bare `import net/tcp`, aliased `import bla: net/tcp`, or quoted relative `import "../A/B"` (the same `/` separator; a quoted path without a `.ace` suffix resolves as a nested-module path instead of a flat file). A subdirectory without `mod.ace` stays private, swallowed by its nearest enclosing module boundary exactly as an ordinary implementation file. Only the leaf segment's identifier becomes the bound qualifier — `import net/tcp` brings `tcp` into scope, not `net` — mirroring single-segment `import tcp` binding `tcp`. There is no transitivity in either direction (rule 8): importing `net/tcp` exposes nothing through `net`, and importing `net` does not expose `tcp` unless `net`'s own `mod.ace` explicitly re-exports it.
2. Members are private unless marked `pub` on the declaration line — `pub max(a: int): int`; the mark lives with the member and cannot drift; one grep finds every public member. Applies to types, functions, and globals alike. In a directory module, `pub` in an implementation file grants access only to other files in that directory module. Only `pub` declarations owned by `mod.ace` are externally visible. A facade re-exports a type explicitly with an ordinary public transparent alias, such as `pub Fd: type = os.Fd`; imports themselves never re-export.
3. Single-pass compiles imports in dependency order, reading only what is imported — nothing scanned, nothing implicit. **Import cycles are a compile error** (a cycle has no first file; structure, not policy).
4. Incremental rebuild granularity: the file.
5. Whole-module imports remain qualified: `import math`, then `math.sqrt(x)`. Aliasing resolves qualifier collisions (`import m: math`), `import util: "./util.ace"` loads a relative local module, and compiler-reserved standard-library implementation uses the visibly distinct `import big: @bigint` form.
6. Selective imports explicitly place public member names into their ordinary namespaces: `import min, max from math`, or `import Point, origin from "./util.ace"`. A bare member list stays on one physical line; the multiline form is `import (` followed by a comma-terminated member list and `) from module`. A selected function name imports its complete public overload set. When a module exports both a public type and public value under one spelling, selection imports each into its separate namespace; visibility is independent across the two namespaces.
7. A selected name conflicts with another import or declaration in the same namespace and scope; imports from different modules never merge overload sets. Nested lexical scopes may shadow selected value names under the ordinary nearest-binding rule. On a collision, import the module qualified instead. Only `pub` names may be selected; the source may be a bare standard module or relative `.ace` path.
8. An import grants the importing file the right to use imported names and never re-exports them. A module's public surface is exactly its own `pub` declarations; imported names are invisible to consumers, who import the modules they use through their own explicit imports.
9. A resolved module path is canonicalized before cache lookup and deduplication. Lexically different paths containing `.` or `..` that resolve to the same file produce one module instance and one set of declarations. This changes neither import scope nor the supported standard/relative import forms and adds no search path.
10. OS- or kernel-specific standard-library code always sits behind a stable public facade at `std/<module>/mod.ace`; target implementations live under `std/<module>/os/<target>.ace` and are selected through `@when(target == "...")` imports in that facade. Public callers cannot import an OS file directly. The target file exposes exactly the operations and types required by its facade; it does not add same-signature target-named forwarding functions. Those declarations are `pub` only within the directory module and carry no compatibility promise outside it. The `os` name is reserved by convention for this implementation layer; `backend` refers to compiler/code-generation backends and is not used for OS bindings. Rationale: platform replacement changes the target layer without spreading kernel ABI, constants, structures, or link names through callers, while the public Ace surface remains reviewable in one place and redundant forwarding layers do not accumulate.
11. `@when(condition)` is the sole conditional-compilation form and attaches to exactly one top-level declaration, including an import, function, type, global, signing, or extern block. The attribute occupies its own line and a preceding top-level construct is separated from it by one blank line. Its deliberately closed boolean expression language contains `true`, `false`, `target == "name"`, `target != "name"`, contextual `and`/`or`/`not`, and parentheses. `target` is the compiler-selected build target string; it defaults from the host and may be overridden with `--target=NAME`. Unsupported targets are rejected rather than implying an unavailable backend or standard-library layer. The compiler lexes and parses an inactive declaration, so its syntax must be valid, then removes it before import loading, binding/duplicate checks, semantic analysis, and emission. Thus mutually exclusive imports may bind the same alias and mutually exclusive declarations may share a signature; if more than one matching declaration survives, ordinary duplicate rules apply. There is no block, statement-level form, user-defined build flag, arbitrary constant evaluation, or runtime condition. A file need not provide an exhaustive arm, but any surviving reference to an absent declaration fails normally. Rationale: one restricted declaration filter supplies zero-cost target selection without a textual preprocessor, hidden runtime branch, general compile-time execution, or mid-function source fragmentation.
## Package System (future work)
Decision: packaging is an optional, explicit layer above file modules; one `ace` executable owns both the direct compiler and the separately grouped package commands. Manifest and lockfile syntax remain open for later carving. A minimal local-path-only v1 slice is implemented (see `CHANGELOG.md`); the items here describe the target full design and remain future work beyond that slice.
1. `ace.pkg` at a package root defines exactly one package and one primary output. Every package explicitly declares `type` as `exe` or `lib` and explicitly names one root `.ace` file. An exe package produces an executable and its root must provide `main(): int`; a library exposes its root module as its sole external import surface. Consumers cannot deep-import its internal files. A library is distributed as source; initially, `ace pkg build` checks its complete graph and produces no reusable binary archive.
2. A package has a required name, exact package version, and Ace language version. The name is case-sensitive, must be lowercase, and follows the Ace identifier shape; its source location is separate from its identity. Package versions are exactly three nonnegative components (`major.minor.patch`) with no initial prerelease/build suffixes. The Ace field names one exact `major.minor` language version, not a compiler release. Every package in one build graph must use the same Ace language version.
3. Required build fields are package name, package version, Ace language version, type, and root. Description, authors, license, repository, and homepage are optional informational metadata and never affect identity, resolution, or compilation. Exact `ace.pkg` structure, delimiters, and field syntax remain [open].
4. The initial dependency sources are explicit relative local paths and Git repository URLs only; there is no registry. Local paths are resolved from the declaring `ace.pkg`, absolute paths and `file` URLs are rejected, and local contents remain live rather than content-locked. Remote Git URLs may use HTTPS or SSH, with credentials supplied only by normal Git configuration and never stored in package files.
5. Every remote dependency declares one exact version; ranges and implicit latest selection do not exist. Version `1.2.3` is located only by tag `v1.2.3`, after which the tagged root `ace.pkg` is authoritative and must declare the requested name and version and a compatible Ace language version. Floating branches and custom references are initially excluded.
6. One remote repository initially contains one package and has `ace.pkg` at its root; subdirectory packages, monorepo workspaces, Git submodules, and symbolic links are excluded. Remote package trees contain ordinary files and directories only. The installed tree omits `.git` metadata.
7. A package may import only dependencies declared directly in its own manifest. A dependency root uses the existing module syntax (`import json`, selective imports, and source-level aliases); package names may not collide with standard-library module names. Manifest dependency names are canonical and cannot rename the fetched package. Every root and relative import must remain within its canonical package directory.
8. The complete dependency graph is acyclic and contains at most one version and one source location for each package name. Conflicting versions, sources, identities, or cycles are errors showing the relevant dependency paths. There is initially one ordinary dependency set only: no dev, test, optional, peer, feature-selected, or platform-selected dependency classes.
9. Generated `ace.lock` is committed and records the complete resolved graph, exact Git commits, and verified remote content hashes. Normal builds never rewrite it. Remote contents use SHA-256 over a canonically sorted tree including each relative path, file length, and file bytes; enumeration order, timestamps, and permission bits do not affect the digest. Local path entries record their path and verified declared identity but not a content digest. Exact lockfile representation remains [open].
10. Fetched remote dependencies are initially copied per project under `.ace/pkg/`, which is generated and normally excluded from version control. An explicit `-g`/`--global` repository may be designed later; there is no initial shared cache or vendoring mode. Only one version of a named package can be installed for a graph.
11. Plain `ace` remains the direct compiler interface and standalone source compilation does not require a manifest. Every manifest-aware operation is grouped under `ace pkg`: the initial commands are `init`, `build`, `run`, `check`, `add`, `remove`, `update`, `resolve`, `fetch`, and `list`. `run` is exe-only. `init` creates a minimal exe or library without overwriting existing files. From a subdirectory, package commands find the nearest enclosing `ace.pkg`; a nested manifest starts an independent package.
12. `ace pkg add`, `remove`, and `update` edit the manifest and update resolution atomically; update always names one dependency and its exact new version. Manual manifest edits use `ace pkg resolve`. Resolution validates the complete graph, writes the lockfile, and fetches newly resolved remote packages; failure changes neither project file. `ace pkg fetch` performs no resolution and never changes the lockfile: it restores or verifies exactly the locked trees, replacing only missing or corrupt generated copies. Build, run, and check are offline and diagnose whether the cure is `resolve`, `fetch`, or repair of a local path.
13. `ace pkg list` reports the locally resolved graph, including direct/transitive and remote/path status. `ace pkg list name` queries that package's configured remote source for canonical version tags and lists only releases whose tagged root manifest has a matching name/version and is structurally valid; releases for another Ace language version remain visible but marked incompatible. Because one graph contains only one version per name, the queried source is unambiguous. Exact display and flags remain [open].
14. Package operations use the installed `git` executable through a shell-free argument vector. Ordinary compilation has no Git dependency. Arbitrary shell scripts, lifecycle hooks, publishing, and releasing are excluded initially. Any package (`exe` or `lib`), not only the root, may declare native link inputs of the same four types the direct compiler driver already accepts — `.o`/`.a` paths, `-L` search directories, `-l` library names, and `-framework` names. An exe's final link step collects the transitive union of every native input declared anywhere in its dependency graph, ordered dependency-before-dependent with each package's own declaration order preserved among its own inputs, and de-duplicates only exact same-type-and-value entries. A native link input's declared name (not its file content) is its logical identity — the given name for `search`/`library`/`framework`, the file's basename for `object`; the same identity declared with a different value by two different packages in one graph is a resolution-time error naming both packages, mirroring the existing one-version-per-package-name conflict rule (#8) rather than attempting silent merging or symbol renaming. A `lib` package's declared inputs stay dormant metadata until some `exe` in the graph actually reaches the link step (#1). Declared through a repeatable `[[link]]` manifest section (`GRAMMAR.md` §7): each occurrence is one record admitting up to four optional keys — `object: PATH`, `search: DIR`, `library: NAME`, `framework: NAME` — each at most once per record; two unrelated native components use two separate `[[link]]` records rather than repeating a key within one. `object` and `search` paths resolve relative to the declaring package's own directory with no `..` traversal, matching the existing import-containment rule (#7), and `object` must end in `.o` or `.a`. There is no absolute or system search path in v1: a package declares only native inputs it vendors inside its own directory, and a host-installed library remains reachable only through the direct driver's own `-L`/`-l`/`-framework` CLI flags. Custom code generation, deployment, assets, and unrelated tasks stay with an external build tool. A future `tls` client transport (carved from philosophy/semantics; see `TODO.md`) gates on this: whether it belongs in `std` or launches as a package depends on native link-input declarations landing here.
15. Conventional output paths keep the manifest small: an exe defaults to `build/<package-name>`, library build state stays under `build/`, and an explicit command-line output path may override an exe build. Exact package command flags remain [open]. Rationale: the package layer preserves explicit imports and source boundaries, performs no hidden network work during compilation, admits no dependency code execution, and uses exact identities plus a committed lockfile. This puts simplicity and modularity ahead of convenience machinery while leaving registries, shared caches, workspaces, ranges, precompiled packages, testing classes, and publishing additive.

## C Interop
Decision: explicit extern/export boundary; Ace internals stay free.
1. Ace-internal symbols may be mangled (multi-return, mut params, modules) — mangling is private to the compiler.
2. `extern "c" { … }` blocks import C declarations — the ABI stated once per block; variadics (`…`) are legal only inside extern blocks (Functions & ABI #7). Export: an `export` modifier on the declaration produces a C-ABI function with an unmangled name. An extern declaration may carry a link name — `c_open(path: *u8, flags: i32, mode: u32): i32 = "open"` — binding an Ace-side name distinct from the linker symbol, which lets a module wrap a C function under the name it wants to own (first customer: `sys`). The linker spelling is always the declared (or link-named) symbol with the platform's underscore convention applied by the compiler. Link names must be nonempty, contain no NUL byte, and in the v1 emitter are limited to 510 decoded bytes; other bytes are accepted because platform symbols need not be C or Ace identifiers. [spellings provisional]
   Implementation note (Apple arm64): C variadic functions receive their unnamed arguments in 8-byte caller-stack slots, not in the remaining argument registers — libSystem's `open` itself reads mode from the first stack-argument slot. The emitter implements that convention, including the ordinary f32-to-f64 default promotion. The initially verified unnamed-argument set is C-compatible integer, floating, distinct-scalar, and pointer values; aggregate and Ace-specific shapes are rejected until their variadic ABI is separately verified. A fixed-surface wrapper may still bind a fixed-arity export such as `__open` rather than expose variadics it does not need.
3. C-facing signatures are checked by a positive, position-aware C-ABI compatibility predicate (chunk 1B decision). Allowed: the fixed-width integers `i8 i16 i32 i64` and `u8 u16 u32 u64`, the platform words `int`/`uint`, `usize`, `f32`, `f64`; explicitly backed enums, represented exactly as their fixed-width backing integer; plain and writable pointers (`*T`, `mut *T`) whose pointee shapes are recursively compatible; distinct types whose bases are allowed in the same position; structs whose fields are recursively compatible; arrays whose elements are recursively compatible only when nested inside a compatible aggregate or behind a pointer; and explicitly `extern "c"` function values only as direct parameters or returns. A no-value (void) shape is allowed only as a function return. Direct array parameters and returns are rejected. Rejected: `bool`, default enums without an external width promise, `string`, slices, contracts, tagged unions, `Error`, error domains, `typeid`, every tuple shape, ordinary Ace function values, type parameters, generic declarations, and generic instantiations. `mut` parameters stay internal-only — writable storage crosses the boundary as an explicit `mut *T` (the former `mut p` → `T*` export mapping under Functions & ABI #6 is struck). The recursive check carries a cycle guard so C-compatible self-referential structs through pointers terminate correctly. Every rejected category is additive later, each only after an exact C representation is separately approved. Rationale: a positive whitelist states exactly what the compiler has verified against the platform ABI; anything absent is unverified, and under the priority stack an explicit opt-in beats accidental shape coincidences (`bool` vs `_Bool` promotion rules, default enum width freedom, slice/string descriptor layout).
4. Ace programs never depend on another programming language or its utility
   runtime — interop is a declared boundary, not a foundation. Ace language and
   standard-library behavior is implemented in Ace or emitted directly. A
   target facility is reached through the OS's stable public interface first;
   when a direct syscall is unavailable or not a stable application contract,
   target code uses the system library or framework supplied by that OS. Thus
   Darwin uses libSystem for OS services, process services, the selected
   process-wide allocator, and comparable platform facilities rather than
   private kernel entry points. This permission is purpose-based: a symbol's
   presence in libc, libSystem, or another system library does not justify using
   another language's sorting, searching, string, memory, numeric-conversion,
   formatting, or other convenience implementation. User programs remain free
   to opt into explicit interoperability. The bootstrap compiler's C11 source
   and its ordinary C implementation facilities are a build-time choice, not
   part of Ace program semantics or the generated runtime model.
5. A callable C function-pointer type is spelled `extern "c" (P1, P2): R`;
   zero parameters use `extern "c" (): R`, and the return clause is omitted for
   no value (`extern "c" ()`). Its parameters and return independently satisfy
   the positive C-ABI whitelist, and variadic, generic, and non-`c` function
   types are rejected. The value is one pointer-sized word with canonical zero
   `null`; equality with `null` is ordinary function-value equality. A checked
   build diagnoses calling null and a release build assumes the callee is
   nonnull. An `export` function or an imported `extern "c"` declaration with
   the exact signature is referenced by its bare name under an explicitly typed
   destination; an ordinary Ace function never converts to this type. Calls use
   the platform C convention in both directions. C function values are admitted
   directly as C parameters and returns, not recursively inside aggregates or
   behind another pointer in this initial slice. There is no address sigil,
   closure, captured context, lifetime management, generic callback, calling-
   convention inference, or arbitrary foreign ABI. Rationale: a distinct type
   makes the foreign convention visible while retaining the existing one-word,
   explicit function-value model; the narrow direct position proves the first
   POSIX-style thread entry callback without enlarging unrelated aggregate ABI.

## Abstraction
Decision: composition over inheritance; dispatch is explicit.
1. Encapsulation = module privacy (see: Modules) — the file is the unit of hiding, not a class.
2. Abstraction + dynamic polymorphism = explicit contract values (vtable + data, constructed and visible). Signatures are contracts: explicit types, explicit exports.
3. Implementation reuse = explicit struct embedding at a known offset + type-owned methods. `embed name: Base` stores an ordinary named `Base` field and promotes its methods one level to the containing struct. A method owned directly by the containing struct wins; otherwise two exact promoted matches are an ambiguity error. Fields are never promoted. Promotion preserves the method's receiver mutation and visibility rules. A method declaration is `Circle.area()`; its implicit `self` receiver is read-only unless the declaration begins with `mut`. Visibility precedes mutation (`pub mut Circle.scale(k: i32)`).
4. Inheritance is rejected: subtyping is served by contracts, implementation reuse by composition.

Carved (contract value layout):
5. A contract value is two words: a pointer to a const vtable (read-only memory) plus a data pointer. Its all-bits-zero value is the null contract; there is no separate optional contract type.
6. A vtable is a struct of function values typed by signature — no new type concept.
7. Each struct has at most one canonical signing, declared in the struct's defining module after all named contracts in its list are declared: `Circle signs Shape, Color`. A second `Circle signs` declaration is an error, as is repeating a contract in the list. A multiline contract list uses parentheses. The signing is checked at end-of-file so its type-owned methods may follow it. Methods may exist without a signing.
8. A signing registers one exact concrete/contract pair per listed contract. Construction names the target contract explicitly: `s: Shape = Shape(&c)`. The compiler looks up the concrete type's one signing and emits one const vtable per exact used pair. There is no named signing value, alternate implementation, open-ended search, runtime lookup, or trait solving.
9. Contract methods are plain signatures with the receiver implied. Conformance matches method name, receiver `mut`, exact parameter types, and return type. Methods may overload by exact parameters and receiver mutation; return types must match a requirement but never distinguish or select overloads.
10. Calls are `c.area()` and `mut c.scale(2)` in both statement and value-producing expression positions; direct disambiguated calls are `Circle.area(c)` and `Circle.scale(mut c, 2)`. `Circle.area` by itself is not a first-class method value. Dispatch through a contract calls the selected vtable slot with `s.data` and retains the same visible `mut` call-site rule.
11. `Shape(&c)` does not mutate `c`; a later `mut s.scale(2)` is the explicit mutation. Const remains absolute: a contract constructed over const backing storage may be read through, but mutating dispatch through it is a compile error. This property is compile-time-only and adds no runtime word or check.
12. Signings and methods are private by default. `pub Circle signs Shape, Color` exports every conformance in that canonical signing; `pub Circle.area()` exports a method for direct external calls. A public signing may use private methods internally.
13. Generic signings ride the bracket rules: `Box[T] signs Value[T]`. Both sides are explicitly instantiated at use (`Value[int](&box)`), and each used exact pair gets its own stamped vtable. The one-signing rule applies to the nominal generic struct; per-instantiation/conditional signings are rejected.
14. Contract promotion is one-way from an explicitly embedded base to its embedder, never in reverse. If the base signs the same contract, its promoted exact methods may satisfy the embedder's explicit signing. Dispatch uses a compiler-generated fixed-offset receiver thunk; it adds no object metadata or allocation. Embedding never makes the base satisfy contracts signed only by the embedder, never creates a subtype relation, and never searches beyond the directly embedded field.

## Tagged Unions
Decision: closed inline sums with explicit construction and exhaustive inspection.
1. A tagged union declares a fixed set of uniquely named variants. A variant carries either no payload or exactly one explicitly typed payload; structs or tuples group multiple values. Variant names are ordinary identifiers, not keywords.
2. Declaration syntax is `Event: union { none, quit, key: u32 }`; generic unions use the ordinary explicit type-parameter form. `union` is a keyword.
3. Construction names the type and variant: payloadless `Event.quit` is a complete `Event` value, while `Event.key(42)` constructs the payload variant. A payload variant is not a first-class function. Untyped literals materialize at the declared payload type; no other conversion is implicit.
4. Representation is an `i32` discriminant at offset zero plus aligned inline storage sized for the largest payload. Declaration order assigns discriminants from zero. Union alignment is the maximum tag/payload alignment and size includes ordinary padding. No niche optimization, allocation, boxing, reflection, or hidden pointer exists.
5. The all-bits-zero union value is the first variant with its payload, if present, recursively zero. Globals without initializers therefore hold the first variant. `null` does not materialize a union value. Inactive payload bytes are semantically irrelevant and cannot be accessed; constructing a payloadless variant need not clear them.
6. Assignment copies the complete value and changing variants is whole-value assignment. A bare local remains uninitialized until whole assignment. Payload binders are initialized local copies, so mutating one never mutates the original union.
7. Union inspection uses `switch`. Patterns are `Event.quit` or `Event.key(code: u32)`; binder types are explicit and must exactly match the payload. Direct inactive-payload projection is not expressible.
8. A union switch must cover every variant once or contain `default` (the last case, per Conditions #2). Duplicate variants, the wrong union, a missing/extra binder, and a mismatched binder type are compile errors. One union variant appears per case in the initial surface; cases do not fall through.
9. `==` and `!=` compare discriminants first and only the active payload when tags match. Equality exists only when every payload type supports equality under the ordinary rules; inactive bytes and padding are never compared. Numeric tags are not exposed and tagged unions are not bit flags.
10. Generic unions stamp only through explicit type arguments like generic structs. Type identity is nominal plus exact type arguments; aliases share identity and `distinct` remains distinct.
11. Public unions and their qualified variants follow normal module visibility. Tagged unions are initially rejected in `extern` and `export` signatures; no C representation is implied.
12. Tagged unions are closed data alternatives, distinct from contracts: contracts provide open-ended behavior through explicit vtable/data values. Ace has no universal `any` type.

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
9. Generic specialization: a generic function declaration may be followed by same-name specializations whose brackets name concrete type arguments — `parse[int](text: string): (int, bool) { … }`. Each specialization is an ordinary, independently checked function, not a template instance.
10. Selection is exact: an instantiation resolves to the specialization whose type-argument tuple equals the written arguments; with no match, the template stamps as usual (rules 1–8). A specialization's parameters, `mut` positions, and returns must equal the substituted generic signature — checked at declaration against the expected signature, never discovered at a call. Duplicate specializations for one exact tuple on one generic overload are a declaration error.
11. A specialization binds to exactly one generic overload, chosen by ordinary substituted-parameter matching (Names & Scoping #7); each overload of a name specializes independently. All type parameters must be supplied positionally — no partial specialization.
12. Bracket interpretation is ordered, not inferred, and brackets hold bare named types only (v1): brackets after a name that already declares a generic are specialization type arguments; brackets after a name without one are a fresh type-parameter list. A specialization must therefore follow its generic; brackets whose identifiers name existing types with no preceding generic are a compile error, never a silent new generic. Compound type arguments (`[]u8`, `*Node`, `Pair[int, string]`) are an additive relaxation.
13. `specialized` after the return clause marks a template with no default body [P: spelling]. Calling it with an unmatched type tuple is a call-site error naming the missing type and the declaration; a `specialized` generic with zero specializations in the compiled program is a declaration error.
14. Scope: specializations live in the generic's module (v1); struct generics (`Box[T]`) are untouched, and type-owned generic methods specialize additively later.

## Functions & ABI
1. Multi-return — decided (see: Errors). Returns are positional: types only, `(int, Error)`; names appear at destructuring, never in signatures.
2. Multi-returns are laid out as anonymous structs: small ones ride platform registers, larger ones use the hidden return pointer — plain platform struct-return rules.
3. `mut` parameters pass the caller's actual address (writes visible to the caller); read-only parameters pass addresses too, but small ones may travel by value — identical observable behavior, legal codegen freedom (same class as copy elision).
4. Internal calling convention: the platform convention (SysV, AArch64, …) — debuggers, profilers, tracers, and the C boundary work unchanged.
5. Parameter aliasing: parameters may alias unless proven otherwise; checked builds may test overlap; read-only marks are the optimizer's facts.
6. Export mapping: a single C-compatible return; parameters and returns must satisfy the C-ABI whitelist (see: C Interop #3). `mut` parameters and multi-return stay internal-only — writable storage crosses as an explicit `mut *T`. The former `mut p` → `T*` export mapping was struck by the chunk 1B decision.
7. No user variadics in v1 — pass slices explicitly; C variadic functions (printf) import via extern blocks only. Slice-sugar variadics are additive later if ever wanted.

## Names & Scoping
1. Qualification is the primary namespacing mechanism — modules provide what C fakes with prefixes: `parser.init()` instead of `ace_parser_init`; import aliasing resolves module-name collisions. Selective import is the explicit opt-in for bringing particular public spellings into their ordinary namespaces.
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
Decision: minimal, allocation-explicit, one fixed process-wide strategy.

Naming convention: public standard-library names are short and descriptive.
Functions use lowercase words separated by underscores when separation improves
reading; types, error domains, and other type-like names use compact PascalCase.
The module qualifier supplies domain context, so a member does not repeat the
module or its primary subject when the shorter name remains unambiguous. Keep
the subject when a module can expose multiple categories of the same form;
`net.SocketType` and `sys.FileType` therefore remain specific rather than
collapsing to `Type`. Preserve each identifying word;
shorten only established, immediately recognizable terms such as `Err`, `Addr`,
and `Fd`, never an ordinary word merely to remove characters. In particular,
`ProcessStatus` remains complete because `Stat` already names filesystem status
and statistics. Error domains use the operation name plus `Err`, omitting a
generic mutator verb when the operation itself is named as a reversible property
(`reuse_addr` / `ReuseAddrErr`, `nonblock` / `NonblockErr`). Public facades and
their selected OS files expose identical spellings. OS-defined constants and
established boundary terms retain their standard spellings (`O_NONBLOCK`,
`mkdir`, `argc`, `errno`). Rationale: module context and conventional
abbreviations remove noise without introducing a private shorthand vocabulary
or making related facade and target APIs differ.

1. Error inspection uses the predeclared packed `Error` value's direct read-only `code`, `domain`, `name`, and `message` fields (see: Errors). The description fields use only compiler-emitted static data when referenced. The `fmt` surface adds no `Error` overload.
2. The raw memory layer is exported by `mem`: the three allocation functions from Memory #10 (`alloc`, `realloc`, and `release`) with their explicitly instantiated typed overloads from Memory #22–27, and the allocation-free byte operations from Memory #15–16 (`copy`, `move`, `fill`, `zero`, and `compare`). `equal` accepts byte-backed arrays and the shared `string`/`[]char`/`[]u8` view type, including writable forms and independently sized fixed arrays, and returns `bool` after comparing length then exact bytes; it allocates, converts, decodes, and normalizes nothing.
3. v1 uses the platform general-purpose allocation boundary directly and adds no release-mode Ace metadata. `[]u8` already carries the only language-visible allocation facts needed by the raw API: pointer and requested byte length. Alignment is supplied explicitly to `mem.alloc`; unsupported alignment is an invalid request and returns null rather than being silently weakened.
4. Checked-build allocation tracking is tooling, not another allocator strategy or a release-mode representation change.
5. Import resolution: bare source name = stdlib only (`import math`, `import min from math`); local files always use an exact `.ace` path (`import util: "./util.ace"`, `import value from "./util.ace"`). A local file cannot shadow a standard module. A bare qualified, aliased, or selective std import resolves either `std/<name>.ace` for a single-file module or `std/<name>/mod.ace` for a directory module. Defining both is an ambiguous std module and a compile error; defining neither reports both attempted paths. Files inside a directory module import one another by relative `.ace` paths, but canonical-path enforcement rejects an import of any implementation file from outside that directory module; `mod.ace` is the only external target. Directory packages for user code remain future work. A single file's canonical path remains its compilation identity, while the `mod.ace` boundary determines external visibility. `std/@/` is the compiler-reserved standard-library implementation namespace and uses an unquoted, suffixless internal source such as `import big: @bigint` or `import value from @shared`; `@name` resolves only to the exact file `std/@/name.ace` beside the compiler. It is not a general path prefix, attribute, package namespace, or user-definable mechanism, and adding `.ace` is invalid syntax. Every source canonically beneath the compiler-shipped `std/` tree may import these files, including imports between reserved files, while sources outside that canonical tree may not. The restriction is checked after path canonicalization, so relative traversal, alternate lexical paths, and symlinks cannot expose reserved modules. It applies to import edges, not roots, and does not attach special status to equivalent source copied elsewhere. No bare `@` or `internal` module exists. Rationale: one conventional entry name gives a directory module an explicit, auditable boundary without repeating its public name, while exact user paths and ambiguity diagnostics keep resolution simple. The suffixless `@name` spelling presents a reserved module rather than an implementation filename and remains visibly distinct from userland and ordinary standard modules.
6. Platform access uses the target OS's stable public interface. A supported
   direct syscall may be used where the target guarantees it; otherwise the
   target layer uses the OS-provided system library or framework through its
   declared ABI (`extern "c"` for macOS libSystem). The ABI spelling describes
   how to call the platform, not a dependency on the C language or runtime. This
   is the OS front door, not permission to outsource Ace behavior; C Interop #4
   defines the boundary.
7. `string` is the complete-text intent spelling of `[]char`/`[]u8` (UTF-8 by convention); all three share type identity and machinery.
8. v1 contents — eight modules: `mem` (the raw allocation layer plus explicit byte/memory operations), `sys` (OS calls), `fmt` (print/format), `bits` (explicit access to and operations on machine representations), `math` (`sqrt`, `floor`, `ceil`, `exp`, `log`, `pow`, `sin`, `cos`, classification, `abs[T]`, `min[T]`/`max[T]`, …), `ascii` (ASCII byte classification, conversion, and comparison), `str` (byte/string operations), `simd` (`f32x4`, `i8x16`, arithmetic and shuffles emitting NEON directly — explicit vectorization, zero compiler analysis). `bits.from_float(f64): u64`, `bits.from_float(f32): u32`, `bits.to_float(u64): f64`, and `bits.to_float(u32): f32` copy the complete representation unchanged rather than performing numeric conversion. The exact overloads preserve signed zero, infinities, subnormals, and NaN payloads and lower to register moves without allocation or runtime calls. Ordinary numeric casts retain their numeric meaning.
   Integer representation operations are exact overloads for `u8`, `u16`, `u32`, and `u64`. `bits.pop_count(value): uint` returns the number of set bits. `bits.lead_zeros(value): uint` and `bits.trail_zeros(value): uint` return the number of consecutive zero bits from the named edge; for zero they return the operand width. `bits.byte_swap(value)` returns the same type with its bytes reversed and is the identity for `u8`. `bits.rotate_left(value, count: uint)` and `bits.rotate_right(value, count: uint)` return the same type and reduce `count` modulo the operand width, so every count is valid. Signed integers and `usize` require an explicit conversion to a fixed-width unsigned type; transparent platform aliases share their underlying overload as usual (`uint` is `u64` on Apple arm64). The three counts return `uint`, rather than `int` or an artificially narrow integer, because their result domain is intrinsically nonnegative; that accuracy costs no runtime performance. All operations allocate nothing and lower to target instructions without runtime-library calls.
   Float classification is the exact `f32`/`f64` overload set `math.isnan(value): bool`, `math.isinf(value): bool`, and `math.isfinite(value): bool`. `isnan` recognizes every exponent-all-ones, nonzero-fraction encoding, including quiet and signaling NaNs with either sign. `isinf` recognizes only positive and negative infinity. `isfinite` recognizes zeros, subnormals, and normal finite values. The pure-Ace implementations inspect representations through `bits`, allocate nothing, and make no libm call. Rationale: the compact established names remain unambiguous under the `math` qualifier, while representation access stays centralized in `bits`.
   Exact rounding operations are the `f32`/`f64` overload set `math.sqrt(value)`, `math.floor(value)`, and `math.ceil(value)`, each returning its operand's type. They are exactly the IEEE-754 operations under round-to-nearest-even: `sqrt` is correctly rounded at the operand's own width (an `f32` root is rounded once at `f32` precision), `floor` and `ceil` round to the integral value toward negative or positive infinity. Zeros, infinities, and already-integral values return unchanged (`sqrt(-0.0)` is `-0.0`, `ceil(-0.25)` is `-0.0`), a NaN operand returns a NaN, and `sqrt` of a negative nonzero value returns a NaN whose sign and payload are unspecified. No errno or floating-point flag is observable. They allocate nothing, make no libm call, and lower to `fsqrt`, `frintm`, and `frintp`. Results are floats; a float-to-integer conversion remains an explicit cast. Rationale: each operation has one exact result, so no accuracy contract is needed, and a compiler-provided instruction is the machine truth.
   Exponential, logarithm, power, sine, and cosine are the `f32`/`f64` overload set `math.exp(value)`, `math.log(value)`, `math.pow(base, exponent)`, `math.sin(value)`, and `math.cos(value)`, each returning its operands' type. They are pure Ace with no `extern` and no platform-libm call (C Interop #4), so results are identical on every target. Every `f64` result is within 1 ULP of the exact result over the whole finite domain, and so is every `f32` result. Special cases follow C99 Annex F: `exp(±0)` is `1`, `exp(+inf)` is `+inf`, `exp(-inf)` is `0`, overflow gives `+inf`, and underflow is gradual through subnormals; `log(+0)` and `log(-0)` are `-inf`, `log` of a negative value is a NaN, `log(1)` is `+0`, and `log(+inf)` is `+inf`; `sin(±0)` is `±0`, `cos(±0)` is `1`, and `sin` or `cos` of an infinity is a NaN; `pow(x, ±0)` is `1` and `pow(1, y)` is `1` for every operand including a NaN, `pow(-1, ±inf)` is `1`, a finite negative base with a finite non-integer exponent is a NaN, `pow(±0, y)` and `pow(±inf, y)` split on whether `y` is an odd integer, and `pow(x, ±inf)` splits on whether `|x|` is below or above 1; `pow(x, 2)`, `pow(x, 1)`, `pow(x, -1)`, and `pow(x, 0.5)` for a positive base are correctly rounded; `pow` is exact whenever the result is a representable integer power, is table-driven with its two 128-entry tables derived in `std/@/pow_tables.ace`, and its `f64` result is within 0.51 ULP, or within 1 ULP when the result is subnormal, while its `f32` result, from a separate plain-`f64` kernel over the same tables, is within 0.51 ULP; a NaN operand returns a NaN, and a generated NaN has unspecified sign and payload. No errno or floating-point flag is observable, and the default rounding mode is assumed. `sin` and `cos` cover the full finite domain: arguments beyond about 1.6e6 use exact Payne–Hanek reduction against a 1280-bit table of 2/π rather than an unspecified range. Platform libm may serve as a differential-test reference, never as a dependency. Rationale: a stated bound is testable and portable, and in-Ace implementations leave `math` usable without a system library.
   Intrinsic declarations: a function declaration may use the trailing body substitute `intrinsic`, as in `pub from_float(value: f64): u64 intrinsic`, only for an exact compiler-provided standard-library operation. It is not a user-extensible foreign or assembly hook. The admitted public operations are the nongeneric, top-level `bits` overloads and the `math` exact-rounding overloads specified in #8; methods, private declarations, generics, specializations, `extern`, and `export` uses are rejected. The compiler emits an ordinary callable symbol for every admitted declaration so explicitly typed function values and indirect calls have the same semantics as direct calls. A statically resolved direct call may lower inline to the same instruction sequence. Rationale: source states honestly that no Ace body exists, the public API remains first-class, and the exact whitelist adds machine operations without exposing backend internals or an unrestricted intrinsic namespace.
   The compiler-shipped `std/@/byte_load.ace` module additionally admits exactly `pub unaligned_u64(text: string, index: usize): u64 intrinsic`. Only standard-library sources can import the reserved module. The caller must establish `index + 8 <= text.len` before the call; it reads one target-native-endian word without an alignment requirement and introduces no public operation, allocation, or platform-library dependency. This narrow primitive supports word-at-a-time internal byte algorithms without exposing pointer reinterpretation or a general intrinsic facility.
9. Output API begins as an exact overload set. `fmt.print(text: string): int` writes the supplied bytes exactly; `fmt.print(value: int): int`, `fmt.print(value: uint): int`, and `fmt.print(value: usize): int` write signed platform integers, unsigned platform integers, and memory-sized unsigned values in base-10 ASCII. None adds a newline or allocates; numeric conversion uses a fixed 20-byte stack buffer and covers each type's complete range. Every overload retries partial writes and returns bytes written, or the non-positive result from the current OS boundary. Overload policy (decided): the platform-word pair (`int`, `uint`) beside `usize` and `string` is the integer output set; integer widths narrower than the platform word get no overload — callers convert explicitly (`fmt.print(int(my_i32))`, `fmt.print(uint(my_u16))`) so the printed type stays visible (owner: keep narrower integers explicit for now). Float output (decided): `fmt.print(value: f32): int` and `fmt.print(value: f64): int` write shortest round-trip decimal — the fewest significant digits that parse back to the exact original value — with non-finite values spelled `nan`, `inf`, and `-inf`, negative zero keeping its sign (`-0`), and no trailing `.0`. Decimal notation for scientific exponents -6 through 20; larger or smaller magnitudes use scientific spelling (`1e21`, `1.5e-7`) with no `+` and no exponent padding. Line output (decided, implemented): `fmt.println` mirrors the complete `print` overload set (`string`, `int`, `uint`, `usize`, `f32`, `f64`) and writes exactly the corresponding `print` representation followed by one `\n`, allocating nothing. It returns the total bytes written including the newline; a non-positive result from a nonempty value write returns immediately with no newline attempted; the empty-string overload writes the newline directly; a newline failure after a written value returns the newline's failure unchanged. There is no zero-argument form — `fmt.print("\n")` already writes a bare newline. Formatted `fmt.printf` is carved in #12; a possible `Error` convenience overload remains additive.
10. `sys` file I/O and process-start access (implemented) — file I/O is pure Ace over `extern "c"` libSystem declarations, exactly the `fmt` pattern. Process arguments and environment entries use compiler-backed declarations because Ace intentionally has no pointer arithmetic over the platform's C pointer arrays. The public surface:
    - `Fd: distinct i32` — the descriptor handle. Canonical zero is fd 0 (stdin, valid), so zero is not an invalid sentinel; `-1` marks a failed `open`, compared directly (`fd == -1`); no named invalid constant.

    - Standard descriptors (implemented): `pub stdin`, `pub stdout`, and `pub stderr` are `const Fd` globals holding fds 0, 1, and 2, valid from process start and consumed through the ordinary `read`/`write` operations. Spellings mirror the OS words (`stdin`/`stdout`/`stderr`), consistent with the flag-constant rule; the owner's original `in` spelling is unavailable because `in` is a keyword. They are the only named descriptor constants. Their initializers are distinct-type constructor constants, folded by the emitter under the widened constructor-constant rule recorded in the implementation-phase list.
    - `read(fd: Fd, buf: mut []u8): int, Error` — bytes read; `0` with `null` error is EOF. Buffer content writability is carried in the parameter's type (Mutability #10); replacing the caller's buffer variable would take a separate `mut buf` mark.
    - `write(fd: Fd, data: []u8): int, Error` — bytes written; partial counts are legitimate results and are returned as-is with no hidden retry (all-or-retry policy belongs to callers or a later honestly-named `write_all`).
    - `close(fd: Fd): Error`.
    - `open(path: string, flags: i32): Fd, Error` and `open(path: string, flags: i32, mode: u32): Fd, Error` — the 2-arg form passes `mode: 0` to the same fixed 3-parameter extern (POSIX ignores mode without `O_CREAT`).
    - Error mapping: each operation has its own nominal error domain (`OpenErr`, `ReadErr`, `WriteErr`, `CloseErr`). A wrapper captures errno immediately after failure and returns it as `error.code`; `pub errno(): i32` remains available for direct platform work but is not needed to preserve a wrapper failure.
    - Paths: copied into a fixed `[1024]u8` stack buffer (`PATH_MAX` including NUL — the kernel rejects longer paths itself, so the buffer never rejects a legitimate one), NUL appended, then passed to the OS. An overlong path returns the ordinary `open` error with `ENAMETOOLONG` written into errno by `sys` itself. Every public `sys` function is allocation-free; only callers allocate.
    - Flags and creation permissions: raw `pub` i32 flag constants mirror Darwin's values and spellings exactly (`O_RDONLY` … `O_NONBLOCK`, including POSIX `O_CREAT`) and compose with `|`. The POSIX permission constants are the `u32` set `S_IRWXU`, `S_IRUSR`, `S_IWUSR`, `S_IXUSR`, `S_IRWXG`, `S_IRGRP`, `S_IWGRP`, `S_IXGRP`, `S_IRWXO`, `S_IROTH`, `S_IWOTH`, and `S_IXOTH`, likewise composed with `|` for the mode argument of file and directory creation. They request permissions; the process umask may remove bits. File-type and special-mode bits remain deferred until real usage justifies them. Rule: `sys` mirrors OS spellings for OS-defined words; Ace names belong to Ace abstractions. Errno name constants remain deferred. Rationale: distinct-typed handles prevent silent integer misuse where the OS cannot diagnose it; flags and POSIX permission bits stay plain because the kernel validates their combinations; per-operation domains plus the errno query keep one fact in one place without bending the static-descriptor rule.
    - Implementation facts: the raw bindings are private link-named externs (`c_open` → `__open` because `open` is variadic and Apple arm64 passes variadic args on the caller's stack). `write(fd: Fd, data: []u8)` accepts text as well because `string` and `[]u8` share identity; a spelling-only overload would be a duplicate declaration.
    - Secure entropy (implemented): `random(storage: mut []u8): Error` fills
      caller-owned storage from the operating system and allocates nothing.
      Empty storage succeeds without a foreign call. Darwin uses `getentropy`
      in chunks of at most 256 bytes, retries `EINTR`, and returns every other
      failure through `RandomErr(code)` after capturing errno immediately.
    - Darwin descriptor control (implemented): `ioctl(fd: Fd, request: uint): Error` performs a request with no unnamed argument; `ioctl(fd: Fd, request: uint, data: mut []u8): Error` passes the address of the caller's first mutable byte as the single unnamed argument and requires nonempty storage. Failure is `IoctlErr` with errno captured immediately; an empty data view fails locally with `EINVAL`. The request type is `uint` because Darwin declares it as `unsigned long`, which is 64 bits on Apple arm64; it is not narrowed to the 32 bits occupied by current encoded values. The target layer binds the genuinely variadic `ioctl` entry point so Apple arm64 places the unnamed pointer in its required eight-byte stack slot. The Darwin BPF requests `BIOCGBLEN`, `BIOCPROMISC`, `BIOCSETIF`, and `BIOCIMMEDIATE`, plus the classification errno constants `EBUSY` and `EACCES`, are exposed only for the packet-sniffer proof and retain their target spellings and values. Their raw definitions, the verified `ifreq` and `bpf_hdr` layouts used to derive them, and the C binding stay in `std/sys/os/darwin-arm64.ace`. BPF records use the native 18-byte `bh_hdrlen` through its last declared field, excluding the C structure's two tail-padding bytes, and advance by four-byte `BPF_WORDALIGN`. The byte-buffer overload does not infer an object type, validate an encoded request size, reinterpret storage, or promise that a request is portable; the caller selects a target request and supplies its exact native bytes. Rationale: one allocation-free escape hatch is smaller and more honest than a premature portable device-control abstraction, while caller-owned bytes preserve explicit layout and make the existing bounded manual parsing baseline sufficient for BPF.
    - Filesystem metadata and mutation (implemented, Darwin/Apple-arm64): `FileType: enum { unknown, regular, directory, symlink, other }` classifies only the stable categories exposed by the facade. `FileInfo: struct { size: u64, file_type: FileType }` is canonical zero for a failed query. `stat(path: string): (FileInfo, Error)` follows symbolic links and returns their targets' metadata; it exposes byte size and file type only. Timestamps, permissions, ownership, inode/device identity, link count, allocated blocks, and a no-follow query remain additive. `mkdir(path: string): Error` is the portable form: on POSIX it requests `S_IRWXU | S_IRWXG | S_IRWXO` (`0777`) and lets the process umask select effective permissions. It creates exactly one directory and never creates missing parents. The overload `mkdir(path: string, mode: u32): Error` supplies an explicit Darwin/POSIX permission request, matching the existing creation-mode parameter of `open`; it is a target-specific facility, not a cross-platform permission abstraction. Windows permissions are ACL-based, so a future Windows target retains the one-argument operation and must separately carve richer security rather than pretending POSIX owner/group/other bits represent ACLs. `rename(old_path: string, new_path: string): Error` and `unlink(path: string): Error` perform exactly the corresponding OS operation, with no recursive behavior or retry.
    - Directory iteration (implemented): `Dir: struct { fd: Fd, buffer: [4096]u8, next: usize, count: usize, base: i64, active: bool, done: bool }` is caller-owned buffered iteration state, not an alias of `Fd` and not an ownership-tracked value. Its representation is visible under the ordinary struct rules but its fields collectively form handle state; callers do not edit them between directory operations. Its canonical all-bits-zero value is invalid even when descriptor zero is usable. `open_dir(path: string): (Dir, Error)` opens only a directory. `read_dir(mut dir: Dir, name: mut []u8): (usize, FileType, bool, Error)` returns, in order, the name byte count, file type, end flag, and error. The parameter mark on `mut dir: Dir` grants updates to the caller's iteration value and must be repeated at the call; independently, type-position `mut` on `name` grants writes to its bytes without replacing its descriptor. A successful entry returns `(name_length, file_type, false, null)`. End returns `(0, unknown, true, null)`. Failure returns `(0, unknown, false, error)`. Names are exact bytes with no appended NUL; `.` and `..` are skipped. Enumeration order is the OS order. An entry file type may be `unknown`, in which case callers use `stat` when classification is required. If the name buffer is too small, the operation returns `ReadDirErr(ERANGE)` without truncating or consuming that entry, so a later call with a larger buffer retries it. `close_dir(mut dir: Dir): Error` closes the resource and replaces the caller's complete `Dir` value with canonical zero, including on close failure. Copying `Dir` copies its descriptor and buffered cursor; aliases are not found or invalidated and using them after either copy is closed has the ordinary manual-handle hazards. There is no hidden allocation, ownership, destructor, automatic close, sorting, recursion, path joining, or retained entry view.
    - Each added operation returns the ordinary packed `Error` type, null on success; error-domain declarations are constructors/classifiers, not signature result types. The public domains are exactly `StatErr`, `MkdirErr`, `RenameErr`, `UnlinkErr`, `OpenDirErr`, `ReadDirErr`, and `CloseDirErr`, and each wrapper captures errno immediately. Every path uses the same fixed 1024-byte stack conversion as `open`; overlong paths report that operation's own domain with `ENAMETOOLONG`. Darwin structure layouts, directory-record parsing, `O_DIRECTORY`, and raw link names remain private to the target layer. Rationale: caller-owned batching gives one-entry iteration without libc allocation, truncation, or an OS-specific record surface; explicit `mut` makes cursor and invalidation changes visible; the deliberately small metadata value avoids freezing incidental Darwin ABI while leaving richer queries additive.
    - Process arguments: `argc(): usize` returns the process argument count in constant time. `arg(index: usize): string` requires `index < argc()`, scans only that NUL-terminated entry, and returns a zero-copy read-only view valid for the process lifetime. Index zero is the program invocation name. The bounds precondition follows the ordinary sequence contract: checked builds diagnose a violation and release assumes it.
    - Environment: `envc(): usize` scans the null-terminated environment pointer array on every call and is therefore O(environment entries); callers capture the count before iteration. `env(index: usize): string` requires `index < envc()`, scans only the selected NUL-terminated entry, and returns its raw zero-copy `KEY=VALUE` bytes. Behavior after environment mutation through linked C code is not yet specified. All four operations are Darwin/Apple-arm64-only and allocate nothing.
    - Process termination: `exit(code: int)` terminates the process immediately with the requested status and never returns. It is callable from any function and declares no return value. It does not unwind the call stack, run function-scope defers, or invoke C-runtime exit handlers. The `int` is passed through the target's process-exit boundary; which status bits a parent observes follows the target operating system. This is deliberate termination with a caller-chosen status, distinct from `panic`, which reports a broken invariant best-effort and aborts with `SIGABRT` (Errors #15). The OS-neutral `sys` facade calls its private target OS layer; `std/sys/os/darwin-arm64.ace` converts the code to `i32` and calls libSystem `_exit(2)`. No bottom type, general non-returning-function annotation, or compiler intrinsic is introduced.
    - Process creation and waiting (implemented, Darwin/Apple-arm64): `Process: distinct i32` is a process handle. `fork(): (Process, Error)` performs exactly one process duplication; it returns the child process handle in the parent, `Process(0)` in the child, or `Process(-1)` with `ForkErr` on failure. It returns in both processes exactly as the OS operation does and performs no allocation, retry, callback, or descriptor cleanup. `ProcessStatus: struct { exited: bool, code: int, signal: int }` is the portable completed-child result. `wait(process: Process): (ProcessStatus, Error)` requires a positive child handle and blocks for that exact child without retrying interruption; zero and negative handles fail with `WaitErr(EINVAL)` rather than acquiring POSIX process-group or any-child meanings. Normal termination returns `exited: true`, the unsigned 8-bit exit code widened to `int`, and signal zero. Signal termination returns `exited: false`, code zero, and the positive signal number. Failure returns canonical-zero status with `WaitErr`. Stopped/continued observations and nonblocking/any-child waits are additive. The facade never exposes Darwin's encoded wait-status word. Rationale: the networking proofs require a real concurrent peer but should contain no platform externs; distinct handles prevent accidental process/fd interchange, while one-operation wrappers preserve machine-visible control flow and keep policy with the caller.
    - Caller-buffer `fill_args`/`fill_env`, keyed environment lookup, and `setenv`/`unsetenv` interaction are additive follow-ups. Rationale: indexed access exposes the existing process storage without manufacturing an incompatible `[]string` descriptor array, hidden allocation, runtime-sized stack storage, or an arbitrary fixed cap.
11. `time` monotonic clocks (implemented) — two parameterless operations return system-wide monotonic clock readings in nanoseconds from origins reset at every boot. `pub elapsed(): u64` continues advancing during system sleep, so differences between its readings measure real elapsed time across sleep and wake cycles. `pub awake(): u64` pauses during system sleep and resumes when the system wakes; it advances whenever the system is awake, regardless of whether this or any other process is running, scheduled, or blocked — it is never process CPU time. Both clocks are immune to calendar-clock changes. Each returned value is a clock reading, not a duration by itself; readings are only meaningfully subtracted when both came from the same operation. Each operation is allocation-free and has no `Error` return: the platform clock reads do not fail here, so an error channel would buy nothing. Implementation is `std/time/mod.ace` over `std/time/os/darwin-arm64.ace` — pure Ace over the private link-named Darwin `clock_gettime_nsec_np` extern (`CLOCK_MONOTONIC_RAW` for `elapsed`, `CLOCK_UPTIME_RAW` for `awake`; the clock-id constants stay private). Calendar time, process CPU time, duration conversion/formatting, and a process-sleep function are deliberately outside this surface; each belongs to its own decision. Rationale: the clocks are distinct operations with distinct time domains, so separate names state whether the reading measures all elapsed time or awake time without a boolean argument.
12. Formatted output (implemented) — `fmt.printf("name=%s count=%d ratio=%.2f\n", name, count, ratio): int` is one compiler-recognized public `fmt` name for compile-time-static formats and runtime values. The format must be a string literal or a const-global `string` initialized from a static string literal; runtime-created or runtime-selected formats are deferred. The compiler parses the format and checks argument count and exact types. There is no runtime format parser, type tag, argument pack, `any`, reflection, allocation, hidden C variadic call, or dependency on C formatting; generated work is direct typed formatting plus the OS output boundary.
    - Initial conversions are `%d` signed decimal, `%u` unsigned decimal, `%b` unsigned binary, `%o` unsigned octal, `%x`/`%X` lower-/uppercase unsigned hexadecimal, `%f` fixed decimal, `%s` exact string bytes, `%c` one byte, and `%%` one literal percent consuming no argument. Binary, octal, and hexadecimal emit digits only, with no base prefix or leading zeroes; zero emits `0`. Pointers, scientific/general float conversions, booleans, errors, and custom formatting remain additive.
    - Exact types: `%d` accepts `int`; `%u`, `%b`, `%o`, `%x`, and `%X` accept `uint` or `usize`; `%f` accepts `f32` or `f64`; `%s` accepts `string`, including embedded NUL bytes; `%c` accepts `u8` and writes exactly that byte. Transparent aliases share their target; distinct types require explicit conversion. Other narrow integers remain explicit. Conversion context materializes literals as `int` for `%d`, `uint` for `%u`/`%b`/`%o`/`%x`/`%X`, `f64` for `%f`, and `u8` for `%c`; a literal intended as `usize` is written `usize(...)`. An argument whose type is not yet known when the plan is built — a call deferred to end of file, such as `g(41)` with a `usize` parameter — is a compile-time error; type its arguments (`g(usize(41))`) or assign its result first. `%c` is deliberately byte-oriented; wider Unicode handling is additive through a future facility, while existing UTF-8 bytes may be written with `%s`.
    - `%f` defaults to exactly six fractional digits; `%.Nf` writes exactly `N`, and `%.0f` writes no decimal point. Rounding is nearest with exact ties to an even final digit, applied to the exact `f32` or `f64` input and independent of the caller's ambient floating-point rounding mode; the helper restores that mode before returning. The original sign is retained when a negative value rounds to zero, including negative zero. Non-finite spellings are always `nan`, `inf`, and `-inf`; precision does not alter them and a NaN sign is not shown. `%f` never switches to scientific notation.
    - No field width, padding, alignment, explicit-plus/space sign, alternate form, dynamic `*`, or positional argument exists initially. The only modifier is the literal precision `%.Nf`; unsupported combinations are compile-time errors.
    - Output proceeds left to right to standard output. A positive partial write is accumulated and its remaining segment retried. Complete success returns the total emitted byte count, including every literal and formatting byte; empty output returns zero. A zero or negative OS result stops output immediately, suppresses later segments, and is returned unchanged even after earlier successful bytes, matching `print` and `println`.
    - Only the canonical standard-module member receives special handling, including through selective import; user declarations named `printf` are ordinary. `fmt.printf` is direct-call-only and cannot be used as a function value because its checked parameter shape depends on the format. Every runtime argument is evaluated exactly once in source order and captured before output begins, so output failure never suppresses later argument evaluation or side effects. The compiler then emits direct typed helper calls, reusing the existing string/signed-decimal/unsigned-decimal paths and adding only private unsigned-radix, fixed-`f32`/`f64`, and one-byte helpers. The unsigned-radix helper receives a compiler-selected base of 2, 8, or 16 and an uppercase flag, using one fixed 64-byte buffer sufficient for the complete binary representation of a 64-bit value. Helpers use fixed stack storage and the shared partial-write retry path; no runtime dispatch or metadata exists.
    - Precision is limited to `0...324` inclusive. Float helpers use one fixed 640-byte stack buffer, sufficient for a sign, 309 integer digits, a decimal point, and 324 fractional digits; conversions reuse it rather than reserving storage per argument. This bound can expose the smallest nonzero `f64` magnitude in fixed notation while keeping runtime and declared memory bounded. Static literal spans are emitted directly from constant storage. There is no `printf`-specific format-length or argument-count cap beyond ordinary compiler limits.
    - Diagnostics distinguish a non-static format, incomplete `%`, unsupported conversion or modifier, malformed precision, precision on a non-`%f` conversion, precision above 324, missing/extra arguments, and exact-type mismatch. They identify the conversion's decoded byte offset and relevant argument; after a malformed format, derived count/type errors for that call are suppressed.
    - Implementation must cover exact runtime bytes and counts for every conversion and boundary; changing runtime values; integer extrema; binary, octal, both hex cases, and both unsigned types; float defaults, explicit precisions, ties-to-even, signed rounded zero, non-finites, and precision 324; empty/NUL-containing strings; byte extremes; `%%`; mixed calls; evaluation order/once-only capture; and closed-output failure. Success fixtures cover literal/const-global formats, qualified/selective imports, aliases, and explicit conversions; diagnostics cover every rejected format, count, type, distinct/narrow type, dynamic format, and precision 325. Normal and sanitizer suites must pass, and object inspection must show no C `printf`, variadic call, allocator, runtime parser, or formatting metadata. When implementation lands, the standard-library guide and its compiler-checked documentation fixture must be updated in the same change; planned features remain outside the implemented manual.
    - Rationale under the priority stack: direct typed lowering, bounded stack storage, and no runtime interpretation put performance first; the small conversion/modifier set and one uniform output contract minimize language, compiler, and runtime machinery; private helper reuse preserves the `fmt` module boundary; static linear validation keeps compile time cheap; and precise compile-time diagnostics add safety without release cost. Every excluded feature can be added later without tightening existing code.
13. Networking (carving) — networking is the multi-file standard-library module rooted at `std/net/mod.ace`. Its public surface is a small stable Ace API backed by private platform bindings, not a public copy of Darwin's socket ABI. The core socket wrapper remains thin, allocation-free, and one-to-one with kernel operations: each public primitive performs one named state transition and adds no bundled setup, buffering, retry policy, callbacks, ownership, or hidden state. Higher-level convenience may compose these primitives later in a separately carved API. Darwin structure layouts, constants, byte-order details, and C link names stay private to the implementation. The initial surface is IPv4-only; IPv6 is an additive future improvement and is not prematurely folded into the first address model. `Addr4: struct { ip: [4]u8, port: u16 }` is the public endpoint value: IP octets are in ordinary textual order and the port is an ordinary host-order number. `net` privately converts that value to and from the Darwin socket representation. Its canonical zero is `0.0.0.0:0`, suitable as wildcard address plus OS-selected port for binding. String parsing/formatting and platform-specific escape hatches are separate additive facilities; the core address and socket operations do not allocate. `net.Socket` is a public transparent alias of `sys.Fd`, not a new distinct handle or duplicated descriptor implementation. Existing `sys` operations are reused wherever their semantics are already correct for sockets, including `sys.close`; socket-specific operations stay in `net`. `SocketType: enum { tcp }` is the initial public type set, and `socket(socket_type: SocketType): (Socket, Error)` creates an IPv4 socket of that type; enum members use ordinary lowercase value naming. UDP is additive as a later `SocketType` member, and a future family-aware overload may add IPv6 without changing existing IPv4 calls. `reuse_addr(socket: Socket, enabled: bool): Error` and `reuse_port(socket: Socket, enabled: bool): Error` each change exactly that socket option; `bind(socket: Socket, address: Addr4): Error` performs only the bind and never enables options implicitly. Blocking acceptance returns `(Socket, Addr4, Error)`, and blocking connection of an existing socket returns only `Error`. `Error` is last and null on success. Failed creation or acceptance returns `Socket(-1)`; failed acceptance also returns the address type's canonical zero. Each operation has its own static error domain, while the OS error number remains available through `sys.errno()`; closing retains `sys`'s existing error domain. The public domain names are exactly `SocketErr`, `ReuseAddrErr`, `ReusePortErr`, `NonblockErr`, `BindErr`, `ListenErr`, `AcceptErr`, `ConnectErr`, `ReceiveErr`, `SendErr`, `ResolveErr`, and `LocalErr`. `nonblock(socket: Socket, enabled: bool): Error` is the separate reversible mode operation; it has its own static error domain, performs no hidden socket-state tracking, and is not folded into initial socket creation. A creation-mode convenience remains additive. Nonblocking connection-in-progress behavior is deferred to the nonblocking-I/O carving. Rationale: networking is a large coherent family deserving a predictable module boundary; hiding incidental Darwin ABI details improves modularity without compromising performance or machine truth. IPv4 first proves that boundary and the later event model with the smallest coherent surface. The explicit `Addr4` value keeps byte order and platform layout out of application code while leaving `Addr6` additive. The alias gives networking a domain name while retaining zero-cost interoperability with ordinary descriptors and the future event queue. One type-parameterized constructor keeps the family predictable as protocols are added; its Ace enum is privately mapped to platform constants. Separate option, bind, and nonblocking operations compose across socket roles and keep every kernel-state transition explicit; convenience belongs above this core rather than inside it. Result shapes follow Ace's existing explicit error convention without returning redundant values from operations on an existing socket.
    - `listen(socket: Socket, backlog: usize): Error` performs only the listen transition. The backlog is a requested pending-connection count; the OS may cap it, and a value outside the platform call's representable range fails rather than truncating. It neither performs other setup nor closes/replaces the socket on failure. Rationale: `usize` expresses a nonnegative count, and retaining a single transition preserves the core composition rule.
    - TCP byte I/O is `receive(socket: Socket, buffer: mut []u8): (int, Error)` and `send(socket: Socket, data: []u8): (int, Error)`. Each performs one receive or send operation, returns partial counts as-is, and never retries or allocates. Receive returns `0` with null error for orderly peer shutdown. Send accepts `string` and `[]char` values by shared identity, while its `[]u8` spelling states transport-byte intent. Send privately uses the platform's per-operation no-signal flag, so a closed peer produces the ordinary send error instead of terminating the process with `SIGPIPE`.
    - `accept(listener: Socket): (Socket, Addr4, Error)` performs exactly one accept attempt and consumes at most one pending connection. On success the listener remains open and listening, while the returned socket retains the properties produced by the platform accept operation; Ace performs no subsequent mode or option configuration. Portable event-loop code therefore configures the accepted socket explicitly rather than assuming inherited nonblocking state. On failure it returns `Socket(-1)`, canonical-zero `Addr4`, and the operation-specific error without retrying; interruption, an aborted pending connection, would-block, and resource failures remain caller-visible through `sys.errno()`. Rationale: the one-attempt rule avoids hidden control flow or renewed blocking, while leaving retry policy and accepted-socket configuration explicit.
    - Implementation organization: the multi-file std module lives in `std/net/`; `mod.ace` is its sole external surface and selects `./os/darwin-arm64.ace` with `@when`, while that target file owns Darwin socket externs, constants, ABI structs, conversions, and the one implementation of each operation. Target-layer declarations used by the facade are module-internal and cannot be imported from outside `net`. Darwin/Apple arm64 is the only supported target; no partial Linux OS layer is carried. Domain-specific bindings do not move into `sys`. Windows remains a separate future checkpoint because its socket handles and completion model do not naturally match the `Fd` alias. Rationale: the directory groups one coherent multi-file module, compile-time selection reflects actual support, and the facade preserves one stable API without redundant target-to-target forwarding.
    - `connect(socket: Socket, address: Addr4): Error` performs exactly one connection attempt. On a blocking socket it returns null once connected or the operation-specific error on failure; it does not retry, close, replace, or configure the socket. Nonblocking outbound completion/error querying, peer-address queries, TCP half-close, and additional TCP options are additive operations deferred until required. Rationale: blocking connect completes the initial client proof with one kernel transition, while the deferred facilities require independent semantics and are unnecessary for the first blocking client or nonblocking servers.
    - `local(socket: Socket): (Addr4, Error)` queries the socket's current local IPv4 endpoint through one OS operation. It changes no socket state and returns canonical-zero `Addr4` with `LocalErr` on failure. After binding port zero, it exposes the OS-selected host-order port without port guessing or process-ID coupling. A socket not yet explicitly bound may report the OS-defined wildcard endpoint; peer queries remain additive. Rationale: the nonblocking proof genuinely requires passing an ephemeral listener endpoint to its client, and the short `local` name leaves the accurate `local`/`peer` pair for endpoint queries.
    - Blocking IPv4 name resolution is `resolve4(host: string, port: u16, addresses: mut []Addr4): (usize, Error)`. It performs one synchronous platform-resolver request, writes addresses in resolver order into caller-owned storage until that storage is full, applies the supplied host-order port to every written address, and returns the number written. Unwritten elements are unchanged. An empty output slice, a hostname longer than the DNS maximum of 253 bytes, an embedded NUL, resolver failure, or a successful lookup with no IPv4 TCP address returns zero and `ResolveErr`; success writes at least one address and returns null error. The initial result does not report whether additional addresses were truncated and exposes no platform-specific resolver code; unlike a socket-operation failure, `sys.errno()` does not describe a resolver failure. On Darwin the private implementation uses `getaddrinfo`/`freeaddrinfo`: the platform temporarily allocates its result list, `net` copies from it and frees it before returning, and no allocation or resolver-owned pointer escapes into Ace. This paired temporary platform allocation is an explicitly approved narrow resolver-boundary exception to the ordinary no-hidden-allocation rule. A future native DNS client may remove the exception while preserving the public operation; richer diagnostics, truncation discovery, IPv6, asynchronous resolution, caching, search-policy control, and caller-selected socket types remain additive. Rationale: caller-owned result storage keeps persistent memory and capacity explicit, one blocking call matches the initial blocking TCP layer, and the narrow platform boundary supplies immediately useful host lookup without first adding UDP, DNS packet parsing, resolver configuration, retry policy, and TCP fallback to Ace.
14. Descriptor polling (implemented) — readiness is the separate multi-file std module rooted at `std/poll/mod.ace`, not part of `net`; it accepts general `sys.Fd` values so sockets, pipes, TTYs, and other supported descriptors share one facility. Its explicit `./os/darwin-arm64.ace` implements kqueue privately. `Queue: distinct sys.Fd` is created by `queue(): (Queue, Error)` with `Queue(-1)` on failure and closed explicitly through `sys.close(sys.Fd(q))`; the queue owns and closes no watched descriptor. `watch(q, fd, readable: bool, writable: bool): Error` registers an fd or replaces its complete readable/writable interest, while `unwatch(q, fd): Error` removes that relationship without closing the fd. Both interests false is `WatchErr` with `EINVAL`; unwatching an unwatched fd is `UnwatchErr` with `ENOENT`. A failed multi-filter kqueue update may leave partial state, so callers must watch or unwatch again before relying on that registration. The paired names follow the general convention that an establishing/reversing relationship uses an accurate lexical pair; semantic accuracy outranks superficial symmetry. Rationale: polling belongs to descriptor I/O rather than networking, while its own module keeps `sys` from becoming an unstructured platform catalogue.
    - Readiness events are level-triggered `Event: struct { fd: sys.Fd, readable: bool, writable: bool, error: bool, hangup: bool }`. At most one returned event represents each fd; OS notifications for that fd are combined. Readable includes an EOF-producing read where applicable, writable promises only that a write-like operation may progress, hangup may coexist with remaining readable data, and error carries no errno value; all bools are independent. Error/hangup need no registered interest. The implementation uses bounded fixed stack collection (currently 64 raw events), may return fewer descriptors than the caller's capacity, allocates nothing, and keeps no registration side table.
    - `wait(q, events: mut []Event, timeout_ns: u64): (usize, Error)` performs one blocking OS collection, fully initializes only `events[0..count]`, leaves the suffix unchanged, and returns zero with null error for timeout/no readiness. An empty slice fails before the OS with `WaitErr` and `EINVAL`. `0` polls; `wait_forever: const u64` (u64 max) blocks without a timeout; other values are finite nanoseconds passed as exact seconds/nanoseconds to kqueue. `EINTR` is caller-visible with no private restart. `QueueErr`, `WatchErr`, `UnwatchErr`, and `WaitErr` are distinct static operation domains with runtime errno queried immediately through `sys.errno()`. Signals, process/vnode/user filters, timers, per-registration data, edge/one-shot triggering, directional half-close, completion I/O, and concurrent mutation guarantees remain additive. Rationale: caller-owned storage, fixed bounded OS-layer work, explicit lifetime, and one wait transition preserve performance and machine truth while OS-neutral public names do not expose kqueue.
    - The implemented nonblocking server proof composes these primitives into one single-threaded multi-client event loop. Client slots, receive assembly, pending broadcasts, per-recipient send offsets, and returned events all live in fixed caller-owned storage. Read and write interest is replaced as that explicit state changes; disconnect handling unwatches and closes the descriptor before making its slot reusable. Partial receive/send progress belongs to the application, and one completed message is broadcast only to the clients connected at that point. The proof adds no queue-owned client state, allocation, retry policy, worker thread, atomic operation, or higher-level networking convenience. Rationale: a deterministic multi-client chat fixture demonstrates that the carved one-attempt operations are sufficient for bounded servers while preserving the machine-visible storage and control-flow model required by `PHILOSOPHY.md` Principles #1–2 and #6.

15. Blocking TCP composition (implemented) — `tcp` is a pure-Ace IPv4 module above the one-operation `net` layer, nested under it and rooted at `std/net/tcp/mod.ace` (imported as `net/tcp`). It declares `Listener: struct { socket: net.Socket, active: bool }` and the `Conn` wrapper whose complete Stage 2 shape is stated below; both are ordinary copyable manual-handle values with no ownership tracking, destructor, or hidden cleanup. Every field declares its canonical-zero default, so `Listener {}` and `Conn {}` spell the empty handle. `active` is true exactly for a handle returned by a successful constructor or accept; canonical zero is therefore the empty handle even though descriptor zero is valid, matching `sys.Dir`. `listen(address: net.Addr4, backlog: usize): (Listener, Error)` composes `net.socket(tcp)`, `net.reuse_addr(true)`, `net.bind`, then `net.listen`, never enables `reuse_port`, and on any setup failure closes the created socket while returning canonical-zero `Listener` plus the original setup error even if cleanup fails. `connect(address): (Conn, Error)` similarly composes creation and blocking connect, closes on connect failure, and returns canonical-zero `Conn` plus the original error. `local(listener)` preserves `net.local`; `accept(listener)` performs one blocking `net.accept` and returns a `Conn`, peer `net.Addr4`, and the original accept error on failure.
    - `receive_all(connection, buffer: mut []u8): (usize, Error)` repeatedly calls `net.receive` over the unfilled suffix until the complete destination is filled. Success returns its full length and null; orderly EOF first returns retained progress and `ReceiveAllErr(0)`; any other failure returns retained progress and the unchanged `net.ReceiveErr`. `send_all(connection, data: []u8): (usize, Error)` similarly calls `net.send` over the unsent suffix; success returns the full length and null, send failure retains progress and the unchanged `net.SendErr`, and a successful zero-progress send returns retained progress with `SendAllErr(0)`. Both retry only an underlying operation error whose code is `sys.EINTR`; they are blocking-only, allocate nothing, and add no one-attempt `send` or `receive` aliases because callers use `net` directly for those semantics. Empty buffers succeed with zero progress.
    - `close(mut listener)` and `close(mut connection)` on an inactive handle perform no syscall, leave the value unchanged, and return null, so a deferred close may precede the constructor's error check exactly as `mem.release` may on a null slice. On an active handle each attempts `sys.close`, then replaces the caller's complete wrapper with canonical zero even when close fails, returning the `sys.CloseErr` unchanged. Copies are not found or deactivated; closing a stale active copy has the ordinary manual-handle hazards. The public high-level domains are exactly `ReceiveAllErr` and `SendAllErr`; constructor, accept, query, I/O, and close failures otherwise preserve their lower-layer domains and codes. `sys.EINTR` is the public target-defined `i32` name required for this explicit retry policy. Rationale: common blocking setup and whole-buffer progress are useful honest compositions, while allocation, capacities, cleanup, handle aliasing, and all failure paths remain visible and the low-level module retains one-kernel-operation semantics.
    - Stage 2 nonblocking connection progress (implemented) extends `Conn` to `struct { socket: net.Socket, interest: Interest, send_offset: usize, active: bool }`. `Interest: enum { none, read, write, both }` has canonical-zero `none`; it states the current level-triggered registration requested from `poll`. The fields remain visible machine state, but `tcp` operations maintain the latter two. Buffers are never retained: callers supply them on every progress call and own their storage, capacity, logical receive occupancy, and peer identity. `Event: union { none, data: usize, sent: usize, connected, disconnected, failed: Error }` returns exactly one explicit state change per call. `data(n)` and `sent(n)` are bytes newly transferred by that call; `disconnected` means receive EOF or exactly `ECONNRESET`; `failed(error)` carries every other unrecoverable lower-layer error unchanged.
    - `connect_nonblock(address: net.Addr4): (Conn, Error)` composes socket creation, `net.nonblock(true)`, and one or more interrupted `net.connect` attempts. Immediate success returns read interest; `ConnectErr(EINPROGRESS)` is successful pending construction with write interest. Other failure closes the socket, returns canonical-zero `Conn`, and preserves the original error even if cleanup fails. `progress_connect(mut connection): Event` is called after pending-connect write readiness and is the only TCP operation using `net.pending_error`: no stored error changes interest to read and returns `connected`; either a stored `ConnectErr` or query `PendingErrorErr` changes interest to none and returns `failed(error)`.
    - `progress_receive(mut connection, destination: mut []u8): Event` and `progress_send(mut connection, source: []u8): Event` each retry `EINTR` but otherwise make one nonblocking socket attempt. `EAGAIN`/`EWOULDBLOCK` returns `none` and enables the corresponding interest. Positive receive returns `data(n)` and suspends read interest, providing explicit backpressure until the caller attempts receive again; the caller appends by passing its writable free suffix. Receive zero or either operation's `ECONNRESET` changes interest to none and returns `disconnected`; every other error changes interest to none and returns `failed(error)`. A positive send returns `sent(n)`; partial completion enables write interest, while complete-source completion resets `send_offset` to zero and removes write interest. While `send_offset` is nonzero, every call must pass the same complete source descriptor and backing bytes unchanged; this is a programmer precondition because the connection deliberately retains neither pointer nor length. `send_all` must not be mixed into an unfinished progress send: its precondition is `send_offset == 0`. An empty receive destination performs no syscall, returns `none`, and suspends read interest. An empty send source performs no syscall, resets the offset, removes write interest, and returns `sent(0)`. A successful zero-progress nonempty send returns `none` and enables write interest. `interest(connection)` returns the current value without mutation. Blocking construction and acceptance initialize read interest; an accepted connection must be made nonblocking through `net.nonblock(connection.socket, true)` before a progress operation may be used. Stage 1 blocking operations otherwise retain their rules. Closing zeros every added field even on failure.
    - The low-level prerequisite `net.pending_error(socket): Error` performs one `getsockopt(SO_ERROR)` query. No stored error returns null, a nonzero stored connection result returns `ConnectErr(code)`, and failure of the query itself returns `PendingErrorErr(errno)`. `sys.EINPROGRESS` and `sys.ECONNRESET` are public target-defined codes used by this policy. The reactor remains pure Ace over `net`/`poll`/`sys`, allocates nothing, owns no buffer, and delivers no callback. Rationale: `tcp` absorbs repeated would-block classification, interest transitions, partial-send accounting, connect completion, and disconnect recognition while the caller retains control flow, event ordering, storage, capacity, identity, and explicit cleanup.

16. Character and string utilities:
    - `char` is a predeclared transparent alias of `u8`, with shared identity, representation, conversions, and overload behavior; `sizeof(char) == 1`. It names one byte used as character data, not a decoded Unicode character. Character literals remain untyped constants holding exactly one byte and retain the existing escapes (`\n`, `\t`, `\r`, `\0`, `\\`, `\"`, `\'`, and `\xNN`); a direct multibyte source character in a character literal remains invalid. Strings remain read-only UTF-8 byte sequences with byte length, indexing, slicing, and iteration. Unicode code points are plain `u32` values; UTF-8 encoding and decoding happen only through explicit `utf8` calls, and no decoding, normalization, or grapheme processing is implicit.
    - `str` is an ordinary free-function module. Strings gain no methods, and no primitive-method or generic-method facility is introduced. Exact overload selection treats `char` and `u8` as the same type, so a `char` overload is also the byte overload.
    - `ascii` is an ordinary allocation-free module over ASCII bytes and byte strings. `ascii.is_control(byte: u8): bool` recognizes exactly bytes `0x00..0x1F` and `0x7F`. `ascii.to_lower(byte: u8): u8` maps `A..Z` to `a..z` and leaves every other byte unchanged. `ascii.is_equal(a: string, b: string, sensitive: bool): bool` compares exact bytes when `sensitive` is true and compares after that ASCII-only lowercase mapping when false; unequal lengths fail, and bytes outside ASCII letters remain unchanged. It performs no Unicode decoding, normalization, locale processing, or allocation.
    - `utf8` is an ordinary allocation-free module over UTF-8 byte strings, validating strictly per RFC 3629: overlong forms, surrogates `0xD800..0xDFFF`, values above `0x10FFFF`, truncated sequences, and unexpected continuation bytes are invalid. `utf8.decode(text: string, at: usize): (u32, usize, bool)` decodes the one sequence starting at byte `at`; success returns the code point, its byte width (1–4), and `true`. Invalid input returns `(0, 1, false)`: the width stays 1 so every call at `at < text.len` advances a decode loop, a deliberate exception to the canonical-zero failure rule. `at >= text.len` returns `(0, 0, false)`. `utf8.encode(value: u32, destination: mut []char): (usize, bool)` writes the complete encoding and returns its width and `true`; an invalid code point or insufficient capacity returns `(0, false)` without writing, and the unused destination suffix is unchanged. `utf8.width(value: u32): usize` returns the encoded width, or 0 for an invalid code point. `utf8.is_valid(text: string): bool` reports whether the complete text is well-formed; empty text is valid. No operation normalizes, folds case, or applies locale rules.
    - Search is allocation-free and byte-exact. `str.find(text: string, needle: string): (usize, bool)` and its `needle: char` overload return the first byte index and `true`, or canonical-zero `usize` and `false`; an empty string needle is found at zero. `str.has(text, needle)` has `string` and `char` overloads and tests occurrence anywhere; an empty string needle is present. The corresponding three-argument overloads `str.has(text, part, suffix: bool)` test one boundary: `false` selects the prefix and `true` the suffix. An empty string part matches either boundary.
    - `str.compare(a: string, b: string): int` compares unsigned bytes lexicographically and returns exactly `-1`, `0`, or `1`; after an equal prefix, the shorter string sorts first. It allocates nothing and performs no Unicode decoding or normalization.
    - `str.copy(text: string, noalias destination: mut []char): bool` copies the complete text into `destination[0..text.len]`. A shorter destination returns `false` and is unchanged; otherwise the copy succeeds, returns `true`, and leaves the destination suffix unchanged. Empty text succeeds without writing. The source and written destination range must not overlap; checked builds diagnose a false promise and release assumes it. This is a capacity-oriented convenience, unlike `mem.copy`, whose two complete ranges must have equal lengths.
    - `str.concat(parts: []string, destination: mut []char): (usize, bool)` concatenates every part in order into caller storage. Success returns the complete byte count and true while leaving the unused destination suffix unchanged. If the complete result exceeds destination capacity, it returns `(0, false)` without writing. Empty parts and an empty parts slice are valid. Source bytes must not overlap the written destination range. The operation allocates nothing and performs no decoding or normalization.
    - `str.split(text: string, separator: string, parts: mut []string): (usize, bool)` and its `separator: char` overload split left-to-right on non-overlapping exact byte matches. Separators are omitted; leading, trailing, and adjacent separators produce empty parts, and empty input produces one empty part. Each written part is a zero-copy view into `text`; unwritten slots are unchanged. The result is the number written and whether the complete result fit. An empty string separator returns `(0, false)` without writing. The operation allocates nothing and performs no decoding or normalization.
    - Exact boundary removal is overloaded as `str.trim(text: string, part: string, suffix: bool): (string, bool)` and with `part: char`; `false` removes at most one matching prefix and `true` at most one matching suffix. A match returns the remaining zero-copy view and `true`; absence returns the original view and `false`; an empty string part succeeds without changing the view. `str.trim(text: string): string` instead removes all leading and trailing ASCII whitespace bytes — exactly `0x09..0x0D` and `0x20` — and returns a zero-copy view. It performs no Unicode whitespace processing.
    - Parsing is the explicitly instantiated generic overload set `str.parse[T](text: string): (T, bool)` and `str.parse[T](text: string, radix: u8): (T, bool)`. The default form admits exactly `int`, `uint`, `usize`, `f32`, and `f64`; the radix form admits exactly the three integer types and radices 2 through 36. Integer digits above nine are ASCII letters, case-insensitive. An explicit radix accepts digits only, with no `0x` or `0b` prefix. Parsing consumes the complete nonempty string, accepts no surrounding whitespace or leading `+`, permits leading `-` only for `int` and signed floats, and accepts leading zeroes. Decimal floats accept ordinary decimal/scientific forms plus the exact non-finite spellings produced by `fmt` (`nan`, `inf`, `-inf`). Finite decimals round directly to the requested IEEE width using nearest with exact ties to even; conversion does not pass through another float width. Parsing assumes the default round-to-nearest floating-point environment. Invalid text, an unsupported runtime radix, or overflow returns canonical-zero `T` and `false`; finite float underflow rounded to signed zero succeeds. Unsupported `T` is a compile-time error. Parsing allocates nothing, performs no locale-dependent interpretation, and uses no platform decimal parser.
    - Value-to-text conversion is the ordinary overload set `str.format(destination: mut []char, value): (usize, bool)` for exact value types `int`, `uint`, `usize`, `f32`, and `f64`, plus radix-taking overloads for the three integer types. It writes the complete representation or, when capacity is insufficient or a runtime radix is outside 2 through 36, writes nothing and returns `(0, false)`. Success returns the byte count and `true`, leaving the unused destination suffix unchanged. Integer radix digits are lowercase ASCII and carry no base prefix; only `int` may emit `-`. The no-radix float representations are exactly those of the corresponding `fmt.print` overloads. The operation allocates nothing.
    - Rationale: `char` improves byte-oriented intent without adding a second representation or conversion domain; code points remain `u32` because a named alias would add no behavior, and a separate `utf8` module keeps `str` encoding-agnostic. Free `str` functions keep library ownership visible and reuse Ace's existing overload and explicit-generic machinery without extending methods to primitives. Every operation works on views or caller storage, exposes capacity and failure explicitly, and adds no hidden allocation, decoding, normalization, locale, or runtime type dispatch. Compact overload sets avoid a family of prefix/suffix/type-specific names while their argument and explicit type shapes remain statically unambiguous.

17. HTTP (protocol core implemented; transport ergonomics carved) — `http` is
    the allocation-free HTTP/1.1 protocol layer. Its parser and formatter core
    performs no network I/O; its higher-level operations compose the concrete
    `net`, `tcp`, and `poll` modules directly, with no transport contract or
    module split. HTTP
    transports bytes: it parses protocol delimiters, method and header-name
    grammar, decimal fields, and case-insensitive names as ASCII, while header
    values and bodies remain byte views. It performs no Unicode decoding,
    validation, normalization, or case folding. A
    later owned-string convenience may compose with `http`, but the base API
    keeps every capacity and allocation decision with the caller.
    - Endpoint text and URLs are different values. The general networking
      convenience is `net.parse4(endpoint: string): (Addr4, bool)`, which
      accepts exactly a dotted-decimal IPv4 address and required decimal port
      separated by one colon, for example `127.0.0.1:8080`. It consumes the
      complete nonempty input, accepts four decimal octets in `0...255` and a
      port in `0...65535`, performs no name resolution, and returns canonical
      zero plus false for invalid text. Hostname resolution remains
      `net.resolve4`; endpoint formatting and IPv6 literals are additive.
    - `Url: struct { host: string, path: string, query: string, port: u16 }`
      contains zero-copy views into the input URL and a host-order port.
      Separating path and query makes a query-only URL representable without
      allocation: its path is the static `/` view and its query retains the
      leading `?`. `parse_url(url:
      string): (Url, Error)` accepts an absolute `http` or `https` URL with a
      nonempty authority: `http://host` or `https://host`, an optional
      explicit decimal port, and an optional path/query request target. An
      omitted port becomes 80 for `http` and 443 for `https`; an omitted path
      becomes `/`; a query without a path therefore composes as `/?`. The host
      may be an ASCII DNS name or dotted IPv4 literal. DNS names are limited
      to 253 bytes with labels of at most 63 bytes. User information,
      fragments, bracketed IPv6 literals, embedded control/NUL bytes, empty or
      out-of-range ports, and every other scheme fail with `UrlErr`. Both
      schemes share this one parser even though the plaintext-only
      `get(url, mut context)` rejects an `https://` URL before connecting —
      it never silently falls back to plaintext — through its own
      scheme-forced `parse_url_for(url, false)` call. Parsing
      allocates nothing, performs no DNS lookup, preserves path/query bytes
      and percent escapes exactly, and never percent-decodes or
      Unicode-normalizes them.
    - The protocol core uses `Header: struct { name: string, value: string }`
      and caller-provided header/body/output storage. Methods are strings rather
      than a closed enum so extension methods need no API change. Serialization
      rejects invalid method and header-name bytes plus CR, LF, and NUL in
      caller-supplied header values. It writes a complete HTTP/1.1 message or
      reports insufficient capacity without emitting a partial message.
      Parsing is incremental state supplied and retained by the caller, accepts
      arbitrary receive fragmentation, returns zero-copy header/value views
      into caller-owned stable input storage, and reports one explicit progress,
      completion, capacity, syntax, or framing result per call.
      The public message surface is `Request { method, target, headers, body }`
      and `Response { version_minor = 1, status = 200, reason = null, headers =
      null, body = null }`. Request keeps its essential method and target
      required; response construction may name only the fields it overrides.
      Response fields remain directly accessible; getter/setter pairs add no
      protection while fields are visible and remain additive only after a
      concrete need. A null response reason selects the module's static
      conventional phrase for a known status, or the empty phrase for an
      unknown valid status. A non-null reason, including `""`, is emitted
      exactly after validation. Parsing preserves the received reason.
      Their
      read-only `format(output: mut []u8): (usize, Error)` methods write one
      complete message or leave insufficient output unchanged. Output must not
      overlap storage reachable through the message. Formatting preserves the
      supplied headers exactly and synthesizes neither `Host` nor framing
      fields; `FormatErr` distinguishes output capacity and invalid start-line
      or header fields. The incremental parser surface is
      `mut RequestParser.parse(input, headers, body, eof): RequestResult` and
      `mut ResponseParser.parse(input, headers, body, eof, no_body):
      ResponseResult`, with public phase, framing, capacity, and cumulative
      progress support types for each parser. Each call resumes from saved
      phase, line-scan, framing, header, and body progress; already-consumed
      bytes are not reparsed or recopied. `input` is the same stable complete
      occupied prefix extended with newly received bytes on every call. The
      parser retains no buffer pointer, while the caller preserves previously
      written header and body storage until completion. Reinitialize the parser
      value before starting another message.
    - Initial request framing supports `Content-Length`, `Transfer-Encoding:
      chunked`, and an empty body when neither is present. Initial response
      framing additionally supports explicit no-body responses and
      connection-close bodies. Chunk syntax is decoded into caller-owned body
      storage; extensions are ignored after syntactic validation and trailers
      are parsed for framing but not exposed initially. Conflicting or invalid
      framing is an error. Informational `1xx` responses are consumed before the
      one final response. Header names compare case-insensitively as ASCII;
      field values are otherwise preserved.
    - The nine standard request methods are public `string` constants spelled
      `method_get`, `method_post`, and so on. Statuses remain plain `u16`
      numbers: response handling accepts every status in `100...999`, without
      duplicating the protocol catalogue as public numeric constants.
      `status(code: u16): string` returns the static conventional
      reason phrase for a known status code and `""` for an unknown code. The
      lookup is allocation-free and does not classify a response as an error;
      `Response.reason` remains the phrase actually supplied on the wire.
    - Blocking TCP socket convenience is the exact overload set
      `request(socket: net.Socket, request: Request, scratch: mut []u8): Error`,
      `request(socket: net.Socket, input: mut []u8, headers: mut []Header, body:
      mut []u8): (Request, Error)`, `response(socket: net.Socket, response:
      Response, scratch: mut []u8): Error`, and `response(socket: net.Socket,
      input: mut []u8, headers: mut []Header, body: mut []u8, no_body: bool):
      (Response, Error)`. The write forms format completely before performing
      all-byte sends; the read forms extend one stable occupied input prefix
      until the corresponding parser completes. They require blocking TCP
      sockets, retry interrupted I/O, preserve lower-layer error domains, and
      retain neither socket nor buffer. `no_body` makes HEAD-style response
      semantics explicit. Connection creation, DNS, retry, TLS, reuse, and
      keep-alive policy remain with the client caller.
    - One-shot client ergonomics use one owning entry point.
      `Storage: struct { addresses: mut []net.Addr4, output: mut []u8, target:
      mut []u8, input: mut []u8, headers: mut []Header, body: mut []u8, url:
      mut []u8 = null }`
      remains the internal neutral workspace for one HTTP exchange.
      `ClientContext: struct { storage: mut []Storage, redirects: RedirectPolicy }`
      owns and grows the plaintext workspace through `mem` and carries HTTP
      policy. `client_context_new` constructs it and `client_context_wipe`
      releases it. The exact public overload is
      `get(url: string, mut context: ClientContext): (Response, Error)`, which
      accepts only plaintext HTTP. Allocation failure is explicit.
      Returned views remain valid only until the corresponding context
      is next mutated or wiped. This operation
      parses the absolute plaintext URL, resolves its host into the supplied address
      storage, tries resolved addresses in order with a fresh blocking TCP
      connection, performs exactly one request and response, and closes the
      connection before returning. `get` emits GET with no body. `post` emits
      POST with the supplied bytes and requires a valid nonempty content type.
      `get` synthesizes exactly `Host` and `Connection: close`; `post` also
      synthesizes `Content-Type` and `Content-Length`. Without an explicit
      redirect policy there is no
      redirect, retry-after-response, connection reuse, authentication, cookie,
      compression, proxy, or user-header policy. Request formatting,
      response parsing, DNS, TCP, close, and capacity failures preserve the
      existing error domains, with an unsuccessful address sequence returning
      the final connection error.
    - Parser failures mapped through those conveniences use the shared public
      `SyntaxErr`, `FramingErr`, and `CapacityErr` domains. Syntax and framing
      variants map to their matching domain, and premature EOF is framing
      failure. `CapacityErr` codes are the public `i32` constants
      `capacity_input`, `capacity_headers`, and `capacity_body`. Formatter
      failures remain `FormatErr`; lower-layer socket/TCP errors retain
      their original domains and codes.
    - Server convenience is exactly one blocking overload:
      `serve(endpoint: string, mut context: Context, router: Router): Error`
      parses `endpoint` through `net.parse4`, binds a blocking listener, then
      loops forever, serially: accept one connection, read exactly one
      request into `context`'s buffers, dispatch it through `router`, format
      and write its response, close, repeat. `Router.handle(method, path,
      handler)` registers a handler for an exact method and a path-segment
      prefix (`"/home"` matches `/home` and `/home/about` but not
      `/homework`); `Router.dispatch` picks the longest matching registered
      prefix, ignoring any `?query` suffix, and returns a bare 404 when
      nothing matches. `Context` owns the `input`, `headers`, `body`, and
      `output` buffers used across every connection; each starts empty and
      grows through `mem.adjust` as a request or response demands, with no
      size cap. `Context.backlog` (default 512) is the one plain, non-growing
      field, passed once to `tcp.listen` when `serve` binds `endpoint`; it is
      not adjusted at runtime. There is no poll queue or session table.
      `serve` returns only if the listener itself cannot be created; any
      per-connection failure
      (parse error, allocation failure, mid-write disconnect) just drops
      that connection and the loop continues. Response serialization adds
      exactly one `Connection: close` field and drops a handler-supplied
      `Connection` field along with the connection, while ordinary
      `Request.format` and `Response.format` continue to synthesize no
      headers. There is no concurrency — one connection is handled at a
      time — so this overload suits demos and local testing rather than
      serving concurrent traffic. A caller-driven nonblocking server core
      existed at one point but was implemented without a carve; it was
      removed pending a fresh carve of how much server mechanism belongs in
      the language versus a framework built on top (see `TODO.md`).
      Concurrency, persistent connections, and TLS server mode remain
      additive or deferred to that future carve.
    - `http` is plaintext-only; HTTPS/TLS support was removed (see the TLS
      client transport item in `TODO.md`) and is not part of the current
      surface. `Url` parsing still accepts an `https://` scheme for syntax
      purposes, but every client entry point rejects it with `UrlErr.invalid`
      since no secure transport exists to serve it.
    - Redirect following is opt-in context policy.
      `RedirectPolicy: struct { limit: usize = 0 }` is stored as
      `context.redirects`; zero performs exactly one exchange, while a nonzero
      value opts into at most that many followed responses.
      Statuses 301, 302, 303, 307, and 308 are followed only with a nonempty,
      valid `Location`; otherwise the ordinary 3xx response is returned.
      Absolute HTTP/HTTPS, scheme-relative, absolute-path, query-only, and
      path-relative references resolve against the preceding URL, but an
      HTTPS redirect target fails with `UrlErr.invalid` under the current
      plaintext-only client.
      Encountering another followable response after `limit` hops fails with
      `RedirectErr.limit`, including at a zero limit. The client reuses explicit
      owned URL workspace so each location is stable before response input is
      reused. Cookies, custom headers, authentication, connection reuse, and
      session state remain deferred. Rationale: explicit overloads preserve
      existing one-request behavior, make network work opt-in, and bound it in
      accordance with `PHILOSOPHY.md` Principles #2, #3, and #7.
    - Known initial limitations and next improvements: content
      decoding, cookies, authentication policy, caching, proxies, upgrades,
      multipart helpers, and exposed trailers are not part of the protocol core.
      Client concerns such as connection reuse, pooling, deadlines, and
      cancellation remain outside the initial conveniences. Each proof updates
      the user guide and compiler-checked or executable fixtures only for
      behavior that has actually landed. Rationale: keeping the framing core
      independent while composing concrete TCP operations at the ergonomic
      boundary preserves machine-visible storage, explicit errors, and the
      lack of hidden allocation without adding dependency inversion for a
      TCP-only protocol. Byte-oriented parsing matches HTTP and permits UTF-8
      payloads to pass through unchanged without making Unicode text processing
      part of networking.

## Conditions
1. Chained comparisons: `lo <= x <= hi` — each operand evaluated once; no boolean-coercion trap (C's `a < b < c` meaning `(a<b)<c` is dead); lowers to two compares and a branch.
2. Multi-way branch: a `switch`-style construct — one value and many comparisons. Cases do not fall through implicitly. A case may end with the contextual statement `fallthrough` to transfer directly to the immediately following case. It takes no target or condition, must be the case's final statement, and is rejected in the final case. `default` is optional, at most one, and when present is the last case; a case after `default` is a compile error. It runs when no other case matches, and a switch with no `default` runs nothing when nothing matches. Because `default` is the final case, `fallthrough` is rejected in it, and the case immediately before it may `fallthrough` into it. Rationale: a fixed position makes the catch-all the last thing a reader meets, so no case after it can look reachable, and a distinct word keeps `else` meaning only the alternative branch of `if`. The rule costs one check in sema and does not change emitted code. [case spelling provisional]
3. Null checks are ordinary comparisons. They do not change a declared type: plain pointers and handles already admit zero, and the checked/release runtime policy governs invalid zero operations.
4. Cleanup jumps: blocks can be labeled, `break label` exits any enclosing block — structured single-exit cleanup on error paths. **goto does not exist.**

## Iterations
1. Memory-size-based iteration is realized as slices: `for b: u8 in buf` walks exactly `buf.len` bytes — pointer + length, nothing else.
2. `for` is the only loop keyword; sequence loops lower to a plain index loop — no iterator contract, no hidden machinery, zero cost (general head forms: #13).
3. Element types are stated explicitly: `for i: int in 0 .. n` (ranges), `for b: u8 in buf` (slices/arrays).
4. A range `a .. b` denotes the half-open numeric interval `[min(a, b), max(a, b))`; endpoint order selects traversal direction. `0 .. 10` visits `0` through `9`, `10 .. 0` visits `9` through `0`, and equal endpoints produce an empty range. The first endpoint is evaluated once, then the second endpoint once, before iteration begins; runtime endpoint ordering is selected once at entry, never re-evaluated per iteration. Swapping the endpoints therefore reverses the same selected values, and no separate reverse form exists. [range spelling provisional]
5. A range step is a positive magnitude with the same integer type as the bounds; an untyped step literal materializes as that type. The default is one. An explicit step may be any value or expression and is evaluated exactly once, after both bounds and even when the interval is empty. A statically known zero or negative step is a compile-time error; a runtime-invalid step is diagnosed once at loop entry in checked builds, while release assumes the positive-step precondition with no validation cost. Stepping is anchored at the numerically smaller endpoint, so swapping endpoints exactly reverses the stepped sequence: `0 .. 9 step 3` visits `0, 3, 6`, while `9 .. 0 step 3` visits `6, 3, 0`. For descending traversal, any alignment/remainder work needed to find that final selected value occurs once before the loop, not per iteration. The spelling is `step`, recognized contextually only after a range's second endpoint; it remains an ordinary identifier everywhere else (`step: int = 2`; `0 .. 9 step step`).
6. Arrays `[N]T` are inline values; slices `[]T` are copyable pointer + element-count views; slicing expressions like `arr[1..n]` produce slices over the same storage. The direct read-only property `sequence.len` is the constant element count on arrays, stored element count on slices, and stored byte count on strings; allocation and ownership do not affect its availability.
7. Sequence-loop binders are arity-directed. `for value: T in values` exposes only a per-iteration element copy. `for index: usize, value: T in values` exposes the original zero-based index first and the element copy second; the index type is exactly `usize` and the value type exactly matches the element type. Assigning either binder changes only that local copy and never the loop cursor or backing sequence. There is no discard binder: omit the index with the one-binder form, or iterate `0 .. values.len` when only indices are wanted. Ranges accept exactly one binder. (The `in mut` head of #16 replaces the value copy with an alias.)
8. Plain `break` and `continue` target the nearest loop as usual. `break label` exits an enclosing labeled block; `continue label` restarts an enclosing labeled block from its beginning. Reaching the block's closing brace still exits normally, so repetition is explicit on the continuing paths. Re-entry reuses storage but begins the block's lexical/definite-initialization state anew; mutations to storage outside the block persist. Both labeled transfers are restricted to enclosing labels, preserving structured control flow with no arbitrary `goto`. A labeled block ending in `continue label` is the explicit indefinite-loop form.
9. A `for-in` loop fixes its pointer and length at loop entry — mutations to the slice variable inside the body do not affect the running loop (element mutation through the loop is fine). Consequence: the compiler provably controls every index, so checked builds need no per-iteration bounds checks inside `for-in`.
10. Implementation guidance: emit `for-in` as a pointer-bump loop (pointer + precomputed end, post-increment addressing) — the closed loop form is the optimization contract; straight loops should reach C-parity in v1.
11. A range bound has no hidden relationship with storage mentioned by its expression. In `for i: usize in values.len .. 0`, the captured first endpoint is unchanged if the slice variable `values` is later resliced or reassigned, while `values[i]` uses the slice descriptor current at that access. Checked builds apply the ordinary index check unless the descriptor's unchanged length is proven; release adds no check. Direct sequence iteration separately retains its entry-captured pointer and length under #9.
12. Arrays, slices, and strings have no value-to-value `==` or `!=`. Byte-backed sequences (`[N]u8`, `[]u8`, and `string`) compare explicitly through `mem.equal` across every pairwise combination and any lengths; it compares length then exact stored bytes without allocation, conversion, decoding, or normalization. Unicode-aware facilities remain layered over UTF-8 strings.
13. One keyword carries every loop: five head forms share `for` — the typed range head (`for i: int in 0 .. n`), the typed sequence head (`for b: u8 in buf`, `for index: usize, b: u8 in buf`), a condition-only head (`for running`), an empty infinite head (`for { }`, also the do-while form through body `break`), and a three-clause head (`for i: int = 0; i < n; i += 1`). Loop binders are always explicitly typed (`ident ":" type`); there is no contextual or untyped binder form.
14. Three-clause heads: the head's clauses sit on one physical line, and `;` is valid only between for-head clauses — the no-terminator rule is otherwise unchanged. `init` scopes over cond, post, and body and is invisible outside the loop; the condition must be `bool`, and an omitted condition is always true; the post clause accepts assignments and expression statements; a head declaration without an initializer falls to definite initialization as usual. `continue` runs the post clause, then the condition.
15. Labels attach to `for` of any form (`outer: for`), sharing the labeled-block namespace of #8: plain `break`/`continue` bind to the nearest `for`; `break label` exits that loop; `continue label` on a loop means the next iteration — distinct from the block-restart meaning. The innermost enclosing match wins and non-enclosing targets are rejected, and each loop emission owns its own exit targets.
16. `in mut iterable` opts a sequence loop into aliasing — the type-level content-permission rule applied to iteration (Mutability #9–11). The iterable must name storage writable under those rules: a mutable-lvalue array, or an lvalue of a `mut []T` or `mut string` view. String literals, `const` globals, unmarked parameters, plain-typed views, array literals, call results, slicing results, ranges, and every other non-lvalue or read-only expression are rejected. Strings participate with no special-casing: `for b: u8 in mut text` requires `text` to be a `mut string`; a plain string is rejected by the same rule. The value binder then aliases the element: reads and writes hit the backing storage directly (no per-iteration copy, no write-back), `&item` yields a pointer into the backing storage, and `mut &item` yields `mut *T` whose writes land there (Mutability #6). The declared binder type must still equal the element type exactly (#7), the index binder stays a plain `usize` copy, and the loop cursor is captured at entry (#9) — binder writes go through the alias and never through the cursor, so reslicing the variable mid-loop changes neither the running loop's captured descriptor nor where the alias points. Plain (non-`mut`) iteration keeps #7's per-iteration copy; the marked form is a strict superset.

## Deferred calls
Decision: explicit cleanup ordered by the language at every function exit — one function-level list, expanded inline; no runtime machinery and no destructor concept.

1. `defer` is a reserved keyword whose only form is the statement `defer` followed by a call: `defer close(fd)`. The operand must be a call; any other expression is a compile error. The deferred call is type-checked exactly like an ordinary call statement, so discarding a multi-return result is allowed, and its own results are always discarded — a deferred call can never change the function's return values.
2. Placement is function-level: a `defer` must be a direct statement of the function's top-level body block. A defer inside any nested block — `if`, `for`, a switch case, a labeled block — is a compile error. Each function therefore owns one defer list in source order; there is no block-scoped defer.
3. Exits are every `return` and the body's fall-off end. At each exit the deferred calls run in reverse declaration order (LIFO): last declared, first run. A `defer` written after the final `return` is legal dead code that never fires; no unreachable diagnostic is issued.
4. No other transfer runs defers. `panic` and `sys.exit` never return and terminate the process without them (Errors #15, Stdlib #10), and `break`/`continue` — plain or labeled — leave the function running, so the deferred calls wait for the real exit. A deferred `panic` or `sys.exit` is legal; when reached during LIFO expansion it terminates immediately, so earlier calls still pending in that defer list do not run.
5. Argument capture is two-tier, mirroring the call-site `mut` marking discipline (Mutability #6). Read-only arguments — no call-site `mut` — are evaluated once at the `defer` statement and snapshotted into frame storage; the exit-time call reads those snapshots, so later changes to the named variables are invisible to it. Arguments marked `mut` at the call site, and the receiver of the receiver-marked form `defer mut c.m(...)`, capture the lvalue's address at the defer statement instead; the exit-time call passes that address, so the callee reads the storage's final state, and its writes land while the function's storage is still live.
6. Codegen is inline expansion: each exit point re-emits its captured calls in reverse immediately before the epilogue — no runtime counter, registration, or epilogue loop exists. Exit-time call sites carry DWARF line marks for the defer statement's source line, and a register-returned result is parked in a frame slot across the deferred calls (an sret result already sits in the caller's buffer). A deferred call to `panic` or `sys.exit` is legal: it terminates at the exit.
7. Out of scope: no detection of duplicate or repeated defers of the same target, and no deferred block or non-call form — the operand is exactly one call.

Rationale: cleanup ordering is what C reaches for `goto` to obtain. Anchoring the list at the function keeps the exit set total — every `return` plus fall-off — with no block-scoped bookkeeping, which is what makes inline expansion possible at zero runtime cost in a single-pass compiler. The two-tier capture keeps the ordinary `mut` call contract truthful at a distance: read-only arguments behave like call-by-value snapshots, while `mut`-marked arguments keep their by-reference meaning up to the function's final moment.

## Sequences
Decision: one view model across arrays, slices, and strings; `string`, `[]char`, and `[]u8` are intent spellings of one byte-view type; content permission lives in the view's type and is established only where `mut` is written. Sequential values have no `==`/`!=`, byte content comparison is the explicit `mem.equal` call, and a literal-`null` operand selects a distinct nullness form.

1. Slicing is uniform: `x[lo..hi]` accepts arrays, slices, and strings, yielding the source's own form — `[]T` for arrays and slices, `string` for strings — so slicing never widens permission. Content permission propagates in the type: a `mut []T` slices to `mut []T`, a `mut string` to `mut string`, plain to plain (Mutability #11). The establishment form `mut array[lo..hi]` creates a writable view only from owned writable array-element storage; applying it to a plain slice or plain string is always rejected because descriptor rebindability cannot restore content permission. Bounds are a half-open `[lo, hi)` interval in the source's index space (elements for arrays and slices, bytes for strings); an omitted first bound is 0 and an omitted second bound is the source's length. The result length is `hi - lo` and the result pointer is the source pointer advanced by `lo * sizeof(T)`; slicing a slice re-bases within that slice's own window — no capacity exists to escape into. Slicing allocates nothing.
2. The bounds precondition is `0 <= lo <= hi <= source.len`: a compile-time error when constants prove a violation, a checked-build diagnostic otherwise, and release assumes it — the same contract shape as `mem.copy`.
3. Empty views are ordinary values: `lo == hi` yields a view of length zero retaining its computed pointer, and the language never canonicalizes an empty view to `null`. Reading `.len` and performing zero-length iteration or byte operations are valid on it; element indexing still requires an in-range element.
4. `string`, `[]char`, and `[]u8` are the same plain type; their writable spellings are likewise one type. Assignment, parameters, returns, fields, generic arguments, and overload selection therefore require no conversion among these spellings. In particular, overloads differing only by one of these spellings are duplicate declarations. No implicit-conversion exception is involved: aliases share identity.
5. The spelling convention is normative style for Ace code: use `string` when the value is complete text, `[]char` for character-oriented parsing, scanning, or partial textual framing, and `[]u8` for raw bytes, memory, serialized data, checksums, and transport payloads. Use the corresponding writable spelling for the same intent. APIs and locals should retain the most accurate contextual spelling even though the compiler treats all three identically.
6. A fixed `[N]char`/`[N]u8` array remains owned inline storage and is not identical to a slice. It reaches the shared view type only through explicit slicing: `arr[lo..hi]` for a plain view or `mut arr[lo..hi]` to establish a writable view from writable array storage. A slice owns no storage; its backing may be an array, an allocation, static literal storage, another view, or explicit external storage.
7. No implicit conversion exists between arrays and views or between unrelated types. The alias identity among `string`, `[]char`, and `[]u8` is not a conversion. Permission shrink from a writable view to its plain form remains implicit; widening remains forbidden. A string literal is read-only and therefore cannot initialize `mut string`, `mut []char`, or `mut []u8`.
8. Sequential types — arrays, slices, and strings — are never compared with `==` or `!=`. This holds regardless of element type, including `[N]u8`, `[]u8`, and `string`. Comparing two sequences for content equality is an explicit call: `mem.equal(a, b)` for byte-backed sequences (`[N]u8`, `[]u8`, and `string`, in any combination and at any lengths) compares length then bytes, with no allocation or decoding. Comparing lengths alone is `a.len == b.len`. Non-byte arrays and slices have no built-in content-equality helper; use an explicit loop. Negation is written against the corresponding defined `mem.equal` or `.len` check; there is no sequence `!=` operator to specify because neither `==` nor `!=` is defined between sequential values. `x == null` is a distinct comparison form, not sequence content equality. When either operand is the literal `null`, equality is a nullness test for any type that admits `null` — exactly pointer, slice, string, function, contract, `Error`, and `typeid` as enumerated in Zero, Null & Absence — using that type's canonical all-bits-zero representation. This form does not grant value-to-value equality to the tested type: `slice == null` is valid, while `slice_a == slice_b` is rejected for every slice type.
9. The shared byte view iterates as `u8`/`char`: `for c: char in text` walks stored bytes — the encoding, never decoded code points.

Rationale: every view operation is explicit and allocation-free; permission is established where `mut` is written, carried in types thereafter, and never escalated — writable bytes exist only from proven-writable provenance, so the literal-storage fault class is rejected at declarations rather than handled at runtime. Removing sequence equality operators avoids both hidden arbitrary-element work and descriptor identity masquerading as value equality. Byte content remains available through the explicit `mem.equal` operation, while the literal-`null` form states nullness directly without granting sequence-to-sequence comparison.

10. Inline sequence types are valid operands of `sizeof` and `alignof`: `sizeof([]u8)`, `sizeof([]char)`, and `sizeof([3]char)` are compile-time expressions with values 16, 16, and 3 on the v1 target, while `alignof([]u8)` and `alignof([3]char)` are 8 and 1. They use the ordinary type grammar and do not require a named alias. Together with `sizeof(string) == 16` and `alignof(string) == 8`, this makes normative layouts directly expressible without runtime work. `mut`-qualified forms measure identically because `mut` never changes representation (Mutability #9).
11. Indexed byte writes require the shared writable view type, spelled `mut string`, `mut []char`, or `mut []u8` according to intent. Each assigns one byte and follows the slice-write bounds contract. Plain spellings remain read-only. The two-word descriptor never grows, and a literal cannot establish any writable spelling because its storage is read-only.
12. Every array, slice, and string element index, for reads and writes alike,
    requires `0 <= index < sequence.len`. A negative index never means an
    offset from the end. A statically known negative or oversized index is a
    compile-time error in both modes; a runtime-invalid index is diagnosed in
    checked mode, while release assumes the precondition. A checked comparison
    of a signed index tests negativity before any unsigned widening, preserving
    the lower-bound violation rather than reclassifying it as an oversized
    index. Unsigned arithmetic underflow is independently an integer-overflow
    violation in checked mode; however an oversized unsigned index produced by
    any path is still rejected by the index upper-bound check.

Standard library #10 addendum (implemented): `sys.pipe(): (Fd, Fd, Error)`
creates one unidirectional byte pipe and returns its read end, write end, then
null error. Failure returns `Fd(-1)` for both ends with `PipeErr`. It allocates
no Ace memory, performs no I/O, and closes neither end; callers close both
explicitly. Rationale: descriptor polling needs a general non-socket readiness
proof without exposing a test-local platform binding.

The earlier errno-name deferral is narrowed by the proven nonblocking pair
`sys.EAGAIN` and `sys.EWOULDBLOCK` and the blocking-retry name `sys.EINTR`.
All are public target-defined `i32` constants; `EAGAIN` and `EWOULDBLOCK` are
equal on Darwin. Callers compare a captured operation-error code with the named
condition instead of embedding a platform number. Further errno names remain
additive until concrete public usage requires them.

## Decision Queue

Only active or trigger-gated decisions live here. Implemented history is recorded
once in `CHANGELOG.md`; uncommitted candidates are surveyed in `TODO.md`.

### Active

### Trigger-gated

- The canonical source formatter (`ace fmt`: in-place, check-only, stdout
  modes; compiler/toolchain functionality, not `std/fmt`) is deferred until
  timely. Decided: `.acefmt` stays normative, searched upward with built-in
  defaults; one shared lexer/parser pass produces either the compact compiler
  AST or, in format mode only, a separate lossless syntax tree (tokens,
  trivia, preserved comments/literal spellings, every `@when` retained) —
  never both. Out of scope: semantic analysis, resolution, reordering, LSP,
  `std/fmt`. Formatting must be deterministic, idempotent, and enforce the
  120-column limit without partially rewriting invalid input; a
  comma-separated list stays on one line if it fits, otherwise one item per
  continuation line with a trailing comma. Resume with staged implementation
  carving when formatter work becomes an active priority.
- Package-system manifest, lockfile, display, and command syntax remain open
  under Package System #3, #9, #13, and #15 (native link-input declarations,
  formerly #14, are carved and implemented; see `CHANGELOG.md`). Resume only
  when package implementation is selected.
- A second target remains trigger-gated until its backend and OS implementations
  are genuinely supported; directory-module facades already select target files
  consistently through `@when`.
- IPv6, UDP, address queries, half-close,
  extra TCP options, and other explicitly additive networking work are carved
  only after the nonblocking proof identifies the required order.
- Threads and atomics follow the networking proof; their semantics are not yet
  carved.
