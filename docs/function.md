# Functions

Ace functions are declared without a `func` keyword. Parameters follow the
language-wide `name: type` order, and the return type follows the parameter
list.

```ace
add(a: int, b: int): int {
    return a + b
}

log_ready() {
    // no return value
}
```

Semicolons are not used. A newline terminates a complete statement.

## Parameters and mutation

Parameter bindings are read-only by default. Ace gives them reference
semantics, while allowing the compiler to pass small read-only values in
registers when that is observably equivalent.

A function that changes an argument marks the parameter `mut`; its caller must
repeat `mut`, and the argument must be an lvalue.

```ace
increment(mut value: int) {
    value += 1
}

main(): int {
    count: int = 12
    increment(mut count)
    return count
}
```

An unmarked call cannot replace the caller's argument value. Assigning to an
unmarked parameter is a compile error. This is independent from content
writes: a parameter whose type carries type-position `mut` — such as
`mut *T`, `mut []T`, or `mut string` — writes through the view with no
argument-level `mut`, because the permission is stated in the parameter's
type. See [Types and declarations](types.md#writable-views).

## Returns and multiple values

A single return type follows one colon. Multiple return types are positional and
parenthesized.

```ace
divide(value: int, by: int): (int, int) {
    return (
        value / by,
        value % by,
    )
}

main(): int {
    quotient: int, remainder: int = divide(13, 5)
    return quotient + remainder
}
```

Multiple returns use the target platform's ordinary aggregate-return calling
convention. Bindings at the call site remain explicitly typed. A bare list such
as `return quotient, remainder` or a typed destructuring list must stay on one
physical line. Parentheses provide the multiline return form shown above;
delimited parameter and argument lists follow the same general layout rule.

## Overloads

Free functions may share a name when their parameter signatures differ.
Selection uses the exact argument count, exact types, and `mut` positions.

```ace
render(value: string): int {
    return int(value.len)
}

render(value: int): int {
    return value
}

main(): int {
    return render("ace") + render(39)
}
```

Overload resolution performs no implicit conversions or ranking. In an overload
call, an uncast integer literal selects `int` and an uncast floating literal
selects `f64`. Use an explicit cast to select another numeric type.

Return types do not distinguish overloads. `noalias` also does not distinguish
them. `mut` does because it is visible at the call site, and type-position
`mut` forms distinguish themselves as distinct types.

Transparent aliases do not distinguish overloads. In particular, `char`/`u8`
and `string`/`[]char`/`[]u8` variations are duplicate declarations.

A `mut`-qualified view argument (`mut *T`, `mut []T`, `mut string`) resolves
exactly when a candidate states that form. When no exact candidate fits, the
call may still resolve against a plain-form candidate through implicit
permission shrink. Exact matches always win first, so `f(s: string)` and
`f(s: mut string)` are distinct, unambiguous overloads.

On the current AArch64 target, `int` and `i64` are the same type, so these cannot
be separate overloads.

```ace
// Duplicate signatures on AArch64:
format(value: int): int {
    return value
}

format(value: i64): int {
    return value
}
```

Overloads are resolved during compilation. The emitted call targets a
signature-mangled function directly; overloads add no runtime dispatch, tag,
boxing, or allocation.

Type-owned methods use the same exact overload rule. Receiver type and receiver
`mut` participate in selection, parameters must match exactly, and the return
type must match after selection, with the same shrink-only fallback for
view permission. Return type alone never creates or selects an
overload. Methods must be called; they are not first-class function values.

## Function values

Function values use signature types. The explicit target type selects one member
of an overload set.

```ace
twice(value: int): int {
    return value * 2
}

twice(value: f64): f64 {
    return value * 2.0
}

main(): int {
    operation: (int): int = twice
    return operation(21)
}
```

A bare overloaded function name without an explicit function-value type is an
error.

## Generic functions

Generic parameters use brackets. Every call supplies its type arguments; Ace
does not infer them.

```ace
choose[T](first: T, second: T): T {
    return second
}

main(): int {
    return choose[int](13, 42)
}
```

Generic functions may be overloaded. The written type arguments are substituted
first, followed by the same exact argument matching used for ordinary overloads.
Each used type tuple emits a specialized function body.

## Generic specialization

A generic function may be followed by same-name specializations whose brackets
name concrete types. A specialization is an ordinary, independently checked
function — not a template instance — and a call whose written type-argument
tuple exactly equals a specialization's tuple resolves to it ahead of the
template stamp.

```ace
pick[T](a: T, b: T): T {
    return a
}

pick[int](a: int, b: int): int {
    return b
}

main(): int {
    if pick[int](1, 2) != 2 {
        return 1 // the specialization body runs for [int]
    }
    if pick[string]("a", "b")[0] != 'a' {
        return 2 // every other tuple stamps the template
    }
    return 0
}
```

Specialization brackets hold bare named types only, supplied positionally — all
of them, with no partial specialization. A specialization must appear after its
generic: brackets naming existing types with no preceding generic of that name
are a compile error, never a silent new generic. Each generic overload of a
name specializes independently.

The declaration is checked against the generic at declaration time: the
specialization's parameters, `mut` positions, and return type must equal the
substituted generic signature. Duplicate specializations for one exact tuple on
one generic overload are a declaration error.

The word `specialized` after the return clause marks a template with no default
body. Calling it with an unmatched type tuple is a call-site error naming the
missing type and the declaration, and a `specialized` generic with zero
specializations in the compiled program is a declaration error.

```ace
count_of[T](v: T): int specialized

count_of[int](v: int): int {
    return 8
}

count_of[f64](v: f64): int {
    return 4
}
```

## Compiler-provided intrinsic declarations

The trailing `intrinsic` marker replaces a function body only for an exact
compiler-provided standard-library operation:

```ace
pub from_float(value: f64): u64 intrinsic
```

It is not a user-extensible foreign-function or assembly mechanism. The
whitelist consists of the exact public operations declared by `bits` (and a
narrow compiler-reserved standard-library byte load). They are ordinary
first-class functions to callers: the compiler emits callable implementations
for function values and may inline an equivalent instruction sequence at
statically resolved direct calls.

Specializations currently live in the generic's module and apply to free
functions. Struct generics are untouched, and type-owned generic methods do not
specialize yet. The standard library uses the feature directly: both `str`
`parse[T]` overloads are `specialized` templates whose entire callable surface
is per-type specializations (see [str](standard-library.md#str)).

## Declaration order and forward calls

Names normally follow declaration-before-use. Top-level function calls are the
exception: calls to functions declared later are recorded and resolved at the
end of the file. This supports forward and mutual calls without prototypes.

## Entry point

An executable defines exactly this source-level entry:

```ace
main(): int {
    return 0
}
```

`main` cannot be generic or overloaded and takes no parameters. On the current
AArch64 target, its `int` result travels in `x0`; the process observes the low
32 bits as its exit status.

## C-compatible functions

`extern "c"` declarations and `export` definitions use the platform C ABI. The
spelling identifies a calling convention; it does not make C part of Ace's
runtime foundation. Ace implements its own language and standard-library
behavior. Target layers reach OS facilities through the OS's stable public
interface, using an OS-provided system library or framework when direct syscalls
are unavailable or unstable. On macOS, that stable entrance is commonly
libSystem. Applications may still bind external C libraries explicitly when
they choose. The bootstrap compiler is an ordinary C11 program and may use C
freely; that source-language choice is separate from generated Ace programs.
Their parameter and return types are restricted to representations the compiler
has verified: fixed-width integers, `int`, `uint`, `usize`, `f32`, `f64`,
compatible pointers and distinct types, and structs composed recursively from
those types. Arrays are allowed as struct fields or behind pointers, but not as
direct parameters or returns. Packed and explicitly aligned structs are
initially C-compatible only behind pointers; their direct parameter and return
classification remains rejected pending target ABI verification.

```ace
Point: struct { x: f32, y: f32 }

extern "c" {
    translate(point: Point, dx: f32, dy: f32): Point
}

export increment(value: mut *i32): i32 {
    *value += 1
    return *value
}
```

Boundary parameters cannot use Ace's `mut parameter` convention. Pass an
explicit `mut *T` when C receives writable storage. An explicitly backed enum
crosses as its fixed-width backing integer; a default enum states no external
width and remains rejected. `bool`, strings, slices, contracts, tagged unions,
errors, tuples, function values, and generic shapes are also rejected at the
boundary. Wrappers must translate those values to an allowed representation
explicitly.

C variadic imports use `...` after at least one fixed parameter. Unnamed
arguments currently accept the verified C-compatible scalar and pointer set;
aggregate and Ace-specific values are rejected. Untyped numeric literals take
their `int`/`f64` defaults and `f32` is promoted to `f64`. On Apple arm64 the
unnamed values use the platform's 8-byte caller-stack slots rather than the
remaining argument registers.

```ace
extern "c" {
    c_variadic(tag: i32, ...): i32
}

value: i32 = 11
result: i32 = c_variadic(0, i32(9), f32(1.25), &value)
```

## Current limits

- Ace-defined functions are not variadic. Pass a slice instead; only imported
  C functions may declare `...`.
- Default and named arguments are not implemented.
- C-boundary `extern` and `export` functions cannot overload because their
  external names are unmangled.
- Exceptions and hidden error propagation do not exist; functions return error
  values explicitly.

The examples are covered by `tests/sema/docs/function_ok.ace`.
