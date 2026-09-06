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
    if len(path) == 0 {
        return FileError(2)
    }
    return null
}
```

`FileError()` uses code zero. `FileError(code)` accepts exactly one `i32`.
Both expressions have type `Error`; signatures return the universal type, not
`FileError`.

The two fields are read-only:

```ace
if error != null {
    if error.domain == typeid(FileError) && error.code == 2 {
        /* handle missing file */
    }
}
```

There is no message field, descriptor, registry, metadata table, allocation,
or runtime registration.

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
not ordering, arithmetic, integer conversion, reflection, or runtime names.

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
