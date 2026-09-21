# std.math

[Standard library](index.md)

`math` needs no platform library. Its ordering and absolute-value helpers are generic and
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

Exact rounding operations are `f32` and `f64` overloads that return their
operand's type:

```ace
root: f64 = math.sqrt(2.0)      // correctly rounded
down: f32 = math.floor(single)  // toward negative infinity
up: f32 = math.ceil(single)     // toward positive infinity
```

- `sqrt(value)` is correctly rounded at the operand's width. `sqrt(-0.0)` is
  `-0.0`; a negative nonzero operand gives a NaN with unspecified sign and
  payload.
- `floor(value)` and `ceil(value)` round to an integral float. Zeros,
  infinities, and integral values are unchanged, and a NaN stays a NaN.

`math.fma(a, b, c)` is the fused multiply-add of the same family: it returns
`a * b + c` with one rounding of the exact product and sum, both widths.

```ace
residual: f64 = math.fma(a, b, -(a * b))   // the part a * b lost to rounding
```

An ordinary `a * b + c` is never contracted into a fused operation, so the two
spellings are different computations and each keeps the value its own text
states. `fma(inf, 0, c)` is a NaN, since the exact product is undefined.

They are compiler-provided instructions (`fsqrt`, `frintm`, `frintp`,
`fmadd`): no allocation, no libm call, no errno or flag. Converting the result
to an integer remains an explicit cast.

Exponential, logarithm, power, sine, and cosine are `f32` and `f64` overloads
that return their operands' type:

```ace
growth: f64 = math.exp(0.5)
level: f32 = math.log(single)
scaled: f64 = math.pow(2.0, 0.5)
height: f64 = math.sin(angle)   // radians
across: f32 = math.cos(single)
```

- Every `f64` result is within 1 ULP of the exact result over the whole finite
  domain. An `f32` result is within 1 ULP too; `pow` has its own `f32` kernel.
- `exp(±0)` is `1`, `exp(+inf)` is `+inf`, `exp(-inf)` is `0`; overflow gives
  `+inf` and underflow is gradual through subnormals.
- `log(±0)` is `-inf`, `log` of a negative value is a NaN, `log(1)` is `+0`, and
  `log(+inf)` is `+inf`. A NaN operand returns a NaN.
- `sin(±0)` is `±0`, `cos(±0)` is `1`, and `sin` or `cos` of an infinity is a
  NaN. Both cover the full finite domain: large arguments are reduced exactly
  (Payne–Hanek) rather than approximately, so `sin(1.0e300)` is as accurate as
  `sin(1.0)`.
- `pow(base, exponent)` is exact, not merely within 1 ULP, whenever the result
  is an exactly representable integer power: `pow(3.0, 5.0)` is `243` on the
  nose. A zero exponent and a base of `1` win over everything else —
  `pow(nan, 0.0)` and `pow(1.0, nan)` are both `1` — as does `pow(-1.0, ±inf)`.
  A finite negative base with a finite non-integer exponent is a NaN. Zero and
  infinite bases follow the exponent's parity: `pow(-0.0, -3.0)` is `-inf`
  while `pow(-0.0, -2.0)` is `+inf`. An infinite exponent takes the limit, so
  `pow(0.5, +inf)` is `+0` and `pow(2.0, +inf)` is `+inf`. The `f64` result is
  within 0.51 ULP of the exact power, or within 1 ULP when the result is subnormal;
  the `f32` result is within 0.51 ULP.
  The exponents 2, 1, -1, and 0.5 take exact fast paths (`x * x`, `x`, `1 / x`,
  `sqrt(x)` for a positive base) and are correctly rounded.

They are pure Ace: no libm call, no errno or floating-point flag, and the same
result on every target. They currently run at about 6 ns per call for `exp` and
8 ns for `log`, 11–12 ns for `sin` and `cos` once the argument needs reducing
and about 4 ns below pi/4, several hundred ns for arguments beyond 1.6e6, and
about 23 ns for `pow(f64)` (12 ns for `pow(f32)`, about 7 ns for the exponents
2, 1, -1, and 0.5). `exp`, `log`, and `pow` are all table-driven over the same
two 128-entry tables: a logarithm table of `invc` and `log(c)`, and a table of
`2^(j/128)`. `pow` forms `log(x)` as a high and low word, multiplies by the
exponent in extended precision, and rebuilds the result from the powers-of-2
table; `exp` and `log` return one rounded double each, so they keep only the
single compensation term that arithmetic needs. `pow`'s `f32` kernel runs the
same tables in plain `f64` with a shorter series. All of them name as few
locals as they can, because the emitter promotes only named locals used at
least twice and only eight of them reach a callee-saved float register.

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

