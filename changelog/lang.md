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
