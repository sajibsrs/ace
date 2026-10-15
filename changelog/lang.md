# Language

- Added the `0o` octal integer literal beside `0x` and `0b` (C and selfhost
  lexers, `GRAMMAR.md`, the VS Code grammar): `0o755`, with `_` separators and
  an `invalid octal digit` error. A bare leading zero stays decimal. The `sys`
  `S_I*` mode constants are now spelled in octal.

- An expression-position type argument may be a pointer type:
  `mem.alloc[*u8](n)`. The parser reads the bracket's `*T` as a dereference;
  `build_type` (C and selfhost) now checks that unary `*` as the pointer type.
- An enum whose member list ends with `_` is open (#8): it states its backing
  type and holds every backing value, so `E(n)` and constants skip the member
  check, `.name` of a non-member is `""`, and `[E]T` rejects it. The parser
  sets `F_OPEN`; `pkg/sdl`'s `EventType` and `Keycode` are open.
- A string literal may name a code point with `\u{H…}` (1–6 hex digits),
  written as its UTF-8 bytes (#127); surrogates and values above U+10FFFF are
  lex errors, and `\u` is rejected in a char literal. Both lexers encode it
  while scanning the string; selfhost diagnostics spell their em dash so.
- An element of a view-typed call result is a place (#120): `f()[i] = v`,
  `f()[i].f = v`, and `mut &f()[i]` follow the view's own permission. Sema
  types a non-place index base with `expr_type` and keeps "not an assignable
  target" for array values; lowering already handled a value base.
- An array literal may be a `for` iterable (#112): sema coerces it to `[]E`,
  `E` the value binder's type, through the `[]T` literal path, so constants
  materialize as `E`; `in mut` rejects it. `Loader.mark_intrinsics` and
  `pkg/json`'s lookup test iterate literals inline.
- A switch case value may be a half-open range `lo .. hi` over an integer or
  `char` scrutinee (reversed bounds select the same set). Sema requires constant
  bounds, rejects an empty range, and rejects any value or range matched by two
  cases (plain duplicate values included); lowering tests a range as one
  unsigned compare, `v - lo < hi - lo`.
- Runtime global initializers and a module-init hook are rejected for good: a
  global initializer is a compile-time constant (`emit` reports a call or any
  non-foldable expression), and runtime setup is an explicit call from `main`.
- In a real overload set an array literal's candidate is the element type its
  elements share under the scalar rule, matching `[]E` and `[N]E`; without
  one it matches nothing. `overload_arg_type` synthesizes the candidate and
  `exact_arg_type` compares element types.
- Returning a direct view of function storage (a local or plain-parameter
  array slice or address, or a runtime `[]T` literal), or storing one into a
  module variable or `mut` parameter, is an error. Sema walks the returned or
  stored expression through tuples, aggregate literals, and view conversions.
- An array literal where plain `[]T` is expected is an anonymous `[N]T`
  viewed as `[]T`: sema checks it against `[N]T` in `coerce`, constant
  elements become a read-only `__ace$lit$` symbol, and others a stack slot of
  the function. `Parser.fail_at` and `pkg/http` pass parts inline.
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
- A nested array literal selects an overload by its innermost element type at
  any depth (#99): its candidate recurses through inner literals, which agree
  past their lengths, and `arraylit_fits` walks literal and parameter together,
  taking any inner length at a `[]E` level and exactly `N` at `[N]E`.
- A cast gives an array literal its type (#100): `[]int([])` names a typed
  empty view that selects an overload `[]` alone cannot, and `[N]T(value)`
  parses (a one-element literal before `T(`) as an identity array cast,
  which lowering and constant evaluation see through to its argument.
- Character and integer constants mixed in an array literal select a `u8`
  overload (#101): a literal of untyped constants takes the natural type of
  its highest kind (integer < character), and `'a' + 1` stays a character
  constant; range checks still reject `['a', 300]`.
- A bitwise operator rejects an unparenthesized equality or comparison operand
  (#104): both parsers' precedence climbing returns the level of a result's
  outermost operator, and `&`, `^`, `|` fail when either operand's is `==`,
  `!=`, or a comparison chain.
- `.name` on an enum value is its member's source spelling as a static
  `string` (Type System #14). Lowering compares the value against each
  member's discriminant, dense or sparse, at the use site, so only enums that
  use `.name` carry names; `Kind.name` stays a member lookup.
- An identifier is at most 128 characters (Source #4); both lexers report a
  longer one.
- `null` selects an overload whose parameter accepts it, as a plain call
  coerces it (`fmt.Buffer.init(null)`); a `null` two overloads accept is
  ambiguous.
- Code that continues past failures collects them in a caller-owned
  `array.Array[R]` of domain records holding an `err: Error` (Errors #14);
  the self-hosted lexer stores its `Diag`s that way, read through `errors()`.
- Struct fields are private to the declaring file unless `pub`, and a `pub`
  field is visible exactly where its struct is (Modules #3), through the
  loader's `decl_visible`, which a facade's `pub` alias also widens. A field
  outside that set cannot be read, written, or named in a literal; `T {}` and
  `blank(T)` stay open.
- Visibility is per overload (Modules #7): a module member is public when any
  overload is `pub`, and sema drops overloads `decl_visible` hides from the use
  site before resolving a call, function value, or generic instantiation. A
  call fitting only a hidden overload by arity reports it as private.
- A leading `mut` marks the last call or slice of its postfix chain, so
  `mut a.view()[i].x = v` assigns in one statement (Mutability #6); later
  selectors address a place in the result. The parser walks index and field
  nodes to mark it; selfhost write-backs now use `mut *T` accessor overloads.
- A range head may omit its binder (Iterations #7): `for 0 .. n { }` runs the
  body once per selected value, counting at the range's unified bound type.
  The parser turns a leading expression followed by `..` into the range head;
  lowering keeps its hidden cursor and skips the binder copy.
- `defer` is legal in any block and runs when control leaves it (Deferred
  calls #2): the block's end, `return`, `break`, `continue`, a labeled restart,
  or `fallthrough`. Lowering keeps one defer stack with a floor per block and
  per loop/label target; each exit re-emits the calls above its target's floor.
- `cond ? a : b`, loosest of all expressions, unparenthesized nesting an error,
  in both compilers. Sema types the branches alike (an untyped one settles to
  the other, or both meet `coerce`'s expected type); lowering joins only the
  selected branch's leaves with phis, branching directly in a test.
