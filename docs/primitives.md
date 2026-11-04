# Primitive values

Ace uses explicit, fixed-width machine types. Declarations always state their
type; literals begin as constants and materialize into the required type only
when the context is unambiguous.

## Integers

The implemented signed integer types are `i8`, `i16`, `i32`, and `i64`. The
implemented unsigned types are `u8`, `u16`, `u32`, and `u64`.

`int` and `uint` are the platform words, 64-bit on the current AArch64 target.
Each is its own type on every target, so an `i64` value needs a cast to become
an `int`, even where the sizes match.

```ace
signed_word: int = 13
unsigned_word: uint = 13
small: i8 = 13
byte_value: u8 = 255
```

Integer arithmetic supports `+`, `-`, `*`, `/`, `%`, bitwise `& | ^ ~`, and
shifts `<< >>`. Comparisons produce `bool`. Signed division truncates toward
zero, and the remainder has the dividend's sign. `!` is logical negation and
accepts only `bool`; `~` complements every bit of an integer value.

A bitwise operator never mixes with an equality or comparison without
parentheses, in either order: `b & 0xC0 != 0x80` and `b == 1 | x` are parse
errors, written `(b & 0xC0) != 0x80` and `(b == 1) | x`.

The conditional `cond ? a : b` selects a value by a `bool` and evaluates only
the selected branch. It binds loosest of all operators, so
`limit: int = n > max ? max : n` needs no parentheses. Its branches have one
type; an untyped constant branch takes the other branch's type, and where the
context expects a type, each branch meets it on its own:

```ace
view: []u8 = empty ? null : data[..n]
origin: Point = flipped ? { x = 1, y = 0 } : { x = 0, y = 1 }
```

Typed branches of different types never convert: `c ? a32 : b64` is an
error, written with an explicit cast. A conditional inside another needs
parentheses (`a ? b : (c ? d : e)`), and inside a repeated array literal the
first `:` belongs to the conditional, so `[(c ? x : y) : 3]` repeats it.

Integer `+`, `-`, `*`, and unary `-` wrap modulo 2^width in every build, with
two's-complement signed results: `i8(127) + i8(1)` is `-128` and `u8(0) - 1`
is `255`. The signed minimum divided by `-1` is the minimum, and its remainder
is `0`. Division or remainder by zero aborts in every build. A shift count is
taken modulo the left operand's width in every build: `u8(1) << 9` is `2`,
`u32(x) >> 32` is `x`, and a signed count of `-1` shifts by width - 1. A
constant global initializer that overflows, or shifts by a negative count or
one of at least the width, is still a compile error.

When the divisor of `/`, `%`, `/=`, or `%=` is an integer literal or named
constant, the compiler replaces the hardware divide with shifts and masks for
powers of two, or a multiply-high by a precomputed reciprocal otherwise. The
results are identical, including signed truncation toward zero. A constant
nonzero divisor cannot fail, so debug output omits its guards; zero and a
signed `-1` divisor keep the ordinary divide and checks.

