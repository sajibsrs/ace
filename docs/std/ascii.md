# std.ascii

[Standard library](index.md)

`ascii` provides allocation-free operations with explicitly ASCII-only
semantics. It does not decode Unicode, normalize text, or consult the locale.

- `control(byte: u8): bool` recognizes `0x00` through `0x1F` and `0x7F`.
- `lower(byte: u8): u8` maps `A` through `Z` to lowercase and leaves every
  other byte unchanged.
- `equal(a: string, b: string, sensitive: bool): bool` compares exact bytes
  when `sensitive` is `true`; when false, it ignores ASCII letter case while
  leaving all other bytes unchanged.

