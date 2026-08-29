# Errors

Ace errors are ordinary values checked with explicit branches. There are no
exceptions, no propagation operator, and no hidden control flow: a failing
function returns an error as its last return, and the caller checks it with
`if error != null`.

```ace
read(path: string): ([]u8, Error?)
```

`null` means success. `Error?` uses the handle's zero value as its null niche,
so a success return costs no extra bits.

## Error and ErrorDescriptor

`Error` and `ErrorDescriptor` are predeclared types, not keywords.

`Error` is an opaque, one-word handle to immutable static data. It is never
null on its own; only `Error?` can hold `null`.

`ErrorDescriptor` has exactly three fields:

- `code: i32` — a compile-time integer code, unique within the domain.
- `domain: typeid?` — `null` or the identity of a named type.
- `message: string` — a static string.

The descriptor is 32 bytes on the current 64-bit target. Only the one-word
handle travels through calls, returns, and stored values.

## Declaring an error

An error descriptor is declared as a `const` global and constructed with a
named struct literal. `Error(&descriptor)` builds the handle; the operand must
be the address of a `const` global `ErrorDescriptor`.

```ace
FileError: distinct i32

not_found_code: const FileError = FileError(2)

not_found_descriptor: const ErrorDescriptor = ErrorDescriptor{
    code: i32(not_found_code),
    domain: typeid(FileError),
    message: "file not found",
}

pub not_found: const Error = Error(&not_found_descriptor)
```

Construction rules enforced by the compiler:

- The `code` must be a compile-time `i32` value.
- The `domain` must be `null` or `typeid(NamedType)`.
- The `message` must have static backing: a string literal or a `const` global
  string with a compile-time-static initializer.
- Descriptors declared as locals or mutable globals, and arbitrary
  pointer-to-`Error` casts, are rejected in every build mode.

Because every handle points at const static data, a returned `Error` is valid
for the whole program without allocation, escape analysis, or runtime checks.

## typeid

`typeid(T)` is a compile-time builtin producing a nonzero, pointer-sized
identity for an exact named type. `typeid` is a builtin function, not a
keyword; the `typeid` type is a predeclared identifier.

- The same exact type compares equal throughout one linked program.
- Different nominal types compare unequal even with identical layouts.
- Transparent aliases share their target's identity; distinct types have
  separate identities; exact generic instantiations have separate identities.
- `typeid?` uses zero as its free null niche.
- Supported operations are storage, passing, `==`, and `!=`. There is no
  ordering, arithmetic, integer conversion, reflection, or runtime type name.
- The numeric value is not stable across builds or processes.

Domain code types conventionally use `DomainError: distinct i32`. The distinct
type separates codes at compile time while `typeid(DomainError)` identifies
the domain at runtime; the descriptor stores the explicitly converted `i32`.

## Checking and matching

`Error == Error` compares descriptor addresses. Two separately declared
descriptors are different errors even when every field matches. Aliases of the
same handle compare equal.

```ace
load(path: string): int {
    data: []u8, error: Error? = fs_read(path)
    if error != null {
        if error == not_found {
            return 1
        }
        if error.domain == typeid(FileError) && error.code == 2 {
            return 2
        }
        return 3
    }
    return int(data.len)
}
```

After an explicit `error != null` check, the handle narrows to `Error` and its
descriptor fields can be read directly. Fields are read-only through an
`Error`; writing `error.code` is a compile error.

Codes are meaningful only within a non-null domain. Errors declared with
`domain: null` have no globally coordinated code namespace.

## Module boundaries

An `Error` handle is exported like any other `pub` const global. The
descriptor and its message may stay private; the exported handle keeps the
referenced descriptor alive for the entire program.

```ace
# errors.ace
pub FileError: distinct i32

not_found_descriptor: const ErrorDescriptor = ErrorDescriptor{
    code: i32(FileError(2)),
    domain: typeid(FileError),
    message: "module error",
}

pub not_found: const Error = Error(&not_found_descriptor)
```

```ace
# main.ace
import errors: "./errors.ace"

fail(): Error? {
    return errors.not_found
}
```

`Error`, `ErrorDescriptor`, and `typeid` are Ace-internal shapes: `extern` and
`export` signatures using them are rejected. C-facing wrappers translate
explicitly to stable `i32` codes or another declared C-compatible shape, and
imported C failures are mapped to static Ace errors.

## Printing and context

Inspecting an error is allocation-free. The static message is already a
string:

```ace
if error != null {
    fmt.print(error.message)
    fmt.print("\n")
}
```

There is no `fmt.print(Error)` overload; printing a handle directly would hide
whether domain and code should also appear. Print them explicitly when wanted.
Occurrence-specific context, such as a failure offset, is returned separately
alongside the error rather than stored inside it.

## Current limits

- Error messages are static strings; owned or formatted messages, runtime
  context fields, wrapped errors, causes, chains, and stacks are not
  implemented.
- There is no automatic propagation; every check is written by the programmer.
- Domain identity values must never be serialized or exposed as stable
  identifiers outside one linked program.
- `panic` is not implemented; an unrecoverable-invariant story remains a
  separate queued decision.

The examples are covered by `tests/sema/docs_errors.ok.ace` and the runnable
fixtures in `tests/emit/errors.ace` and `tests/modules/error_export/`.
