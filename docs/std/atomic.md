# std.atomic

[Standard library](index.md)

Indivisible, lock-free operations on shared `u32`, `u64`, and `usize` words.
With thread creation and joining, they are the only operations that
synchronize execution contexts; the ordering rules are in
[Memory](../memory.md#memory-model). Each operation is a compiler-backed
`intrinsic` declaration: applications call it as an ordinary public function,
may store an exact overload in a typed function value, and nothing allocates or
calls a runtime library.

```ace
import atomic

count: u32

bump(): u32 {
    return atomic.add(mut &count, u32(1)) // the previous value
}
```

An untyped integer literal does not choose between the overloads, so values are
spelled `u32(1)`. `uint` uses the `u64` overload on Apple arm64; `usize` has
its own overloads, so a pointer-sized word needs no conversion.

## Orderings

The ordering is part of the name. The unsuffixed name is sequentially
consistent; a suffix names a weaker ordering, so an invalid pairing cannot be
written:

| Operation | Orderings |
|---|---|
| `load(p)` | unsuffixed, `_acquire`, `_relaxed` |
| `store(p, value)` | unsuffixed, `_release`, `_relaxed` |
| `exchange`, `add`, `sub`, `and`, `or`, `xor` `(p, value)` | unsuffixed, `_acquire`, `_release`, `_acq_rel`, `_relaxed` |
| `cas(p, expected, desired)` | unsuffixed, `_acq_rel`, `_relaxed` |
| `fence()` | unsuffixed, `_acquire`, `_release` |

`load` takes `p: *T`; every operation that writes takes `p: mut *T`.
`exchange`, `add`, `sub`, `and`, `or`, and `xor` return the previous value.
`add` and `sub` wrap modulo 2^width and never report overflow.

## Compare and swap

`cas` writes `desired` only when the word equals `expected`, and returns the
value it observed with whether it wrote. It never fails spuriously, so a retry
loop reads the observed value instead of loading again. Its `_acq_rel` form
acquires on failure; `_relaxed` orders neither outcome:

```ace
claim(flag: mut *u32): bool {
    seen: u32 = 0
    won: bool = false
    seen, won = atomic.cas_acq_rel(flag, u32(0), u32(1))
    return won
}
```

## Checks and targets

`p` must be non-null and aligned to the word. A debug build traps on a null
address (`null use violation`) and on a misaligned one
(`misaligned atomic access`); a pointer built in Ace is already aligned by its
construction check, so the second trap guards pointers that come from C. A
release build checks neither.

The darwin-arm64 target lowers to the ARMv8.1 atomics (`swp`, `ldadd`, `ldclr`,
`ldset`, `ldeor`, `cas`) with their acquire and release bits, `ldar` and
`stlr` for an ordered load or store, plain `ldr` and `str` for a relaxed one,
and `dmb` for a fence. `sub` is `ldadd` of the negated operand and `and` is
`ldclr` of the inverted one.

The linux-x64 target relies on x64's total store order: a load and a relaxed,
release, or acquire store are plain `mov`, a sequentially consistent store is
`xchg`, and `exchange`, `add`, `sub`, and `cas` are `xchg`, `lock xadd`, and
`lock cmpxchg`, which are sequentially consistent whatever their name. `and`,
`or`, and `xor` are one locked instruction when the result is unused and a
`cmpxchg` loop when it is read. Only `fence()` emits an instruction (`mfence`);
the acquire and release fences only stop the compiler from reordering.

## Design rationale

Why the rules in `spec/std/atomic.md` are shaped as they are.

- Module of intrinsics (Stdlib #25): no syntax, no wrapper type; it follows
  `bits`, so the compiler surface is one IR operation and the library can add
  an `Atomic[T]` type later without changing it.
- Ordering in the name (Stdlib #25): Ace has no compile-time arguments, and a
  runtime ordering would need a switch per call. Names make every pairing
  valid by construction, and the short name is the safe one.
- `seq_cst` default: on arm64 its load and store are `ldar` and `stlr`, the
  same instructions as acquire and release, so the safe default costs nothing
  there.
- `u32`, `u64`, and `usize` only: a futex-style word needs 32 bits, a counter 64,
  and an index or address a pointer-sized word; narrower atomics are rarely
  needed and easy to misalign.
- No pointer overload: a generic one would break the nongeneric-intrinsic rule,
  and a pointer travels as `usize` through an explicit conversion.
