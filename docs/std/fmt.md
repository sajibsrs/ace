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
- `print(value: bool): int` — `true` or `false`.

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

The same seven overload types are available (`string`, `int`, `uint`,
`usize`, `f32`, `f64`, `bool`), selected exactly like `print`; narrower integer
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
exact types, and emits direct typed formatting calls. It opens a sink over
1024 bytes of the calling function's frame, writes each piece into it, and
closes it. The piece writers are exact-type specializations of the
per-shape generics in the internal `std/@/formatting.ace` — `piece[T]` for
text, bytes, and decimal integers, `piece_precision[T]`,
`piece_scientific[T]`, and `piece_general[T]` for `%f`, `%e`, and `%g`,
and `piece_radix[T]` for binary, octal, and hexadecimal, each taking the
sink, the value, the conversion's extras, its width, and its zero flag —
selected by the format interpreter's concrete argument type (see
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
- `%e` accepts `f32` or `f64` and writes scientific notation, `d.dddddde+XX`:
  one digit before the point, six after by default (`%.Ne` writes `N`, and
  `%.0e` no point), and an exponent of at least two digits. `%g` writes six
  significant digits by default (`%.Ng` writes `N`; `%.0g` means one): as
  `%e` when the decimal exponent is below -4 or at least the precision,
  otherwise as `%f`, then drops trailing fraction zeroes and a bare point, so
  `%g` prints `1234.5678` as `1234.57`, `0.00001` as `1e-05`, and the smallest
  denormal as `4.94066e-324`. Both round the exact value as `%f` does, keep
  the sign of negative zero, and spell non-finite values as `%f` does. Their
  output is byte-identical to C's for every finite value.
- `%s` accepts `string` and writes every byte exactly, including embedded NUL.
- `%c` accepts `u8` and writes exactly one byte. Wider Unicode characters use
  their UTF-8 bytes through `%s`; a future character facility may add more.
- `%%` writes one percent sign and consumes no argument.

Transparent aliases work as their target type. Distinct types and narrower
integers must be converted explicitly. A literal receives the conversion's
natural type: `int`, `uint`, `f64`, or `u8`; write `usize(...)` when that is
the intended unsigned type.

A literal width between `%` and the conversion (`%5d`, `%6.2f`, `%12e`, `%4s`) pads
the piece with spaces on the left to at least that many bytes, from 1 through
1000000. A `0` flag before the width (`%05d`, `%08x`, `%064b`, `%06.2f`) pads
numeric conversions with zeroes after any sign instead: `-0042`. `nan` and
`inf` still take spaces, and `%s`/`%c` reject the flag. A longer value is
never truncated. Write a base prefix as literal text (`0x%08x`). Left
alignment, sign and alternate-form flags, dynamic `*`, and positional
arguments are not implemented; the precision on `%f`, `%e`, and `%g` is the
only other modifier, from 0 through 324.

All runtime arguments are evaluated exactly once, in source order, before
output begins. Output collects in the sink's storage and is written when the
storage fills and when the call ends, so a call whose output fits in 1024
bytes makes one write; text longer than the storage is written straight from
its source, and padding streams through the storage in runs. Nothing is kept
between calls. On success `printf` returns the complete byte count. Positive
partial writes are retried; a zero or negative OS result stops the rest of
the call and is returned unchanged, matching `print` and `println`. Formatting
uses fixed stack storage and never allocates. `fmt.printf` is direct-call
only and cannot be stored as a function value because its checked argument
shape depends on the static format.

An argument with no value, such as a `panic(...)` call, is a compile-time
error.

A first argument of type `sys.Fd` writes to that descriptor instead of
standard output, as C's `dprintf` does; everything else is the same:

```ace
import fmt
import sys

main(): int {
    written: int = fmt.printf(sys.stderr, "warning: %d retries\n", 3)
    return written - 19
}
```

`fmt.Buffer` keeps output across calls, for code that writes many small
pieces. `fmt.Buffer.init(fd, storage)` takes a descriptor and caller-owned
storage, allocating nothing. `buffer.printf(format, ...)` is `printf` with
the same checks and conversions, but its bytes go into the storage and reach
the descriptor only when the storage fills or on `buffer.flush()`, so
thousands of calls cost a write per storage-full rather than one each:

```ace
import fmt
import sys

main(): int {
    storage: [4096]u8
    out: fmt.Buffer = fmt.Buffer.init(sys.stdout, storage[..])
    for i: int in 0 .. 3 { out.printf("line %d\n", i) }
    return out.flush() - 21
}
```

`Buffer.printf` returns the bytes the call took; `flush` returns the bytes it
wrote. A piece larger than the storage is written straight through. A failed
write sticks: that call, every later one, and `flush` return its non-positive
result. Nothing is flushed implicitly — call `flush` before the program exits
or the storage goes out of scope.

`fmt.Buffer.init(storage)`, with no descriptor, keeps its output in the
storage: bytes past the end are dropped, `flush` writes nothing, and
`buffer.text()` reads what was kept, `buffer.dropped()` how many bytes did
not fit. It carries error context: each layer
that fails appends one line and returns its `Error` unchanged, and the caller
prints the buffer innermost-first, with no allocation and nothing paid on the
success path:

```ace
import fmt

ParseErr: error

parse(text: string, diag: *fmt.Buffer): Error {
    diag.printf("line %d: unexpected '}'\n", 12)
    return ParseErr()
}

load(path: string, diag: *fmt.Buffer): Error {
    err: Error = parse("}", diag)
    if err != null {
        diag.printf("load config %s\n", path)
        return err
    }
    return null
}

main(): int {
    storage: [256]u8
    diag: fmt.Buffer = fmt.Buffer.init(storage[..])
    if load("app.json", &diag) != null { fmt.print(diag.text()) }
    return 0
}
```

A caller that wants no context passes `fmt.Buffer.init(null)`.

Code that continues past failures and handles each one programmatically, such
as a lexer reporting every error in a file, takes a caller-owned
`errors: *array.Array[R]` instead, where `R` is its own record type holding
an `err: Error` plus whatever position or payload it reports, and pushes one
record per failure. Std defines no shared failure record: each domain's record
carries what that domain needs.

## Design rationale

Why the rules in `spec/std/fmt.md` are shaped as they are.

- Stdlib: formatted output #12: one write per call rather than per piece (Go's
  `fmt` formats a whole call into one buffer before writing) without C stdio's
  cross-call buffer and its flushing rules.
- Stdlib: formatted output #12 (under the priority stack): direct typed
  lowering, bounded stack storage, and no runtime interpretation put performance
  first; the small conversion/modifier set and one uniform output contract
  minimize language, compiler, and runtime machinery; private helper reuse
  preserves the `fmt` module boundary; static linear validation keeps compile
  time cheap; and precise compile-time diagnostics add safety without release
  cost. Every excluded feature can be added later without tightening existing
  code.
- `Buffer.printf` is a method rather than a `printf` overload so that plain
  `printf` keeps its guarantee that bytes are written before it returns; the
  receiver shows where deferred output goes (Zig's `writer.print` beside
  `std.debug.print`). The buffer reuses `printf`'s sink in place, so it costs
  no copy per call, and takes caller storage so `fmt` stays allocation-free.
- Error context lives in a caller's buffer rather than in `Error`, which stays
  one register-sized value: Go's `%w` wrapping allocates a new error at every
  layer, while Zig's `Diagnostics` out-parameters, followed here, write into
  storage the caller provides once. Dropping bytes past the end, rather than
  failing, keeps the innermost cause, which is written first.
