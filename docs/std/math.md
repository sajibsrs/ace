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
  domain. An `f32` result is within 1 ULP too; `pow`, `sin`, and `cos` have
  their own `f32` kernels.
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
8 ns for `log` (2.4 ns and 3.4 ns for `f32`), about 4 ns for `sin` and `cos` up to 1.6e6 (about 3.5 ns for
`f32` `sin` and `cos`, which reduce by pi and take one polynomial whatever the
quadrant, so no branch depends on the argument below 4.2e8), about 7.5 ns for
`f64` arguments beyond 1.6e6, and
about 23 ns for `pow(f64)` (12 ns for `pow(f32)`, about 7 ns for the exponents
2, 1, -1, and 0.5). `exp`, `log`, and `pow` are all table-driven over the same
two 128-entry tables: a logarithm table of `invc` and `log(c)`, and a table of
`2^(j/128)`. `pow` forms `log(x)` as a high and low word, multiplies by the
exponent in extended precision, and rebuilds the result from the powers-of-2
table; `exp` and `log` return one rounded double each, so they keep only the
single compensation term that arithmetic needs. The `f32` `exp`, `log`, and
`pow` kernels run the same tables in plain `f64` with shorter series (a
degree-3 exponential, a logarithm series to `r^5`) and round once into `f32`;
every `f32`, subnormals included, is a normal `f64`, so only zero, infinite,
NaN, and negative operands leave the main path. The worst `f32` error over all
inputs is 0.504 ULP for `exp` and 0.501 ULP for `log`. `sin` and `cos` write the
argument as `n * pi/64 + r` with `|r| <= pi/128`, read `S = sin(k * pi/64)` and
`C = cos(k * pi/64)` from one 128-entry table (the cosine is the same table 32
entries on), and return `S + C*r + (S*(cos(r) - 1) + C*(sin(r) - r))`, the two
differences being three-term series. The table spans a full period and holds
each value as a high and low word, so quadrant, sign, and which of the two
functions is wanted are all its index: no branch depends on the argument, and
the worst error found is 0.55 ULP. LLVM-libc and Intel's libm reduce the same
way. From 1647099 on, the exact Payne–Hanek product `x * 2/pi mod 4` takes the
place of `n` and `r`: its top seven bits, rounded, are the table index, and the
rest is `r` as a signed fixed-point fraction of pi/64, so large arguments run
the same table step with no branch on the argument either; the worst error
found there is 0.543 ULP. Their rare paths stay inline, because a call anywhere
in a function makes it save its callee-saved float registers on entry. All of them name as few
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

## Design rationale

- Large-argument `f64` `sin` and `cos` (#146): from 1647099 on, the 192-bit
  Payne–Hanek product `x * 2/pi mod 4` used to become a double-double remainder
  on [-pi/4, pi/4] and a quadrant, which then chose between a sine and a cosine
  kernel and flipped signs: five branches on the argument's phase, about 15.3
  ns against libm's 7.8 on `bench/std`'s `sin-large`. The same product is now
  read straight onto the pi/64 grid the smaller range already uses: the top
  seven bits, rounded, index the 128-entry table, which spans a full period,
  so quadrant, sign, and sine against cosine are all the index, and the rest is
  the signed offset from that grid point. Nothing depends on the argument but
  table addresses, which keeps the path free of mispredictions on the varied
  arguments a large range sees. The `f32` large path reads its quadrant from
  the same product, so one reduction serves both widths, and the old
  pi/2 kernels and their coefficients are gone.
- The 2/pi table is 168 bytes, least significant first, rather than 32-bit
  words. Three unaligned 64-bit loads at the byte that holds the wanted bits
  each read 64 consecutive bits, and the mantissa is shifted left by the 0 to
  7 bits that remain, which zeroes at most the product's last seven bits.
  Reading aligned 64-bit words and shifting them together ran 8.2 ns.
- Accuracy rests on an argument, not a sample: the product drops less than
  2^-125 of a pi/64 step, and the closest any `f64` comes to a multiple of
  pi/64 is 2^-61.5 of a step (`0x1.6ac5b262ca1ffp+844`, from a
  continued-fraction search over every exponent). Against a GMP reference
  over 10.6 million large arguments the worst error is 0.543 ULP for `sin` and
  0.541 for `cos`, down from 0.848 (libm 0.904); 3.3% of large results moved by
  one last bit. Results below 1647099, the special values, and the `f32` large
  path are bit-identical to before.
- `sin-large` runs in 7.47 to 7.49 ns against libm's 7.75 to 7.90 on Apple
  arm64, 0.95x to 0.97x. CORE-MATH's correctly rounded `sin` and LLVM-libc's
  large-range reduction onto pi/128 also read the reduced argument as a table
  index plus a fixed-point fraction rather than as a quadrant and a remainder.
