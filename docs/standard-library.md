# Standard library

The standard library ships beside the compiler in `std/`. Bare imports
(`import fmt`) resolve only there — a local file can never shadow a standard
module. How imports resolve, aliasing, and visibility are described in
[Modules](module.md).

Today's modules:

- `fmt` — allocation-free text output
- `math` — minimal generic helpers
- `mem` — raw allocation and allocation-free byte operations

Everything else in the planned v1 set (`sys`, `str`, `simd`) does not
exist yet.

`mem` exports `alloc`, `realloc`, `release`, `copy`, `move`, `fill`, `zero`, and
`compare`. Import it qualified or select those names explicitly; there are no
unimported global memory functions. The current bootstrap compiler lowers
these exact standard-module functions directly to the platform boundary, with
no extra Ace call, allocation header, or registry. See [Memory](memory.md).

```ace
import alloc, release from mem

main(): int {
    data: []u8 = alloc(64, 8)
    release(mut data)
    return 0
}
```

## fmt

`fmt.print` writes bytes to standard output (file descriptor 1). It never adds
a newline, never allocates, and returns the number of bytes written, or the
non-positive result reported by the OS on failure. Partial writes are retried
until done or error.

Three overloads exist, selected exactly like any other overload set:

```ace
import fmt

main(): int {
    fmt.print("ready")           // prints: ready
    fmt.print(42)                // prints: 42
    fmt.print(usize(7))          // prints: 7
    return 0
}
```

- `print(text: string): int` — writes the bytes exactly.
- `print(value: int): int` — signed decimal, complete `int` range including
  `INT_MIN`, formatted through a fixed 20-byte stack buffer.
- `print(value: usize): int` — unsigned decimal for lengths, sizes, and
  indices, zero through `USIZE_MAX`.

Numeric output is ASCII with a `-` only for negative signed values. There is
no padding, no base selection, and no formatting language.

## math

Minimal, pure-Ace, no floating-point functions yet. All three are generic and
require explicit instantiation:

```ace
import fmt
import math

main(): int {
    fmt.print(math.min[int](3, 9))   // prints: 3
    fmt.print(math.max[int](3, 9))   // prints: 9
    fmt.print(math.abs[int](0 - 7))  // prints: 7
    return 0
}
```

- `min[T](a: T, b: T): T`
- `max[T](a: T, b: T): T`
- `abs[T](x: T): T` — negates when `x < 0`

The body is checked per instantiation; `T` must support the comparison and
arithmetic used by the function.

## Current limitations

- No `fmt.println` or formatted printing yet; end lines by printing your own
  bytes.
- No output overloads for `uint`, floats, or narrower integer widths; an
  explicit cast selects between the existing `int` and `usize` overloads.
- `math` has no `sqrt`, floats, or libm binding.
- There is no user-configurable search path for `std/`.
