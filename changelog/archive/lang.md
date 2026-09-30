# Archive — Core language, types, and memory

Entries recorded before the changelog split; oldest first.

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
- Lowered `binding.member` in the scoped import walk, so a local or parameter
  named like a whole-module binding shadows it: `.` on it is a field access,
  where it was rejected as an unknown module member, or silently resolved to
  the module when the member existed. Type positions (`shape.Box`,
  `shape.Box{...}`, sizeof and typeid operands) still name the module's type.
  The separate unscoped qualified-lowering pass is gone.
- Indexed an imported file's renamed declarations in an open-addressed FNV-1a
  table built once before the reference walk. Each unshadowed name had scanned
  every declaration, so lowering was quadratic in the file's size: an imported
  file of 8000 struct types and functions took 25.1G instructions to compile,
  now 3.0G (500: -23.5%). The regex and json package checks gain 0.3%.
- Merged the imported-file rename walk into the scoped reference walk: one
  pass, after an imported file's declarations take private names, lowers
  selected imports, `binding.member`, and that file's own names, so one set of
  scope rules serves all three. The rename walk's own gaps close with it: an
  `offsetof` field path named like a module global was renamed to the global,
  and a signing's type parameters were not bound. Checking the regex and json
  packages takes about 1% fewer instructions.
- Lowered imported names in array lengths within type positions. The scoped
  import walk skipped an array type's length expression and the parts of a
  tuple, function, or map type in a sizeof-style operand, so `[sizes.SIZE]u8`
  (since the qualified pass moved into that walk) and `[SIZE]u8` from a
  selective import reported an unknown name.
- Scoped a switch payload binder to its case block in module lowering, so
  `Event.data(count: int)` shadows a module global in an imported file, a
  selected value, or a whole-module binding there instead of being lowered to
  it; an imported file's `return count` in that case read the global.
- Scoped a local from its declarator in module lowering as sema already did,
  so an imported file's `total: usize = total + 1` names the uninitialized
  local instead of the module global, and a selected import no longer reaches
  a same-named local's initializer. A bare name in a multi-name `=` is now
  lowered as a reference rather than bound as a new local, so an imported
  file can reassign a module global there.
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
- Implemented the `inline` declaration modifier (Functions & ABI #8). Sema
  checks the declaration rules and, after forward calls resolve, walks inline
  call trees for cycles. Before a function's planners run, `inline_plan`
  attaches to each direct call a copy of the callee (stamped first when
  generic) with fresh binders and `Sym`s, so loop-register and promotion
  planning treat the expansion as the caller's own locals; `gen_inline` binds
  the arguments, emits the copy, and joins returns into a result temp, while
  statement boundaries reset temps to a base that keeps the enclosing
  expression's temps live. `std/map` marks `mix`, `probe`, and its `hash` and
  `same` specializations; `pkg/json` marks eleven `Formatter` helpers. Release
  builds: the 158-byte formatter document drops from 1096 to 821 ns, and
  16k-key `set`/`get`/`unset` from 71.5/21.2/32.4 to 46.5/15.9/26.1 ns; the
  benchmarks' text grows 20% and 6%.
- Limited inline planning to functions that call an inline function: sema
  marks such a body `F_INLCALLS` in `check_call`, `inline_plan` walks only
  marked bodies and marked callee copies, and `plan_loops` searches
  expressions only once a site exists. Unmarked code had paid for both walks:
  checked compiles of the 20,000-function, 20,000-statement, and 20,000-type
  generated corpora had run 6.4%, 9.2%, and 5.3% more instructions than before
  inlining, and now run 1.0%, 2.9%, and 1.1% more.
- Allowed `return f()` to forward a multi-return call whose result tuple
  matches the signature exactly (count, order, and `same_type` per value);
  sema checks it in `N_RETURN`, and lowering already returned any tuple-typed
  expression. Mismatches name the first differing position.

## Types and abstraction

- Added the allocation-free `ascii` module with control-byte classification,
  lowercase conversion, and selectable case-sensitive comparison; HTTP now
  reuses it, exposes serialization and incremental response parsing as
  type-owned methods, uses `str.concat` for multi-part text assembly, and keeps
  `mem.copy` for single-range byte movement.
- Added allocation-free, all-or-nothing `str.concat` over a string-parts slice
  and caller-owned destination storage.
- Added formatted `str.format(destination, "format", ...)`: `fmt.printf`'s
  checked plan (one `check_format_call` for both, parameterized by the
  format's argument position, helper names, and diagnostic name) lowered to
  `put_*` helper calls on the unwritten rest of the destination, returning
  `(0, false)` at the first piece that does not fit. Radix and fixed-float
  rendering moved into the internal `std/@/rendering.ace`, shared by `fmt`
  and `str`. `str.concat` and the `str.format` value overloads no longer
  promise to write nothing on insufficient capacity; `concat` now copies in
  one pass. `demo/sdl3-rgb-cube` builds its overlay with it.
- Added width and zero padding to `fmt.printf`/`str.format` conversions
  (`%5d`, `%08x`, `%06.2f`), and moved both calls onto one lowering over a
  `rendering.Sink`: storage plus a drain, with sticky failure. `printf`'s
  storage is 1024 bytes of the caller's frame drained to standard output, so
  a call makes one write instead of one per piece (about 5x faster on a
  five-piece line to `/dev/null`); `str.format`'s is the destination, with a
  drain that refuses (release speed unchanged). The shared
  `rendering.piece*` writers replace fmt's `print_*` and str's `put_*`
  helpers; padding streams through `Sink.fill`, so width is not bounded by
  any buffer.
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
- Method calls auto-deref pointer receivers like field access: sema rewrites
  `p.m()` on `p: *T` (any pointer depth) into the explicit `(*p).m()`, so
  permission checks and codegen are unchanged.
- Extended receiver-marked mutating method calls to every expression position,
  so their return values initialize and assign ordinary values while retaining
  the same visible `mut` receiver mark.
- Implemented explicit generic instantiation and exact function specialization.
  Generic bodies are checked per concrete stamp; compiler-backed standard
  generics share the ordinary resolution path.
- Implemented generic function values (`choose[int]`). Sema resolves an index
  expression over a generic function name to the matching specialization or
  template of that arity, stamping it through the call path's memo; a declared
  function-value type selects among same-arity overloads. The emitter loads the
  address of that stamp's mangled symbol.
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
