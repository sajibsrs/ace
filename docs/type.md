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
an initializer. Assignment copies the value.

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

`*T` is a read-only, non-null pointer. `mut *T` grants writes through the pointer.
Address-of `&value` requires an lvalue, and `~pointer` explicitly dereferences.
Field access automatically dereferences pointers.

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

Pointer arithmetic is not implemented. Use arrays or slices for bounded
sequences.

## Optional types

`T?` adds `null` to a type. Plain `T` is never null. Pointer, slice, string, and
interface optionals use an existing null niche; other values carry a tag.

```ace
maybe: *Point? = null
```

After an explicit `value != null` check, the compiler narrows the value inside
the taken branch.

## Arrays and slices

`[N]T` is an inline array containing exactly `N` elements. `[]T` is a two-word
view containing a pointer and length.

```ace
values: [4]int = [10, 20, 30, 40]
middle: []int = values[1..3]
answer: int = middle[0] + middle[1]
```

Array assignment copies all elements. Slicing does not allocate. The current
compiler does not insert runtime index or slice-bound checks; out-of-range access
is unchecked.

## Multiple-return and function types

Multiple-return signatures use positional tuple layout. Function types use
parameter types followed by their return type.

```ace
operation: (int, int): int = add
```

Multiple-return values use the platform aggregate ABI and are consumed through
typed destructuring. Context-typing a standalone tuple literal into a tuple
variable is not implemented by the current checker.

## Structs and enums

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

Tagged unions are not implemented.

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

## Interfaces

An interface value is two words: a const vtable pointer and a data pointer. An
`impl` explicitly maps interface methods to functions, and construction names
that implementation. There is no inheritance or automatic implementation
selection.

```ace
Value: interface {
    get(): int
}

Number: struct { value: int }

number_get(number: *Number): int {
    return number.value
}

number_value: impl(Number, Value) {
    get: number_get,
}

main(): int {
    number: Number = Number{ value: 13 }
    value: Value = number_value(&number)
    return value.get()
}
```

Interface calls dispatch through the explicit vtable. This is runtime dispatch;
ordinary function overloads remain compile-time direct calls.

## Current limits

- Type inference and implicit generic instantiation are not implemented.
- Packed structs, explicit alignment attributes, volatile access, and tagged
  unions are not implemented.
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
