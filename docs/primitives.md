# Primitive values

Ace uses explicit, fixed-width machine types. Declarations always state their
type; literals begin as constants and materialize into the required type only
when the context is unambiguous.

## Integers

The implemented signed integer types are `i8`, `i16`, `i32`, and `i64`. The
implemented unsigned types are `u8`, `u16`, `u32`, and `u64`.

`int` and `uint` are platform-word aliases. On the current AArch64 target:

```ace
signed_word: int = 13       // same type as i64
unsigned_word: uint = 13    // same type as u64
small: i8 = 13
byte_value: u8 = 255
```

Integer arithmetic supports `+`, `-`, `*`, `/`, `%`, bitwise `& | ^ !`, and
shifts `<< >>`. Comparisons produce `bool`. Signed division truncates toward
zero, and the remainder has the dividend's sign.

Integer literals are untyped constants until used. They must fit the destination
type. In overload selection, an uncast integer or character literal uses the
natural `int` candidate; cast explicitly to select another overload.

```ace
small: i32 = 13
wide: int = int(small)
unsigned: uint = uint(wide)
```

There are no implicit numeric conversions.

## Floating point

Ace implements IEEE-style `f32` and `f64` values.

```ace
single: f32 = 1.5
double: f64 = 1.5
converted: f64 = f64(single)
```

Floating literals are untyped until context materializes them. In overload
selection, an uncast floating literal uses `f64`. Arithmetic supports `+`, `-`,
`*`, and `/`, with the ordinary comparisons.

## Boolean

`bool` has only `true` and `false` values.

```ace
ready: bool = true
stopped: bool = !ready
```

Conditions require `bool`; integers are not implicitly treated as conditions.
Logical operators are `&&`, `||`, and `!`, with short-circuit evaluation for
`&&` and `||`.

## Character literals

Ace currently has no separate `char` primitive type. A character literal is an
untyped integer constant representing one byte and can materialize into a fitting
integer type.

```ace
letter: u8 = 'A'
newline: u8 = '\n'
hex_escape: u8 = '\x41'
```

UTF-8 source bytes pass through string literals, but character literals remain
single-byte values.

## Strings

`string` is a two-word byte view: a pointer and a length. String literals have
static storage, do not allocate, and contain exactly their encoded bytes.

```ace
message: string = "Welcome Ace!\n"
length: usize = message.len
first: u8 = message[0]
```

Strings are UTF-8 by convention. Indexing addresses bytes, not Unicode code
points. A string does not carry a terminator as part of its value.

## Other primitives

`usize` is the unsigned type used for object sizes, lengths, and indexing-related
machine quantities. It is 64 bits on the current target.

`fmt.print` accepts `usize` directly, so runtime lengths need no signed
conversion merely for output:

```ace
import fmt

main(): int {
    message: string = "Welcome Ace!\n"
    fmt.print(message.len)
    return 0
}
```

The overload writes the complete unsigned decimal range without allocating or
adding a newline. As with every numeric conversion, converting between `usize`
and another primitive requires an explicit cast such as `int(message.len)`.

`typeid` is an implemented opaque machine-word primitive type. `typeid(T)`
produces the nonzero identity of an exact named type at compile time (see
[Errors](errors.md)). Values can be stored, passed, and compared with `==` and
`!=`; the language exposes no ordering, arithmetic, or runtime type reflection.
The all-bits-zero `typeid` value is `null`, meaning no type identity.

`null` materializes the all-bits-zero value of pointers, function values,
contracts, slices, strings, `Error`, and `typeid`. Numeric zero uses `0` or
`0.0`, and boolean zero is `false`. Ace has no optional type constructor; use
an explicit `present: bool` field when an application must distinguish absence
from an ordinary zero value.

## Current limits

- `f16`, complex numbers, Unicode scalar/rune types, and separate `byte` or
  `char` aliases are not implemented.
- Numeric suffixes on literals are not implemented; use explicit casts or a
  typed declaration.
- Strings provide byte storage and indexing, but higher-level Unicode operations
  are not implemented in the standard library yet.

The examples are covered by `tests/sema/docs_primitive.ok.ace`.
