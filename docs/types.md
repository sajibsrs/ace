# Types and declarations

Ace declarations use `name: type` everywhere: globals, locals, fields,
parameters, and destructuring bindings. Type inference is not implemented.

```ace
limit: const int = 1024

main(): int {
    count: int = 13
    count = 42
    return count
}
```

Locals are mutable by default. `const` is an explicit type modifier and requires
an initializer. Assignment copies the value. A bare local declaration reserves
uninitialized stack storage; it does not emit a zero fill. The compiler rejects
every direct read until the local is definitely initialized.

Multiple typed bindings can be declared and initialized together. Every binding
states its own type, and values are matched by position, so the types do not need
to match each other:

```ace
number: int, message: string = 1, "old"
```

Multi-assignment evaluates and preserves every value on the right before it
writes any target:

```ace
number, message = 2, "new"
```

Both forms require equal numbers of bindings or targets and values. Newly
declared bindings remain uninitialized while their initializer is evaluated.

This also provides a direct swap without a user-declared temporary:

```ace
a, b = b, a
```

## Aliases and distinct types

A type alias is transparent; both names denote the same type.

```ace
Count: type = int

total: Count = 13
```

A distinct type has the same machine layout as its base but a separate nominal
identity. Crossing the boundary requires an explicit constructor-shaped cast.

```ace
Meters: distinct f64

distance: Meters = Meters(12.5)
raw: f64 = f64(distance)
```

## Pointers

`*T` is a read-only pointer and may be null. `mut *T` grants writes through that
pointer level. Address-of requires an lvalue: `&value` produces `*T`, while
`mut &value` produces `mut *T` and is rejected for const or otherwise
read-only storage. Both forms emit the same machine address; `mut` is a
compile-time permission. `~pointer` explicitly dereferences, while field
access automatically dereferences pointers.

```ace
Point: struct { x: int, y: int }

read_x(point: *Point): int {
    return point.x
}

main(): int {
    point: Point = Point{ x: 13, y: 29 }
    pointer: *Point = &point
    return read_x(pointer)
}
```

Parameter mutation and pointee mutation are independent. A `mut` parameter
may replace the value stored in the caller; a `mut *T` may write its pointee:

```ace
set_x(point: mut *Point, value: int) {
    point.x = value       // pointee writable; parameter itself read-only
}

retarget(mut point: *Point, replacement: *Point) {
    point = replacement   // caller's pointer writable; pointee read-only
}

main(): int {
    first: Point = Point{ x: 13, y: 29 }
    second: Point = Point{ x: 42, y: 50 }

    writable: mut *Point = mut &first
    set_x(writable, 21)

    current: *Point = &first
    retarget(mut current, &second)
    return current.x
}
```

`mut point: mut *Point` combines both permissions. Multiple pointer levels
retain them independently: `mut **T` may replace the stored inner pointer,
`*mut *T` may read that inner pointer and write `T`, and `mut *mut *T` permits
both. Only the level performing a write needs `mut`.

Pointer arithmetic is not implemented. Use arrays or slices for bounded
sequences.

Every inline value layout must be finite. A pointer breaks a layout cycle, so
self-pointers and mutually pointer-linked structs are valid:

```ace
Node: struct {
    next: *Node,
}

Left: struct {
    right: *Right,
}

Right: struct {
    left: *Left,
}
```

The later name in a same-module field is permitted only when every type edge
from the field is a pointer (`*Later`, `**Later`); the same rule applies to a
tagged-union payload type. The declaration must appear later in the file.
Arrays, tuples, aliases, and distinct wrappers do not extend the pointer-only
path, so `[1]*Later` and `*[1]Later` still require `Later` to be declared first.
Before the declaration, member access, dereference, `sizeof`, and other
operations requiring the exact layout are rejected. An unresolved pointer-only
name is diagnosed at the end of the file.

Direct or indirect inline recursion is rejected instead of receiving a
placeholder size:

```ace
Invalid: struct {
    nested: Invalid // recursive value layout
}
```

## Zero and absence

Every type has an all-bits-zero representation. Pointer, slice, string,
function, contract, `Error`, and `typeid` values spell that representation
`null`; no optional type constructor or hidden presence tag exists.

```ace
maybe: *Point = null
```

Zero-capable values may be compared directly with `null`. Dereferencing a zero
pointer, calling a zero function, dispatching through a zero contract, or
inspecting a zero `Error` is invalid. When zero is ordinary application data,
represent presence explicitly with a struct containing a `present: bool` field.
The current bootstrap has no checked/release mode yet and inserts no runtime
null checks; invalid zero operations therefore follow the unchecked path.

## Definite initialization

A local without `= value` is uninitialized storage, not an implicitly zeroed
value. Whole assignment initializes it. Struct fields and constant array
indexes can be initialized separately; using the whole aggregate requires all
of its parts to be initialized.

```ace
Point: struct { x: int, y: int }

main(): int {
    point: Point
    point.x = 40
    point.y = 2
    return point.x + point.y
}
```

After a branch, an initialization fact survives only if every continuing path
establishes it. A loop alone cannot establish initialization after the loop,
because it may execute zero times. Taking an address does not read the value;
pointer aliases, inline assembly, and extern code are programmer-trusted
boundaries with no hidden alias analysis.

## Arrays and slices

`[N]T` is an inline array containing exactly `N` elements. `[]T` is a two-word
view containing a pointer and length.

