# std.fmt

[Standard library](index.md)

`fmt.print` writes bytes to standard output (file descriptor 1). It never adds
a newline, never allocates, and returns the number of bytes written, or the
non-positive result reported by the OS on failure. Partial writes are retried
until done or error.

Six overloads exist, selected exactly like any other overload set:

```ace
import fmt

main(): int {
    fmt.print("ready")           // prints: ready
    fmt.print(42)                // prints: 42
    fmt.print(uint(7))           // prints: 7
    fmt.print(usize(7))          // prints: 7
    fmt.print(0.5)               // prints: 0.5
    fmt.print(1.0e21)            // prints: 1e21
    return 0
}
```

- `print(text: string): int` — writes the bytes exactly.
- `print(value: int): int` — signed decimal, complete `int` range including
  `INT_MIN`, formatted through a fixed 20-byte stack buffer.
- `print(value: uint): int` — unsigned platform-word decimal, zero through
  `UINT_MAX`. Values typed `u64` select it directly.
- `print(value: usize): int` — unsigned decimal for lengths, sizes, and
  indices, zero through `USIZE_MAX`.
- `print(value: f32): int` and `print(value: f64): int` — shortest
  round-trip decimal: the fewest significant digits that read back as the
  exact original value.

Float output spells the special values `nan`, `inf`, and `-inf`, and keeps
the sign of a negative zero (`-0`). Values print in decimal notation when
the scientific exponent is between -6 and 20; anything larger or smaller
uses scientific spelling like `1e21`, `1.5e-7`, or `3.4028235e38` — no `+`,
no exponent padding. Whole-valued floats print without `.0` (`3.0` prints
as `3`).

Float output is converted natively from its exact IEEE representation using
fixed-capacity stack arithmetic shared with `str.format`. It does not allocate,
call a platform numeric-conversion routine, or depend on the ambient
floating-point rounding mode.

Numeric output is ASCII with a `-` only for negative signed values. There is
no padding, no base selection, and no formatting language.

Integer widths narrower than the platform word have no overload; convert
explicitly at the call site — `fmt.print(int(my_i32))`,
`fmt.print(uint(my_u16))` — so the printed type is always visible.

`fmt.println` writes exactly the representation of the matching `fmt.print`
overload followed by one newline. It allocates nothing and returns the total
bytes written, including the newline. If writing the value fails, that
failure is returned and no newline is attempted; if the value is written but
the newline write fails, the newline's failure result is returned. An empty
string still emits the newline, so `fmt.println("")` returns `1`. There is
no zero-argument form — `fmt.print("\n")` already writes a bare newline.

The same six overload types are available (`string`, `int`, `uint`,
`usize`, `f32`, `f64`), selected exactly like `print`; narrower integer
widths convert explicitly (`fmt.println(int(my_i32))`).

```ace
import fmt

main(): int {
    fmt.println("ready")  // the bytes `ready` plus one newline
    fmt.println(42)       // the bytes `42` plus one newline
    fmt.println(0.5)      // the bytes `0.5` plus one newline
    return 0
}
```

`fmt.printf` combines static text and runtime values while keeping the runtime
as direct as separate typed `print` calls:

```ace
import fmt

main(): int {
    name: string = "Ace"
    count: int = 3
    ratio: f64 = 1.25
    return fmt.printf("name=%s count=%d ratio=%.2f\n", name, count, ratio)
}
```

The format must be a string literal or a const-global `string` initialized
from a static literal. The compiler parses it, checks the argument count and
exact types, and emits direct typed formatting calls. Those calls are
exact-type specializations of the private per-shape generics in `std/fmt/mod.ace`
— `print_counted[T](value: T): int` for text, bytes, and decimal integers,
`print_precision[T](value: T, precision: usize): int` for fixed floats, and
`print_radix[T](value: T, radix: u8, uppercase: bool): int` for binary, octal,
and hexadecimal — selected by
the format interpreter's concrete argument type (see
[Functions](../function.md#generic-specialization)). Runtime-created or
runtime-selected formats are rejected. There is no runtime parser, argument
pack, type tag, reflection, allocation, or C variadic call.

The initial conversions are deliberately small:

- `%d` accepts `int` and writes signed decimal.
- `%u` accepts `uint` or `usize` and writes unsigned decimal.
- `%b` and `%o` accept `uint` or `usize` and write binary or octal digits
  without a prefix or leading zeroes. Zero is written as `0`.
- `%x` and `%X` accept `uint` or `usize` and write hexadecimal digits in the
  selected case, without a prefix.
- `%f` accepts `f32` or `f64` and writes fixed decimal with six fractional
  digits by default. `%.Nf` writes exactly `N` fractional digits for
  `N` from 0 through 324. Rounding is nearest with exact ties going to an
  even final digit, independent of the caller's floating-point rounding mode;
  that mode is restored before `printf` returns. Negative zero and negative
  values rounded to zero keep their sign; special values are `nan`, `inf`,
  and `-inf`.
- `%s` accepts `string` and writes every byte exactly, including embedded NUL.
- `%c` accepts `u8` and writes exactly one byte. Wider Unicode characters use
  their UTF-8 bytes through `%s`; a future character facility may add more.
- `%%` writes one percent sign and consumes no argument.

Transparent aliases work as their target type. Distinct types and narrower
integers must be converted explicitly. A literal receives the conversion's
natural type: `int`, `uint`, `f64`, or `u8`; write `usize(...)` when that is
the intended unsigned type.

Width, padding, alignment, sign and alternate-form flags, dynamic `*`, and
positional arguments are not implemented. Consequently `%b`, `%o`, `%x`, and
`%X` cannot yet render fixed-width or zero-padded register values. The only
modifier is the literal precision on `%f`.

All runtime arguments are evaluated exactly once, in source order, before
output begins. On success `printf` returns the complete byte count. Positive
partial writes are retried; a zero or negative OS result stops later output
and is returned unchanged, matching `print` and `println`. Formatting uses a
fixed 640-byte stack buffer and never allocates. `fmt.printf` is direct-call
only and cannot be stored as a function value because its checked argument
shape depends on the static format.

An argument whose type is still unresolved when the format is checked, such as a
call deferred to end of file (`g(41)` with a `usize` parameter), is a
compile-time error; type the call's arguments (`g(usize(41))`) or assign its
result to a typed variable first.

