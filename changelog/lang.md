# Language

- A type parameter before `.` calls its bound type's associated function
  (`T.make()`), resolved per instantiation through the stamping substitution;
  a non-struct binding is an error. Selective-import lowering lets a type
  parameter shadow a same-named selected type in that position too.
- Methods name their receiver explicitly (`Counter.get(self)`,
  `Counter.add(mut self, k)`, contracts too); a `Type.f(...)` without `self`
  is an associated function, so `::` and the leading `mut` are gone and `.`
  reaches both. A type and a value may not share a name in overlapping scopes,
  so a type base before `.` resolves with no value-first fallback; the loader
  enforces it for selective imports, which select exactly one namespace.
- Type-qualified method calls (`Counter.get(c)`) are rejected: methods are
  called only as `x.m(...)`, and an ambiguous promoted method is reached
  through its named embedded field. Lowering drops the receiver-as-argument-zero
  call path.
- `x.f(args)` and `(x.f)(args)` call a function value stored in field `f`
  with no receiver, through auto-deref, chains, and indexing; sema marks the
  call and lowering emits an indirect call on the loaded field. Fields and a
  struct's own or promoted methods share one namespace, so a collision errors.
- Error domain entries are comma-separated with an optional trailing comma,
  like every other brace-delimited list (`E: error { 1: a = "a" }`), replacing
  the newline-terminated entry form.
- Functions, methods, associated functions, generics, specializations, and
  signings join the order-independent top level, replacing forward-call
  fixups and their re-check: check enters each overload set and resolves every
  signature and signing before any body; root names stay unseen from imports.
- Top-level types, aliases, globals, constants, C globals, and imports are
  order-independent within a file, replacing the pointer- and root-slice-only
  forward types: check enters every name first and resolves a declaration on
  first lookup; a constant, alias, or layout query reaching itself is an error.

- A parameter's function-value type selects an overloaded function passed by
  bare name, as a declared local's type does: `run(twice)`,
  `apply[int, int](3, twice)`. The name gives overload selection no type, so
  the sole candidate's parameter chooses it; an overloaded callee is an error.
- Function types state their return, `void` for no value: `(int): void`,
  `extern "c" (): void`; declarations still omit the clause. `void` is also a
  type argument, standing only where a return stands (`apply[int, void]`), and
  a no-value function may `return f()` of a no-value call. `(T)` and `()`
  alone are no longer types; built-in type names can no longer be redeclared;
  function types are type arguments in expressions too (`same[(int): void]`).
- `@cold` marks a function with a body as rarely called; it changes only
  layout. It excludes `inline`, `main`, `extern "c"` definitions, and
  bodiless declarations, and a cold function is never auto-inlined.
- Link names on definitions: `extern "c" f(…): R as "sym" { … }` and
  `extern "c" [mut] g: T as "sym" = value` export under `sym` while Ace keeps
  the Ace name. Two Ace definitions exporting one C symbol are an emit error.
- `extern "c"` defines C globals in Ace: an initialized global in the block,
  or `extern "c" [mut] name: T = value`, is an ordinary constant-initialized
  global under its unmangled C name, external, read-only (in `__DATA_CONST`
  or `.rodata`) unless `mut`.
- One `extern "c"` boundary in both directions: inside a block, a function
  with a body is defined in Ace and C-visible; `extern "c" f(…) { … }` is the
  single form. `export` is no longer a keyword, and link names are spelled
  `as "name"`, freeing `=` for a future exported-global initializer.
- `extern "c"` blocks bind C global variables (C Interop #6): `name: type`,
  read-only unless `mut`, typed at struct-field position, accessed in place;
  Apple arm64 reaches them through `GOT_LOAD` relocations, x64 through
  `GOTPCREL`. `tests/driver/globals.ace` links them from an object and a
  shared library.
- Added the `_` discard to destructuring declarations and assignments: a bare
  `_` drops its value, binds nothing, may repeat, and is never read; `_ = f()`
  drops one result. `_` can no longer name a declaration, so `_: T` binders in
  `std/utf8`, `pkg/http`, and tests became bare `_`.
- A switch case's value list may span lines inside parentheses
  (`(K.a,` … `K.b) {`), per Syntax #7; the parser treats a case's `(` whose
  match holds a top-level comma as the list. Sema now rejects switching on a
  type without `==` (tuple, string, …), which previously failed in lowering.
- Enum-indexed arrays `[E]T` over a dense enum: indexed only by `E` with no
  bounds check (sema marks the index proven), filled by a keyed literal
  `[E.a = x, ...]` that sema checks names every member once; keyed entries
  store at the member's slot in written order. Conversions to an enum must
  give a member: constants at compile time, runtime values by a debug check.
- An untyped constant taking an enum type implicitly (`k: K = 7`, a `K`
  argument) must name a member, as `K(7)` already must; the constant is checked
  whole and its operands fold at the backing integer type.
- A member name (field, bit field, enum member, union variant, error entry,
  contract method, method or associated function) may be a keyword: the parser
  accepts any keyword token where it declares a member or reads one after `.`,
  and the lexer ends a line after such a keyword as after an identifier;
  bindings stay reserved.
- An assignment evaluates its right side before its target (Memory #9): `x = e`
  and `x op= e` lower `e` first, then the target's place, then the old value,
  so a right side that grows the target's slice stores into the new storage; an
  aggregate copied from a place is snapshotted only when the target has a call.
- A root-file type no longer turns an imported generic's same-named type
  parameter (`[T]`) into specialization arguments: bracket interpretation
  (Generics #12) skips root-file types for imported declarations, as lookup
  does (#91); a specialization with no generic, bracketing a declared type,
  hints to rename the type parameter.
