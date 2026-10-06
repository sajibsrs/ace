## Memory
Decision: manual management through one fixed process-wide allocator strategy (C school, with explicit failure).
1. Explicit allocation/deallocation; no GC, no hidden runtime memory management.
2. The allocator is not a value, contract, parameter, or selectable policy. The public raw API is the standard-library module `mem`: `mem.alloc`, `mem.realloc`, `mem.release`, and `mem.usable`. There is no `Allocator` abstraction, allocator argument, implicit allocation, or unqualified predeclared allocation function.
3. The release-mode implementation stays at C cost: it uses the platform allocation boundary and carries no Ace header, guards, registry, quarantine, or other per-allocation diagnostic metadata. Debug builds add a live-allocation registry for invalid/double release and leak diagnostics; detecting arbitrary reads or writes through a dangling alias requires separate access instrumentation and is not promised by the allocator alone.
4. Machine-true: sizes and layout stay visible to the programmer (see: Iterations, Errors).
5. A value lives on the stack unless explicitly allocated; allocation is the only path to heap memory, so stack vs heap is always statically known.
6. Pointer type is `*T` (`p: *Point`); unary `*p` explicitly dereferences a pointer, while infix `*` remains multiplication and `*T` remains the pointer-type spelling. Field access and method calls auto-deref at every pointer level (`p.x` ≡ `(*p).x`, `p.m()` ≡ `(*p).m()`); a mutating method keeps its `mut` receiver mark and needs a writable pointee. `->` never exists. A pointer's all-bits-zero value is `null`; there is no separate optional-pointer type.
7. Unary `!` is logical negation and accepts only `bool`; unary `~` is bitwise complement and accepts only integers. Binary `+ - * /` take two operands of one numeric type; `%`, `& | ^`, and `<< >>` take two operands of one integer type. A compound assignment `x op= y` follows the operand rule of `x = x op y`, apart from typed pointer `+=`/`-=` (#9).
8. Address-of exists only for lvalues (variables, `arr[i]`, `s.field`, and dereferences). `&expr` yields a read-only `*T`; `mut &expr` yields a writable `mut *T` and is rejected for const or otherwise read-only storage. Both emit the same machine address: `mut` is a compile-time permission with zero runtime representation or cost. Its C jobs are otherwise gone: out-params died to by-reference parameters and multi-return; a `mut` parameter supplies the outer reference needed to replace caller storage. What remains: aliasing stack values, interior/field pointers, bridging storage into pointer signatures, and explicit writable-pointer construction.
9. Typed pointer arithmetic is deliberately narrow. For `p: *T` or
   `p: mut *T` and `n: usize`, `p + n` and `p - n` offset the stored address by
   `n * sizeof(T)` bytes and preserve the exact pointer type, including its
   read/write permission. Untyped nonnegative integer literals materialize as
   `usize` in the offset position. The corresponding `+=` and `-=` forms are
   valid only when the pointer binding itself is assignable; they grant no
   additional pointee permission and remain subject to the ordinary outer
   `mut` parameter rules. `T` must have a complete, nonzero-sized layout.
   Debug builds diagnose a null starting pointer, overflow of the scaled byte
   offset, and overflow or underflow of the machine address; release emits the
   scaled machine add or subtract with no guard and assumes those preconditions.
   A pointer carries no extent, allocation identity, lifetime, or provenance,
   so neither mode promises bounds or same-object validation. Pointer-pointer
   difference, pointer ordering, `n + p`, and pointer indexing (`p[i]`) do not
   exist. Use slices for bounded traversal; an extent-free access remains
   explicit as `*(p + n)` or `*(p - n)`.
   Pointer-integer conversion uses `usize` as the sole address integer; no
   separate `uintptr_t`-class type exists. `usize(p)` converts any data pointer,
   plain or writable and including `null`, to its exact target machine address.
   `*T(address)` and `mut *T(address)` convert exactly one `usize` value to the
   explicitly written read-only or writable data-pointer type. The latter is a
   programmer-trusted assertion of write authority; conversion never infers or
   implicitly widens permission. `T` must have a complete, nonzero-sized layout.
   Address zero produces canonical `null`. A statically known nonzero address
   not divisible by `alignof(T)` is rejected in both modes; debug builds
   diagnose dynamic nonzero misalignment, while release emits no guard. No mode
   validates mapping, lifetime, extent, allocation membership, provenance, or
   actual readability or writability. Pointer-to-integer conversion discards
   type and permission facts; converting an unchanged address back restores
   only the explicitly written pointer type and address. Function values,
   slices, strings, contracts, and other callable or multiword values do not
   participate.
   `*U(pointer)` and `mut *U(pointer)` reinterpret a data pointer `*T` or
   `mut *T` as the same address viewing `U` (untyped storage, below). Both `T`
   and `U` must be plain bits, and permission never widens: `mut *U(p)`
   requires `p: mut *T`. `null` stays `null`. When `alignof(U)` exceeds
   `alignof(T)`, the address alignment rule above applies; otherwise the
   conversion is proven aligned and emits nothing. `*T(address)` from `usize`
   remains unrestricted as the raw-address boundary.
   A slice view may be constructed explicitly from an exact element pointer and
   element count: `[]T(pointer, count)` accepts `*T` or `mut *T`, while
   `mut []T(writable_pointer, count)` accepts only `mut *T`. The count is
   exactly `usize`; permission may shrink but never widen. Null plus zero is
   canonical null, while a non-null pointer plus zero remains a non-null empty
   view. Null plus a positive count is rejected when locally constant,
   diagnosed in debug output when dynamic, and is a release precondition.
   The same static/debug/release policy applies to `count * sizeof(T)` and
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
15. Allocation-free byte operations are complete-slice operations: `mem.copy(noalias dst: mut []u8, src: []u8)`, `mem.move(dst: mut []u8, src: []u8)`, `mem.fill(data: mut []u8, value: u8)`, `mem.zero(data: mut []u8)`, and `mem.compare(a: []u8, b: []u8): int`. `copy` and `move` require equal slice lengths and copy exactly that many bytes without changing either slice descriptor; callers explicitly slice larger storage to the intended range. Empty slices are valid. `copy` requires its complete source and destination ranges not to overlap, expressed by `noalias` on the writable destination; `move` permits overlap in either direction. `fill` writes one byte value throughout the complete slice and `zero` writes zero throughout it. All four writes return nothing. The typed overloads `mem.copy[T](noalias dst: mut []T, src: []T)` and `mem.move[T](dst: mut []T, src: []T)` follow the same rules over equal element counts and copy `len * sizeof(T)` bytes. Write destinations carry type-position `mut` (Mutability #10); `mem.release(mut data: []u8)` keeps its call-site mark because it replaces the caller's descriptor, not bytes.
16. `mem.compare` compares complete slices lexicographically as unsigned bytes and returns exactly `-1`, `0`, or `1`; after an equal prefix, the shorter slice sorts first. It accepts unequal and empty slices and changes nothing. Every byte operation uses only caller-owned storage and performs no allocation. A `copy`/`move` length mismatch or a false `copy` no-alias promise is a programmer error: debug builds diagnose it at the call boundary, while release assumes the preconditions and pays no validation cost.

Carved (compile-time layout verification):
`offsetof(T, field.path)` is the compile-time `usize` byte offset of a direct or nested field within a complete struct `T`. Each path component traverses inline struct storage; pointers, arrays, and non-struct intermediates are rejected rather than implicitly dereferenced or indexed. `static_assert(condition)` is a reserved, zero-code statement at module or function scope. Its single Boolean condition must be compile-time provable from literals, const and enum values, integer and Boolean operations, comparisons, and layout builtins; false and non-constant conditions reject the program. Messages and broader constant evaluation remain additive.

Carved (memory representation):
17. Representation is normative, not implementation detail: the layout rules below may be relied on by programmer code, including inline-asm bindings, descriptor inspection through `&`, and `sizeof` reasoning (Memory #4 machine-true; zero runtime cost; later additions remain additive).
18. A slice `[]T` is exactly two words: the element pointer at offset 0 and the element count at offset 8. `sizeof([]T)` is 16 with alignment 8. There is no capacity word and no hidden field, ever; capacity, where wanted, belongs to an explicit user struct, never to the view.
19. `string`, `[]char`, and `[]u8` are three intent spellings of one type and therefore share identity, representation, conversions, and overload behavior; `char` is the transparent alias of `u8` specified by Standard Library #16. The shared view is exactly two words — byte pointer at offset 0 and byte count at offset 8 — with size 16 and alignment 8, and the language never appends a NUL terminator. `mut string`, `mut []char`, and `mut []u8` likewise name one writable-view type; `mut` never changes layout, size, canonical zero, or emitted code (Mutability #9). Plain spellings are read-only through their bytes.
20. A string literal resides in static read-only storage holding exactly its written bytes — no appended NUL, nothing hidden. The empty literal `""` is a valid non-null string (byte pointer, count 0); only the all-bits-zero string is `null`. `mem.equal("", zero_string)` is true by content, while `"" == null` is false because nullness tests the complete descriptor (Values & Passing #11).
21. An array `[N]T` is `N` contiguous elements in index order with no metadata and no inter-element padding: `sizeof([N]T)` is `N * sizeof(T)`, and array alignment is the element's alignment. Padding and byte order inside one element are that element type's own platform facts.

Carved (untyped storage):

- Storage has no dynamic type. Reading memory as `T` at an address satisfying
  `alignof(T)` reads those `sizeof(T)` bytes as `T`, and a write of `T` writes
  exactly those bytes. Passes never assume two accesses are disjoint because
  their types differ; non-overlap comes only from stated facts (`noalias`,
  read-only parameters).
- Plain-bits types are the integers, `char`, `bool`, the floating types, all
  enums, bits types, data pointers, C function values (C Interop #5), distinct
  forms of these, and arrays and structs composed only of them. Every byte pattern of a
  plain-bits type is a value, except that a `bool` read from reinterpreted
  storage is trusted to be `0` or `1` (Type System #12). Tagged unions, `Error`,
  error domains, `typeid`, contracts, Ace function values, slices, strings, and
  tuples are not plain bits: their values are created only by the language, and
  reinterpretation never produces one.
- `bitcast(U, value)` is a reserved builtin copying the bits of `value` as `U`,
  in native byte order. `value` must be typed (an untyped constant is converted
  first), both types plain bits of equal size, and the source type free of
  padding bytes, so a copy never reads bytes without a value. A float and an
  integer of one width move between registers; any other pair goes through a
  stack object aligned for both.

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

Carved (typed allocation):
22. The `mem` module exports four explicitly instantiated generic overloads beside the raw byte forms: `mem.alloc[T](count: usize): mut []T`, `mem.zalloc[T](count: usize): mut []T`, `mem.realloc[T](data: []T, new_count: usize): mut []T`, and `mem.release[T](mut data: []T)`. Generic instantiation stays explicit (Generics #5): deallocation is spelled `mem.release[T](mut data)`, never inferred from the argument. As in the raw forms, allocation and successful reallocation originate writable-content proof in their results, while reallocation and release inputs require authoritative descriptors but not element-write permission. The raw and typed forms remain one overload set per name.
23. `mem.alloc[T](count)` requests storage for exactly `count` contiguous `T` elements. The byte size is `count * sizeof(T)`, computed safely before the platform allocator is called; the alignment is selected automatically from `T` and the caller supplies neither. The returned `mut []T` is the allocation pointer plus `count` as its element length — no capacity, allocator value, ownership marker, header, or other metadata (Memory #18). Storage uses the target's native representation, alignment, and byte order: typed native memory, not serialization.
24. Typed allocation is uninitialized exactly like the raw form: no value initialization, zeroing, constructor, destructor, or hidden loop. Heap contents remain an explicit programmer-trusted boundary under the existing definite-initialization model; the compiler adds no ownership, lifetime, per-element initialization, or alias analysis, and reading an element before the programmer has established a valid `T` representation is programmer error. `mem.zalloc[T](count)` is the one zero-filling form: it follows every rule of `mem.alloc[T]` (Memory #23, #25) but every byte of the returned storage is zero, which is a valid `T` for every type (every type has an all-bits-zero representation), so a zero-valid container needs no initialization loop. It lowers to the platform's zero-filling allocator, which can avoid touching pages the allocator already holds zeroed; `realloc[T]` growth stays uninitialized.
25. A multiplication overflow, an unsupported type alignment, a zero byte-size request — `sizeof(T) == 0` or `count == 0` — or allocation failure returns the canonical null `mut []T`; nothing aborts, and an overflowing or zero-size request is rejected without calling the allocator. The `mut` qualification changes no null representation and may shrink implicitly to plain `[]T`. Zero-sized types get no dangling-pointer or zero-sized-allocation machinery.
26. `mem.realloc[T]` requires the complete authoritative non-null typed slice returned by typed allocation or a prior successful typed reallocation of the same exact `T`; the input may have been permission-shrunk to plain `[]T`. It may move the storage. On success the `mut []T` result is the only authoritative slice and has length `new_count`; on overflow, invalid size/alignment, or failure it returns null while the original allocation remains live and unchanged. Newly added elements are not initialized and removed elements are not destroyed. Copied slices remain ordinary aliases and may dangle after a successful move, exactly as with the raw API (Memory #12); no ownership or alias search exists.
27. `mem.release[T](mut data)` releases the complete authoritative typed allocation. The `[T]` instantiation must match the argument's `[]T` element type exactly. A null typed slice is a no-op; otherwise release performs no element destruction or hidden iteration and sets only the passed caller variable to canonical zero — zero pointer and zero length. Copied slices are not found or cleared and become dangling aliases.
28. `mem.usable(data: []u8): usize` reports the usable byte size of the live allocation whose base pointer `data` holds: at least the requested length, and allocator-defined beyond it. Release lowering is the platform allocator's own size query (Darwin `malloc_size`) with no Ace header or registry lookup. A null slice, a pointer the allocator does not own, and an interior pointer report 0. It is read-only: the argument takes no call-site `mut`, and typed `[]T` slices are not accepted (`[]T` does not convert to `[]u8`).
29. A view of storage that ends when its function returns may not directly escape it. Such storage is a local, a plain parameter, or a `[]T` array literal with a runtime element (Syntax & Parsing #8); a module variable, a `mut` parameter, and a constant `[]T` literal outlive the call. A view of it is `path[lo..hi]` of an array or `&path`/`mut &path`, where `path` stays in the variable's own bytes (fields of a value, elements of an array, never through a pointer or slice), or such a literal. It escapes directly when it is the value of `return`, or is assigned to a path rooted in a module variable or `mut` parameter, including inside a tuple, aggregate literal, or pointer, slice, `string`, or contract conversion. Either is a compile error. The check is syntactic: a view reaching `return` or outer storage through a variable, a call, or a pointer is not tracked (Memory #12).

## Values & Passing
Decision: value semantics with explicit copies; references only at call boundaries.
1. Assignment copies — `q = p` gives `q` its own value storage; mutating that copied value never changes `p`. For a slice this copies only its pointer and length: reassigning/reslicing `q` does not change `p`, while element writes through either view reach their shared backing storage (Memory #12).
2. A `mut` parameter or method receiver is a reference to the caller's argument storage; its writes are visible to the caller. A plain parameter is a read-only value whose representation is the compiler's choice: a copy, or the address of the caller's argument. A program must not depend on which: a plain parameter's address does not outlive the call (Memory #29), and writing its argument's storage during the call — through an aliasing `mut` parameter, a module variable, or a pointer — is programmer error after which the parameter's value is unspecified.
3. Plain declarations are stack storage; heap memory exists only via explicit allocation (see: Memory #5).
4. Copy-on-write is not a language semantic — assignment is a plain copy. COW, where wanted, is an explicit opt-in library type.
5. The compiler may elide copies when the result is observably identical (codegen freedom, not language semantics).
6. Every type has a canonical all-bits-zero representation. Numeric zero is `0`/`0.0`, boolean zero is `false`, pointer-like and handle-like zero is `null`, aggregates are recursively zero, and the first enum member is represented by zero.
7. Globals without initializers use canonical zero and reside in BSS. A local without an initializer reserves uninitialized storage and may not be read until definitely initialized. Whole assignment initializes the whole value; field and constant-index element assignments initialize those exact subobjects. A whole aggregate read requires every subobject to be initialized. Branch joins retain only facts true on every continuing path; a loop cannot establish a fact after itself because it may execute zero times.
8. Initialization checking follows direct Ace storage paths. A view of a local is checked where it is created and is not tracked afterwards. A read-only view (`&path`, `array[lo..hi]`) reads every subobject it may reach: a constant path exactly, and the whole array through a dynamic index or any array slice. A writable view (`mut &path`, `mut array[lo..hi]`) reads nothing and initializes the subobjects it certainly covers — a constant path, or a slice spanning every element — as a programmer-trusted promise that they are written before they are read; any other writable view initializes nothing. Pointer aliasing, inline assembly, and extern code are explicit programmer-trusted boundaries and receive no hidden alias or postcondition analysis.
9. An assignment evaluates its right side before its target. `x = e` and `x op= e` evaluate `e` exactly once, then the target's address, then, for `op=`, read the target's old value, and then write it, so a right side that moves or writes the target's storage leaves no stale address or old value. Multi-assignment is simultaneous on its right side: `a, b = b, a` evaluates every right-hand expression exactly once in source order and preserves the complete result tuple before evaluating or writing any target. Targets are then evaluated and written from left to right, so if targets overlap, the later target's write is the final one. The number of values must equal the number of targets. Types are checked positionally, not uniformly: each right-hand value must be assignable to its corresponding target under the ordinary explicit-conversion rules, so heterogeneous assignment such as `number, message = 2, "new"` is valid. The compiler may keep the preserved values in registers or temporary stack storage; no user-visible temporary or allocation is created.
10. A multi-name `=` statement decides declaration or reassignment independently at each position. A typed name must not already exist and declares a new binding; a bare name must already exist and reassigns it: `written: int, error = write(...)`. Any order and mixture is valid, including the all-typed declaration `number: int, message: string = 1, "old"` and the all-bare assignment `number, message = 2, "new"`. Arity and types are checked positionally under Syntax & Parsing #9. Every name is resolved against the scope as it stood before the statement, so one position never makes another position valid; typed names are uninitialized while the right side is evaluated. The statement requires an initializer and stays on one physical line under Syntax & Parsing #7.
11. Sequential types — arrays, slices, and strings — are never compared with `==` or `!=`, regardless of element type. Byte-backed sequences (`[N]u8`, `[]u8`, and `string`, including plain and `mut`-qualified forms) compare content explicitly through `mem.equal(a, b)` in every pairwise combination, either operand order, and at any fixed-array lengths: length first, then exact stored bytes, with no allocation, conversion, decoding, or normalization. Length-only comparison is written `a.len == b.len`. Non-byte arrays and slices have no built-in content-equality helper; use an explicit loop. Where a content or length comparison is negated, the result is exactly the negation of the corresponding `mem.equal` or `.len` check; sequences themselves have no separate `!=` semantics. A comparison whose other operand is the literal `null` is instead the distinct nullness form specified by Sequences #8 and grants no sequence-to-sequence equality.
12. A global `const` initializer may name another initialized global `const`, including a public constant reached through an imported module. The referenced initializer is recursively evaluated at compile time and must itself be representable by the existing constant-expression rules; this introduces no runtime module initialization or storage read. A global initializer of a primitive integer or float type may also be constant arithmetic over literals, initialized global constants, and casts: integers fold `+ - * / % & | ^ << >>` and unary `-` `~` at the destination's width and signedness, floats fold `+ - * /` and unary `-` at the destination's precision (`f32` rounded after each operation, non-finite results rejected), and a cast folds over any such expression, including an integer cast to a float. Per Safety & Debug Builds #4 and #6, overflow, division or remainder by zero, and an out-of-range shift count are compile errors in every mode. The destination's stated type remains authoritative, and ordinary exact type checking still applies.

## Mutability
Decision: locals are mutable by default; mutation marking lives at reference boundaries.
1. Local variables are mutable by default; `const` is the rare opt-in. [keyword provisional]
2. Unmarked by-reference parameters protect the caller's argument storage; `mut` on a parameter grants assignment to that caller storage — visible in the signature and repeated at the call site. Pointer reachability is a separate permission: `*T` reads its immediate pointee and `mut *T` may write it. Thus `mut p: *T` may replace the caller's pointer but may not write `T`; `p: mut *T` may write `T` but may not replace the caller's pointer; `mut p: mut *T` permits both. `mut` never marks local declarations, which remain mutable by default.
3. Per-parameter `const` does not exist — unmarked already means read-only. Function-level `const` does not exist — "writes nothing" is spelled "no `mut` in the signature." (A future opt-in purity annotation — no writes anywhere, globals included — remains possible; it would carry information the signature cannot.)
4. Const globals are the performance-relevant case: truly immutable data can live in read-only memory.
6. An argument whose own caller storage may be replaced carries one `mut` token at the call site — reassignment is greppable, and an unmarked call never changes the argument value itself. Writes through a `mut *T` need no argument-level `mut`: the pointer value is not replaced, and its pointee permission is already explicit in the pointer's type. Composes with method syntax: a mutating receiver is visible in the method signature and call. A `mut` receiver is passed by address exactly as `mut &receiver` is, so definite initialization treats the two alike: the call may take a not-yet-initialized receiver and leaves it initialized; a pointer receiver is read as usual.
7. Composition order of `mut` × `*T` is per pointer level: `mut` means writes are granted through that pointer level and `*` means this is a pointer. `*Node` is read-only and may be null; `mut *Node` is writable and may be null. Every `*` in a multi-level pointer has its own permission: `mut **T` may replace the inner `*T` but not write `T`; `*mut *T` may read the inner pointer and then write `T` but not replace the stored inner pointer; `mut *mut *T` permits both writes. Only the pointer level immediately used for a write needs `mut`.
8. `noalias` parameter marker: a signature-stated fact that memory reachable through one `noalias` parameter is not reachable through another or through any parameter that can write the same reachable storage (an outer `mut` reference or a writable pointer level). The emitter may vectorize and reorder on it. Debug builds verify slice-range disjointness pairwise in O(1) at call boundaries — a false claim crashes in dev, never in prod. Release cost: zero. Signature facts are optimizer facts, same pattern as read-only.
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
2. Zero pointers may be stored, copied, compared, and replaced. Dereferencing one is invalid. Zero function and contract values likewise may be stored, copied, and compared; calling or dispatching through one is invalid. Debug builds diagnose these invalid operations; release inserts no null check and has C-level unchecked behavior.
3. A zero slice or string has zero pointer and zero length. Length queries, empty iteration, and zero-length operations are valid; indexing still requires an in-range element.
4. Zero `Error` means success/no error; its packed domain and code bits are both zero. Zero `typeid` means no type identity; every `typeid(T)` result remains nonzero.
5. When application data must distinguish absence from an ordinary zero value, it declares that distinction explicitly, normally as a struct containing a `present: bool` field and the value. There is no hidden tag or implicit optional ABI.
6. `blank(T)` is the builtin that spells the canonical zero of any type `T`: a compile-time, shadowable name like `sizeof`, taking one type in the full type grammar and evaluating to a `T` whose bits are all zero. It works for every type with a value layout — including type parameters of a stamped generic, where `null` and literals cannot name a generic zero — and rejects error-domain names (use `Error`) and forward-declared types. It performs no runtime work beyond filling the value with zero bytes. It states a filler, not absence: a value that must mean "none" still travels with an `Error`, a pointer, or a `present` field (#5). [spelling provisional]

Historical design note (non-normative): `T?` optionals were explored, specified, and implemented, including niche and tagged representations. They were removed because canonical zero already covers machine-level sentinel values, while application-level presence is clearer as an explicit struct. Removing the operator also eliminates a second family of layouts, coercions, flow rules, and ABI cases.
