# std.bits

[Standard library](index.md)

`bits.from_float` and `bits.to_float` copy complete IEEE-754 representations
between floating and same-width unsigned integer values. They do not perform
numeric conversion, allocate, or call a runtime helper.

```ace
import bits

one_bits: u64 = bits.from_float(1.0) // 0x3FF0000000000000
one: f64 = bits.to_float(one_bits)
```

The exact overloads are `from_float(f64): u64`, `from_float(f32): u32`,
`to_float(u64): f64`, and `to_float(u32): f32`. They preserve signed zero,
infinities, subnormals, and NaN payloads. Ordinary casts such as `u64(value)`
and `f64(integer)` remain numeric conversions.

Their declarations use the restricted trailing `intrinsic` marker. Applications
call them as ordinary public functions and may store an exact overload in a
typed function value; only the compiler-owned standard library may declare an
intrinsic. Direct calls may inline the register move, while indirect calls use
a compiler-emitted callable implementation with identical semantics.

The integer operations have exact overloads for `u8`, `u16`, `u32`, and `u64`:

```ace
ones: uint = bits.pop_count(u32(0xF00F))
high: uint = bits.lead_zeros(u16(1))          // 15
low: uint = bits.trail_zeros(u64(8))          // 3
reversed: u32 = bits.byte_swap(u32(0x12345678))
rotated: u8 = bits.rotate_left(u8(0x81), uint(1)) // 0x03
```

`pop_count`, `lead_zeros`, and `trail_zeros` return `uint`. On a zero
operand, either zero-count operation returns the operand width. `byte_swap`
returns the input type and is the identity for `u8`. Rotations return the input
type and reduce their `uint` count modulo its width, so every count is valid.
Signed values and `usize` require an explicit conversion to one of the four
fixed-width unsigned types. Transparent platform aliases share their underlying
overload as usual, so `uint` uses the `u64` overload on Apple arm64. Like the
float representation operations, these operations allocate nothing, make no
runtime-library call, and remain usable through exact function values.

Representation access supports algorithms that deliberately operate on a
float's encoding, including the classic fast inverse-square-root seed:

```ace
raw: u32 = bits.from_float(value)
raw = u32(0x5F3759DF) - (raw >> 1)
estimate: f32 = bits.to_float(raw)
```

`mem` exports `alloc`, `realloc`, `release`, `copy`, `move`, `fill`, `zero`,
`compare`, and `equal`. Each allocation name is one overload set with a raw byte form and an
explicitly instantiated typed form. Import the module qualified or select those
names explicitly; there are no unimported global memory functions. The current
bootstrap compiler lowers these exact standard-module functions directly to the
platform boundary, with no extra Ace call, allocation header, or registry. See
[Memory](../memory.md).

```ace
import alloc, release from mem

main(): int {
    data: []u8 = alloc(64, 8)
    release(mut data)

    numbers: []usize = alloc[usize](4)
    if numbers == null {
        return 1
    }

    numbers[0] = 42
    release[usize](mut numbers)
    return 0
}
```

- `alloc(size: int, align: int): mut []u8` — writable raw bytes with an explicit size and
  power-of-two alignment.
- `alloc[T](count: usize): mut []T` — writable typed storage: `count * sizeof(T)` bytes, alignment
  taken from `T`, element length `count`, uninitialized storage, null on
  overflow/zero-size/failure.
- `realloc(data: []u8, new_size: int): mut []u8` — raw growth; failure leaves the
  original live.
- `realloc[T](data: []T, new_count: usize): mut []T` — typed growth with the same
  failure contract; the result is the only authoritative slice.
- `release(mut data: []u8)` and `release[T](mut data: []T)` — free the complete
  authoritative allocation and null only the passed variable; null is a no-op.
- `equal(a, b): bool` — exact, allocation-free content equality for every
  pairing of `[N]u8`, `[]u8`, and `string`; sequential `==`/`!=` is unavailable.

