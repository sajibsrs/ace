# std.math

[Standard library](index.md)

`math` is pure Ace. Its ordering and absolute-value helpers are generic and
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

Float classification uses exact overloads for `f32` and `f64`:

```ace
if math.isnan(value) {
    // NaN, including quiet and signaling encodings.
} else if math.isinf(value) {
    // Positive or negative infinity.
} else if math.isfinite(value) {
    // Zero, subnormal, or normal finite value.
}
```

- `isnan(value): bool` is true exactly when the exponent is all ones and the
  fraction is nonzero.
- `isinf(value): bool` is true for positive and negative infinity only.
- `isfinite(value): bool` is true for zero, subnormal, and normal finite values.

Classification inspects the IEEE-754 representation through `bits`, allocates
nothing, and does not call libm.

