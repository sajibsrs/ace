# std.str

[Standard library](index.md)

`str` is a pure-Ace module of allocation-free, byte-exact string utilities.
Plain strings are read-only UTF-8 byte views, `char` is the transparent `u8`
alias, and every operation works on views or caller storage: nothing here
allocates, decodes Unicode, normalizes, or uses the locale. Each
`suffix: bool` parameter selects a boundary with `false` for the prefix and
`true` for the suffix.

```ace
import str

main(): int {
    at: usize, found: bool = str.find("hello world", "world")
    if !found {
        return 1
    }
    trimmed: string, hit: bool = str.trim("hello world", "world", true)
    if !hit || trimmed != "hello " {
        return 2
    }
    return 0
}
```

Search and ordering:

- `find(text: string, needle: string): (usize, bool)` and
  `find(text: string, needle: char): (usize, bool)` — the first byte index and
  whether it was found; an empty string needle is found at zero and absence
  returns canonical zero. The search scans eight bytes per step, filtering
  candidate positions by the needle's first and last bytes before comparing
  the whole needle; the worst case stays proportional to text length times
  needle length.
- `has(text: string, needle: string): bool`,
  `has(text: string, needle: char): bool` — occurrence anywhere; an empty
  string needle is present.
- `has(text: string, part: string, suffix: bool): bool` — one boundary test;
  an empty string part matches either boundary.
- `compare(a: string, b: string): int` — unsigned byte-wise order returning
  exactly `-1`, `0`, or `1`; after an equal prefix the shorter string sorts
  first.

Caller storage and views:

- `copy(text: string, noalias destination: mut []char): bool` — copies the
  complete text into `destination[0..text.len]`; a shorter destination is
  unchanged and fails, and the unused suffix is untouched. Empty text succeeds
  without writing. Unlike `mem.copy`, the ranges need not be equal — only the
  source and the written range must not overlap.
- `cstring(text: string, noalias destination: mut []char): bool` — copies the
  text and a trailing NUL for a C function, then passes `&destination[0]`. It
  fails when the text and its NUL do not fit or the text holds a NUL byte,
  which C would read as its end.
- `concat(parts: []string, destination: mut []char): (usize, bool)` — writes all
  parts in order and returns the complete byte count and whether it fit. A short
  destination returns `(0, false)` and its contents are unspecified; empty
  parts are valid and on success the unused destination suffix is untouched. Source bytes must not overlap the
  written destination range.
- `split(text: string, separator: string, parts: mut []string): (usize, bool)`
  and the `separator: char` overload — splits left to right on non-overlapping
  exact matches. Separators are omitted, boundaries produce empty parts, empty
  input produces one empty part, and every written part is a zero-copy view
  into `text`. The result is the number written and whether the complete
  result fit; unwritten slots are unchanged. An empty separator returns
  `(0, false)` without writing.
- `trim(text: string, part: string, suffix: bool): (string, bool)` and the
  `part: char` overload — removes at most one exact boundary match, returning
  the remaining zero-copy view and whether it matched.
- `trim(text: string): string` — removes all leading and trailing ASCII
  whitespace bytes (`0x09`–`0x0D` and `0x20`) as a zero-copy view; no Unicode
  whitespace processing.

