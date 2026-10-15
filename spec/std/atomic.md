## Stdlib: `atomic`

25. `atomic` (implemented on darwin-arm64) — indivisible, lock-free operations on
    shared integer words; their ordering rules are Memory, "Carved (memory
    model)". Every operation is an exact `u32` and `u64` overload (`uint` shares
    the `u64` overload where it is `u64`; `usize` is a distinct type and takes an
    explicit pointer conversion, as `bits` does); there is no generic
    atomic type, no narrower width, and no pointer overload (a pointer travels
    as `usize` through an explicit conversion). Operations that write take
    `p: mut *T`, `load` takes `p: *T`. Each is an `intrinsic` declaration
    (Stdlib #8) and allocates nothing.
    - Ordering is part of the name, never an argument. The unsuffixed name is
      sequentially consistent; weaker orders take a suffix, so an invalid
      pairing cannot be written:

      | Operation | Orders |
      |---|---|
      | `load(p): T` | seq_cst, `_acquire`, `_relaxed` |
      | `store(p, value)` | seq_cst, `_release`, `_relaxed` |
      | `exchange`, `add`, `sub`, `and`, `or`, `xor` `(p, value): T` | seq_cst, `_acquire`, `_release`, `_acq_rel`, `_relaxed` |
      | `cas(p, expected, desired): (T, bool)` | seq_cst (both orders); `_acq_rel` (success acq_rel, failure acquire); `_relaxed` (both relaxed) |
      | `fence()` | seq_cst, `_acquire`, `_release` |

    - `exchange` and the arithmetic and bitwise forms return the previous
      value. `add` and `sub` wrap modulo 2^width, as `bits.wradd` does, and
      never report overflow. `cas` writes `desired` only when the location holds
      `expected`, and returns the value it observed with whether it wrote; it
      never fails spuriously.
    - `p` must be non-null and aligned to `sizeof(T)`; debug builds trap
      otherwise, release builds do not check. A target that cannot perform an
      operation at its width as one native instruction sequence rejects the
      program; there is no lock or library fallback.
