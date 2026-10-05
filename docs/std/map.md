# std.map

[Standard library](index.md)

`map` provides a hash map from keys to values over the process allocator. A
`Map[K, V]` holds `len` live entries in `slots`, whose length is the capacity:
a power of two, or zero for the canonical zero value and the empty literal
`Map[K, V]{}`. Those are valid empty maps holding no storage, so no constructor
is required and the first `set` allocates. Entries live in open-addressed slots
probed linearly from the key's hash. The table doubles and rehashes into fresh
storage whenever an insertion would exceed three quarters full; the load factor
is fixed. Type arguments are explicit, as everywhere: `map.Map[string, int]`,
`map.Map[string, int].new(64)`.

The key type is one of the integer types (`int`, `uint`, and `char` included),
`usize`, `bool`, or `string`. Any other key type is a compile-time error at the
instantiation. `string` keys compare by content.

- `Map[K, V].new(capacity: usize): (Map[K, V], Error)` creates an empty map
  that holds `capacity` entries without growing. Zero allocates nothing.
  Failure returns `MapErr.allocation`.
- `wipe()` releases the storage and leaves the map empty.
- `set(key: K, value: V): Error` stores `value`, replacing any earlier value
  for `key`. Failure returns `MapErr.allocation` and leaves the map unchanged.
- `get(key: K): (V, bool)` returns the stored value and true. A missing key
  returns `blank(V)` and false.
- `has(key: K): bool` reports whether `key` is present.
- `unset(key: K): bool` removes `key` and reports whether it was present.
- `reserve(count: usize): Error` ensures room for `count` entries in total; it
  never shrinks.
- `clear()` drops every entry and keeps the storage.

`wipe`, `set`, `unset`, `reserve`, and `clear` mutate, so the call site states
`mut`. `get` and `set` copy values, so no accessor hands out a slot address
that growth could invalidate. `len` is readable directly.

```ace
import map

ports: map.Map[string, int] = map.Map[string, int]{}
defer mut ports.wipe()

err: Error = mut ports.set("http", 80)

if err != null {
    /* allocation failed; ports is unchanged */
}

port: int, found: bool = ports.get("http")   /* 80, true */
gone: bool = mut ports.unset("http")         /* true */
```

Iteration order is unspecified and iteration is not offered. Removal shifts
later entries back over the hole, so no tombstones accumulate.

## Design rationale

Why the rules in `spec/std/collections.md` are shaped as they are.

- Stdlib: `array`, `map`, and `file` #19: direct specialized calls let `hash`
  and `same` expand `inline` (Functions & ABI #8) and preserve a valid zero
  value, which function-valued fields would forfeit at one indirect call per
  operation.
