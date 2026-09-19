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

Integer arithmetic supports `+`, `-`, `*`, `/`, `%`, bitwise `& | ^ ~`, and
shifts `<< >>`. Comparisons produce `bool`. Signed division truncates toward
zero, and the remainder has the dividend's sign. `!` is logical negation and
accepts only `bool`; `~` complements every bit of an integer value.

Checked output aborts when integer `+`, `-`, `*`, or unary `-` cannot produce a
value of its declared type, and on division/remainder by zero or the signed
minimum divided by `-1`. Shift counts must be nonnegative and below the left
operand width. Release output emits direct fixed-width operations without
guards; explicit casts and bitwise operations are not overflow-checked.

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

Ace implements IEEE-style `f32` and `f64` values. `float` is a predeclared
transparent alias of `f64`: both spellings have the same type identity,
representation, conversions, ABI behavior, generic identity, and overload
signature. They cannot distinguish overloads.

```ace
single: f32 = 1.5
double: f64 = 1.5
default_width: float = 1.5
converted: f64 = f64(single)
```

Floating literals are untyped until context materializes them. In overload
selection, an uncast floating literal uses `f64`. Arithmetic supports `+`, `-`,
`*`, and `/`, with the ordinary comparisons.

The `bits` standard module provides bit-preserving `from_float` and `to_float`
overloads for `f32`/`u32` and `f64`/`u64`. These expose the complete IEEE-754
representation and are distinct from numeric casts.

## Boolean

`bool` has only `true` and `false` values.

```ace
ready: bool = true
stopped: bool = !ready
```

Conditions require `bool`; integers are not implicitly treated as conditions.
Logical operators are `&&`, `||`, and `!`, with short-circuit evaluation for
`&&` and `||`.

## Character and text aliases

`char` is a predeclared transparent alias of `u8` with shared identity,
representation, conversions, and overload behavior: `sizeof(char)` is 1, a
`[]char` is a `[]u8`, and an overload taking `char` also accepts `u8` values. It
names one byte used as character data, not a decoded Unicode character.

`string`, `[]char`, and `[]u8` likewise share type identity and the same
two-word view representation. Use `string` for complete text, `[]char` for
character-oriented parsing or partial textual framing, and `[]u8` for raw
bytes. These intent spellings cannot distinguish overloads.

A character literal is an untyped constant holding exactly one byte and can
materialize into a fitting integer type. The escapes are `\n`, `\t`, `\r`, `\0`,
`\\`, `\"`, `\'`, and `\xNN`; a direct multibyte source character in a character
literal is invalid.

```ace
letter: char = 'A'
newline: u8 = '\n'
hex_escape: u8 = '\x41'
bytes: [2]char = [letter, newline]
```

UTF-8 source bytes pass through string literals, but character literals remain
single-byte values.

## Strings

`string` is the text-intent spelling of the same type as `[]char` and `[]u8`:
a byte pointer and a byte count, so `sizeof(string)` is 16. String literals have static
read-only storage, do not allocate, and contain exactly their encoded bytes; the
language appends no terminator. A plain string is a read-only view — indexed
reads stay value reads, and no syntax writes through its bytes. The
`mut string` spelling of `mut []char`/`mut []u8` ([Types](types.md)) carries writable bytes: it
establishes only where `mut` is written over proven-writable storage, so a
literal can never back one, and `s[i] = c` through it assigns exactly one byte.
The empty literal `""` is a valid non-null string (pointer, length 0); only the
all-bits-zero string is `null`.

```ace
message: string = "Welcome Ace!\n"
length: usize = message.len
first: u8 = message[0]
```

Every string index must satisfy `0 <= index < message.len`. The compiler
rejects statically provable violations in every mode; checked output validates
dynamic indices before reading or writing, while release output assumes them.

Strings are UTF-8 by convention. Indexing addresses bytes, not Unicode code
points. A string does not carry a terminator as part of its value.

Slicing a string yields a `string`: `message[1..4]` is a zero-copy view with
bounds defaulting to `0` and `source.len`, exactly like array and slice
slicing ([Types](types.md)). Where the bounds are compile-time constants that
violate `0 <= lo <= hi <= source.len`, the compiler rejects the program.
Checked output enforces the same complete precondition for dynamic bounds;
release output emits no guard.
`for-in` walks the stored bytes. Assignment and calls among `string`, `[]char`,
and `[]u8` require no conversion and perform no work ([Types](types.md),
[Control flow](control-flow.md)).

```ace
prefix: string = message[..7]
suffix: string = message[8..]
complete: string = message[..]
```

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

`typeid` is an implemented opaque machine-word primitive type carrying a
compiler-assigned nonzero 32-bit identity. `typeid(T)` produces the identity
of an exact named type at compile time (see
[Errors](errors.md)). Values can be stored, passed, and compared with `==` and
`!=`; the language exposes no ordering, arithmetic, or runtime type reflection.
The all-bits-zero `typeid` value is `null`, meaning no type identity.

`null` materializes the all-bits-zero value of pointers, function values,
contracts, slices, strings, `Error`, and `typeid`. Numeric zero uses `0` or
`0.0`, and boolean zero is `false`. Ace has no optional type constructor; use
an explicit `present: bool` field when an application must distinguish absence
from an ordinary zero value.

## Current limits

- `f16` and complex numbers are not implemented. Unicode code points are plain
  `u32` values; UTF-8 encoding and decoding live in [`utf8`](std/utf8.md).
- Numeric suffixes on literals are not implemented; use explicit casts or a
  typed declaration.
- Strings provide byte storage, indexing, and slicing, but higher-level Unicode
  operations are not implemented in the standard library yet.
- A character literal cannot yet initialize a floating-point target, and untyped
  literals materialize only where the existing coercion rules admit them.

The examples are covered by `tests/sema/docs/primitive_ok.ace`.
