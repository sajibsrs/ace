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

A call returning multiple values may be returned directly when its result has
exactly the function's return types, in the same order:

```ace
checked_divide(value: int, by: int): (int, int) {
    return divide(value, by)
}
```

The match is exact: forwarding `(int, bool)` into `(i32, bool)`, or into a
different number of values, is rejected. Destructure and return the values
explicitly to convert them.

A bare `_` in a destructuring list drops that value. It takes no type, since
the call's signature already fixes it, binds no name, may appear more than
once, and can never be read. `_ = f()` drops a single result. The call still
runs; only its value is dropped:

```ace
_, remainder: int = divide(13, 5)
quotient: int, _ = divide(13, 5)
_, remainder = divide(20, 6)
```

`_` must drop a typed value: `n: int, _ = 1, 2` is rejected because dropping
a literal does nothing. It takes only plain `=`, and no declaration, parameter,
or type may be named `_`. Bit-field padding (`_: 5` inside `bits`) is a
separate use.

## Parameter-list layout

A parameter list stays on one line when the complete declaration fits within
120 columns. Crossing that limit uses one parameter per line and a trailing
comma after the final parameter:

```ace
pub get(
    url: string,
    mut context: Context,
): (Response, Error) {
    // ...
}
```

The trailing comma is the canonical multiline style. It makes continuation
explicit and lets parameters be added, removed, or reordered without also
editing the neighboring line. A formatter does not expand a fitting single-line
declaration merely because the parameter list could be written vertically.

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

A function with a `mut` parameter cannot become a function value: a
function-value type has no syntax for a parameter's mutability, so a call
through one could never validate `mut` agreement against the real callee.

C-callable function values state their convention in the type:

```ace
export thread_entry(argument: mut *u8): mut *u8 {
    return argument
}

entry: extern "c" (mut *u8): mut *u8 = thread_entry
empty: extern "c" () = null
```

Use `extern "c" (): R` for a zero-parameter function returning `R`, and omit
the return clause for a function returning no value. Only an exact `export`
function or imported `extern "c"` declaration can initialize a non-null value
of this type; an ordinary Ace function has a distinct convention. Parameters
and returns must be C-ABI-compatible. C function values are one word, may be
`null`, and may be compared with `null`. They may also be fields of
C-compatible structs and pointer targets, so C callback tables and
out-parameters bind directly:

```ace
Hook: struct { call: extern "c" (i32): i32, base: i32 }
```

Call a field's function by first copying it to a local. Calling null is diagnosed in debug
builds; release builds require a non-null callee.

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

A generic function becomes a function value once its type arguments are
written. The declared function-value type never supplies them, so a bare
`choose` is an error:

```ace
pick: (int, int): int = choose[int]
```

The value is the stamp for that type tuple, or the written specialization when
one matches. When generic overloads take the same number of type arguments,
the declared function-value type selects one, as it does for ordinary
overloads. The typed `mem` builtins are lowered at their call and cannot be
taken as values.

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
pub popcount(value: u32): uint intrinsic
```

It is not a user-extensible foreign-function or assembly mechanism. The
whitelist consists of the exact public operations declared by `bits`, the
`math` exact-rounding overloads `sqrt`, `floor`, and `ceil` (and a narrow
compiler-reserved standard-library byte load). They are ordinary
first-class functions to callers: the compiler emits callable implementations
for function values and may inline an equivalent instruction sequence at
statically resolved direct calls.

Specializations currently live in the generic's module and apply to free
functions. Struct generics are untouched, and type-owned generic methods do not
specialize yet. The standard library uses the feature directly: both `str`
`parse[T]` overloads are `specialized` templates whose entire callable surface
is per-type specializations (see [str](std/str.md)).

## Inline functions

The `inline` modifier asks for a function's body to be expanded at every
direct call instead of called. It follows `pub` and precedes a receiver `mut`:

```ace
inline mix(value: u64): u64 {
    x: u64 = value ^ (value >> 31)
    return x * 3
}

Buffer: struct { at: usize }

pub inline mut Buffer.skip(n: usize) {
    self.at += n
}

