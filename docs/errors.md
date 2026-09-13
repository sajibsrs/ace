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
    0: unknown = "unknown file failure"
    2: not_found = "file not found"
}
```

Entries are line-oriented and take no commas. Codes must be exact `i32`
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

`Error`, error domains, and `typeid` are rejected in `extern` and `export`
signatures. C-facing wrappers translate explicitly to stable C-compatible
values. The standard `sys`, `net`, and `poll` wrappers capture errno immediately
after a failing OS call and place it in `error.code`; operations with direct
non-errno status codes preserve those codes instead.

There is no `fmt.print(Error)` overload. Programs choose contextual text
explicitly and may print the code separately. Occurrence-specific context such
as an offset or path is returned alongside the error rather than hidden inside
it.

## Panic

`panic(message: string)` is the separate unrecoverable bug channel. It writes
the supplied bytes best-effort to standard error and aborts with `SIGABRT`. It
is not catchable, convertible to `Error`, or a first-class value.

```ace
if error != null {
    panic("file read failed")
}
```

`assert(condition)` is the checked-build invariant primitive. It accepts one
`bool`, evaluates it once, and reports its source location and aborts when it is
false. Release output removes the assertion and does not evaluate its
condition, so program logic must not depend on assertion side effects. It is
useful for simple checked-mode tests; use `panic(message)` when an unconditional
abort needs contextual text. `sys.exit(code)` is different again: deliberate
immediate process termination with a caller-selected status.

The examples are checked by `tests/sema/docs/errors_ok.ace` and the runnable
fixtures in `tests/emit/errors/` and `tests/modules/error_export/`.