Integer literals are untyped constants until used. They must fit the destination
type. In overload selection an uncast integer literal prefers `int`, and otherwise
takes the one candidate it fits; see [Overloads](function.md#overloads).

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

Floating literals are untyped until context materializes them. An integer literal
never becomes a float: `x: f64 = 1` is an error. In overload selection, an uncast
floating literal prefers `f64`. Arithmetic supports `+`, `-`,
`*`, and `/`, with the ordinary comparisons. `%`, `%=`, and the bitwise and
shift operators take integers only.

The `bits` standard module provides the bit-preserving `cast` overloads for `f32`/`u32` and `f64`/`u64`. These expose the complete IEEE-754
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

A character literal holds exactly one byte and is `u8` in every position, as is
a constant computed from one (`'a' + 1`). Any other type takes it through a
cast: `n: int = 'a'` is an error, `n: int = int('a')` is not, and the same goes
for an operand beside a `u32`, an `enum u32` value, or a `%d` argument. The escapes are `\n`, `\t`, `\r`, `\0`,
`\\`, `\"`, `\'`, and `\xNN`; a direct multibyte source character or a `\u{...}`
escape in a character literal is invalid.

```ace
letter: char = 'A'
newline: u8 = '\n'
hex_escape: u8 = '\x41'
bytes: [2]char = [letter, newline]
```

UTF-8 source bytes pass through string literals, but character literals remain
single-byte values. A string literal may also name a code point with
`\u{H...}`, 1-6 hex digits, which writes its UTF-8 encoding; surrogates
(U+D800-U+DFFF) and values above U+10FFFF are rejected. `\xNN` still writes one
raw byte, so a string can hold bytes that are not UTF-8.

```ace
dash: string = "a\u{2014}b"     // the same bytes as "a\xE2\x80\x94b"
clef: string = "\u{1D11E}"      // four bytes
```

## Strings

`string` is the text-intent spelling of the same type as `[]char` and `[]u8`:
a byte pointer and a byte count, so `sizeof(string)` is 16. String literals have static
read-only storage, do not allocate, and contain exactly their encoded bytes; the
language appends no terminator. `s[i] = c` assigns exactly one byte through
any string view ([Types](types.md#views)); a write into literal storage
faults, since the page is read-only.
The empty literal `""` is a valid non-null string (pointer, length 0); only the
all-bits-zero string is `null`.

```ace
message: string = "Welcome Ace!\n"
length: usize = message.len
first: u8 = message[0]
```

Every string index must satisfy `0 <= index < message.len`. The compiler
rejects statically provable violations in every mode; debug output validates
dynamic indices before reading or writing, while release output assumes them.

Strings are UTF-8 by convention. Indexing addresses bytes, not Unicode code
points. A string does not carry a terminator as part of its value.

Slicing a string yields a `string`: `message[1..4]` is a zero-copy view with
bounds defaulting to `0` and `source.len`, exactly like array and slice
slicing ([Types](types.md)). Where the bounds are compile-time constants that
violate `0 <= lo <= hi <= source.len`, the compiler rejects the program.
Debug and release output enforce the same complete precondition for dynamic
bounds.
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
- Untyped literals materialize only where the existing coercion rules admit
  them.

The examples are covered by `tests/sema/docs/primitive_ok.ace`.

## Design rationale

- A character constant is `u8` everywhere, as Rust's `b'a'` is, rather than an
  integer constant of no fixed type as in Go, where `var n int = 'a'` compiles.
  Ace has no implicit conversion between typed values, and a byte that silently
  arrives as a `u32` would be one in all but name. The cast costs nothing at run
  time and states the widening. Integer constants stay contextual because they
  carry no type of their own to convert from.
- `\u{H...}` follows Rust, Swift, and Zig: one braced form covers the whole
  range, where C, Go, and Python need both `\uXXXX` and `\UXXXXXXXX`, and the
  brace ends the escape so a following hex digit cannot be misread. It writes
  bytes at compile time, so it costs nothing at run time. A character literal
  is one byte, so `\u` is rejected there in every case rather than accepted
  only below U+80.
- Bitwise operators and comparisons do not mix unparenthesized because C's
  ranking (`&` looser than `==`) makes `b & mask != value` mean
  `b & (mask != value)`, and reranking would silently change what such code
  means. Rejecting the mix never changes meaning and can later relax to
  Go's ranking (`&` above `==`) without breaking code. GCC and Clang flag the
  same mix with `-Wparentheses`.
- The conditional is spelled `cond ? a : b`, as in C, rather than an
  if-expression, because Ace has no block-valued expressions: an `if` whose
  branches are blocks yielding values would bring in a block-result rule for
  one construct. `?` was otherwise unused.
- A conditional nested unparenthesized in another is an error, as in PHP 8,
  because chained conditionals read ambiguously (C groups `a ? b : c ? d : e`
  to the right, PHP 7 grouped it to the left), and requiring parentheses
  matches the bitwise/comparison rule above. It can later relax to C's
  right grouping without breaking code.
- Integer overflow wraps in every build, as in Go and Java, rather than
  trapping as in Swift or trapping only in debug as in Rust. Trapping in
  release measured 1.18x on `spectralnorm`, 1.19x on `bench/std/string`, and
  1.30x on `bench/std/collections`: each checked operation pins its result,
  so the optimizer cannot move, merge, or vectorize it. A debug-only trap would
  give one program two meanings. Bounds checks still catch a wrapped index.
- A shift count is masked by width - 1, as in Java and C# and as arm64's
  register shifts do for 32 and 64 bits, rather than Go's rule that a count of
  at least the width gives 0 or sign fill. Masking costs no instruction at 32
  and 64 bits and one `and` at 8 and 16 bits; Go's rule adds a compare and
  select to every shift whose count the compiler cannot bound, measured 1.11x
  on `sin-large` in `bench/std/math` and 1.06x on `map-miss` in
  `bench/std/collections`, while masking left every bench within noise.
- A conditional is not yet a compile-time constant; constant folding of it
  can be added later without changing any accepted program.
