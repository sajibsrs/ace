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
unmarked parameter is a compile error. This is independent from pointer
reachability: a parameter of type `mut *T` may write its pointee without
replacing the pointer value, so that call does not use an argument-level `mut`.
See [Types and declarations](types.md#pointers).

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
them. `mut` does because it is visible at the call site.

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
type must match after selection. Return type alone never creates or selects an
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

## Current limits

- User-defined functions are not variadic. Pass a slice instead.
- Default and named arguments are not implemented.
- C-boundary `extern` and `export` functions cannot overload because their
  external names are unmangled.
- Exceptions and hidden error propagation do not exist; functions return error
  values explicitly.

The examples are covered by `tests/sema/docs_function.ok.ace`.