main(): int {
    b: Buffer = Buffer{ at = 0 }
    mut b.skip(2)
    return int(mix(u64(b.at)))
}
```

Expansion is guaranteed, not a hint, and does not change what a call means:
the arguments are evaluated left to right as for any call, a read-only scalar
parameter sees the value it was passed, and a `mut` parameter or receiver
writes to the caller's storage. A `return` in the body finishes the expanded
call with its value. Declaration order and module boundaries do not matter,
and a call inside an inline function's body expands too, so small helpers can
be written as functions without paying for the call. Expanded code keeps the
call site's line in debug information.

The function still has an ordinary symbol, so it can be used as a function
value, and a `defer` of it calls that symbol at exit. Generic templates,
methods, associated functions, and specializations may be `inline`; each
specialization carries its own marker. An inline function may return any type,
structs and tuples included, except a type parameter; it must not take a
`noalias` parameter, and must not contain `defer` or inline assembly. `intrinsic`
declarations, bodiless `specialized` templates, and `main` cannot be `inline`,
and a chain of inline functions that calls back into itself is rejected.
Release builds also expand small leaf functions on their own: a body of at
most 24 syntax nodes that calls nothing but conversions and compiler-provided
operations, within the limits above, and not generic, `main`, or recursive.
Debug builds keep those calls, so backtraces show them. Mark a larger or
non-leaf helper `inline` to expand it.

An expansion binds a receiver or `mut` parameter to the argument itself when
that is a local, or a field of one, so a struct or scalar local passed to
inline methods stays in registers. It lives in memory for the whole function
once anything needs its address: `&`, a `mut` or receiver argument to a call
that is not expanded (a deferred call, a helper too large to auto-inline), or
an inline body that passes its parameter on by address.

## Self tail recursion

A function whose `return` is a call to itself does no work after that call:

```ace
count_down(n: usize, total: usize): usize {
    if n == 0 { return total }
    return count_down(n - 1, total + n)
}
```

Release builds compile such a `return` as a branch back to the function's
parameter reads, reusing its stack frame, so this recursion runs as fast as the
equivalent loop and at any depth. Debug builds keep every call so a backtrace
shows each level; there, deep enough recursion still overflows the stack.

Release builds do this whenever sharing one frame across levels cannot be
observed:

- no `defer` is pending at the `return`;
- the parameters fit the argument registers: at most 8 integer-class ones (a
  scalar, pointer, `mut` parameter, or method receiver takes 1; a slice or
  `string` takes 2) and at most 8 float ones;
- every by-value parameter and every local is a scalar, a slice, or a `string`;
- the function takes no address of its own locals or by-value parameters
  (`&x`, `mut x` at a call, or a method call on one);
- the function is not `main`, generic, or exported, and contains no call to
  an `inline` function.

Otherwise the call stays an ordinary call, with the same result. Only a direct
call to the function itself qualifies, and never one whose result is used
afterwards (`return n * factorial(n - 1)`).

> **This is not general tail-call optimization (TCO)**
>
> Returning another function's call
> is always an ordinary call. That is deliberate: general tail-call optimization
> adds cost without a performance payoff. Jumping into another function means
> tearing down the frame first, and its main use, handlers that tail-call one
> another, still runs several times slower than the same code written as a
> `switch` loop. Every backend would carry the extra machinery, and callers
> would vanish from backtraces throughout ordinary code. A self call needs no
> teardown, so it costs one branch and brings tail recursion to loop speed.

This is an optimization, not a guarantee: recursion that must not overflow, in
any build, is written as a loop.

## Declaration order and forward calls

Names normally follow declaration-before-use. Top-level function calls are the
exception: calls to functions, and method calls on a struct, declared later are
recorded and resolved at the end of the file. This supports forward and mutual
calls without prototypes. A call whose untyped argument needs the complete
overload set, such as `f(5)` against a single `f(a: u64)`, is resolved the same
way. Every function body that made such a call is then checked once more, with
the same declarations in view as before except that the deferred calls see the
whole file, so each use of their results is checked at its real type:
`y: u8 = later()` is an error when `later` returns `u64`.

A local shadows a same-named function or global from its declarator to the end
of its block, its own initializer included. `get: usize = get(a)` is therefore
an error (`'get' is not callable`); name the local differently. A call written
before the local still reaches the function, even when it is resolved at the
end of the file.

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
- A declaration or function type takes at most 64 parameters. A method's
  receiver counts, so a method declares at most 63 more; so does a contract
  method, whose receiver is implicit. The variadic arguments of a C variadic
  call are not counted.
- C-boundary `extern` and `export` functions cannot overload because their
  external names are unmangled.
- Exceptions and hidden error propagation do not exist; functions return error
  values explicitly.

The examples are covered by `tests/sema/docs/function_ok.ace`.

## Design rationale

Why the rules in `spec/compiler.md` are shaped as they are.

- Functions & ABI #8: the marker keeps expansion visible in source and declares
  at the module boundary that callers compile against the body; release builds
  additionally expand only small leaf functions (Compiler #6), so unmarked code
  pays no call for a trivial helper while larger expansion stays the author's
  choice (PHILOSOPHY Considerations #1). Any result type but a type parameter
  expands, because lowering already splits every aggregate into scalar leaves:
  a struct or tuple result joins as several SSA values at no extra cost, and
  value-type helpers (vector math, color constructors) are exactly the small
  functions worth expanding. Aggregate results cut `bench/core/raytrace` from
  0.842 s to 0.602 s (1.4x) with its helpers marked `inline`.
- Errors #12 (`_`): multiple returns make dropping an unwanted value common
  (`value, ok`, `value, err`). A dedicated discard binds nothing, so repeated
  uses never collide and nothing unread lingers in scope. The bare, untyped
  spelling follows Go's blank identifier: the callee's signature already fixes
  the type, so an annotation checks nothing.