Numeric conversion — the explicitly instantiated `parse[T]` overloads and the
ordinary `format` overloads. The `parse[T]` templates are declared
`specialized` and are implemented entirely as generic specializations (see
[Functions](../function.md#generic-specialization)): the callable surface is the
per-type specializations for `int`, `uint`, `usize`, `f32`, and `f64`, so any
other type argument is a call-site compile error naming the missing tuple:

```ace
import str

main(): int {
    n: int, ok: bool = str.parse[int]("-42")
    if !ok {
        return 1
    }
    r: usize, radix_ok: bool = str.parse[usize]("ff", u8(16))
    if !radix_ok || r != 255 {
        return 2
    }
    buf: [32]char = [char(0) : 32]
    dst: mut []char = mut buf[0..32]
    count: usize, fit: bool = str.format(dst, n)
    if !fit || count != 4 {
        return 3
    }
    return 0
}
```

- `parse[T](text: string): (T, bool)` — the default form admits exactly `int`,
  `uint`, `usize`, `f32`, and `f64`. Parsing consumes the complete nonempty
  string: no surrounding whitespace, no leading `+`, leading `-` only for
  `int` and the signed floats, and leading zeroes accepted.
- `parse[T](text: string, radix: u8): (T, bool)` — the radix form admits
  exactly the three integer types and radices 2 through 36. Digits above nine
  are ASCII letters, case-insensitive; the form accepts digits only, with no
  `0x` or `0b` prefix.
- Invalid text, an unsupported runtime radix, or overflow returns
  canonical-zero `T` and `false`; finite float underflow rounded to signed
  zero succeeds. Decimal floats accept ordinary decimal/scientific forms plus
  the exact non-finite spellings `fmt` produces (`nan`, `inf`, `-inf`).
  Finite decimals round directly to the requested IEEE width with exact
  ties-to-even, assuming the default round-to-nearest floating-point
  environment; parsing allocates nothing and uses no platform decimal parser.
  Naming any other `T` is a compile-time error at the call.
- `format(destination: mut []char, value): (usize, bool)` — overloads for
  `int`, `uint`, `usize`, `f32`, and `f64`, plus radix-taking overloads for
  the three integer types. A runtime radix outside 2 through 36 writes nothing
  and returns `(0, false)`; insufficient capacity returns `(0, false)` with the
  destination's contents unspecified. Integer radix digits are lowercase ASCII
  with no base prefix, only `int` may emit `-`, and the no-radix float
  representations are exactly the matching `fmt.print` overloads'.
- `format(destination: mut []char, format: string, ...): (usize, bool)` —
  formatted text into caller storage, chosen when the second argument is a
  `string`. The format and arguments follow [`fmt.printf`](fmt.md) exactly:
  the format is a string literal or a `const` global, and the compiler checks
  each conversion's argument type. The text is `destination[..n]`; no NUL is
  appended, so text for a C API writes `\0` in the format. A short destination
  returns `(0, false)` with its contents unspecified. It cannot be used as a
  function value.

```ace
line: [64]u8 = [0 : 64]
n: usize, ok: bool = str.format(mut line[..], "FPS %d, %.2f ms\0", fps, frame_ms)
if ok { renderer.debug_text(6.0, 6.0, string(line[..n])) }
```

Float formatting uses the same internal exact binary-to-decimal engine as
`fmt`; the two APIs therefore share one rounding implementation rather than
merely testing for matching output. Float parsing uses a companion internal
exact decimal-to-binary engine, so it does not pass `f32` input through `f64`
or depend on a target conversion routine.

## Design rationale

Why the rules in `spec/std/text.md` are shaped as they are.

- Stdlib: character and string utilities #16: text built from values is one
  checked call rather than one conversion per piece with manual offset tracking;
  one name means text into a buffer as `printf` means text to output. Capacity
  failure keeps `bool` because a caller-sized buffer is not an external failure
  warranting `Error`, and leaves the destination unspecified — for `str.concat`
  and the value overloads too — so every writer runs in one pass without
  measuring first. `str.split` instead reports its fitting parts, which remain
  valid views.
- Stdlib: character and string utilities #16: `char` improves byte-oriented
  intent without adding a second representation or conversion domain; code
  points remain `u32` because a named alias would add no behavior, and a
  separate `utf8` module keeps `str` encoding-agnostic. Free `str` functions
  keep library ownership visible and reuse Ace's existing overload and
  explicit-generic machinery without extending methods to primitives. Every
  operation works on views or caller storage, exposes capacity and failure
  explicitly, and adds no hidden allocation, decoding, normalization, locale, or
  runtime type dispatch. Compact overload sets avoid a family of
  prefix/suffix/type-specific names while their argument and explicit type
  shapes remain statically unambiguous.
