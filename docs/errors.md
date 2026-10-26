# Errors

Ace errors are ordinary values checked with explicit branches. There are no
exceptions or propagation operators. A failing function conventionally returns
an `Error` last, and `null` means success.

```ace
read(path: string): ([]u8, Error)
```

## Representation and domains

`Error` is an opaque 64-bit value. Zero is `null`. Its upper 32 bits identify a
nominal error domain and its lower 32 bits preserve the raw bits of an `i32`
occurrence code. Equality compares the complete packed value.

Declare a domain at top level and construct errors by calling it:

```ace
pub FileError: error

not_found(path: string): Error {
    if path.len == 0 {
        return FileError(2)
    }
    return null
}
```

`FileError()` uses code zero. `FileError(code)` accepts exactly one `i32`.
Both expressions have type `Error`; signatures return the universal type, not
`FileError`.

A domain may declare named codes and static descriptions:

```ace
pub FileError: error {
    0: unknown = "unknown file failure",
    2: not_found = "file not found",
}
```

Entries are comma-separated like `enum` members; a trailing comma is optional
and written whenever the domain spans several lines. Codes must be exact `i32`
compile-time integers; zero, negative values, and both `i32` limits are valid.
Names and codes are unique within one domain, and messages are string literals.
The domain's visibility also controls its entries.

`FileError.not_found` is an `Error`, exactly equal to `FileError(2)`. Equality
still includes the domain, so an entry in another domain with code 2 is not
equal. Constructors remain available for undeclared runtime codes.

The four fields are read-only:

```ace
if error != null {
    if error.domain == typeid(FileError) && error.code == 2 {
        /* handle missing file */
    }
}

domain_name: string = error.name
description: string = error.message
```

`name` is the originating declaration's unqualified spelling, even through an
alias or re-export. `message` requires an exact domain/code match. Null errors
have empty names and messages; an unlisted code has an empty message. Entry
labels are not fields on arbitrary values, so `error.not_found` is invalid.

Descriptions use static read-only strings and allocate nothing. The compiler
emits their lookup data and code only when `.name` or `.message` is used. There
is no runtime registry or initialization and no metadata inside the 64-bit
`Error` value.

## Identity

`typeid(T)` produces a nonzero compiler-assigned 32-bit identity represented in
the pointer-sized `typeid` type. An error's domain bits compare directly with
`typeid(FileError)`.

- One declaration has one identity throughout a linked program.
- Transparent aliases and re-exports preserve the originating identity.
- Independent declarations differ, even when they have the same spelling.
- Distinct types and exact generic instantiations retain separate identities.
- Zero `typeid` is `null`.

IDs are collision-free only within one linked program. Their numeric values are
unstable across builds and processes and must not be serialized or used as
external identifiers. `typeid` supports storage, passing, `==`, and `!=`, but
not ordering, arithmetic, integer conversion, reflection, or general runtime
type names. `Error.name` is the narrow error-domain description described
above.

Resolved module paths are canonicalized before loading and caching. Importing
the same file through `./errors.ace` and `./dir/../errors.ace` therefore reaches
one declaration and one domain identity.

## Boundaries and context

`Error`, error domains, and `typeid` are rejected in `extern "c"`
signatures. C-facing wrappers translate explicitly to stable C-compatible
values. The standard `sys`, `net`, and `poll` wrappers capture errno immediately
after a failing OS call and place it in `error.code`; operations with direct
non-errno status codes preserve those codes instead.

There is no `fmt.print(Error)` overload. Programs choose contextual text
explicitly and may print the code separately. Occurrence-specific context such
as an offset or path is returned alongside the error rather than hidden inside
it.

When the context belongs to a long-lived object, keep it there and expose it
through a query, as Zig's `std.json` does with its `Diagnostics`. A lexer that
returns `(Token, Error)` records where the error happened and offers it
separately, so the error stays one word and callers that ignore the position
pay nothing:

```ace
tok: Token, err: Error = lex.next()
if err != null {
    line: u32, col: u32 = lex.where()
    fmt.printf("%d:%d: %s\n", int(line), int(col), err.message)
}
```

## Panic

`panic(message: string)` is the separate unrecoverable bug channel. It writes
the supplied bytes best-effort to standard error and aborts with `SIGABRT`. It
is not catchable, convertible to `Error`, or a first-class value.

```ace
if error != null {
    panic("file read failed")
}
```

`assert(condition)` is the debug-build invariant primitive. It accepts one
`bool`, evaluates it once, and reports its source location and aborts when it is
false. Release output removes the assertion and does not evaluate its
condition, so program logic must not depend on assertion side effects. It is
useful for simple debug-mode tests; use `panic(message)` when an unconditional
abort needs contextual text. `sys.exit(code)` is different again: deliberate
immediate process termination with a caller-selected status.

## Source location

`@file` and `@line` name where they are written: `@file` is a `string` constant
and `@line` an untyped integer constant, typed by context like an integer
literal. There are no macros and no implicit caller location, so a helper that
reports its caller's position takes the values as ordinary arguments:

```ace
check(error: Error, file: string, line: usize) {
    if error != null {
        fmt.print(file)
        fmt.print(":")
        fmt.print(line)
        fmt.print("\n")
        panic(error.message)
    }
}

check(error, @file, @line)
```

Paths are build-relative, never absolute: standard-library files appear as
`std/<path>`, and everything else is relative to the directory holding the
entry file, or to the root package's directory under `ace pkg`, with `..` when
outside it. `assert` diagnostics use the same `file:line:col` paths.

The examples are checked by `tests/sema/docs/errors_ok.ace` and the runnable
fixtures in `tests/emit/errors/` and `tests/modules/error_export/`.

## Design rationale

Why the rules in `spec/safety-errors.md` are shaped as they are.

- Safety & Debug Builds #10: the compiler never guesses a mode from its own
  build, an output name, or the surrounding environment; the selected artifact
  semantics remain visible and reproducible. Release-by-default gives an
  unmarked build the priority stack's highest requirement, performance, with no
  dynamic safety cost; debug is requested explicitly where its checks earn their
  cost, as every test runner does. Compile-time proofs remain mode-independent
  because rejecting a known-invalid program costs nothing at runtime and avoids
  giving one source two static meanings. The exact arithmetic set checks
  operations that claim to produce a numeric value while keeping casts and bit
  manipulation machine-direct. Call-boundary range comparisons, direct null
  guards, and allocator bookkeeping are bounded local work rather than
  whole-program safety analysis. Assertions add one minimal development
  primitive and reuse abort semantics without weakening the visible distinction
  among recoverable `Error` values, broken-invariant `panic`, and caller-chosen
  `sys.exit` status.
- Errors #14: static descriptions improve diagnostics at no per-value or
  unused-program cost while preserving the packed ABI, explicit errors, and
  machine-visible allocation model.
- Errors #16: panic, log, and error-report text gains location without macros or
  hidden parameters, and build-relative paths keep binaries identical across
  machines without exposing the builder's directory layout.