```ace
values: [4]int = [10, 20, 30, 40]
middle: []int = values[1..3]
answer: int = middle[0] + middle[1]
```

Intrinsic `.len` is available on arrays (constant element count), slices
(stored element count), and strings (stored byte count). It is not added to any
other type. A struct can still declare an ordinary field named `len`.

Array assignment copies all elements. Slice assignment copies only its pointer
and length, so both values view the same backing storage; it never copies the
elements. Slicing does not allocate. See [Memory](memory.md) for allocation and
lifetime rules. The current compiler does not insert runtime index or
slice-bound checks; out-of-range access is unchecked.

## Multiple-return and function types

Multiple-return signatures use positional tuple layout. Function types use
parameter types followed by their return type.

```ace
operation: (int, int): int = add
```

Multiple-return values use the platform aggregate ABI and are consumed through
typed destructuring. Context-typing a standalone tuple literal into a tuple
variable is not implemented by the current checker.

## Structs, enums, and tagged unions

Struct fields retain declaration order and natural alignment. Struct values have
copy semantics.

```ace
Point: struct { x: i32, y: i32 }

point: Point = Point{ x: 13, y: 42 }
```

An enum is a nominal type with distinct-typed named constants and `i32` layout.

```ace
State: enum { idle, running, stopped }

state: State = State.running
```

A tagged union is a closed set of named alternatives stored inline. Variants
may carry no data or one explicitly typed payload:

```ace
Event: union {
    none,
    quit,
    key: u32,
    cursor: Point,
}
```

Variant names are chosen by the programmer. `none` and `quit` are not
keywords. Construct a payloadless variant as a qualified value and a payload
variant with one argument:

```ace
event: Event = Event.quit
event = Event.key(42)
event = Event.cursor({ x: 10, y: 20 })
```

Inspect a union with `switch`. A payload binder is an explicitly typed local
copy, so changing it does not change the union:

```ace
switch event {
    Event.none {}
    Event.quit {}
    Event.key(code: u32) {
        code += 1
    }
    Event.cursor(point: Point) {
        point.x = 0
    }
}
```

Every variant must appear once unless the switch contains `else`. Payloads
cannot be accessed outside their matching arm. Replacing a payload or variant
uses whole-value assignment.

The representation is an `i32` tag followed by aligned storage for the largest
payload. Declaration order assigns tags from zero, but numeric tags are not
exposed. The first variant is the canonical all-bits-zero value, including for
a global without an initializer. There is no allocation, boxing, reflection,
`any`, or niche optimization.

Payloadless variants are complete values and can be compared directly when all
of the union's payload types support equality:

```ace
Status: union { none, quit, key: u32 }
status: Status = Status.quit

if status == Status.quit {
    return
}
```

Whole-union `==` and `!=` compare the tag and then the active payload, never
inactive storage or padding. Generic unions use explicit instantiation just
like generic structs:

```ace
Result[T]: union { empty, value: T }
result: Result[int] = Result[int].value(42)
```

Tagged unions are not currently permitted in `extern` or `export` signatures.

## Generic types

Generic structs name their type parameters in brackets. Instantiation always
states the arguments explicitly.

```ace
Box[T]: struct { value: T }

box: Box[int]
box.value = 13
```

Every used type tuple has a concrete layout. Ace performs no constraint or trait
solving.

## Contracts

A contract value is two words: a const vtable pointer and a data pointer. A
struct has one canonical signing listing every contract it fulfills, and
construction explicitly names the target contract. There is no inheritance,
runtime lookup, or trait solving.

An Ace contract is a required method set. It does not introduce preconditions,
postconditions, assertions, or hidden runtime checking.

```ace
Value: contract {
    get(): int
}

Number: struct { value: int }

Number signs Value

Number.get(): int {
    return self.value
}

main(): int {
    number: Number = Number{ value: 13 }
    value: Value = Value(&number)
    return value.get()
}
```

Methods are owned by their struct. `self` is read-only by default; a mutating
method is declared and called with visible `mut`:

```ace
mut Number.set(value: int) {
    self.value = value
}

mut number.set(21)
```

One signing may list multiple contracts on one line:

```ace
Number signs Value, Printable
```

Bare comma-separated lists cannot cross a newline. Use parentheses when the
contract list needs multiple lines:

```ace
Number signs (
    Value,
    Printable,
)
```

Single-line contract lists omit the parentheses, giving each layout one
canonical spelling.
A second `Number signs` declaration is an error. Contract calls dispatch through
the generated const vtable; concrete method overloads remain compile-time direct
calls. `Type.method(...)` is available for a disambiguated direct call, but a
bare `Type.method` is not a function value.

Signings and methods are private by default. Put visibility first:
`pub Number signs Value` and `pub mut Number.set(value: int)`. A contract
over const backing storage may be read, but mutating dispatch through it is a
compile error.

## Current limits

- Type inference and implicit generic instantiation are not implemented.
- Packed structs, explicit alignment attributes, and volatile access are not
  implemented.
- Pointer arithmetic is not implemented.
- Generic aliases are not implemented by the current compiler; generic structs
  are supported.
- Generic struct literals are not implemented; declare the explicitly
  instantiated type and initialize its fields.
- Standalone tuple-literal initialization is not implemented; tuple layout is
  currently used by multiple returns and destructuring.
- Checked-build bounds and allocator diagnostics are specified but not yet
  implemented in the runtime layer.

The examples are covered by `tests/sema/docs_type.ok.ace`.
