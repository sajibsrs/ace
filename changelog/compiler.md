# Compiler

- Selected x64 instructions on operands in place: each reads its operands
  from their registers and computes in its result's in two-operand form, a
  fitting constant as an immediate, a stack object as an rbp offset, floats
  through xmm directly. `bench/core` hotloop on x64: 1.71-2.80x faster.
- Fused an x64 compare into its branch or check when that is its only use in
  its block: `cmp`/`test` or `ucomis` sets the flags `jcc` reads, a float's
  parity qualifying eq and ne. `bench/core` hotloop on x64: 1.10-1.21x on
  saxpy, dot, and particle; cube-transform and length-vec3 unchanged.
- Folded an x64 load's or store's address when that is its only use in its
  block: a global in the object rip-relative, an unchecked pointer offset's
  constant into the displacement, a register index as `[base + index*scale]`
  (scale 1, 2, 4, or 8). `bench/core` hotloop on x64: dot-f64 1.36x,
  cube-transform 1.26-1.35x; the rest unchanged.
- Guarded an x64 stack-object access whose displacement, the object's rbp
  offset plus a folded constant, overflows int32: the object's address is
  materialized in rax by `lea` and the access keeps the constant alone,
  where it had wrapped to a wrong address.
- Let an inline function return any type but a type parameter: a struct,
  tuple, or string result joins after the expansion as one SSA variable per
  leaf, and one of over 16 leaves is stored in memory, as a call's result is.
  Release auto-inlining takes aggregate results too. `bench/core/raytrace`
  (new, with a C twin) with its helpers inline: 0.842 s to 0.602 s.
- Bound an inline expansion's receiver or `mut` parameter to the argument's
  own SSA variables when it is a local or a field of one, keeping the local in
  registers unless `&`, an unexpanded call, or the inline body needs its
  address. `bench/core/raytrace` release: 0.643 s to 0.390 s (0.336 s with `hit` and `trace` inline); calls/method 2.2x.
