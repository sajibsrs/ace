# std.utf8

[Standard library](index.md)

`utf8` provides allocation-free UTF-8 encoding and decoding over byte strings.
Code points are plain `u32` values; Ace has no separate code-point type.
Nothing decodes implicitly: iterating a `string` still yields bytes.

Validation is strict per RFC 3629. Overlong forms, surrogates
`0xD800..0xDFFF`, values above `0x10FFFF`, truncated sequences, and unexpected
continuation bytes are invalid.

- `decode(text: string, at: usize): (u32, usize, bool)` decodes the sequence
  starting at byte `at` and returns the code point, its width (1–4), and
  `true`. Invalid input returns `(0, 1, false)`, so a loop that adds the width
  always advances past a bad byte. `at >= text.len` returns `(0, 0, false)`.
- `encode(value: u32, destination: mut []char): (usize, bool)` writes the
  complete encoding and returns its width and `true`. An invalid code point
  or a destination that is too short returns `(0, false)` and writes nothing.
- `width(value: u32): usize` returns the encoded width, or 0 for an invalid
  code point.
- `valid(text: string): bool` reports whether the whole text is well-formed.
  Empty text is valid.

```ace
at: usize = 0

for at < text.len {
    cp: u32, n: usize, ok: bool = utf8.decode(text, at)

    if !ok {
        /* one bad byte; substitute, count, or reject */
    }
    at += n
}
```

It does not normalize, fold case, segment graphemes, or consult the locale.
