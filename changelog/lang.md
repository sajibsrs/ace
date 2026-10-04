# Language

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
