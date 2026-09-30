# Compiler

- Selected x64 instructions on operands in place: each reads its operands
  from their registers and computes in its result's in two-operand form, a
  fitting constant as an immediate, a stack object as an rbp offset, floats
  through xmm directly. `bench/core` hotloop on x64: 1.71-2.80x faster.
- Fused an x64 compare into its branch or check when that is its only use in
  its block: `cmp`/`test` or `ucomis` sets the flags `jcc` reads, a float's
  parity qualifying eq and ne. `bench/core` hotloop on x64: 1.10-1.21x on
  saxpy, dot, and particle; cube-transform and length-vec3 unchanged.
