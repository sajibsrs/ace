## Type System
Decision: fully explicit types; no inference.
Predeclared types: `bool`, `i8`/`i16`/`i32`/`i64`,
`u8`/`u16`/`u32`/`u64`, `f32`/`f64`, `float`, `string`, `usize`,
`typeid`, and the platform words `int`/`uint` (i64/u64 on Apple arm64).
These names, `char`, `StaticMap`, `Error`, and `void` (Syntax & Parsing #5)
cannot be redeclared as types.
`float` is a transparent alias of `f64`: the spellings have the same type
identity, representation, conversions, ABI behavior, generic identity, and
overload signature. Consequently, overloads cannot distinguish `float` from
`f64`. Floating literals continue to default to this shared type.
`char` is the transparent `u8` alias specified by Stdlib #16.
1. Every signature, global, field, parameter, return, local, and multi-binding states its type. Generic instantiation is also explicit.
2. No type inference keeps compilation direct, types deterministic, declared intent locally visible, and generated code machine-true. Initializer expressions are checked against the type written on their bindings.
3. Context never supplies a type argument. A declared type reaches into an expression in exactly two ways: an untyped constant or `null` takes the type of the declaration, assignment, field, payload, parameter, return, or tuple member it initializes (Syntax & Parsing #10–11; Zero, Null & Absence #1), and a declared function-value type, including a parameter's, selects one written function from an overload set (Names & Scoping #8).
4. Type aliases: `Name: type = Existing` — transparent, zero-cost notation; the alias IS the type, fully interchangeable. Generic aliases allowed: `Pair[T]: type = (T, T)`.
5. Distinct types: `Name: distinct Existing` — same layout, new nominal type. `Name(value)` constructs (constructor position is free via the namespace split); distinct types do not mix with their base or each other without explicit conversion. Zero runtime cost — pure compile-time discipline for units, handles, and IDs.
6. `typeid(T)` is a compile-time builtin for an exact named type. The compiler assigns a nonzero 32-bit ID represented in the existing pointer-sized `typeid`, allowing an error's upper 32 bits to compare directly with `typeid(Domain)`. The same exact type compares equal throughout one linked program; different nominal types and exact generic instantiations compare unequal; transparent aliases share their target's identity. Values support storage, passing, `==`, and `!=`, but no ordering, arithmetic, reflection, integer conversion, runtime name, serialization, or cross-build/process stability. Zero/null means no type identity.
7. Every inline value type has a finite compile-time layout. A struct, union payload, array element, tuple member, distinct base, or alias expansion may not contain itself through an all-inline path, directly or indirectly; such a cycle is a compile error. `A: struct { a: A }` and the mutual `A: struct { b: B }` / `B: struct { a: A }` therefore have no layout and are rejected.
8. A pointer breaks a layout cycle because its size and alignment do not depend on its pointee. Self-pointers are valid, as are mutually recursive pointer-linked types: `Node: struct { next: *Node }` and `A: struct { b: *B }` followed by `B: struct { a: *A }`. Pointer mutability changes write permission, not this layout rule. A slice breaks a layout cycle the same way (fixed pointer-plus-count representation): `Json: union { none, object: []Pair }` and `Pair: struct { key: string, value: Json }`.
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
- Each field begins at the first offset satisfying its effective alignment,
  following the preceding field, unless field `@offset(N)` places it at exactly
  byte `N`: a compile-time `usize` that must be a multiple of the field's
  effective alignment. On a field, `@offset` precedes `@align`. Struct size is
  the largest field end rounded up to struct alignment. Nested values retain their
  internal layouts; packing changes placement, not contents. Explicit byte
  arrays represent reserved or unnamed bytes. `sizeof`, `alignof`, and
  `offsetof` expose the result.
- A struct whose fields share bytes is an overlapping struct (a C union is one
  whose fields are all `@offset(0)`). Every field must be plain bits (Memory,
  untyped storage) and none may have a default. Reading a field reads its
  bytes and writing one writes only its bytes. It has no literal and no `==`;
  it starts as zero or uninitialized, and initialization checking tracks it
  whole, so any member write initializes it. Its zero is all zero bytes.
- Packed, explicitly aligned, and explicitly offset structs are C-compatible in
  every position, direct `extern "c"` parameters and returns included, when all
  recursive rules hold and the layout matches the external definition; one
  crosses by value as the target's C ABI passes the matching packed struct,
  aligned struct, or union. Layout attributes do not determine byte order, bit
  fields, observability, or synchronization.

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
    promise.
12. `bool` is one byte with alignment one; `false` is `0` and `true` is `1`,
    and no other byte value is a valid `bool`. It maps to C's `_Bool`/`bool`
    and is C-ABI-compatible in every position an integer is (C Interop #3).
13. Enum-indexed arrays. `[E]T`, with `E` a dense enum (its
    discriminants are exactly `0 .. n-1`, whatever their declaration order),
    is an array of `n` `T`s with `[n]T`'s layout (Memory #21) but its own type
    identity: no conversion to or from `[n]T`. A sparse enum is rejected. The
    bracket names a type, so it is `[E]` by namespace (Grammar notes #4).
    Indexing takes exactly `E` — an integer, or another enum, is a type error —
    and needs no bounds check in either mode, because an `E` value is always a
    member (Safety #2). `.len` is `n` as `usize`; `for v: T in a` iterates in
    discriminant order, and the two-binder form binds the index as `E`
    (Iterations #7). Slicing `a[lo..hi]` takes `E` bounds and yields `[]T`.
    The literal form is Syntax & Parsing #16; repetition `[v : n]` applies
    with `n` equal to the member count. A type parameter as `E` is not admitted
    yet.
14. Enum member names. `v.name`, on a value `v` of an enum type, is the
    member's unqualified source spelling as a read-only `string` (`Kind.binary.name`
    is `"binary"`). It is opt-in static metadata exactly as Errors #14: the
    name table and lookup are emitted only for an enum whose `.name` is used,
    the lookup is allocation-free and maps each discriminant (dense or sparse)
    to its name, and there is no runtime registry, initialization, or general
    reflection. Because a closed enum's value is always a member (Safety #2),
    there is no unknown-value case; an open enum's non-member is `""` (#15).
    `.name` is value lookup; `E.name` remains type lookup and names a member
    called `name` (Syntax #2).
15. Open enums. An enum whose member list ends with `_` is open: it must
    state its backing type, and every value of that type is a value of the
    enum. `E(n)` converts without a membership check and a constant need not
    name a member (Safety #2 covers closed enums only); `.name` of a
    non-member is `""`; an open enum cannot index `[E]T` (#13). Every other
    rule of #10 and #11 applies. [`_` spelling provisional]
16. Enum members. `members(E)`, with `E` an enum type (a type parameter
    included, checked per instantiation), is a constant `[n]E` of `E`'s named
    members in declaration order: `for f: F in members(F)` iterates them and
    `members(F).len` is the member count wherever an array's `.len` is a
    constant. It is a value, not storage, so it is never assigned or viewed; an open enum's `_` contributes no member. Like `blank`, it is a
    shadowable builtin taking a type in the full type grammar, and its array is
    static data emitted only for an enum that uses it. A non-enum `E`, a
    distinct type over an enum included, is an error. [spelling provisional]

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
   but cannot be addressed, passed as a receiver, used with `offsetof`, or
   accessed individually through `volatile`. Assignment requires an ordinarily
   writable containing place and performs one preserving read-modify-write of
   the complete backing value. The containing place and assigned expression are
   each evaluated exactly once. Separate assignments remain separate updates.
4. A constant write outside the field's signed or unsigned range is rejected in
   every mode. A runtime violation aborts in debug mode; release keeps the low
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
   exactly the backing width. A bits type crosses `extern "c"` signatures and C-facing
   aggregates exactly as its backing integer.
   No C bit-field ABI equivalence, atomic field operation, volatile field RMW,
   reflection, wider backing unit, or cross-statement fusion is implied.

## Abstraction
Decision: composition over inheritance; dispatch is explicit.
1. Encapsulation = module privacy (see: Modules) — the file is the unit of hiding, not a class.
2. Abstraction + dynamic polymorphism = explicit contract values (vtable + data, constructed and visible). Signatures are contracts: explicit types, explicit exports.
3. Implementation reuse = explicit struct embedding at a known offset + type-owned methods. `embed name: Base` stores an ordinary named `Base` field and promotes its methods one level to the containing struct. A method owned directly by the containing struct wins; a field of the containing struct sharing a promoted method's name is an error; otherwise two exact promoted matches are an ambiguity error. Fields are never promoted. Promotion preserves the method's visibility rules. A method declaration names its receiver as an explicit, untyped first parameter, passed by address: `Circle.scale(self, k: i32)` sees and may write the caller's value; `Circle.area(const self)` only reads it (Mutability #6). `self` is reserved and valid only as the first parameter of a type-owned or contract method declaration; a call never passes it.
4. Inheritance is rejected: subtyping is served by contracts, implementation reuse by composition.

Carved (contract value layout):
5. A contract value is two words: a pointer to a const vtable (read-only memory) plus a data pointer. Its all-bits-zero value is the null contract; there is no separate optional contract type.
6. A vtable is a struct of function values typed by signature — no new type concept.
7. Each struct has at most one canonical signing, declared in the struct's defining module: `Circle signs Shape, Color`. A second `Circle signs` declaration is an error, as is repeating a contract in the list. A multiline contract list uses parentheses. The signing and its type-owned methods may appear in any order (Compiler #5); its method set is checked once every method is known. Methods may exist without a signing.
8. A signing registers one exact concrete/contract pair per listed contract. Construction names the target contract explicitly: `s: Shape = Shape(&c)`. The compiler looks up the concrete type's one signing and emits one const vtable per exact used pair. There is no named signing value, alternate implementation, open-ended search, runtime lookup, or trait solving.
9. Contract methods are plain signatures whose first parameter is `self`, as in a method declaration (`area(self): f64`). Conformance matches method name, exact parameter types, and return type. Methods may overload by exact parameters; return types must match a requirement but never distinguish or select overloads.
10. Calls are `c.area()` and `c.scale(2)` in both statement and value-producing expression positions, the only method call form. `Circle.area` by itself is not a first-class method value. Dispatch through a contract calls the selected vtable slot with `s.data`.
11. `Shape(&c)` does not mutate `c`; a later `s.scale(2)` writes it through the contract. `Shape(&c)` on a local `const` is rejected (Mutability #1); over a `const` global, a write faults at run time.
12. Signings and methods are private by default. `pub Circle signs Shape, Color` exports every conformance in that canonical signing; `pub Circle.area()` exports a method for direct external calls. A public signing may use private methods internally.
13. Generic signings ride the bracket rules: `Box[T] signs Value[T]`. Both sides are explicitly instantiated at use (`Value[int](&box)`), and each used exact pair gets its own stamped vtable. The one-signing rule applies to the nominal generic struct; per-instantiation/conditional signings are rejected.
14. Contract promotion is one-way from an explicitly embedded base to its embedder, never in reverse. If the base signs the same contract, its promoted exact methods may satisfy the embedder's explicit signing. Dispatch uses a compiler-generated fixed-offset receiver thunk; it adds no object metadata or allocation. Embedding never makes the base satisfy contracts signed only by the embedder, never creates a subtype relation, and never searches beyond the directly embedded field.
15. Type-owned associated functions (informally "static methods"): a `Type.name(...)` declaration whose first parameter is not `self`, for constructors and other factory-shaped operations with no receiver to dispatch on. Methods and associated functions are both declared and reached with `.`; only the presence of `self` tells them apart. `pub Map[K, V].new(capacity: usize): (Map[K, V], Error)` declares one; `Map[K, V].new(capacity)` calls it, never through a value (`m.new()` is a compile error). A generic owner's type arguments are spelled once, on the type, and reused by the function's body rather than repeated on the function itself. An associated function never acquires a vtable slot. A method and an associated function may never share owner+name. The owner may be a type parameter (`T.make()`), resolved in each instantiation to its bound type, which must be a struct. An associated function's own additional type parameters, beyond the owner's, are additive and not yet supported.

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
11. Public unions and their qualified variants follow normal module visibility. Tagged unions are initially rejected in `extern "c"` signatures; no C representation is implied.
12. Tagged unions are closed data alternatives, distinct from contracts: contracts provide open-ended behavior through explicit vtable/data values. Ace has no universal `any` type.

## Generics
Decision: explicit monomorphization.
1. Generic functions and types exist; instantiation is explicit — types named at the use site.
2. No trait solving, no comptime reflection, no implicit instantiation — each instantiation compiles as plain stamped code.
3. Runtime-perfect specialization; duplication solved; compile cost bounded by what is actually written.
4. Type parameters ride brackets: `max[T](a: T, b: T): T`; generic types likewise: `Vec[T]: struct { items: []T, cap: int }`.
5. Instantiation always names its types — `max[int](3, 9)`, `sort[Point](pts)` — no deduction from arguments, ever; the no-inference rule (Type System #1) covers type arguments too.
6. Instantiated function values type cleanly: `cmp: (int, int): int = max[int]`.
7. No constraint syntax exists — T is unbounded. Checking happens per instantiation: the body must typecheck for the stated types; an error names its line in the body and each instantiation site that led to it.
8. Each distinct type-argument tuple stamps a separate plain function/type — stamping is bounded by written instantiations.
9. Generic specialization: a generic function declaration may have same-name specializations, anywhere in its file, whose brackets name concrete type arguments — `parse[int](text: string): (int, bool) { … }`. Each specialization is an ordinary, independently checked function, not a template instance.
10. Selection is exact: an instantiation resolves to the specialization whose type-argument tuple equals the written arguments; with no match, the template stamps as usual (rules 1–8). A specialization's parameters and returns must equal the substituted generic signature — checked at declaration against the expected signature, never discovered at a call. Duplicate specializations for one exact tuple on one generic overload are a declaration error.
11. A specialization binds to exactly one generic overload, chosen by ordinary substituted-parameter matching (Names & Scoping #7); each overload of a name specializes independently. All type parameters must be supplied positionally — no partial specialization.
12. Bracket interpretation is by name, not inferred, and brackets hold bare named types only (v1): brackets any of whose identifiers name a type (primitive, `void`, imported, or declared anywhere in the file, Compiler #5; never the root file's names in an imported module) are specialization type arguments; otherwise they are a fresh type-parameter list. A specialization whose name has no generic declaration in the file is a compile error, never a silent new generic. Compound type arguments (`[]u8`, `*Node`, `Pair[int, string]`) are an additive relaxation.
13. `specialized` after the return clause marks a template with no default body [P: spelling]. Calling it with an unmatched type tuple is a call-site error naming the missing type and the declaration; a `specialized` generic with zero specializations in the compiled program is a declaration error.
14. Scope: specializations live in the generic's module (v1); struct generics (`Box[T]`) are untouched, and type-owned generic methods specialize additively later.
15. `void` is a type argument (`apply[int, void]`, `Task[void]`). A type parameter bound to it stands only where a return stands — a function type's return or the generic's own — and means no value there; anywhere else (a parameter, local, field, pointee, element, `sizeof`, `typeid`) that instantiation is an error. Overload candidates whose signature would place it elsewhere do not match.
