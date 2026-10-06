# Compiler

- The self-hosted lexer keeps every error in a `Diag` list (`Lexer.diags`)
  with its position and offending byte, and `lexer.report` words each as C
  does (`'$'`, `0xC3`, `'\q'`); consecutive bad bytes no longer drop errors,
  `next` returns only the token, and a run ends with C's `N errors` line.
- The self-hosted parser writes `@file` as the root file's base name, the
  path relative to its own directory, as C's `build_path` does; C's `std/<rel>`
  mapping for files under the compiler's std is not ported.
- Both parsers parse binary operators by precedence climbing: one `level`
  lookup per operator, not a call per precedence level for every operand.
  The self-hosted parser runs 1.26–1.66x faster (`shader.ace` 1.9x → 1.1x of
  C); the C parser 1.02–1.04x. ASTs are unchanged.
- An arithmetic operator on two operands of one non-numeric type reports
  `operator * does not apply to [2]i64` rather than an operand type mismatch.
- `mut` on a fixed array type now names the fix in its diagnostic: the array is
  already writable, and `mut name[..]` takes a writable view (both parsers).
- Globals join function reachability: a global is emitted only once a compiled
  function references its symbol (C exports always, everything without
  `main`), so importing `sys` for a type no longer links its ~70 constants
  (`bench/core/raytrace` 62456 → 37112 bytes).
- Front end roughly halved (`bench/std/string` check 119.6M → 61.9M
  instructions): built-in std modules are recognized by comparing the
  module's resolved path under the std root, not by `realpath` on candidate
  paths, and the lexer compares a keyword's first byte before its length.
- A generic stamp-time error is followed by a note per instantiation that led
  to it (`in 'outer[string]' instantiated here`), innermost first, from a
  chain of stamp frames on the checker; a `void` type argument is spelled
  `void`, not "no value".
- A parser check made after the construct it names is parsed reports at a
  saved token (`perr_at`); a `mut` that heads no call, at statement level or
  after `defer`, names its `mut`, not the token after the expression.
- A `mut` argument, an expression statement, and a range are positioned at
  their `mut`, first token, and `..`; like binary expressions, they took the
  token after their contents.
- A binary expression is positioned at its operator, like unary, assignment,
  and comparison-chain nodes; it took the token after its right operand, so
  its diagnostics named the next line or a closing brace.
- A forward call's arguments keep declaration-before-use: resolving the call
  at end of file widens only its callee's view, so a function value declared
  after the call is an error wherever the callee is declared. An argument
  whose check fails during overload selection is reported once, not twice.
- Laid out the arm of a direct call to a `@cold` function, with the blocks
  reached only from it, after the function's other blocks, so the common path
  falls through; and let a phi share a loop phi's location when it is that
  phi's operand, removing an `if` join's copies from a loop's hot edge.
  `bench/std` array-push 1.35x to 1.0x of C.
- Let an inline function return a type parameter, its result typed by the
  stamp each expansion already makes, and scanned a generic inline body for
  the receiver and `mut` parameters needing storage once per instantiation
  rather than counting it as needing all; `std/array` `get` is now inline
  and its locals stay in registers. `bench/std` array-get 2.4x to 1.0x of C.
- Release cleanup threads a jump into a branch whose condition is constant
  along that edge (an inline call's `null` error, a switch's dispatch) to the
  arm it selects, giving that arm a phi for each of the branch block's phis
  it uses. `bench/std` array-push 1.5x to 1.1x of C on x64.
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
- Import cycle diagnostics list only the loop, in import order
  (`a -> b -> c -> a`): the loading stack is innermost-first, so it is cut at
  the repeated module and printed reversed, dropping the importers before it.

- Compile only reachable functions: `main` and the C exports are compiled
  first, then each still-undefined link symbol naming an Ace function, until
  none is left; an object without `main` compiles all. Release `bench/core`
  and `bench/std`: compile 1.3–1.9x faster, machine code 1.7–6.7x smaller.
- Lowered x64 division by a constant as arm64 does, through the shared
  `div_magic`: shifts and masks for a power of two, else a multiply-high by
  the reciprocal. `bench/core/cpu` on x64: 1.42x faster; `bench/std/string`
  format-f64 2.0x, parse-int and parse-hex 1.3x.
- Five IR-verifier messages no longer end in a `%s` fed the helper's spare
  `int` argument: `fprintf` ignores excess arguments, so the directive only
  read `0` as a string and printed `(null)`.
- A module path or `pkg build` output path past its 4096-byte buffer is now
  an error (`module path is too long`, `build path ... is too long`) rather
  than a silent truncation, and `parse_stmt`'s identifier case returns instead
  of falling into `default`; gcc builds warning-free.
- `ace pkg run` passes the arguments after the first `--` to the program
  unchanged (as `cargo run --` does); flags before it stay the build's.
- `ace pkg --dir=DIR` starts the `ace.pkg` walk-up in `DIR`, as a prior `cd`
  would, for every command but `init`; it is read only before a `--`.
- `std/` is found beside the real executable (`_NSGetExecutablePath`, or
  `/proc/self/exe`, then `realpath`) rather than `argv[0]`, so `ace` run by
  bare name from `PATH` or through a symlink finds it.
- Diagnostics name a module type as written (`net.SocketType`, not
  `darwin-arm64$SocketType`): the loader records each module prefix's display
  name (a directory module's name for every file beneath it, else the stem)
  and `type_name` maps prefixed names through it.
- The Ace lexer moves from `experiment/lexer/` to `compiler/`, starting the
  self-hosted compiler beside the C bootstrap in `src/`; `compiler/check.sh`
  diffs its token stream against `ace --tokens` over every repository file.
- `compiler/parser.ace` ports `src/parser.c` whole, declarations and
  `@when` included: nodes in one flat array addressed by `u32` (0 absent),
  children as runs in a shared array, the file lexed up front, the first error
  ending the parse; `Out` moves to `compiler/output.ace`. `main.ace --ast`
  dumps the tree, and `compiler/check.sh` diffs it against `ace --ast`.
- `Error` fields and enum `.name` read through pointers (`p.name`, `pp.code`)
  via `deref_value`, null-checked in debug, where lowering had rejected them.
  `--ast` prints a selector `x.f` as `selector`, no longer `field` like a field
  declaration; the self-hosted dump now prints `x.kind.name` with no table.
- A syntax error quoting an identifier prints the whole name, in a buffer
  sized to the longest identifier; an 80-byte one had cut it to 79 bytes,
  losing its closing quote. The unreachable default link-name length check is
  gone.
