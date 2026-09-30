# Compiler

- Selected x64 instructions on operands in place: each reads its operands
  from their registers and computes in its result's in two-operand form, a
  fitting constant as an immediate, a stack object as an rbp offset, floats
  through xmm directly. `bench/core` hotloop on x64: 1.71-2.80x faster.
