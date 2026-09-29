# Ace — Implementation History

The sole historical record of implemented compiler and language work. Entries
state what landed and the essential implementation approach; `SPEC.md` contains
current normative rules, and Git preserves patch-level detail.

## Bootstrap compiler

- Implemented the C11 lexer, recursive-descent parser, semantic checker, and
  direct AArch64/Apple-arm64 emitter without third-party dependencies.
- Emission writes Mach-O objects directly, follows the platform ABI, resolves
  forward calls with narrow fixups, and emits minimal multi-file DWARF line
  tables. Inline recursive layouts are rejected; forward type names are allowed
  only through pointer-only paths whose fixed size is already known.
- Added `--tokens`, `--ast`, `--report`, object emission, and executable
  linking. The driver passes ordered objects, archives, library paths, libraries,
  and Apple frameworks to `cc` through an argument vector rather than a shell.
- Added mutually exclusive checked/release emission modes, checked by default,
  with statically elided runtime guards and aborting diagnostics for dynamic
  array, slice, and string indexing and slicing bounds. Bounds diagnostics now
  identify the failing source file, line, and column; release objects retain
  no bounds failure machinery or metadata.
- Renamed the checked emission mode to debug: `--debug` replaces `--checked`
  with no alias, `EMIT_DEBUG` replaces `EMIT_CHECKED`, and the spec section
  is Safety & Debug Builds. Individual checks keep their names (checked
  arithmetic, the `tests/checked_*` fixtures).
- Made release the default emission mode for direct compilation and
  `ace pkg build`/`run`; the core and package test runners pass `--debug`
  explicitly so tests keep their checks.
- Made release builds expand small leaf functions no source marks `inline`: a
  body of at most 24 syntax nodes calling only conversions and compiler-backed
  operations goes through the same expansion as `inline`, the verdict kept per
  declaration. The spinning-cube math fell from 99 to 76 ns per frame, matching
  hand-marked `inline`; the std and package benchmarks, already marked by hand
  where it paid, stayed within noise with identical checksums.
- Moved the object writer out of `emit.c` into `macho.c` behind `backend.h`
  (IR migration step 1a): sections, symbols, relocations, fixups, line rows,
  DWARF, and the Mach-O image, plus the shared runtime-routine flags, live in
  one `Obj` that every code generator emits into. Objects are byte-identical
  across the test, bench, and demo sources in both modes.
- Added the per-function SSA IR and its lowering (IR migration step 1b):
  `ir.c` builds, verifies (structure and dominance), and prints it; `lower.c`
  builds SSA during one walk of a checked function (Braun et al.) for scalar
  integer and `bool` code: locals, assignment, `if`, general `for`, labeled
  `break`/`continue`, direct calls, casts, and comparison chains. Debug marks
  checked integer operations as trapping `.chk` instructions, `main` reports
  leaks before returning, and release turns self tail calls into a loop. `--ir`
  prints the root file's functions, or why one was not lowered. `tests/ir`
  holds expected output for both modes.
- Fixed narrow (8/16-bit) integers leaving their range while in a register:
  casts to them now truncate and re-extend, `~` on `u8`/`u16` is masked, and
  `<<` (both modes) and unchecked `+`, `-`, `*`, and unary `-` (release)
  re-extend their result, per Safety & Debug Builds #6; a peephole folds the
  resulting back-to-back extends.
- Compiled lowered functions through a new AArch64 backend (IR migration step
  1c): `arm64.c` selects instructions (compare-and-branch, `cbz`, add/sub/cmp
  and bitmask immediates, `madd`/`msub` with a register addend, constant
  division), leaves SSA by parallel moves at the end of each predecessor after
  splitting critical edges, allocates registers by linear scan over one
  interval per value (constants rematerialized, call-crossing values in
  callee-saved registers or reused stack slots, hints coalescing phis), and
  gives a leaf without spills no frame. Narrow values keep the Apple PCS
  32-bit register form. Debug checks branch to one failure call per function;
  a function past conditional-branch range is re-emitted with far branches.
  Every other function still goes to the AST emitter, byte-identical.
  `tests/ir-diff.sh` requires identical output from random scalar programs
  under two compilers. On `scaling-corpus.sh long 20000` machine code
  shrinks 44% and compile time 5-8%.
- Extended the IR to memory and the scalar ABI (IR migration step 2a): `ptr`
  values; `slot`, `global`, `load`/`store` (folded offset, volatile),
  `padd`/`psub`, and `calli`; locals whose address is needed, `mut`
  parameters, and method receivers live in memory; enums, function values,
  extern and variadic calls, and range loops lower. Debug null-checks loads,
  stores, and calls through pointer and function values, and checks pointer
  arithmetic (Pointers #9). `arm64.c` rematerializes stack-object addresses as
  it does constants, folds a single-use global or unchecked pointer offset
  into the access (`adrp` + page offset, immediate, or scaled register
  index), passes arguments past the eighth and variadic ones on the stack
  from an outgoing area at the frame's base, and reaches externs by branch
  relocation. Every `tests/emit` fixture passes in both modes, and
  `tests/ir-diff.sh` now also covers globals, pointers, `mut` parameters, a
  method, function values, and range loops. Release self tail recursion
  also loops through `mut` parameters and method receivers (their address is
  an SSA variable), unless a local lives in the frame. The `tests/run.sh`
  code-shape checks on functions the IR now compiles state backend-neutral
  properties instead of AST-emitter register policy. On `compile-corpus.sh 5000`,
  compile time rises 10%, all of it from lowering these functions instead of
  the AST emitter; benchmark runtime is unchanged.
- Extended the IR to floats (IR migration step 2b): `f32`/`f64` values reuse
  the arithmetic, `neg`, and comparison ops by type (IEEE: every ordering and
  `eq` false and `ne` true on a NaN, so `not(lt)` is not `ge`); `cast`
  converts between integers and floats (`scvtf`/`ucvtf`, saturating
  `fcvtzs`/`fcvtzu` then a narrow result wrapped) and between widths; a float
  `const` holds its bit pattern. Float arithmetic is never checked or
  contracted. `arm64.c` allocates
  v registers in the same linear scan as a second class (caller-saved
  v16-v28 and v0-v7, callee-saved d8-d15 saved in the frame, v29-v31
  scratch), passes floats in v0-v7 and on the stack at natural size (a
  variadic `f32` promoted to `f64`), returns them in v0, selects `fcmp`
  (against `#0.0` for a zero) with the AST emitter's NaN-false conditions, and
  rematerializes constants by `fmov` or a literal load from a per-function
  pool. IR coverage of the `tests/emit` and bench roots rises from 232 to 288
  functions; the rest of the float code waits on intrinsic and `inline`
  calls, arrays, and structs. Every `tests/emit` fixture passes in both
  modes, `tests/ir-diff.sh` now also generates float programs, and compile
  time and benchmark runtime are unchanged.
- Extended the IR to structs, tuples, and `Error` values (IR migration step
  2c1). The IR stays scalar: `ir_leaves` flattens a struct or tuple into its
  scalar leaves (at most 16) in offset order. A local is one SSA variable per
  leaf, or a slot read and written by leaf when its storage is needed;
  literals, field reads and writes, whole copies, fields of call results, and
  tuple destructuring (new binders, existing places, or both) lower to leaf
  moves. Parameters, arguments, and `ret` carry leaves, grouped into parts by
  an `IrSig` when one is an aggregate, and a new `proj` reads leaf k of a
  call's aggregate result. `Error` is a `u64`: `Domain(code)` ORs the domain
  id with the code's raw bits, `Domain.entry` is a constant, `.code`
  truncates, `.domain` shifts, and `null` is zero; `.name` and `.message`
  stay with the AST emitter. `arm64.c` maps leaves onto the AAPCS64 classes
  of `assign_cc`: a floating aggregate one v register per leaf, a word
  holding one leaf that leaf's x register, a word shared by smaller leaves
  through memory (the sender stores the leaves and loads the word, the
  receiver the reverse), over 16 bytes by the address of a caller-made copy,
  a result over 16 bytes through the buffer in x8, and past the registers on
  the stack; the callee saves incoming words and addresses before its
  parameters move in parallel, then loads the rest. IR coverage of the
  `tests/emit` and bench roots rises from 331 to 548 of about 1,045
  functions. Every `tests/emit` fixture passes in both modes, a new fixture
  crosses both backends with each shape, and `tests/ir-diff.sh` now also
  generates random records and `(T, Error)` results, keeping some functions
  on the AST emitter by a `switch`. Compile time is unchanged; the call
  benchmark's 64-, 24-, and 8-byte struct loops run 30%, 9%, and 9% faster.
- Extended the IR to strings and slices, and to calls of stamped generics
  (IR migration step 2c2). A string or slice is two leaves, a `ptr` and a
  `u64` length, so locals, parameters, results, fields, and destructuring
  reuse the aggregate lowering and its AAPCS64 mapping. A new `str` gives the
  address of a literal's bytes, interned through `obj_string` in the shared
  `Obj` so both backends number `__ace$str$N` from one counter; a module
  `const` string lowers as its literal. `.len` reads the second leaf, `x ==
  null` compares the first with zero, `null` is two zero leaves, and
  `string(b)` and `[]u8(s)` keep their leaves. Indexing and slicing stay with
  the AST emitter until bounds checks lower. Calls to a template stamp, a
  written specialization, or a generic method, and `larger[int]` as a value,
  take their symbol from `fn_call_mangle` and `fn_value_mangle`, shared with
  the AST emitter. A `bool` alone in an incoming aggregate word is now
  zero-extended, since AAPCS64 leaves the word's other bits unspecified,
  and a release self tail call rebinds every leaf of an aggregate parameter,
  so such functions still run in one frame.
  Functions left to the AST emitter across the `bench` and `demo` roots fall
  from 149 to 129, those kept out by a string or slice from 75 to 16, and
  `bench/core/calls` `main` now lowers. `tests/ir-diff.sh` also generates
  strings and generic calls.
- Shared a loop phi's AArch64 location with the value it carries around the
  back edge (IR migration step 2c2b). Allocation gives each value one
  interval in block order, so a phi also used after its loop spans the whole
  body and its back-edge value took a temporary, copied back each iteration.
  When the phi is dead from that value's definition on and the value is not
  live into the phi's block, the value now joins the phi's interval and is
  computed in its register or slot, taking the copy off the loop-carried
  chain: the call benchmark's float accumulation runs 40% faster and its
  24-byte struct loop 2% faster; other benchmarks are unchanged.
- Lowered string and slice indexing and slicing to IR, with their bounds
  checks (IR migration step 2c3a). An element is a place at `padd ptr, i *
  size`, so reads, writes, compound assignment, `&s[i]`, and fields of
  elements reuse the place lowering; a slice is `padd ptr, lo * size` and
  `hi - lo`. A debug check is a new `check` of a comparison, trapping when
  false: an index is checked nonnegative when signed, then below the length;
  a slice's signed bounds nonnegative, then `lo <= hi`, then `hi <= len`,
  each skipped where sema proved it. `arm64.c` fuses the comparison into a
  branch to an out-of-line stub per site after the body, which passes
  `path:line:col: bounds violation` to `__ace$bounds_fail`; `obj_located`
  now formats that message for both backends. Slicing now reads the source
  descriptor once, then evaluates `lo`, then `hi`, in both backends (the AST
  emitter evaluated `hi` first and read the pointer last), and sema records
  the type of assignment targets and address operands on their nodes. Arrays
  stay with the AST emitter. A value `a && b` or `a || b` now branches on
  `a` and joins `b`'s own value with the constant `a` decided, so `b`'s
  comparison sets the bool, and widening a `u8`, `u16`, or `bool` to 64 bits
  emits no instruction, since it is held zero-extended. Functions lowered
  across the `bench` and `demo` roots rise from 218 to 273 of 629. In release, `str.copy`'s loop shrinks
  to four instructions, and `bench/std/string` runs `utf8-valid` 27%,
  `format-f64-shortest` 10%, and `copy-64` 6% faster; the other benchmarks
  are unchanged. `tests/ir-diff.sh` also indexes and slices a `mut []u8` and
  a string window between guard bytes, and the bounds fixtures check the
  exact diagnostic location.
- Lowered arrays to IR (IR migration step 2c3b). An array's leaves are its
  elements, as AAPCS64 flattens it, so one of up to 16 leaves travels like a
  struct; a local, parameter, or field holding an array lives in memory, where
  a constant index is an offset on the access and any other a `padd` checked
  in debug below the fixed length (a signed one first nonnegative), and a
  slice's length is the array's. An aggregate of over 16 leaves stays in
  memory: a parameter arrives by address and an argument is a copy, as
  AAPCS64 passes it (an array parameter over 16 bytes, not a floating
  aggregate, likewise lives at its incoming address); copies and `[v : n]` fills are word stores, looping past
  16 words (a fill repeats `v`'s bytes in a word). A new `bitcast` lowers
  `bits.cast`, and `sys.argc`, `sys.arg`, `sys.envc`, and `sys.env` read
  Darwin's accessors in IR. Functions lowered across the `bench` and `demo`
  roots rise from 334 to 572 of 680 (compiler-backed declarations excluded).
  `bench/std/collections` runs `map-get`, `map-miss`, and `map-unset` 12-19%
  and `map-set` 7-10% faster, and `bench/std/string` `format-f64-shortest`
  28%; the other benchmarks are unchanged. `tests/ir-diff.sh` also indexes,
  slices, and copies a `[22]u8` struct field between guard bytes.
- Shared an AArch64 cast's location with its operand when the operand's
  register form already holds the result (a same-width or zero-extending
  integer cast), its interval lies within the operand's, and no value
  computed into the operand's location is defined while it is live. A loop
  index cast to `usize` is then read in place by the element load, as
  `tests/emit/basics/register_operands` requires, whose check now looks at
  the load's index register. Each benchmark binary sheds 96-132
  instructions; `bench/std/string` `copy-64` runs 5% faster, the rest are
  unchanged.
- Lowered compiler-backed calls and aggregates of over 16 leaves to IR (IR
  migration step 2d), finishing step 2. `bits.*` become `popcnt`, `clz`,
  `ctz`, `bswap`, `rotl`, and `rotr`; `math.sqrt`, `floor`, `ceil`, and `fma`
  become `sqrt`, `floor`, `ceil`, and `fma` (one `fsqrt`, `frintm`, `frintp`,
  or `fmadd`, rounding once). `mem.*` and `fmt.printf` call the private byte
  routines through `rtcall` (arguments in x15-x17, clobbering only x8,
  x15-x17, x30, v24-v31, and the flags, so a value live across one avoids
  them) or the allocator registry, and a contract failure is an unlocated
  `check`. A result of over 16 leaves is written through a `result` pointer,
  and a call passes the caller's memory as a last `sret` argument in x8;
  `blank(T)`, big tuple literals, and their destructuring move through
  memory. In debug, a call of a function with a `noalias` parameter stays
  with the AST emitter until contract checks lower (step 3). Only calls of
  `inline` functions and one computed global constant still keep a function on the AST emitter across the `bench` and
  `demo` roots. `bench/std/math` runs `pow-f64` 29%, `sin`/`cos` 44-51%, and
  `string` `copy-64` 65% and `find-4096` 24% faster; packages are unchanged.
- Lowered `inline` calls to IR (IR migration step 3a). A call expands in
  place: `inline_copy`, shared with the AST emitter, stamps a generic body and
  copies the callee's parameters and body with fresh locals; each argument is
  evaluated in order and bound, a `mut` parameter or receiver to the
  argument's own storage and any other to a copy; each `return` writes an SSA
  result variable and jumps to a join block, where the variable is read. Inlined
  statements keep the call site's line, debug checks keep their own source
  locations, and a function value of an `inline` function names its out-of-line
  symbol. A function that expands a call keeps its self tail calls, as before.
  A cycle of inline calls, a body that never returns, or 16 nested expansions
  keep the function on the AST emitter. Release lowers all 561 functions of
  the `bench` roots and their std imports (516 before) and 1,459 across the
  package benchmarks and demos (1,330); those left there wait on `union`
  (113), `defer` (18), `switch` (14), and computed global constants (12).
  Each benchmark binary sheds 2-15% of its machine code and compile time is
  unchanged. `bench/std/math` runs `exp` and `log` 36-48%, `sin-large` 32%, and
  `classify` 15% faster; `bench/std/string` `parse-int` and `parse-hex` 29-31%,
  `trim` 11%, and `format-f64-shortest` 6% faster; the map workloads read 7-13%
  slower on byte-identical code, a placement shift `HEAD` reproduces with 48
  bytes of padding; the other benchmarks are unchanged. `tests/ir-diff.sh`
  also generates `inline` functions with every parameter form.
- Lowered `switch` on a scalar to IR (IR migration step 3b). The scrutinee is
  evaluated once, then each pattern, any expression and evaluated only when
  reached, is compared with it in source order, as the AST emitter tests them:
  a chain of `ne` branches whose not-taken side is the next test and whose taken
  side is the case, so the tests lay out ahead of the bodies as a run of `cmp`
  and `b.eq`. A miss on the last test goes to `default` or past the switch; a
  case ends past the switch or, when its last statement is `fallthrough`, in the
  next case; a plain `break` still names the enclosing loop. A union scrutinee
  keeps the function on the AST emitter until tagged unions lower. A leaf
  dispatch needs no frame and compares small constants as immediates (a
  five-case one shrinks from 29 instructions to 18). Release lowers 1,465
  functions across the package benchmarks and demos (1,459), the `bench` roots
  having none; those left there wait on `union` (121), `defer` (18), and
  computed global constants (12). `pkg/regex` loses 1.3 KB of machine code
  (`parse_item` 484 to 232 instructions), its matcher keeps its instruction
  counts, and its benchmark moves within placement noise. A dense dispatch
  loop runs 2.8% faster, most of its time being mispredicted branches. Compile
  time is unchanged. `tests/ir-diff.sh` now generates scalar switches (literal,
  variable, and computed patterns, several to a case, `fallthrough`, `default`),
  and an `asm` statement replaces the `switch` that kept some functions on the
  AST emitter.
- Lowered `defer` to IR (IR migration step 3c). A `defer` evaluates its call's
  arguments where it stands, each read-only one into a fresh variable (a
  literal or constant global is left as it is) and each `mut` argument or
  receiver into the address of its storage, as does a function-value callee;
  the call over those names is lowered again before every `return` and at the
  fall-off end, last defer first, carrying the defer's line. The result is
  already SSA values or the caller's memory, so no save area is needed. A
  `return` of a self call keeps its branch only while no defer is pending. A
  deferred call of a contract method keeps the function on the AST emitter.
  Release lowers the 18 functions that waited on `defer` across the `bench`
  roots, package benchmarks, and demos; three of them stay behind on an `Error`
  value they use. `pkg/regex` sheds 2.0 KB of machine code and the
  `http-simple-server` demo 0.9 KB; runtime is unchanged, the cleanup paths
  being cold. `tests/ir-diff.sh` now defers notes of parameters.
- Lowered tagged unions as values to IR (IR migration step 3d1). A union lives
  in memory, a slot or the caller's address, and is never split into SSA
  values; on its own its leaves are the words the ABI moves, `u64` for each
  full word and a `u32` for a last half word, and one of over 16 bytes travels
  by address like a large aggregate. `Event.key(42)` stores the payload where
  it lives, then the `i32` tag; `Event.quit` stores only the tag. Assignment
  and copies move bytes, and `==`/`!=` compare the tags, then the active
  payload as the AST emitter does (a nested union by its own rule, a float by
  value, anything else word by word, up to 64 bytes). A union inside a struct,
  tuple, or array, and `switch` on a union, keep the function on the AST
  emitter (step 3d2). Functions left on the AST emitter for `union` across
  the `bench` roots, package benchmarks, and demos fall from 121 to 61.
  `pkg/regex` sheds 2.9 KB of machine code (4.2%), the `http-simple` demos
  0.7 KB, and the `json` benchmark 48 bytes; the regex benchmark's runtime is
  unchanged within noise. `tests/ir-diff.sh` now builds, passes, returns, copies, and compares
  unions.
- Lowered `switch` on a union and unions inside aggregates to IR (IR migration
  step 3d2). The scrutinee's address is taken once (a place's own, else a
  stack object holding it), the `i32` tag is loaded once, and each pattern
  compares it with the variant's index; an arm's payload binder is a copy of
  the payload, made where the arm starts, as the AST emitter does. A struct,
  tuple, or array holding a union at any depth lives in memory, is built member
  by member, and crosses calls as its words (`u64` each, a `u32` for a last
  half word; by address above 16 bytes), like a union on its own. Functions
  left on the AST emitter for `union` across the `bench` roots, package
  benchmarks, and demos fall from 61 to 0. Machine code shrinks 9.3% for the
  `json` benchmark, 7.3% for the `json-parse-file` demo, and 1.7% for the
  regex benchmark; the `json` benchmark runs about 14% faster (0.75 s to
  0.65 s, three runs). `tests/ir-diff.sh` now switches on unions and keeps them
  in a struct member and an array.
- Lowered the debug `noalias` contract checks to IR (IR migration step 3e).
  After a call's arguments, each slice argument's byte range is checked
  representable (the length times the element size does not overflow, and the
  end does not wrap), and each pair of slice arguments with a `noalias`
  parameter among them is checked disjoint when both are nonempty, through the
  call contract failure. A `mut` descriptor argument to such a slice
  parameter keeps the function on the AST emitter. An empty range inside another
  now passes, as SPEC Safety #7 says; the AST emitter traps on it. Release
  emits nothing. Functions left on the AST emitter for `noalias contract`
  across the `bench` roots, package benchmarks, and demos fall from 29 to 0 in
  debug builds, so debug and release now leave the same 638 functions there.
  `tests/ir-diff.sh` now calls a function with a `noalias` slice pair over
  random windows of a buffer.
- Lowered the small leftovers to IR (IR migration step 3f). `continue label`
  on a labeled block jumps back to a head block the label opens, the block's
  own back edge (a loop's `continue` still runs its post clause). `typeid(T)`
  is its identity constant. A global constant not a literal (an expression, or
  another module's constant) is read from its storage, as the AST emitter
  does. An Error's `name` or `message` compares the packed value with each of
  the program's error domains (name) or entries (message) in declaration order
  and yields the matching string, else the empty one, and is no longer a place.
  `panic(message)` writes the message to descriptor 2 and calls `abort`, then
  falls through to code that never runs. `ir_lower` now takes the program for
  the domains. Functions left on the AST emitter for `Error value`, `computed
  global constant`, `non-variable name`, and `continue target` across the
  `bench` roots, package benchmarks, and demos fall from 44 to 0; what remains
  besides compiler-backed builtins is `slice` (6), `string` (2), and `static
  map` (2). Release machine code shrinks 9.5% for the `http-simple-server`
  demo, 4.0% for `regex-search-file`, 3.0% for `json-parse-file`, and 1.7% for
  the regex benchmark; the `json` benchmark is unchanged. `tests/ir-diff.sh`
  now restarts a labeled block, reads a computed constant, and looks up an
  Error's description.
- Added pass 6 to the IR backend, release only (IR migration step 4a, new
  `src/opt.c`): sparse conditional constant propagation, then dead-code and
  control-flow cleanup. Every integer, bool, and pointer value is unknown, one
  constant, or varying, and only blocks a branch can reach are evaluated, so a
  constant condition prunes its other arm; a value found constant becomes a
  `const`, through phis and loops too. An operation debug would trap on (a zero
  divisor, `MIN / -1`, a shift count past the width) is left for run time, and
  floats are not folded. Cleanup folds constant and same-target branches,
  removes unreachable blocks and trivial phis, merges a block into its sole
  predecessor, skips blocks holding only a jump, and removes unused values
  (never calls, stores, volatile loads, parameters, or call results). Release
  `__text` shrinks 2.4% for the `http-simple-server` demo, 2.4% for
  `regex-search-file`, 3.4% for `json-parse-file`, 3.9% and 2.7% for the `json`
  and regex benchmarks, and 4% to 10% across `bench/core` and `bench/std`;
  timings are unchanged within run-to-run noise (`bench/core` within 0.5%).
  `tests/ir/fold.ace` folds through locals, a loop phi, and a pruned arm.
- Added global value numbering to pass 6 (IR migration step 4b, `src/opt.c`),
  between constant propagation and cleanup. A walk of the dominator tree with a
  scoped table replaces a pure operation equal to one in a dominating block
  (commutative integer operands in either order, a constant by its value rather
  than its id) and an identity (`x + 0`, `x - 0`, `x | x`, `x & x`, `x * 1`,
  `x / 1`, a shift or rotation by 0, a pointer offset of 0, a cast to the same
  type) by its operand; loads and calls are never numbered. `ir_dominators` and
  `ir_dominates` are now shared from `ir.c`. Release `__text` shrinks 0.7% to
  0.8% for the demos and package benchmarks and 0.7% to 1.2% across `bench/core`
  and `bench/std`; three alternating rounds show `format-f64-shortest` 9%,
  `pow-f64` 6%, `parse-f64` 5%, and `compare-64` 5% faster and nothing slower
  beyond run-to-run noise. Loop-invariant code motion was built and dropped: it
  gained nothing over numbering alone and made the regex benchmark 5% to 10%
  slower, because the backend fuses compares into branches, pointer offsets
  into addressing, and `mul` with `add`, and has no live-range splitting for
  a value held across a loop. `tests/ir/numbering.ace` covers a repeat, a
  commuted operand, and an identity. Loop strength reduction (replacing a
  multiply of a linear induction variable by a constant, when the product is
  reused so `madd` fusion cannot fold it into the surrounding add or
  subtract) was also built and dropped: it produced byte-identical machine
  code across every demo, `bench/core`, `bench/std`, and `bench/package`
  benchmark, since the pattern it targets does not occur in real code here,
  and cost every function a small compile-time scan for nothing.
- Rotated loops at block layout instead of as an IR pass (IR migration step
  4c, `src/arm64.c`): a header that only tests (a two-way branch with one
  successor in the loop, one outside) and whose one back edge is a plain jump
  is laid out after its latch, so an iteration ends in the test's own
  conditional branch instead of a jump to the test, with a jump to the test
  once at entry. This applies in both modes, since it is layout, not an IR
  pass; it needed no SPEC-listed IR rotation pass, no exit-edge SSA repair,
  and no code duplication. A separate `layout`/`eix` emission order carries
  the rotation; `order`/`lix`/`bfirst`/`idx`/`blast`, and therefore liveness,
  linear-scan allocation, coalescing, and hints, keep every value's position
  exactly as an unrotated layout would, so none of their ordering assumptions
  change. An earlier version rotated `order` itself and widened `live_in`'s
  start position for a value used before its home block's new position;
  that broke coalescing's own use of `start`, which shares a register between
  a phi and its loop-carried operand, and gave wrong values for a call
  returning more than one leaf inside a rotated loop, release mode only
  (`tests/emit/staticmap`-style differential testing missed it; an
  independent recompute of the affected benchmarks caught it). `saxpy`
  (`bench/core/hotloop`) is 18.9% faster, `particle-vec3` 5.3%, `length-vec3`
  4.6%, `bench/core/calls`'s `struct-24` 25.1%, `bench/std/string`'s
  `compare-64` 12.5%, `find-4096` 16.5%, `ascii-fold-64` 9.1%, and
  `bench/std/math`'s `exp-f32` 10.0%; isolated single-loop and nested-loop
  programs shaped like `bench/std/collections`'s map operations and
  `bench/core/calls`'s `call-4`/`call-8` (a call, then a conditional check)
  are flat to 15-17% faster. Those same shapes regress 2% to 15% inside
  `bench/core/calls.ace`, `bench/std/collections.ace`, `bench/std/string.ace`,
  and `bench/std/math.ace` themselves: each packs dozens of loops into one
  function, so shrinking one loop's code shifts every later loop's address
  and its alignment against cache lines, a placement effect `bench/README.md`
  already documents as separate from a change's own cost. Two `tests/run.sh`
  checks (`struct_registers.o`'s HFA loop, `byref_registers.o`'s `Counter.run`)
  found a loop's back edge by its own address to bound where they check
  register use; both assumed a top-tested loop's unconditional backward jump
  and needed widening to recognize the rotated bottom test's conditional
  branch (`cbz`/`cbnz` included) as the back edge instead.
- Added loop strength reduction to pass 6 (IR migration step 4d,
  `src/opt.c`), between value numbering and cleanup. An unchecked pointer
  offset `base +/- i * S` whose scale is not a power of two, by a basic
  induction variable (a 64-bit header phi stepped by a constant at the one
  back edge, used directly or through a cast to `u64`) with a base available
  before the loop (a global is placed there afresh), becomes a pointer phi
  started at `base +/- init * S` and stepped by the constant times `S` at the
  latch; offsets with one base (a global by its symbol), scale, direction, and
  induction variable share one pointer. The two phis step together, so the
  pointer is right wherever the header dominates, exit included, with no
  exit-value repair. A power-of-two scale is left alone: the backend already
  folds it into the access or an `add`, while any other scale cost a `mov`
  and a `madd` per offset per iteration. A function with no such offset skips
  the pass before computing dominators. `bench/core`'s `particle-vec3` is
  2.22x faster and `length-vec3` 1.55x (`__text` 26,856 to 26,620 bytes);
  the `json` benchmark and JSON demos shrink 16 bytes with timings flat; every
  other `bench/core`, `bench/std`, `bench/package`, and demo binary has
  byte-identical code. Release compile time is unchanged. The earlier dropped
  attempt rewrote `i * C`, which `madd` fusion already absorbed.
  `tests/ir/strength.ace` covers a global and a parameter base, a nonzero
  start, a falling counter, an inner loop, and a power-of-two scale left alone.
- Lowered generic function instantiations to IR (IR migration step 5a,
  `src/emit.c`, `src/lower.c`): each per-instantiation memo entry now tries
  `ir_lower`/`arm64_covers` before falling back to the AST emitter, the same
  as a monomorphic function. `ir_lower` takes the instantiation's
  type-argument tuple and stamps the template's body (`sema_stamp_body`)
  itself when it is not already an explicit specialization, exactly as
  `inline_copy` already does for an inlined generic call. `bench/std/
  collections.ace` shrinks 8.0% (24,204 to 22,268 bytes) and the `json`
  package benchmark binary 9.6% (171,320 to 154,808 bytes), both
  generics-heavy; every other measured benchmark, demo, and package binary is
  unchanged, and checksums match. `tests/ir-diff.sh` (100 random programs)
  shows no mismatches.
- Closed six of the reachable gaps an audit of `lower.c`'s 91 `unsupported()`
  bailouts found (IR migration step 5b), ahead of removing the AST-emitter
  fallback: `l->inl` and `l->tgt` (inline-expansion and loop/label nesting)
  now grow dynamically through the existing `grow()` helper instead of
  bailing past a fixed 16/64 cap, matching the AST emitter's own unbounded
  `inl` growth (each is indexed fresh through `l->inl`/`l->tgt` at every
  step rather than through a cached pointer, since expanding a nested inline
  call can reallocate the array mid-lowering); an inline call whose body
  never returns no longer bails, since `enter()` already marks its join
  block unreachable (`l->cur = -1`), the same idiom every other construct in
  the file already relies on; `.len` on a non-place fixed-array expression
  (for example a call result) evaluates the expression for its side effects
  and returns the static count; every bare expression-statement of
  aggregate type, not only `Error.name`/`.message`, now goes through
  `value()` instead of the scalar-only `expr()`, since `N_EXPRSTMT` was the
  one caller passing an aggregate through a function that can only return
  one scalar id; and `StaticMap[bool]V` lookups use the same splitmix64
  integer-hash path as any other non-float, non-pointer key. The `union
  payload comparison` cap (over 64 bytes) turned out to be dead:
  `union_supports_equality` (`sema.c`) permits `==`/`!=` on a union only
  when every payload is a primitive, pointer, function, enum, bits,
  contract, error, or nested union of those, each capped at 8 bytes, so the
  comparison's byte-at-a-time loop can never actually see more than 64
  bytes; the cap is removed anyway since the loop was already fully
  general. `make test`, `make test-packages`, and `tests/ir-diff.sh` (300
  random programs across three seeds) pass with no mismatches.
- Closed three more of the same audit's reachable gaps (IR migration step
  5c): an exported function's function value, and a direct call to one from
  IR-lowered code, now resolve through `link_name` (`src/lower.c`) to the
  export's unmangled C-boundary symbol — the same symbol `emit.c`'s
  `gen_addr` already targets — closing the `function value` and `exported
  call` bails (an intrinsic's function value needed no change, since it
  already fell through to the ordinary mangled symbol its own out-of-line
  body is emitted under); a `for` loop's runtime (non-constant) step now
  gets the same debug-mode positivity trap (`IR_CHECK`, "range step
  violation") the AST emitter already has, since sema only rejects a
  non-positive *constant* step at compile time; and contract method
  dispatch (`s.area()` / `mut s.scale(...)`) lowers directly — a
  null-guarded vtable-slot read at the method's declared position, then an
  indirect call with the contract's data pointer as receiver, mirroring
  `emit.c`'s mechanism exactly, and reaching `defer_capture` alongside the
  existing concrete-method case. That last one is not yet reachable by any
  real program: contract values have no representation in the IR's
  aggregate-type layer (`is_agg`, `ir_leaves`), so a contract cannot yet be
  bound to a parameter or local at all; general contract-value support is
  separate, larger future work. The seventh audited item, `aggregate of
  over 16 leaves` in `store_value`, still has no known repro after a second
  audit against every `store_value` call site and 1,500 fuzzer-generated
  programs; left in place, unlike the union-payload cap, for lack of a
  proof of unreachability. `make test`, `make test-packages`, and
  `tests/ir-diff.sh` (400 random programs across four seeds) pass with no
  mismatches.
- Resolved the remaining three gaps from the same audit (IR migration step
  5d). A function with a `mut` parameter can no longer become a callable
  value at all — not a function value (`sema.c`'s three coercion sites:
  the plain, overloaded, and generic cases) and not a contract method
  (rejected once, at the contract's own declaration) — since neither a
  function-value type nor contract dispatch has any way to validate `mut`
  agreement against the real callee; this closes the `mut argument through a
  function value` bail outright; rather than extending the grammar and
  runtime, a mut-parameter function simply cannot reach these call
  paths, so any `mut`-marked argument there is now a plain sema error. The
  `noalias contract` bail (a by-address slice parameter's descriptor) is
  resolved by loading `{ptr, len}` through the address `call_arg` already
  computed, the same pattern `mem_release` uses to read a slice place by
  address, rather than misreading it out of the by-value leaf array. The
  `asm` bail needed no code change: `tests/ir-diff.sh`'s own comment already
  documents that a function containing `asm` permanently stays on the AST
  emitter with calls crossing both backends, so this is the migration's
  intended per-function fallback working as designed, not a gap; recorded in
  `SPEC.md`'s `Excluded:` line. `tests/ir-diff.sh` (100 random programs) and
  the affected `tests/sema`, `tests/emit`, `tests/checked_contracts`, and
  `tests/modules` fixtures pass with no mismatches.
- Gave contract values an IR leaf representation and lowered `Shape(&c)`
  construction, closing the gap the same audit flagged: contract values had
  no representation in the IR's aggregate-type layer, so one could not yet be
  bound to a parameter or local. `leaves_at` (`src/ir.c`) now yields a
  contract's vtable and data pointer as two `IT_PTR` leaves (Carved #5), and
  `is_agg` (`src/lower.c`) recognizes them, so a contract binds to a local,
  parameter, field, or return like any other aggregate. Construction reuses
  `emit.c`'s `vtable_name` (now shared through `backend.h`, the same
  treatment `fn_mangle` already gets) to reference the vtable module-level
  signing emission already defines, rather than re-deriving its mangled name;
  the two leaves are an `IR_GLOBAL` naming that symbol and the argument's
  address, with no new object-file work needed. Contract-method dispatch
  (step 5c) no longer requires the contract to already be an addressable
  place — it reads the two leaves directly, a net simplification, and what
  makes that dispatch reachable by real programs for the first time.
  Construction over a generic concrete type still bails with
  `unsupported(l, "generic contract construction")` and falls back per
  function, same as `asm`: such a signing's vtable is only materialized on
  demand by the AST emitter's own construction-site codegen (`Em`-coupled),
  not yet by `lower.c`; closing it needs `emit_vtable` reachable through
  `Obj*` instead, left for later. `tests/ir/contracts.ace` covers
  construction and dispatch; `tests/emit/contracts`, `tests/checked_contracts`,
  `make test`, `make test-packages`, and `tests/ir-diff.sh` (100 random
  programs) pass with no mismatches.
- Lowered `asm` statements to IR (IR migration step 5e), reversing step 5d's
  permanent exclusion. `IR_ASM` carries its instruction text, its "in"
  values as arguments with an `IrAsm` naming each one's x register, and one
  `IR_PROJ` per "out" register after it; `reserved` is every register any
  binding or clobber names. The allocator keeps a value out of a statement's
  reserved registers only when its interval crosses that statement, rather
  than giving the whole function frame slots as the AST emitter does, and a
  statement reserving x29 or x30 keeps the frame. A move cycle into a
  statement that binds x16 swaps in place with three `eor`s, since any other
  register may hold a value live across it; pass 6 counts `IR_ASM` as an
  effect, so one with no outputs survives release. A binding naming an
  aggregate or an address-taken local, or an instruction outside `nop`,
  `svc`, and `brk`, still keeps its function on the AST emitter. No `std`,
  `pkg`, or `bench` code uses `asm`. `tests/ir/asm.ace` and
  `tests/emit/basics/asm_bindings.ace` cover bindings, the in-place swap, a
  value live across the statement, and callee-saved registers on both
  backends; `tests/ir/fallback.ace`,
  `tests/emit/staticmap/asm_keys.ace`, and `tests/ir-diff.sh` now keep
  functions on the AST emitter through an address-taken binding.
  A function whose asm reserves any of x19-x28 now saves and restores it
  (Inline Assembly #3): the IR backend adds every reserved register to the
  set its allocator already saves, and the AST emitter, which promotes
  nothing in an asm function, saves x19 up to the highest one reserved.
  Both backends previously returned with the caller's registers
  overwritten. `make test`, `make test-packages`, and `tests/ir-diff.sh`
  (200 random programs across two seeds) pass with no mismatches. Renamed
  `src/opt.c` to `src/optimizer.c`.
- Lowered generic instantiations to IR (IR migration step 5, group 2). Every
  instance whose return type named a type parameter bailed with "unsupported
  type" and fell back to the AST emitter: `ir_lower` read the return type from
  the template's symbol, still `T`, while stamping already re-binds the
  parameters under substitution. `sema_stamp_body` now returns the return
  type it builds under the same substitution, and `ir_lower` uses it. The 30
  fallback instances across `tests/`, `bench/`, and `demo/` (among them
  `Array.get` and `Map.get`) now lower; in release, `bench/std/collections.ace`
  shrinks `Map.get` from 464 to 208 bytes and `Array.get` from 112 to 16, and
  `map-get` and `array-get` run about 25% faster. `make test`,
  `make test-packages`, and `tests/ir-diff.sh` (100 random programs) pass
  with no mismatches.
- Lowered every remaining function to IR (IR migration step 6a), so no
  function across `tests/`, `bench/`, `demo/`, and `pkg/` reaches the AST
  emitter in either mode. A compiler-backed declaration's body is a
  synthesized `return f(p0, ...)`; a C export is its global `_name` symbol and
  is never turned into a loop; `assert`, `*T(addr)`, and `[]T(p, n)` lower
  with their debug checks; multi-name declarations take untyped tuple
  elements at their targets' types; `q.x` dereferences every level of
  `*mut *T`; a contract compares with `null` by its vtable leaf; and a
  `[]u8(s)` view conversion is one value, as `string(b)` was. An asm binding
  of a local or parameter kept in memory is loaded before the instruction
  and stored after it, and sema now rejects an aggregate or global binding
  (Inline Assembly #2). A `bits` value is its backing integer (`ir_type_of`):
  a field read is a shift and mask (or a shift up and arithmetic shift down
  when signed), and a field write one read-modify-write of the whole value
  through its containing place, after a debug range test; a literal ORs its
  fields into zero. Construction over a generic concrete type records its
  pair in the `IrFunc`, and `emit_object` defines the vtable through
  `obj_vtable`, which with its embed thunks now takes only an `Obj*`, before
  compiling the function. Lowering keeps a call's or signature's argument
  leaves in 40-entry buffers that move to the arena past them, so there is no
  argument-leaf cap; an outgoing area over 352 bytes allocates the frame in
  two steps (the frame record and callee-saved registers first, at its base,
  within `stp`/`ldp` reach), and a declaration or function type takes at
  most 64 parameters, a method's receiver included (Functions & ABI #9).
  Touched fixtures shrink 12-62%; `bench/std`
  shrinks 848-1,008 bytes, with timings changed only through layout.
  `tests/ir/asm_home.ace` replaces `tests/ir/fallback.ace`,
  `tests/sema/asm/binding_err.ace` and
  `tests/sema/functions/param_count_{ok,err}.ace` cover the new diagnostics, and
  `tests/ir-diff.sh` (100 random programs) lowers all 329 functions with no
  mismatches.
- Removed the AST emitter (IR migration step 6b): every function compiles
  through the IR backend, and one it does not cover is a compile error naming
  the construct. `src/emit.c` keeps the object driver, global and constant
  data, static maps, vtables and signings, mangling, and the runtime and byte
  routines, dropping from 10,691 to 2,086 lines; `arm64_covers` is gone (sema
  already caps alignment at 16, and extern globals do not exist), and
  `inline_target`/`inline_copy` are private to `lower.c`. The asm checks the
  emitter made move to sema at the same positions: the instruction set, an
  `svc`/`brk` immediate, `x0`-`x30` binding names (clobbers now included),
  and an `out` target that is a local or parameter. SPEC's migration entry
  folds into Compiler #2, #4, and #6. Every non-debug section of the 816
  objects built from `tests/`, `bench/`, `demo/`, and `pkg/` in both modes is
  identical to step 6a's, so machine code and runtime are unchanged; the
  compiler's `__TEXT` shrinks 28% with compile time unchanged.
  `tests/sema/asm/instruction_err.ace` and `register_err.ace` cover the moved
  diagnostics.
- Lowered `for` over a string, slice, or array to IR (IR migration step 3g).
  The sequence's pointer and length (an array's address and fixed count) are
  read once at entry, the end is `padd ptr, len * size`, and a pointer runs to
  it bumped by the element size (Iterations #9-10), with a `u64` counter when
  there is an index binder. A value binder is a copy of the element; under
  `in mut` its variable lives at the pointer, so reads and writes are loads
  and stores. The body has no bounds checks. Functions left on the AST emitter
  for `slice`, `string`, and `array` across the emit fixtures, `bench` roots,
  package benchmarks, and demos fall from 5, 4, and 17 to 0; the `static map`
  fallback (2) is what remains of those three, besides compiler-backed
  builtins. Release machine code shrinks 1.4% for the `regex-search-file`
  demo and 1.6% for the regex benchmark and is unchanged elsewhere, since only
  three functions in `pkg/regex` were left; timings are unchanged (the
  `pathological` workload moves about 5% either way when unused functions
  shift the layout). `tests/ir/forin.ace` covers each sequence kind, both
  binder forms, and `in mut`, `tests/ir/fallback.ace` keeps only the `asm`
  case, and `tests/ir-diff.sh` now generates for-in loops over its windows.
- Lowered `static map` lookups to IR (IR migration step 3h). `table[key]` hashes
  the key as the emitted table was laid out (an integer by splitmix64 over its
  sign- or zero-extended `u64`, a string by an FNV-1a loop, both seeded), loads
  the `u32` slot, and confirms the key exactly (a value compare, or length and
  `mem_compare`); the seed and mask are constants, since the perfect hash is now
  built once per map (`map_plan`, `staticmap_hash` in `src/emit.c`) and shared by
  lowering and the data emitter, so a lookup no longer depends on emission order.
  The `$values` array gains one zeroed entry at index `count`; a hit or miss
  ends in one `u64` entry index (a phi), so the place is `values + idx * size`
  and needs no stack temporary or copy. Lookups in a `static map` loop run about
  20% faster than the AST emitter's (30M integer and 10M string lookups, 0.27 s
  to 0.22 s). Nothing remains on the AST emitter but compiler-backed builtins.
  The AST emitter's lookup hashed a narrow signed key without extending it to
  64 bits (the table is laid out with it extended), so negative `i16` keys
  missed (22 of 40 present ones); it now widens the key first.
  `tests/emit/staticmap/large_tables.ace` covers signed narrow keys,
  occupied-slot misses, and colliding string keys, and `asm_keys.ace` the AST
  path, kept there by an `asm` statement.
  `tests/ir/staticmap.ace` covers integer, string, and struct-valued lookups.
- Implemented `@when` for top-level compile-time declaration filtering with a
  closed target/boolean expression language, plus host-defaulted `--target`;
  inactive declarations remain syntax-checked but never reach imports, semantic
  checking, or emission.
- Deferred method calls on a struct receiver that find no method yet to the same
  end-of-file `Pending` fixup plain calls use (`check_method_call`, re-run with
  `final`), so methods on one type may call each other in any declaration order.
- Extended the narrow forward-reference fixup class to a bare slice exactly at
  an aggregate field or union variant root (`[]Later`), alongside the existing
  pointer-chain shape (`*Later`, `**Later`): a slice's fixed pointer-plus-count
  representation is equally independent of its element's size, so the same
  end-of-file placeholder-patch mechanism (`TypeFix`) now also records a fix
  slot for a slice's element type. The allowance does not extend through or
  past the slice itself — a slice reached via a preceding pointer, or nested
  inside another compound, stays ordinary declaration-before-use — while a
  pointer reached via a preceding root slice still forwards on its own
  existing rule. This lets two nominal types reference each other through a
  runtime-sized collection without boxing every element behind a pointer, e.g.
  `Json: union { none, object: []Pair }` followed by
  `Pair: struct { key: string, value: Json }`.
- Numbered `IR_GLOBAL` in global value numbering, keyed by symbol (a name-only
  reference with no symbol, the vtable case, never merges), so a repeated
  address materialization of the same global is replaced by one already
  dominating it; `simplify` now returns unchanged on a zero-argument operation
  instead of reading past an empty argument list. Loop strength reduction's
  preheader hoist for a global base no longer needs a non-power-of-two scale:
  a power-of-two access keeps its scaled-register offset in the loop body (the
  backend already folds that for free) but still moves its base to the
  preheader, as the pointer case's is, memoized per loop by symbol.
  `bench/core`'s `saxpy-f64` drops from 0.784 to 0.507 ns/element (1.62x to
  1.05x scalar C, `dot-f64` improving alongside it), and `hotloop`'s machine
  code shrinks 25,612 to 25,552 bytes. `tests/ir/strength.ace` gains a global
  summed by a power-of-two stride to cover the new hoist, and
  `tests/ir-diff.sh` (200 random programs) shows no mismatches.
- Added the IR's internal vector types for auto-vectorization (Compiler #6,
  step 1 of 3): `v4f32` and `v4i32`, with `splat` and `select` beside lane-wise
  `add`, `sub`, `mul`, `neg`, `div` (float), compares to a lane mask, `cast`
  (`i32` to `f32`), `load`, `store`, and `phi`; the verifier confines them to
  those operations. `arm64.c` emits NEON `.4s` forms, allocates vectors in the
  v registers as whole `q` registers, moves and parks them whole, and spills
  one to a pair of slots, never keeping one in a callee-saved register across a
  call (a callee preserves only the low half). No pass creates vectors yet:
  `tests/vector_ir.c` builds them by hand, checks `--ir` printing against
  `tests/vector_ir.ir`, and compares every lane with C, bit for bit.
  Existing programs compile to identical machine code.
- Added auto-vectorization to pass 6 (Compiler #6, step 2 of 3), after
  cleanup so loops are merged: an innermost counted loop (header of only the
  `u64` or `i64` counter from 0 by 1 under `lt i, n`, one body block) of
  lane-wise `f32`/`i32` `add`, `sub`, `mul`, `neg`, and float `div` over
  `base[i]` elements gains a four-lane loop ahead of it when each store's
  object is another access's only at the same element (distinct globals or
  stack objects, or the same base and offset) and no body value is used after
  the loop. The vector loop counts whole vectors (`k`, addressed `base + 16k`,
  which the target folds) with invariants splatted in the preheader; the
  original loop runs the remainder from `4k`. A reduction, a carried value, a
  neighbouring element, two possibly overlapping pointers, a remaining call,
  or `f64` elements keep a loop scalar. `bench/core` `saxpy-f32` drops from
  0.507 to 0.131 ns/element (3.9x; C `-O2` scalar 0.486, vectorized 0.092).
  `tests/ir/vectorize.ace` covers accepted and rejected loops, and
  `tests/vectorize/lanes.ace` must print the same bits in release and debug
  for counts leaving every remainder and for NaN, signed-zero, infinite, and
  subnormal lanes.
- Extended auto-vectorization to branches and the index as a lane value
  (Compiler #6, step 3 of 3). The body may be a sequence of blocks, each
  forking on a compare into arms of at most one block that meet at the next;
  arms may not load or store, since the vector loop runs both, and each join's
  phi becomes a `select` on the compare's lane mask. Under a constant count
  that fits, `i32(i)` is a lane value: a vector phi from the new `lanes` op
  (`<0, 1, 2, 3>`) stepping by 4, feeding `v4i32` `and`, `or`, `xor`, and
  `shl`/`shr` by a constant. Equal constants share one splat, which the
  target emits as `movi`/`fmov .4s` when the lane fits eight bits, and a
  constant count of whole vectors drops the scalar remainder loop. `bench/core`
  `cube-transform` drops from 61.0 to 41.4 ns/cube (1.47x; C `-O2` scalar
  53.0, vectorized 31.4), and `saxpy-f32` from 0.131 to 0.126 ns/element.
- Began the linux-x64 target. `--target=linux-x64` (the Linux x64 host's
  default) checks and lowers, and object emission reports that its code
  generator is not written yet; the build defines `_DEFAULT_SOURCE` so
  `-std=c11` exposes POSIX on glibc.
- Added `src/elf.c`, the ELF64 x64 relocatable writer for the same `Obj`
  the Mach-O writer serializes: `R_X86_64_*` relocation types (the shared
  DWARF writers' type 0 read as an absolute address), each addend moved from
  its field into a `.rela` entry, locals before globals, and a
  `.note.GNU-stack`. `tests/elf_object.c` links a hand-encoded object through
  `cc` on a linux-x64 host.
- Added `src/x64.c`, the linux-x64 code generator (step 1: integers).
  Every value lives in its own frame slot in canonical form (a narrow integer
  extended to 64 bits by its type) and each instruction works through rax,
  rcx, and rdx; there is no register allocation yet. It covers integer and
  pointer arithmetic, comparisons, casts, bit operations, loads, stores,
  stack objects, globals, strings, scalar calls (System V registers, then
  stack slots), phis (critical edges split), and debug checks, whose shared
  failure routines it emits itself. Release arithmetic matches the arm64
  target: shifts count modulo the 32- or 64-bit operation width, and a
  division by zero or MIN / -1 does not trap. A function using floats,
  aggregate parts, byte routines, the allocator, or asm is refused with a
  diagnostic. `ace_target` selects the backend and writer; the ELF writer
  drops the Mach-O leading underscore from every symbol, and a contract
  embed stub on x64 is `add rdi, offset; jmp method`.
- Made IR numbering independent of the host C compiler. Lowering and the
  vectorizer passed two emitting calls as arguments of one call, whose order C
  leaves unspecified, so a gcc-built compiler numbered (and, for `and`/`eq`
  operands, ordered) instructions differently from a clang-built one and
  failed the `tests/ir` expectations. Each such argument is now its own
  statement, in the left-to-right order clang used; the gcc-built compiler's
  IR and objects match the clang-built `HEAD` on every repository file.
- Passed aggregates by System V on linux-x64. `x64.c` plans each call's,
  parameter list's, and result's parts: up to 16 bytes, one register per
  eightbyte (SSE when every leaf in it is a float, not yet emitted), the whole
  part on the stack when its registers run out; over 16 bytes, a copy on the
  stack, with a result through memory addressed by `rdi` and returned in
  `rax`. Leaves meet register words in 16 bytes of memory (the outgoing area
  for a call, a scratch slot in the callee), so no leaf shifts or masks. The
  IR's new `IrPart.addr` marks an aggregate over 16 bytes that lowering holds
  by address (over 16 leaves, or a parameter it reads in memory): arm64 still
  passes the address, while x64 copies the aggregate onto the stack
  (`rep movsb`) and the callee takes the address of its stack copy, so both
  meet C. An embed thunk advances `rsi` instead of `rdi` when the method's
  result travels through memory.
- Added the linux-x64 byte routines and debug allocator registry.
  `__ace$mem_copy`/`move`/`fill`/`compare` take a private convention (`rdi`
  destination or `a`, `rsi` source, byte, or `b`, `rdx` count, `rax` the
  compare result) and clobber only `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r11`,
  `xmm10`-`xmm15`, and flags. They use SSE2 alone and follow the arm64
  structure: overlapping chunks up to 64 bytes, loaded before any store, and
  32-byte loops beyond. From 1024 bytes, forward copies and fills use
  `rep movsb`/`rep stosb`, but a leftward-overlapping move keeps the loop.
  `mem_compare` finds the first differing byte with `pcmpeqb`/`pmovmskb` and
  `bsf`. The registry routines are ordinary System V functions over
  `malloc`/`free`. Against glibc's AVX2 routines on an ERMS/FSRM Xeon, copy
  up to 64 bytes runs 1.2-2.2x faster and fill 0.9-1.8x; from 100 bytes to
  1 KB both are 1.3-2.1x slower (16-byte stores against 32); from 4 KB `rep`
  matches glibc. Compare is even to 64 bytes and 1.3-1.7x slower beyond.
- Added floats to linux-x64, which now requires x86-64-v3. A float's slot
  holds its bits (an f32's zero-extended), so constants, phis, memory, and
  aggregate leaves take the integer paths; arithmetic, `ucomis` compares,
  conversions, `sqrt`, `roundsd` floor and ceil, and `vfmadd213` move
  through `xmm0`-`xmm2`, leaving `xmm10`-`xmm15` to the byte routines. Float
  compares are false on a NaN except `ne` (`eq` also tests PF, `lt`/`le`
  swap operands to `seta`/`setae`); negation flips the sign bit. Conversions
  match arm64: a float to a 64-bit integer saturates at 64 bits, to a
  narrower one at 32 bits and then wraps, a NaN to 0, correcting the
  `cvtts2si` sentinel with `cmov`; a u64 at or past 2^63 converts less 2^63,
  and one converted to a float is halved with a sticky low bit and doubled.
  Calls pass floats and SSE eightbytes in `xmm0`-`xmm7`, return them in
  `xmm0`/`xmm1`, and set `al` to the SSE count of a variadic call; an
  aggregate result's integer words now come from `rax` and `rdx` by class.
- Added vectors to linux-x64, so release auto-vectorization compiles there.
  A vector value takes a 16-byte slot aligned to 16, and phi staging sizes
  each entry by its type; operations move whole through `xmm0`-`xmm2` in SSE
  up to SSE4.1, matching arm64's lane operations: float compares by `cmpps`
  (`gt`/`ge` as `lt`/`le` exchanged, `ne` as `NEQ_UQ`, true on a NaN),
  integer ones by `pcmpeqd`/`pcmpgtd` with `ne`, `le`, and `ge` inverted,
  float negation as a sign-bit flip, `pmulld`, `psrad` for `shr`, a splat by
  `pshufd`, and `select` by `blendvps` on the mask in `xmm0`.

## Core language

- Replaced `:` with `=` for named struct and bits literal field initialization,
  reserving `:` for declarations and type annotations and rejecting the former
  literal spelling in the parser.
- Removed local `:=` declaration elision so every storage binding states its
  type and initializer checking always has an explicit declared expectation.
- Implemented predeclared module-level `StaticMap[K]V` constants with
  compile-time entry checking and duplicate detection, read-only key/value and
  deterministic perfect-hash-plan emission, exact candidate confirmation, and
  canonical-zero misses; `std/http` now uses the facility for status phrases.
- Implemented per-field compile-time struct defaults and omission of defaulted
  fields in named literals, including empty all-default literals and explicit
  overrides, without changing canonical zero, implicit storage initialization,
  layout, or ABI.
- Split unary operators by role: `*` now explicitly dereferences pointers, `~`
  complements integers, and `!` negates only Booleans, with dereference lvalues
  carried through semantic checking, constant evaluation, and direct emission.
- Implemented typed pointer `+`, `-`, `+=`, and `-=` with `usize` element
  offsets, exact pointer-permission preservation, complete nonzero pointee
  validation, checked null/scaling/address diagnostics, and guard-free release
  lowering. Pointer indexing, difference, ordering, bounds, and provenance stay
  excluded.

- Added `float` as a predeclared transparent alias of `f64`; both spellings
  share type and overload identity, representation, conversions, and ABI
  behavior.
- Implemented explicit `name: type` declarations, per-name
  declaration/assignment in multi-name `=`, constants, aggregates,
  distinct types, aliases, enums, tuples, functions, overloads, and simultaneous
  multi-return assignment.
- Implemented value-copy assignment and by-reference parameters. `mut` controls
  caller-visible mutation at signatures and call sites; pointer and view
  writability is tracked independently at each type level; `noalias` supplies a
  compile-time non-overlap fact.
- Fixed module-private name lowering to preserve lexical value and type scopes,
  so parameters and locals shadow same-named module functions without changing
  unshadowed overload, qualified-module, or method resolution.
- Lowered `binding.member` in the scoped import walk, so a local or parameter
  named like a whole-module binding shadows it: `.` on it is a field access,
  where it was rejected as an unknown module member, or silently resolved to
  the module when the member existed. Type positions (`shape.Box`,
  `shape.Box{...}`, sizeof and typeid operands) still name the module's type.
  The separate unscoped qualified-lowering pass is gone.
- Indexed an imported file's renamed declarations in an open-addressed FNV-1a
  table built once before the reference walk. Each unshadowed name had scanned
  every declaration, so lowering was quadratic in the file's size: an imported
  file of 8000 struct types and functions took 25.1G instructions to compile,
  now 3.0G (500: -23.5%). The regex and json package checks gain 0.3%.
- Merged the imported-file rename walk into the scoped reference walk: one
  pass, after an imported file's declarations take private names, lowers
  selected imports, `binding.member`, and that file's own names, so one set of
  scope rules serves all three. The rename walk's own gaps close with it: an
  `offsetof` field path named like a module global was renamed to the global,
  and a signing's type parameters were not bound. Checking the regex and json
  packages takes about 1% fewer instructions.
- Lowered imported names in array lengths within type positions. The scoped
  import walk skipped an array type's length expression and the parts of a
  tuple, function, or map type in a sizeof-style operand, so `[sizes.SIZE]u8`
  (since the qualified pass moved into that walk) and `[SIZE]u8` from a
  selective import reported an unknown name.
- Scoped a switch payload binder to its case block in module lowering, so
  `Event.data(count: int)` shadows a module global in an imported file, a
  selected value, or a whole-module binding there instead of being lowered to
  it; an imported file's `return count` in that case read the global.
- Scoped a local from its declarator in module lowering as sema already did,
  so an imported file's `total: usize = total + 1` names the uninitialized
  local instead of the module global, and a selected import no longer reaches
  a same-named local's initializer. A bare name in a multi-name `=` is now
  lowered as a reference rather than bound as a new local, so an imported
  file can reassign a module global there.
- Implemented canonical all-bits-zero values and path-sensitive definite
  initialization for direct Ace storage. Pointer aliases, extern calls, and
  inline assembly remain explicit trust boundaries.
- Implemented explicit enum discriminants and fixed integer backing types with
  zero-first, fit, and uniqueness validation. Explicitly backed enums use their
  selected layout throughout ordinary values and C-compatible boundaries;
  default enums retain their existing `i32` layout and remain C-ABI-rejected.
- Implemented nominal `bits u8`/`u16`/`u32`/`u64` types with exact LSB-first
  signed, unsigned, and anonymous fields. The compiler lowers whole values at
  backing width, emits sign-extending reads and preserving checked/release RMW
  writes, zero-fill literals, exact raw conversions, same-type equality,
  aggregate placement, whole-value volatile access, and C-boundary rejection.
- Implemented operators, chained comparisons, `if`, exhaustive `switch`,
  contextual explicit `fallthrough`, labeled blocks, labeled break/continue,
  `for-in`, ranges and steps, and general condition/three-clause/infinite
  `for` forms. Loops lower directly to structured branches and index traversal.
- Implemented arrays, slices, strings, slicing, indexing, the direct read-only
  `.len` structural property, and mutable
  views. Unified `string`, `[]char`, and `[]u8` as three intent spellings of
  one type, with the same identity rule for writable forms; spelling-only
  overload variations are duplicate declarations. Sequential value equality
  is rejected; byte content comparison is the explicit compiler-backed
  `mem.equal`.
- Audited sequential/aggregate composition with positive and negative coverage
  for nested arrays and slices of structs, enums, unions, distincts, and exact
  generic instances across initialization, copying, mutation, calls, returns,
  and checked bounds; the existing value and view model required no widening.
- Implemented call-only, function-scope `defer`. Calls are captured at the
  statement according to the specified two-tier model and emitted in LIFO order
  at every function exit.
- Widened a fixed array type's length (`[N]T`) and an array literal's
  repetition count (`[value : count]`) from a bare integer literal to the
  existing restricted compile-time constant-expression surface already used
  by `@align(N)`: literals, `const` identifiers, `sizeof`/`alignof`/
  `offsetof`, and integer/bitwise arithmetic, resolved with the same `ut_const`
  evaluator. Both positions parse a general expression and defer constant
  resolution and positivity/exact-match checking to sema, which reports a
  non-constant or non-positive length or a mismatched repetition count as a
  compile-time error. Raised while carving `std/http`'s router, where a
  module-level `const usize` could not back a buffer or route-table size.
- Implemented the `inline` declaration modifier (Functions & ABI #8). Sema
  checks the declaration rules and, after forward calls resolve, walks inline
  call trees for cycles. Before a function's planners run, `inline_plan`
  attaches to each direct call a copy of the callee (stamped first when
  generic) with fresh binders and `Sym`s, so loop-register and promotion
  planning treat the expansion as the caller's own locals; `gen_inline` binds
  the arguments, emits the copy, and joins returns into a result temp, while
  statement boundaries reset temps to a base that keeps the enclosing
  expression's temps live. `std/map` marks `mix`, `probe`, and its `hash` and
  `same` specializations; `pkg/json` marks eleven `Formatter` helpers. Release
  builds: the 158-byte formatter document drops from 1096 to 821 ns, and
  16k-key `set`/`get`/`unset` from 71.5/21.2/32.4 to 46.5/15.9/26.1 ns; the
  benchmarks' text grows 20% and 6%.
- Limited inline planning to functions that call an inline function: sema
  marks such a body `F_INLCALLS` in `check_call`, `inline_plan` walks only
  marked bodies and marked callee copies, and `plan_loops` searches
  expressions only once a site exists. Unmarked code had paid for both walks:
  checked compiles of the 20,000-function, 20,000-statement, and 20,000-type
  generated corpora had run 6.4%, 9.2%, and 5.3% more instructions than before
  inlining, and now run 1.0%, 2.9%, and 1.1% more.
- Allowed `return f()` to forward a multi-return call whose result tuple
  matches the signature exactly (count, order, and `same_type` per value);
  sema checks it in `N_RETURN`, and lowering already returned any tuple-typed
  expression. Mismatches name the first differing position.

## Types and abstraction

- Added the allocation-free `ascii` module with control-byte classification,
  lowercase conversion, and selectable case-sensitive comparison; HTTP now
  reuses it, exposes serialization and incremental response parsing as
  type-owned methods, uses `str.concat` for multi-part text assembly, and keeps
  `mem.copy` for single-range byte movement.
- Added allocation-free, all-or-nothing `str.concat` over a string-parts slice
  and caller-owned destination storage.
- Added formatted `str.format(destination, "format", ...)`: `fmt.printf`'s
  checked plan (one `check_format_call` for both, parameterized by the
  format's argument position, helper names, and diagnostic name) lowered to
  `put_*` helper calls on the unwritten rest of the destination, returning
  `(0, false)` at the first piece that does not fit. Radix and fixed-float
  rendering moved into the internal `std/@/rendering.ace`, shared by `fmt`
  and `str`. `str.concat` and the `str.format` value overloads no longer
  promise to write nothing on insufficient capacity; `concat` now copies in
  one pass. `demo/sdl3-rgb-cube` builds its overlay with it.
- Added width and zero padding to `fmt.printf`/`str.format` conversions
  (`%5d`, `%08x`, `%06.2f`), and moved both calls onto one lowering over a
  `rendering.Sink`: storage plus a drain, with sticky failure. `printf`'s
  storage is 1024 bytes of the caller's frame drained to standard output, so
  a call makes one write instead of one per piece (about 5x faster on a
  five-piece line to `/dev/null`); `str.format`'s is the destination, with a
  drain that refuses (release speed unchanged). The shared
  `rendering.piece*` writers replace fmt's `print_*` and str's `put_*`
  helpers; padding streams through `Sink.fill`, so width is not bounded by
  any buffer.
- Implemented `@packed` structs and compile-time `@align` on structs and fields,
  with declaration-order effective alignment, exact stored field offsets,
  layout introspection, 16-byte stack/static support, and pointer-only initial
  C interoperability for special-layout aggregates.
- Implemented structs, explicit one-level field embedding, type-owned methods,
  exact method overloads, contracts, canonical signings, explicit contract-value
  construction, and per-used-pair vtables. Promoted methods use fixed-offset
  receiver thunks; fields are never promoted.
- Extended bare (type-omitting) struct-literal contextual typing to `return`
  statements and ordinary call arguments, matching the existing var-decl,
  assignment, field-default, static-map-entry, and union-payload contexts;
  applies equally to plain and generic struct targets. `coerce` now resolves
  a bare struct literal directly, and overload resolution defers a bare
  struct-literal argument's type to its matched parameter the same way it
  already deferred bare array literals.
- Audited aggregate-ABI classification (`classify_hfa`/`classify_ret`/
  `hfa_leaves`) across the cross-product of struct-in-struct, array-in-struct,
  struct-in-array-in-struct, tagged unions, multi-return tuples, and generic
  struct instances, at HFA, register-boundary, indirect, and
  register-exhausted positions, for ordinary Ace-to-Ace calls
  (`tests/emit/basics/abi_matrix.ace`) and, for the categories legal at the
  boundary, extended the clang-checked bidirectional C-ABI oracle
  (`tests/driver/abi.ace`/`abi.c`) with nested-struct and array-in-struct
  cases. No defect found; the existing recursive classification already
  covers every case correctly.
- Method calls auto-deref pointer receivers like field access: sema rewrites
  `p.m()` on `p: *T` (any pointer depth) into the explicit `(*p).m()`, so
  permission checks and codegen are unchanged.
- Extended receiver-marked mutating method calls to every expression position,
  so their return values initialize and assign ordinary values while retaining
  the same visible `mut` receiver mark.
- Implemented explicit generic instantiation and exact function specialization.
  Generic bodies are checked per concrete stamp; compiler-backed standard
  generics share the ordinary resolution path.
- Implemented generic function values (`choose[int]`). Sema resolves an index
  expression over a generic function name to the matching specialization or
  template of that arity, stamping it through the call path's memo; a declared
  function-value type selects among same-arity overloads. The emitter loads the
  address of that stamp's mangled symbol.
- Implemented transparent generic type aliases by stamping and memoizing each
  explicit argument tuple to its fully substituted target type.
- Implemented standalone tuple-literal initialization and whole assignment;
  member expressions resolve independently before exact positional checking.
- Implemented closed tagged unions as an i32 declaration-order tag plus aligned
  maximum-size inline payload. Construction is qualified; switches are
  exhaustive and bind payload copies; no boxing, reflection, or hidden
  allocation is involved.
- Implemented opaque packed 64-bit `Error` values with compiler-assigned nominal
  domains and i32 codes. Errors remain ordinary values. The earlier static
  descriptor representation was removed.
- Added structured error-domain entries and allocation-free `Error.name` /
  `Error.message` lookup, emitting static description data only when used while
  retaining the packed 64-bit representation and arbitrary-code constructors.
- Implemented reserved `panic(message)` as best-effort direct stderr output
  followed by platform `abort()`; it cannot be caught or used as a value.
- Implemented the remaining checked-build diagnostics: exact integer arithmetic
  and shift guards, slice-range `noalias` and byte-operation length contracts,
  null dereference/call/dispatch checks, and an allocation-backed live registry
  for authoritative extent validation and normal-return leak reporting. Release
  objects retain none of this machinery. Added reserved checked-only
  `assert(bool)` with source-located failure and fully stripped release
  evaluation.

## Memory

- Implemented exact data-pointer/address conversions and explicit
  pointer-plus-count slice construction. The compiler preserves written
  permission, lowers the address and two-word descriptor directly, rejects
  locally provable alignment/null/extent violations, and emits only local
  alignment, null, scaling, and end-address guards in checked output.
- Implemented access-site volatile scalar reads and writes with once-only
  address/value evaluation and one exact-width AArch64 access. Packed and
  volatile fields share typed-place offsets and scalar load/store lowering;
  volatility adds no permission, atomicity, synchronization, or barrier.
- Implemented compile-time `alignof(T)` over the full type grammar, exposing the
  compiler's target-native required alignment as `usize` for raw allocation and
  layout checks.
- Implemented nested inline-struct `offsetof(T, field.path)` and zero-code
  module/function `static_assert` over the bounded constant-expression set.
- Implemented raw `mem.alloc`, `realloc`, and `release` over authoritative
  slice descriptors using the platform allocator directly. Release mode carries
  no headers or allocation registry; failures return canonical null, successful
  release nulls only the passed descriptor, and aliases are not tracked.
- Implemented explicit typed allocation overloads. The compiler checks
  `count * sizeof(T)`, derives alignment from `T`, calls the same allocator,
  and returns an uninitialized `mut []T` whose length is the element count.
- Implemented allocation-free `mem.copy`, `move`, `fill`, `zero`,
  `compare`, and `equal` through direct compiler-backed platform operations.
  Empty views avoid calls; release builds assume carved length/alias contracts.
- Fixed slice and string representation at two words (pointer and count), with
  arrays stored contiguously and strings carrying exact non-NUL-terminated bytes.

## Modules and C boundary

- Made `mod.ace` the sole external facade of a standard-library directory
  module, with recursive implementation files importable only from within that
  directory boundary; facade-owned `pub` declarations, including transparent
  type aliases, explicitly define the external surface. Renamed existing
  directory facades from `module.ace` and shortened compiler-reserved imports
  from `@name.ace` to `@name`.
- Added explicit callable C function values with the distinct
  `extern "c" (parameters): return` type spelling, including zero-parameter and
  no-value shapes, contextual references to exact `export`/`extern` functions,
  positive ABI checking, null values with checked-call diagnostics, and
  C-convention indirect lowering. A clang fixture verifies the POSIX thread-entry
  shape in both C-to-Ace and Ace-to-C directions.
- Implemented canonical-path, depth-first module loading with whole-module and
  selective imports, aliases, visibility checks, cycle diagnostics, and
  declaration-before-use ordering. Modules merge into one compilation unit under
  private prefixed identities; canonical files load once.
- Made `from` contextual: the parser matches the identifier text after a
  selective import's member list, so `from` is otherwise an ordinary name.
- Added standard directory modules (`std/name/module.ace`) beside single-file
  modules, with ambiguity diagnostics. OS-layer files remain explicitly imported
  behind stable public facades.
- Represented compiler-reserved standard-library implementation as the
  canonical `std/@/` subtree and unquoted `@file.ace` import source, enforcing
  the existing boundary after path canonicalization while leaving direct
  reserved-source checks available.
- Implemented `extern "c"`, `export`, link names, and C variadics behind a
  positive ABI whitelist. Apple-arm64 lowering covers compatible scalars,
  pointers, distincts, recursive structs, nested arrays, register exhaustion,
  homogeneous floating aggregates, indirect aggregates, and structure returns;
  clang-compiled fixtures serve as the ABI oracle.
- Admitted `bool` at the C boundary as `_Bool` in every integer position,
  including C variadics (promoted to `int`); the backend already kept `bool`
  as a 0/1 byte, so only the ABI whitelist changed. The clang oracle covers
  direct values, struct fields, a `*bool` out-parameter, and variadics.
- Stated untyped storage and the plain-bits type category, and admitted C
  function values in every C-ABI position (struct fields and pointees
  included); the clang oracle calls a callback field by value and through a
  pointer in both directions.
- Added pointer reinterpretation `*U(p)`/`mut *U(p)` between plain-bits
  pointees through the existing pointer-conversion callee: sema checks plain
  bits and permission and proves alignment when `U` is no more aligned than
  `T`, so only an over-aligning view carries the debug alignment check.
- Added the reserved `bitcast(U, value)` builtin for plain-bits types of equal
  size and padding-free source, and removed the four `bits.cast` float
  intrinsics it subsumes. A float-integer pair still lowers to `IR_BITCAST`;
  other pairs go through a stack object. Callers were migrated by a sema probe
  recording each call's result type; the generated IR of the float benchmarks
  and tests is identical to before.
- Added field `@offset(N)` and overlapping structs, which complete storage
  reinterpretation. Layout places an offset field exactly and takes the size
  from the largest field end; sema rejects misaligned offsets and, where fields
  share bytes, non-plain-bits fields, defaults, literals, and by-value C use.
  Overlapping structs lower like unions (in memory, moved as words, a partial
  last word as u32/u16/u8 pieces), `field_off` reads their stored offsets, and
  definite initialization stops its path at them so any member write counts.
- Implemented statement-only AArch64 inline assembly with explicit
  input/output/clobber bindings and no constraint language or reordering.
- Implemented Package System v1's manifest and dependency-graph resolution
  (`src/package.c`): a line-based `ace.pkg` format (`key: value` fields, `//`
  comments, one `[dependencies]` section of `name: relative/path` entries)
  and a depth-first resolver mirroring the existing module loader's
  canonical-path dedup and cycle diagnostics. Validates required fields,
  exact `major.minor.patch`/`major.minor` version formats, `app`/`lib` kind
  (only `lib` may be a dependency), a `root` file confined to its package
  directory, dependency aliases that exactly match the target's declared
  name, one source per declared package name, a shared `ace` language
  version across the graph, and no collision with standard-library module
  names. The `--pkg-graph=DIR` flag exposes this stage for testing, printing
  the resolved dependency-first order with no codegen involvement. Local-path
  dependencies only; no git, lockfile, or registry. Cross-package import
  resolution and the `ace pkg` command set remain separate next steps.
- Wired Package System v1 into `module.c`'s import loader: each loaded module
  now carries its owning `Package*` (propagated from the compiled root, or
  NULL for a manifest-free compile), and a bare/aliased/selective import
  checks the importing module's declared dependencies before falling back to
  stdlib resolution. Every relative import from a package-owned module must
  stay within that package's own canonical directory, which both keeps a
  dependency's non-root files from being deep-imported and blocks a
  package's own relative imports from escaping it; a dependency's further
  dependencies stay invisible to consumers unless redeclared (no automatic
  transitivity). The `--pkg-check=DIR` flag exposes this stage: full
  semantic check of a package's root file with cross-package imports
  resolved, no codegen. The real `ace pkg` command set remains a separate
  next step.
- Implemented the real `ace pkg init`/`build`/`run`/`check` CLI as a distinct
  argument-parsing mode in `main.c`, alongside (not replacing) the
  `--pkg-graph`/`--pkg-check` debug flags. `build`/`run`/`check` find the
  nearest enclosing `ace.pkg` by walking up from the working directory
  rather than taking a directory argument. `check` promotes `--pkg-check`'s
  logic verbatim. `build` resolves and fully checks the graph; a `lib`
  package stops there plus ensuring `build/` exists (v1 libraries are
  source-only, with no reusable archive), while an `app` package additionally
  compiles and links into `build/<package-name>`, reusing the direct
  compiler's TLS-recipe and linker machinery and accepting the same
  `--checked`/`--release`/`--target=`/`--tls-backend=`/`--report` flags.
  `run` is app-only and executes `build`'s linked binary in place,
  propagating its exit code. `init` creates a minimal `ace.pkg` plus root
  file (an app gets `main(): int { return 0 }`; a lib gets an empty root
  module) without overwriting an existing manifest or root file, deriving
  the package name from the target directory's basename. Added
  `tests/package_cli/` fixtures covering app/lib builds, cross-package builds,
  `run`'s app-only restriction, and nested-directory `check` discovery.
- Added `author`, `license`, and `repository` manifest fields alongside the
  existing `description` — all free-form single-line optional metadata with
  no format validation and no effect on identity, resolution, or
  compilation. `ace pkg init` now writes all four fields present but empty;
  an empty value for any of the four is treated as absent rather than the
  usual malformed-`key: value` parse error, so a freshly initialized manifest
  still builds unedited. Added `tests/package/metadata_blank` and
  `tests/package/metadata_dup_author` covering the blank-is-absent carve-out
  and that a real duplicate value is still rejected.
- Implemented native link-input declarations (SPEC.md Package System #14,
  `GRAMMAR.md` §7): a repeatable `[[link]]` manifest section, one record per
  occurrence admitting up to four optional keys once each — `object`/
  `search` (relative to the declaring package, no `..` traversal, `object`
  ending in `.o` or `.a`) and bare-name `library`/`framework` — parsed in
  `package.c` alongside the existing `[dependencies]` section (`path_is_
  contained` factored out of `path_is_local_ace` and reused for both).
  `object`/`search` are existence-checked and canonicalized at manifest-load
  time the same way `root` already is; validated entries populate a new
  `Package.links` list (`ace.h`) in file order. `ace pkg build`/`run` (`main.c`)
  collect every package's `links` across `package_resolve`'s dependency-first
  `order` into the direct driver's own flag vocabulary (`object` positional,
  `-L`/`-l`/`-framework`), de-duplicating exact matches and diagnosing a
  same-identity-different-value conflict (basename for `object`, the name
  itself for the other three kinds) as a resolution-time error naming both
  declaring packages — mirroring the existing one-version-per-package-name
  conflict handling rather than merging or renaming. A `lib` package's
  declared inputs stay dormant until some `app` in the graph reaches the
  link step. Added `tests/package/link_*` (parsing and per-package
  validation) and `tests/package_cli/link_framework`/`link_conflict`
  (end-to-end transitive linking through a real `cc` invocation, and the
  conflict diagnostic); `tests/run.sh` gained the `// expect-build-err:`
  marker since checking alone never reaches the link step.
- Added `[[link]] path: DIR` beside `library`: `package.c` resolves it to
  the exact file in the Apple linker's per-directory order (`.tbd`,
  `.dylib`, `.a`), keeping symlinks as written, and that file replaces
  `-l<name>` at the library's first position in the graph, so the path
  applies to one library, not every `-l`. A dependency's path is a hint that
  falls back to `-l<name>` where the file is missing; the root's is binding,
  overrides the hints, and a missing file is a manifest error. Dependencies
  hinting different paths for one library conflict at link collection
  whether or not either exists, so the outcome never depends on the host. A
  `path` needs a `library` in its record and must be absolute. Records now
  carry their occurrence number so a key can pair with its record's
  `library`.
- Replaced path dependencies with version-only entries resolved through a
  reserved `pkg/` directory (SPEC.md Package System #4–#6, #9–#13): a
  `[dependencies]` value is one exact `major.minor.patch` version, and every
  name in a graph resolves from the root package's directory as the nearest
  existing `<dir>/pkg/<name>` (`find_in_pkg` in `package.c`), which must
  declare that name and version; a dependency's own `pkg/` is never read.
  `package_contains` excludes `pkg/` from a package's canonical directory for
  its root file, `[[link]]` paths, and relative imports (`module.c`). `ace pkg
  add` takes `NAME VERSION`. Git sources, the lockfile, and
  `fetch`/`resolve`/`update` were dropped from the design pending a central
  registry. Package fixtures moved their dependencies under `<case>/pkg/`;
  demos, benches, and `pkg/*/tests` now reach the repository's `pkg/` by
  walking up. The `bench/pkg/` section became `bench/package/`, since a
  directory named `pkg/` is now read as a dependency directory.
- Implemented recursive nested modules: a subdirectory with its own `mod.ace`
  is itself addressable via a `/`-separated path (bare, aliased, or quoted
  relative without a `.ace` suffix). The parser accepts `ident (/ ident)*` for
  a bare/alias-target module reference, threading the full segment chain
  through the import node (`N_IMPORT.c`) while `str`/`b` keep naming the leaf
  segment, the sole bound qualifier. `module.c`'s import resolution walks
  every non-leaf segment requiring `<dir>/mod.ace` (erroring by name if
  absent — a subdirectory without `mod.ace` stays private, not a nested-module
  segment) before resolving the leaf exactly like today's flat-vs-directory
  stdname lookup; the same walk covers std, package-dependency, and quoted
  relative paths. The existing std directory-module privacy check now finds
  the *nearest* enclosing `mod.ace` rather than assuming one level, so a
  nested module's own implementation files stay private to it rather than to
  whichever outer directory module contains it.
- Moved the blocking TCP composition module from the flat `std/tcp.ace` into
  `std/net/tcp/mod.ace`, its first real use as a nested module: `import
  net/tcp` replaces `import tcp` everywhere it is used.

## Standard library

- Folded secure HTTP composition into the shared transport module and marked
  its redirecting client helper with the TLS external component, preserving
  standalone plaintext objects without a separate HTTP TLS file. Plaintext and
  TLS redirect entry points are now private overloads over shared redirect
  state preparation and advancement.
- Reorganized `std/http` as a directory module with focused parsing,
  formatting, client, redirect, transport, and TLS implementation files, and
  replaced six `get` overloads with plaintext `Context` and
  TLS-capable composed `Client { context, tls }` entry points. Redirect and TLS
  choices now live on those reusable values without multiplying overloads.
- Added opt-in bounded HTTP redirects to the owning client through explicit
  `RedirectPolicy` overloads, including relative URL resolution, HTTP-to-HTTPS
  upgrades, downgrade rejection, deterministic limits, and reusable owned URL
  workspace; existing GET overloads retain one-response behavior.
- Implemented the first blocking secure-client slice: a private TLS adapter uses
  system trust by default or decodes one explicit DER override, enforces
  hostname verification and SNI, and exposes partial record/application I/O to
  backend-neutral `tls` storage
  over `tcp`. Consolidated HTTPS into an exact allocation-free `http.get`
  overload with backend-neutral `TlsConfig`, default-null TLS storage, and one
  generic HTTP exchange path. Added compiler-owned conditional external components:
  TLS use automatically links the bundled, selectable BearSSL recipe, while the
  allocation-free plaintext HTTP path emits none of it. BearSSL engine failures retain
  their raw backend code behind a private offset.
- Moved secure entropy into allocation-free `sys.random`, with Darwin
  `getentropy` chunking, `EINTR` retry, and explicit errno failures; collapsed
  the target-neutral TLS facade to `std/tls.ace`; and renamed the compiler-owned
  conditional marker and internal requirement terminology from `@native` to
  `@extern`.
- Generalized compiler-managed external components to ordered recipes keyed by
  component, target, and selected backend. Migrated distributed target-native
  TLS artifacts to `external/` and BearSSL source, headers, rebuild guidance,
  and staging files to `vendor/`; the Darwin BearSSL recipe now carries its
  object, archive, Security, and CoreFoundation inputs.
- Added Darwin system-trust HTTPS as the default behind the stable private TLS
  adapter ABI, while preserving a nonempty DER anchor as an explicit override.
  The adapter feeds BearSSL's delivered chain to the Security framework with an
  SSL hostname policy; SNI remains mandatory. The owning `http.Client` now
  accepts both URL schemes and lazily manages TLS workspace, while the
  caller-storage HTTPS path remains allocation-free.
- Fixed owning HTTPS clients to allocate DNS address storage and grow chunked
  response bodies to the active chunk size instead of retrying unchanged
  capacity indefinitely.
- Pinned BearSSL 0.6 as an experimental TLS-client proof backend and added a
  private adapter/fixture proving direct archive linkage, opaque caller-owned
  client/X.509 storage, supported alignment, half-duplex record-buffer sizing,
  enforced TLS-1.2-only policy, chunked Darwin `getentropy` acquisition through
  `sys.random`,
  and explicit caller-supplied BearSSL seeding without expanding Ace's C ABI or
  expanding Ace's C ABI.
- Standardized concise stdlib naming across public facades, selected OS layers,
  documentation, and fixtures. Subject words remain where a module can expose
  multiple categories (`net.SocketKind`, `sys.FileKind`); established
  abbreviations shorten `Addr` and `Err` without
  clipping ambiguous ordinary words; reversible networking properties use
  `reuse_addr`, `reuse_port`, and `nonblock`; and directory operations use the
  consistent `open_dir` / `read_dir` / `close_dir` family.
- Implemented allocation-free `fmt.print` and `println` overloads using small
  fixed stack buffers and retrying partial writes; portable formatting lives in
  its facade while libc primitives stay in its selected target layer.
- Implemented compiler-planned `fmt.printf` for static format strings. Semantic
  analysis validates exact argument types and builds a format plan; emission
  calls typed helpers directly, with no runtime parser, argument tags, reflection,
  allocation, or C variadic formatter.
- Zero-initialized every deferred-call record, which read uninitialized arena
  memory under the sanitizer build.
- Rejected `fmt.printf` value arguments whose type is still unresolved when the
  plan is built (calls deferred to end of file), which previously matched the
  first helper specialization and silently dropped output.
- Added `%b` binary and `%o` octal `fmt.printf` conversions for `uint` and
  `usize`, generalizing hexadecimal's private helper into one fixed-buffer
  unsigned-radix path. Prefixes, width, and zero padding remain explicit
  additive work.
- Indexed compile-time name lookup: scopes past 32 names build an
  open-addressing hash over their value and type lists, and emit interns symbols
  through a hash table, removing the quadratic scans that made 20,000 generated
  functions take 12.9 s (now 0.4 s). Added `bench/scaling-corpus.sh`, the
  20,000-function point, and growth columns to `bench/run-compile.sh`.
- Removed the remaining per-function quadratic scans behind long-function
  compile time: a `PtrMap` (open-addressing pointer hash in `lexer.c`) indexes
  sema's definite-initialization variables and emit's frame slots and promotion
  candidates, and select lowering looks a name up among the few selections
  before it searches the locals. A 20,000-local function compiles in 92 ms
  (was 2.35 s) with identical machine code.
- Removed the cubic merge from definite initialization. Joining two branch
  snapshots searched each for every variable of the base snapshot; since
  `k->vars` only grows at its head and snapshots list it head first, an
  earlier snapshot is the tail of a later one, so each base variable is found
  by offset. A function of 5,000 locals and 2,500 `if`/`else` statements took
  378G instructions to check, now 2.5G; the regex and json package checks gain
  0.5%. Snapshot cloning itself stays linear in the declared variables.
- Replaced definite initialization's whole-function snapshots with an undo
  trail. A variable's state is saved before its first change after a branch
  point; a branch collects the variables it changed and unwinds, and the paths
  reaching a join intersect only those, a path that left one alone standing in
  with the unwound state. `if`, `switch`, loops, and labeled blocks cost what
  their branches change, not every variable declared so far. A function of
  10,000 locals and 5,000 loops checks in 376M instructions (was 4.2G), with
  5,000 `if`/`else` 298M (was 9.4G); the regex and json package checks gain
  0.55-0.75%. Random programs mixing partial writes with every branch form
  report identical diagnostics under both.
- Added `tests/definite-init-diff.sh`, the differential check behind that
  change: an awk generator writes random functions mixing partial struct and
  array writes with every branch form, and two compilers must report
  byte-identical diagnostics. Both compilers must exist, any baseline output
  other than initialization diagnostics (another diagnostic, a crash) fails
  the run, and mismatching programs are kept.
- Materialized a float constant with `fmov Vd, #imm8` when it is one of the 256
  encodable values (0.125 to 31.0 in sixteenths of a power of two, either sign),
  and folded the scaled index into a scalar array or slice load as
  `ldr Rt, [base, index, lsl #n]` when the element is 1, 2, 4, or 8 bytes wide
  (`index_addr`; stores and other strides keep the separate add). On a `saxpy`
  loop over `f64` arrays, 1.25 ns fell to 0.97; a branchy `f32` collision loop
  fell 19%, a `u8` blend 9%, and a dot product 5%.
- Corrected the float load and store fallbacks for an offset the scaled `imm12`
  cannot reach (over 32 KiB for `f64`, 16 KiB for `f32`). Both materialized the
  byte offset in `x16` and then emitted the unsigned-offset opcode, where the
  `Rm` field lands inside `imm12`: the load ignored `x16` and read a wrong
  constant displacement instead. They now emit the register-offset form,
  `ldr`/`str Vt, [base, x16]`, matching the sibling integer paths. Reachable
  through a scalar tuple member past that distance.
- Read a global integer `const` with a compile-time initializer as an immediate
  instead of a memory load, emit `and`/`orr`/`eor` with an encodable constant
  operand as one bitmask-immediate instruction, and let call-free functions keep
  every used local and parameter in x9-x14 even when read once, removing the
  argument spill and reload from scalar leaf calls (`call-1` 5.7 to 3.2 ns).
- Returned struct literals and tuples whose leaves are call-free scalars, one
  whole 8-byte integer or float per result register (`gen_return_regs`),
  evaluate straight into `x0`-`x1` or `d0`-`d3` instead of a frame temp that is
  then reloaded; leaf w is computed with temp w free so earlier leaves stay live.
  Other shapes (packed sub-8-byte fields, missing fields, call-bearing leaves,
  sret) keep the temp path. `struct-8` 9.3 to 7.6, `struct-16` 9.8 to 7.5,
  `struct-hfa` 13.7 to 12.3, `tuple-return` 16.4 to 12.9 ns.
- A call whose result returns in registers as whole 8-byte chunks (an HFA, or a
  struct or tuple whose size is a multiple of 8) stores them straight into a
  frame-resident local destination instead of a staged temp plus copy: `x = f()`
  and `x: T = f()` into a plain local slot or a field of one, and typed tuple
  destructuring into whole 8-byte scalar binders (`gen_call_to_slot`,
  `gen_call_to_binders`). A one-shot `call_sink` armed by the statement is
  consumed by `emit_call` only for the matching call node and return type, so
  nested calls in arguments never take it. `struct-8` 7.6 to 6.1, `struct-16`
  7.5 to 5.5, `struct-hfa` 12.3 to 9.8, `tuple-return` 13.0 to 11.6 ns.
- A by-value aggregate argument that is a plain frame-resident local (or a
  field of one) and rides registers is loaded straight from its slot at
  marshal time (`ldur x<n>` / `ldur d<n>`) instead of through an address temp
  that was spilled and reloaded; the calling-convention units are now computed
  before the arguments are evaluated. Slice arguments keep the address temp in
  checked builds for the `noalias` range check. Text of `bench/calls.ace`
  fell 33,984 to 33,712 bytes; the call benchmarks moved within layout noise.
- A call-free function's read-only by-value HFA parameter whose every use is a
  constant read of one float leaf (`p.x`, `r.max.y`) keeps only the leaves it
  reads in v23 downward, copied once from the incoming argument registers, with
  no frame slot (`plan_param_homes`, `param_home`). Any whole-value use, `&`,
  runtime index, or call in the body, or more than eight homed leaves within
  the fp temps the body leaves free (`vtf_cap`), keeps the slot. Float `+ - * /`
  reads a homed operand in place, as `src_reg` does for integers. v24-v31 are
  the byte helpers' scratch, so homes sit at the top of the fp temp pool.
  `struct-hfa` 9.7 to 4.9 ns.
- The same read-only leaf-read rule homes whole 8-byte integer-class leaves of a
  by-value struct of at most 16 bytes that arrives in `x` registers: each read
  leaf is copied once into the x14-down registers leaf promotion leaves unused,
  within the int temps (`vti_cap`), and `src_reg` reads it in place. Packed or
  mixed-class structs and narrower leaves keep the slot. `struct-8` 6.5 to 3.9,
  `struct-16` 5.4 to 2.4 ns.
- Float `+ - * /` reads a promoted float local or a register-homed parameter
  leaf in its own register (`fsrc_reg`, the float twin of `src_reg`; the HFA
  fast path in `gen_binop` now goes through it), and an assignment to a promoted
  float local (`x = a op b`, `x op= y`) emits the final operation straight into
  its register. The old `fmov` copy in and out cost about 1 ns each on a
  loop-carried chain. Call-free sum 3.4 to 1.2, multiply-add 3.4 to 1.3,
  `x = x * 0.5 + 1.0` 4.9 to 2.7 ns; the call-carried `float-acc` workload
  4.8 to 2.4 ns.
- Fixed a multi-register return with a pending `defer` returning a corrupted
  second register: `emit_defers` addressed save-area unit w above the slot
  (`retsave_off + w * 8`), which fell below `sp`, instead of within it
  (`retsave_off - w * 8`), so the deferred call's frame overwrote it.
- A union `switch` loads the scrutinee's tag once, before its compare chain,
  instead of reloading the parked address and the tag for every pattern
  (`emit_union_switch`). A scalar field read folds the field's constant offset
  into its load, `ldr Rt, [base, #off]` or the register-offset form past the
  scaled `imm12`, instead of a separate `add` (`field_base`, shared with
  `gen_addr`). The regex bench retires about 5.8% fewer instructions and runs
  3-11% faster (`pathological` 244 to 216, `word-ing` 89 to 79 ns/B); `calls`
  retires 3.3% fewer.
- Register promotion plans integer-class locals by live interval instead of
  for the whole function (`plan_promotion`). `promo_scan` numbers positions in
  emission order and gives each candidate [first, last]: a local is defined
  after its initializer, a parameter at entry; an operand the emitter reads in
  place lives until its consumer runs, a direct call argument to that call's
  `bl` and any other use to the end of its statement part; a use in a loop (or
  labeled block) begun after the definition extends to its end; a use inside a
  `defer` lives to the exits. Real calls are recorded at their `bl`, except the
  deferred call itself, whose arguments still run at the statement. Candidates
  go heaviest first into a register holding no overlapping interval: x14 down
  to x9 in any function when the interval crosses no call, else x19 up.
  `emit_call` errors if a call lies strictly inside an x9-x14 interval or was
  never scanned, except for exit-time defer calls. `store_preg` retargets the
  instruction that just produced a value to write the promoted register
  instead of copying after it, when no label is bound in between and the
  result already has `set_preg`'s width (`fold_preg`). Integer union payload
  binders are promoted and load their payload straight into the register.
  The regex bench retires 7.2-8.3% fewer instructions per workload and runs
  8-17% faster; `calls` retires 1.6% fewer and `cpu` 3.3%.
- `ins` rewrites the instruction just emitted to absorb the next one when no
  label is bound and no site recorded between them (`peep`): `mov t, p` folds
  into a following op that reads and overwrites `t` (add/sub and logical
  immediates, `sbfm`/`ubfm`, shifted add/sub, logical register ops other than
  the mov alias, 2- and 3-source ops, integer loads by unsigned or register
  offset; a 32-bit copy feeds only 32-bit ops), and `add t, s, #a` folds into
  a following `ldr t, [t, #b]` or `add t, t, #b` when the sum fits. Fixups,
  branch sites, relocations, and line rows set `peep_bar`, and it runs only
  inside a function body without `asm`.
- A by-reference parameter's pointer (method receivers, `mut` parameters
  under the internal ABI) is an integer-class promotion candidate: it never
  escapes, takes part in interval sharing, call crossing, and the `emit_call`
  x9-x14 check like any local, and at entry moves from its argument register
  or stack word into its register with no slot. Addressing copies it into a
  temp (`bref_reg`), which `peep` folds into the consuming load, and a scalar
  read loads straight through it. With the peephole, the regex bench retires
  11.1-14.0% fewer instructions per workload (the whole bench 2.72x C, from
  3.10x) and spends 1.9-7.4% fewer cycles, repeatable across two interleaved
  rounds; the peephole alone had saved 5.6-7.4% of instructions but no time.
  `calls` and `cpu` are unchanged.
- A register range loop that ascends by one (`range_up`: step omitted or a
  constant 1, starting at a constant zero over an unsigned type or between
  two constant endpoints in order) loads its start into the cursor and its
  end into a second register, or a cmp immediate up to 4095, and runs
  `cmp`/`b.hs` (`b.ge` when signed) then `add #1`: no bound slots, direction
  test, or division, and one or two loop registers rather than three
  (`loop_own_regs`). Every other range keeps the counted lowering. The regex
  bench retires 7.6-11.0% fewer instructions per workload (the whole bench
  2.47x C, from 2.72x) and spends 5.1-11.2% fewer cycles on all but
  `captures`, flat; `calls` retires 1.2% fewer but spends 1.7% more cycles, all
  in two cases whose smaller frame puts a struct's slots within `ldur` reach
  (see `TODO.md`).
- Checked builds test a runtime range step once at loop entry, after both
  bounds and even over an empty interval: at the step's own width (narrow
  steps extended by the type's signedness), a signed step must be above zero
  and an unsigned one nonzero, or the loop aborts with `file:line:col: range
  step violation` at the step. The located-diagnostic writer behind bounds
  failures is shared (`emit_located_fail_call`). Constant steps, already
  checked by sema, and release builds emit nothing.
- `pkg/regex` declares its `NIL` sentinels (`matcher.ace`, `program.ace`) as
  `const`, so each read is an immediate rather than a global load. The regex
  bench retires 1.5-1.9% fewer instructions per workload, with cycles within
  1% on each and 2.9-3.3% lower for the whole bench across two rounds.
- Removed the quadratic scans live-interval promotion brought back to
  long-function compile time, where interval sharing lets thousands of locals
  hold registers: candidates are ranked once by weight (ties in declaration
  order) instead of rescanned per pick; each register keeps its intervals
  sorted, so an overlap test is one binary search; a loop keeps the list of
  candidates it holds live instead of `scan_loop_pop` visiting all of them;
  and past 16 assignments `promo_reg`/`bref_reg` look a binder up through a
  `PtrMap` and the `emit_call` x9-x14 check searches merged interval
  interiors. Machine code is byte-identical. A 20,000-local function compiles
  in 87-91 ms again, from 1.60 s; the function and type corpora are unchanged.
  `bench/run-compile.sh` times `cc -O2` for the scaling shapes too.
- `&&` and `||` as a value branch on each operand but the last as a condition
  (`gen_branch`, flags straight to `b.cond`) to the short-circuit constant,
  and take the last operand's own 0 or 1 as the result: only the final
  compare materializes a bool, and a stored bool is normalized as `!` does
  (`bool_exact`). `Queue.has` in `pkg/regex` falls from 23 to 17
  instructions. The regex bench retires 1.2-2.1% fewer instructions per
  workload and spends 0.3-1.3% fewer cycles (1.4% for the whole bench) over
  ten interleaved runs; `calls` and `cpu` are unchanged. Settling the last
  operand through a branch too measured no better; a by-hand nested-`if`
  rewrite stays about two points faster in cycles, unexplained.
- A single-binder local of a struct with at most four direct scalar fields
  and no padding (not packed or over-aligned) lives in registers field by
  field. Each field gets a placeholder binder that competes for a register as
  a scalar local does (`split_add` in `promo_scan`), so live intervals, call
  crossing, and the `emit_call` x9-x14 check apply per field; a field that
  loses stays in the struct's slot, allocated only then. A field read or
  write uses its register (`split_reg`, read in place by `src_reg`). Whole
  assignments go field by field (`split_write`): a literal evaluates every
  value before writing any, a copy from another such local reads all fields
  first since the two may share registers, and any other source loads each
  field at its own width from its address; a store to a place is one store
  per field (`split_store_at`). Passed by value, returned, or assigned from a
  register-returned call, the fields move straight between their registers
  and the argument or result registers, packed with `bfi` and split with
  `ubfx` (`split_unit`, and `split_take` through the call sink). Any other
  use keeps the struct whole in its slot: an address, `mut`, a method
  receiver, a field used as a place, `==`, a deferred argument, or passing a
  struct the ABI sends by address (over 16 bytes, not an HFA), which as
  fields would be copied to a temp at every call. `place = T{...}` of such a
  type stores each evaluated field straight into the place instead of
  building a temp and copying it (`lit_store_at`). The `pkg/regex` matcher
  now loads each popped frame field at its width and stores each pushed
  field. The regex bench retires 2.9-3.4% fewer instructions per workload
  and spends 7% fewer cycles on `captures` and 3.0-3.5% for the whole bench
  over two interleaved rounds (`pathological` flat); `calls` retires 2.3%
  fewer and spends 3.6% fewer cycles, with `struct-8` at 1.20x noinline C
  from 1.70x, `struct-16` 1.00-1.09x from 1.38x, `struct-hfa` 3.06x from
  3.38x, and `struct-24` unchanged.
- A union payload binder of such a struct type is split the same way
  (`split_add` at its `N_VARPAT`): the matching arm loads each field that won
  a register straight from the payload, copies only the remaining fields to
  the binder's slot, and allocates that slot only for them. An address or
  other whole-struct use keeps the full payload copy. A loop whose `pc` feeds
  through a `Fork` payload's field ran at 1.00 ns per iteration, from 3.49 ns,
  and `pkg/regex`'s `Matcher.add` no longer stores and reloads its split
  targets; the regex bench is unchanged at four controlled code placements.
- A register-homed float leaf of an HFA parameter now stays in the argument
  register it arrives in instead of being copied to v23 down. Float temps map
  through a per-function table (`vreg`, `vmap`), and `plan_param_homes` swaps
  each home's top temp index with its argument register's index, so no temp
  ever lands on a live home. A float result that the last instruction
  computed into a dead temp is retargeted to its result register (`fold_fp`,
  deleting an `fmov` that becomes `fmov d, d`), for a scalar return and each
  leaf of a returned literal (`gen_return_regs`). A literal leaf whose result
  register is the home of a parameter leaf a later leaf reads waits in its
  temp, which the swap placed in v16-v23, and moves after every leaf is
  computed. `vec_add` is three `fadd`s, and `calls` runs `struct-hfa` at 3.39
  from 4.42 ns and `float-acc` at 1.93 from 2.41 ns at eight controlled code
  placements, with every other row and the `cpu`, `string`, `hotloop`,
  `memory`, and regex code unchanged.
- Implemented `str` character/string operations and explicit
  `str.parse[T]` specializations in pure Ace where practical, with libc float
  conversion isolated in its selected target layer. `char` remains the
  transparent byte alias and strings remain UTF-8 byte sequences.
- Implemented `sys` file-descriptor I/O, descriptor constants, errno capture,
  command-line arguments, and environment access through thin platform
  bindings.
- Implemented `mem.alloc_size` as a compiler-backed raw `mem` name lowered to
  the platform allocator's size query; the benchmarks use it and `time.awake`
  in place of their private externs.
- Implemented `time.elapsed` and `time.awake` as separate monotonic clocks in
  the `time` module over a private Darwin binding.
- Renamed `json.writer_new` to `json.writer`: a constructor carries the `_new`
  suffix exactly when a matching `_wipe` releases what it returns, and the
  writer is a plain value over a caller-owned buffer.
- Added the Darwin Apple-arm64 `sys.ioctl` escape hatch with the native
  `unsigned long` request width, variadic-call ABI, caller-owned byte arguments,
  immediate `IoctlErr` capture, and SDK-verified BPF constants/layouts; the
  allocation-free BPF demo manually walks aligned native headers and bounded
  Ethernet/IPv4 bytes, with deterministic decoding and ABI fixtures.
- Added immediate `sys.exit(code)` through the OS-neutral `sys` facade and
  a private Darwin OS layer calling libSystem `_exit`; it runs neither defers
  nor C-runtime exit handlers and requires no compiler intrinsic.
- Implemented blocking IPv4 TCP `net`: socket creation, explicit options,
  bind/listen/accept/connect, partial send/receive, peer addresses, DNS
  resolution, and operation-specific error domains. The public directory module
  wraps a private Darwin OS layer and owns no hidden allocation.
- Implemented the first allocation-free HTTP slice: strict IPv4 endpoint and
  HTTP URL parsing, all-or-nothing caller-storage request serialization, and
  incremental response parsing for bodyless, fixed-length, chunked,
  connection-close, and informational framing with caller-owned views/storage.
- Completed the symmetric HTTP message surface with `RequestParser` and
  `RequestResult`, renamed response state parsing to `ResponseParser.parse`,
  and exposed all-or-nothing `Request.format` and `Response.format` methods.
- Made both HTTP parsers genuinely incremental by preserving phase, line scan,
  framing, header, and body progress across calls instead of reparsing the
  complete occupied input prefix.
- Added HTTP message and parser-progress field defaults, named method and
  status constants, and conventional response reason selection for a null
  reason while preserving explicit and unknown-status empty phrases.
- Renamed the HTTP formatting error domain to `FormatErr`, matching the public
  `Request.format` and `Response.format` operations.
- Added named, allocation-free descriptions to the existing HTTP URL,
  formatting, and client error codes without changing their domains or numeric
  values; HTTP now constructs those failures through their named entries.
- Exposed the existing allocation-free HTTP status text lookup as
  `http.status(code)`, returning conventional phrases for known codes and an
  empty string for unknown codes without changing `Response`; removed the
  redundant public numeric status constants in favor of ordinary status codes.
- Added named descriptions for standard-library-defined TCP terminal conditions,
  network and poll validation failures, resolver statuses, and deterministic
  system-facade validation failures while preserving raw OS error passthrough.
- Added allocation-free `http.get` over neutral caller-owned `Storage` and an
  owning heap-backed `Client` whose workspace buffers are lazily allocated and
  automatically resized, with the conventional explicit `client_new` and
  `client_wipe` lifecycle operations.
- Added pure-Ace blocking IPv4 `tcp` composition over `net`: listener and
  connection constructors with failure cleanup, blocking accept, exact receive,
  all-byte send, EINTR retry with retained progress, and explicit wrapper close.
  Lower-layer operation errors remain unchanged; only premature EOF and
  successful zero-progress send introduce TCP-level domains. A fresh blocking
  echo fixture establishes the Stage 2 carving baseline without preselecting a
  nonblocking event model or public reactor state shape.
- Implemented the selected `tcp` nonblocking connection-progress model with
  caller-owned per-call buffers, explicit tagged events, managed poll interest,
  partial-send offsets, backpressure, and nonblocking connect completion.
  Added the one-operation `net.pending_error` prerequisite and named Darwin
  errno constants without adding OS code or externs to `tcp`.
- Reshaped the public `tcp` vocabulary around its module scope: `Conn`,
  `Interest`, and `Event` keep type names compact; blocking and nonblocking
  construction remain the distinct `connect` and `connect_nonblock` operations;
  `interest` exposes readiness state; and whole-buffer receive/send use the
  paired `receive_all` / `send_all` names with `ReceiveAllErr` / `SendAllErr`.
- Implemented general descriptor `poll` with allocation-free queues, paired
  watch/unwatch, level-triggered read/write interest, error/hangup reporting,
  caller-owned event storage, and finite/infinite nanosecond waits. Darwin kqueue
  is private to the OS layer.
- Proved nonblocking TCP server composition over `poll` with a forked loopback
  echo fixture: the listener and accepted peer are configured explicitly,
  readiness drives one-attempt accept/receive/send operations, would-block stays
  caller-visible, partial progress is retained, and all descriptors are closed
  explicitly. It exposed the need to query an OS-selected local endpoint.
- Extended the proof into a deterministic multi-client chat fixture with one
  single-threaded event loop, fixed caller-owned client and buffer storage, and
  forked child test clients. It covers multiple accepts, readiness-driven
  fragmented reads and writes, per-recipient broadcast progress, orderly
  disconnects, and reuse of a vacated client slot without adding API surface,
  allocation, threads, atomics, or retry policy.
- Added allocation-free `sys.Process`, `fork`, and `wait` wrappers with decoded
  exit/signal status, keeping process-control externs in the private Darwin OS
  layer. Added `net.local` for querying an ephemeral bound endpoint; networking
  fixtures now bind port zero instead of guessing ports from process IDs.
- Removed generated programs' `strlen` dependency: `sys.arg` and `sys.env` now
  scan their OS-provided NUL-terminated entry bytes directly in emitted code.
- Removed generated programs' `memcmp` dependency: `mem.compare`, `mem.equal`,
  and byte-sequence equality now use direct emitted unsigned-byte loops.
- Removed generated programs' `memset` dependency: `mem.fill` and `mem.zero`
  now use direct emitted byte-store loops.
- Removed generated programs' `memcpy` and `memmove` dependencies: `mem.copy`
  emits a forward byte loop and `mem.move` selects forward or backward traversal
  from the source and destination addresses.
- Added the public `bits` standard module with exact compiler-backed
  `from_float` and `to_float` overloads for `f32`/`u32` and `f64`/`u64`; all
  four preserve complete IEEE-754 representations through register moves with
  no runtime call. Added the restricted trailing `intrinsic` declaration form;
  admitted declarations receive real callable symbols while direct calls may
  inline the same instruction sequence.
- Extended `bits` with exact `u8`/`u16`/`u32`/`u64` population count,
  leading/trailing-zero count, byte-swap, and rotate overloads. Counts return
  `uint`, zero-count operations return the operand width, and rotations reduce
  every `uint` count modulo that width; direct and indirect calls use native
  Apple-arm64 scalar/SIMD instruction sequences with no runtime dependency.
- Shortened the public zero-count families to `bits.lead_zeros` and
  `bits.trail_zeros`, retaining their exact overloads and lowering.
- Added pure-Ace `math.isnan`, `math.isinf`, and `math.isfinite` overloads for
  `f32` and `f64`, using `bits` to classify every IEEE-754 encoding without
  allocation or a libm dependency.
- Added `math.sqrt`, `math.floor`, and `math.ceil` for `f32` and `f64` as
  compiler-provided intrinsics (`BI_MATH_*`, marked in `module.c`, signature-
  checked in `sema.c`) lowering to in-place `fsqrt`, `frintm`, and `frintp` on
  the operand's FP temp, with the same instruction in the callable symbol. The
  intrinsic whitelist grew from `bits` to include these `math` overloads.
- Added pure-Ace `math.exp` and `math.log` for `f32` and `f64`, within 1 ULP over
  the whole finite domain. They use the fdlibm scheme: `exp` reduces by
  `k * ln2` (split high/low) and evaluates a rational polynomial, scaling by
  exact exponent-bit construction in two steps for subnormal results; `log`
  splits the exponent, normalizes the significand into `[sqrt(2)/2, sqrt(2))`,
  and evaluates a polynomial in `s = f / (2 + f)`. The `f32` overloads compute at
  `f64` and round once. Verified against platform libm on 2.2M inputs with none
  beyond 1 ULP; libm is a test reference only.
- Added pure-Ace `math.sin` and `math.cos` for `f32` and `f64`, within 1 ULP
  over the whole finite domain. Small arguments use the fdlibm sine and cosine
  kernels; arguments below about 1.6e6 use three-stage Cody-Waite reduction by
  pi/2; larger arguments use Payne-Hanek reduction: the 53-bit significand
  times eight 32-bit limbs of a 1280-bit 2/pi table (generated with big-integer
  arithmetic) forms a 256-bit fixed-point sum whose truncation error is 2^-170,
  from which the quadrant and two exact 53-bit fraction pieces are taken, then
  multiplied by pi/2 as a double-double (Dekker product, no fused multiply-add).
  Verified against 960K libm comparisons and, independent of libm, against
  exact big-integer arithmetic: 22K random arguments (worst 0.80 ULP) and 2.5K
  continued-fraction worst cases whose reduced fraction reaches 2^-62 (worst
  0.50 ULP).
- Added pure-Ace `math.pow` for `f32` and `f64`, table-driven, with tables and
  constants derived in `std/@/pow_tables.ace` from 100-digit arithmetic.
  `log(x)` is `k*ln2 + log(c) + log1p(r)` with `c = 1/invc` from a 128-entry
  table whose interval holding 1 uses `c = 1`; `invc` is a short binary
  fraction, so `r = z*invc - 1` splits into exact halves and `k*ln2hi + logc`
  is exact. The log is a high and low word with a Taylor tail to `r^10`;
  `y*log(x)` is formed in extended precision (26-bit halves); the result is
  `2^e * 2^(j/128) * exp(er)` from a table of scales and relative corrections
  with a degree-5 Taylor polynomial, where `n = 128e + j` is kept as a signed
  integer so no unsigned wrap is needed. Sign is a final flip; exponents near
  the ends of the range scale in two steps so only the last multiply rounds,
  which keeps subnormal results within 1 ULP. `f32` computes at `f64` and
  rounds once. Against an exact 70-digit reference the worst error is 0.506 ULP
  over 93K normal-result inputs (0.61 ULP in the subnormal range); against
  libm 200K inputs match bit-for-bit or differ by 1 ULP. Exact integer powers,
  C99 Annex F cases, and subnormal and overflow edges are tests. `pow` runs at
  51 ns against libm's 9.5 ns; the remaining gap is backend code quality.
- Added a dedicated `f32` `math.pow` kernel replacing `f32(pow(f64, f64))`:
  plain-`f64` arithmetic over the same tables (no tail word), a log series to
  `r^5`, a degree-3 exponential, and one final conversion that rounds into the
  subnormal range or to infinity. Zero, infinite, NaN, and negative bases and
  non-finite exponents defer to the `f64` overload. Worst error is 0.5025 ULP
  over 59,890 inputs against a 60-digit exact reference; 54 ns fell to 17 ns.
- Inlined finite negative bases with ordinary exponents into `pow(f64)`: the
  units bit of `y` gives the parity by one shift, replacing the `pow_special`
  call and its tuple return. Bit-identical to the old path over 400K operands;
  negative-base integer powers went from 76 ns to 50 ns, positive bases unchanged.
- Added exact fast paths to `pow` for both widths when the exponent is 2, 1,
  -1, or 0.5: a zero-mantissa test selects the powers of two, then `x * x`, `x`,
  `1 / x`, and `sqrt(x)` (positive bases only, so `pow(-0, 0.5)` and
  `pow(-inf, 0.5)` keep the general path). Results are correctly rounded and
  agree with the kernel within 1 ULP over 8M operands; the general path shows no
  measurable cost, and these exponents run at about 7 ns against 49 ns (`f64`).
- Added `math.fma` for `f32` and `f64` as a compiler-provided intrinsic
  (`BI_MATH_FMA`) lowering to `fmadd`: `a * b + c` with one rounding of the
  exact product and sum. The three existing math intrinsics are one-operand and
  write their result in place, so the three-operand form needed its own
  signature clause in `sema.c`, its own emitter, and its own inline expansion,
  which evaluates the operands into FP temps and frees the last two. An
  ordinary `a * b + c` stays two instructions: nothing contracts it.
- Put `exp`'s and `log`'s polynomials on `math.fma` and nested `log`'s
  single-use names: `exp`'s five-term Horner chain becomes five `fmadd`s, and
  `log`'s `t1`, `t2`, and `r` collapse into one expression whose two Horner
  chains and closing `s * (half_f2 + r) + dk * LN2_LO` step fuse as well. Both
  kernels are latency-bound, their dependent chains running about three times
  their throughput, so cutting a Horner step from a multiply plus an add to one
  fused instruction pays on the chain and not on throughput: `exp` fell from
  39.8 to 35.1 ns and `log` from 48.4 to 42.4 ns on a dependent chain, with
  throughput unmoved. The nesting on its own is bit-identical. Against an exact
  90-digit reference over 25,200 inputs covering the finite domain, both
  reduction boundaries, and dense bands near zero and near 1, the worst case is
  unchanged at 0.8025 ULP for `exp` and 0.7517 ULP for `log`; 12 results move at
  all, none past 0.7 ULP, and each of `exp`'s seven lands closer to the exact
  value than before.
- Gave `sin` and `cos` the same treatment, and folded away the `reduce`
  dispatcher: `sin` and `cos` now choose between Cody-Waite and Payne-Hanek
  reduction themselves. That wrapper was one call deep and returned a
  three-value tuple, and since float promotion reaches `d24-d31` every call
  spills and reloads those registers, so removing the layer was worth 28 per
  cent of `sin` on its own while leaving every result bit-identical. On top of
  it, both kernel polynomials moved to `math.fma`, `kernel_cos`'s single-use
  `hz` folded into its one use, `reduce_medium`'s leading Cody-Waite product
  fused (`fn * PIO2_1` is exact, so `r` cannot change), and `product_error`
  disappeared: the low part of a product is by definition `fma(a, b, -p)`, one
  instruction where Dekker's splitting took eleven, which also retires the
  `SPLIT` constant. In the Cody-Waite band `sin` fell from 39.0 to 29.0 ns and
  `cos` from 38.2 to 27.6 ns, below pi/4 from 13.4 to 13.1 and 15.8 to 11.5 ns,
  and beyond 1.6e6 from 607 to 595 ns; the dependent chains fell about a fifth.
  Against an exact reference built from Machin's pi and Taylor series, over
  57,000 arguments spanning the finite domain and 24,000 of them placed within
  a few ULP of a multiple of pi/2 where the reduced argument cancels hardest,
  the worst case is unchanged at 0.7871 ULP for `sin` and 0.7884 ULP for `cos`;
  only `sin` moves at all, on 96 of 33,000, and lands closer to the exact value
  on 66 of those. Reassociating the kernels' accurate-tail expression onto
  `fma` was measured too and rejected: it bought nothing and pushed the worst
  case to 1.48 ULP, past the documented bound.
- Rewrote `exp` and `log` as table-driven kernels over the two 128-entry tables
  `pow` already uses, so all three now share `pow_tables`. `exp` reduces by
  `n*ln2/128` instead of `k*ln2`, reads `2^(j/128)` from the table, and applies
  a fifth-degree correction, which retires the rational form and with it the
  division `(r*c)/(c-2.0)` that sat on the dependent chain; `log` splits the
  exponent, takes `log(c)` from the table, and evaluates `log1p` of the residue,
  which retires the seven-term polynomial in `s = f/(2+f)` and its division too.
  `pow` carries its logarithm as a high and low double because the value must
  survive multiplication by the exponent, and that is the bookkeeping a
  standalone kernel does not need: `exp` and `log` return one rounded double, so
  the low-word arithmetic collapses to a single compensation term. Two table
  facts make the short form exact where it has to be. `z*invc - 1` is
  representable for every entry, so one `fma(z, invc, -1.0)` replaces `pow`'s
  split of `z` into halves and carries `r` with no error at all; and `1.0` sits
  at the exact centre of entry 71, whose `invc`, `logc`, and `logctail` are
  `1.0`, `0.0`, and `0.0`, so near 1 the kernel degenerates to a plain `log1p`
  series with nothing to cancel and `log(1)` is `+0` by construction. Elsewhere
  `w + r` does cancel, losing up to half an ulp of itself, and `(w - t) + r` is
  that loss exactly; carrying it is what keeps a single rounded double honest,
  and dropping it costs a full ULP. `|r| <= 0.00585`, so `log1p` needs the
  series only to `r^8`: `LOG_C9` and `LOG_C10`, which `pow` still uses, do not
  appear. Instruction counts barely move — `exp` 208 to 204 and `log` 196 to 192
  — because the win is dependency-chain depth, not code size. Over 4096 inputs,
  32 repeats, best of seven rounds, glue-subtracted through a non-inlined
  identity: `exp`'s dependent chain fell from 38.3 to 15.2 ns over [-1, 1] and
  from 50.7 to 15.2 ns over [-700, 700], its throughput from 13.1 to 5.8 ns;
  `log`'s chain fell from 45.1 to 23.8 ns and its throughput from 15.5 to 7.8
  ns. The old figures carry the wider spread: `exp`'s branch on `k == 0` makes
  its cost input-dependent, so its baseline throughput ranges over 13-15 ns and
  its chain over 38-53 ns run to run, while the table-driven kernels are
  branch-free on the hot path and reproduce to 0.2 per cent. Accuracy improves,
  so the 1 ULP guarantee is untouched: against an exact 90-digit reference over
  48,216 inputs covering all 128 table-entry edges and their neighbours, 400 ULP
  either side of 1, every power of two, subnormal inputs and subnormal results,
  and both of `exp`'s boundaries, the worst case falls from 0.8050 to 0.7495 ULP
  for `exp`, at -709.04535484224982, and from 0.8128 to 0.5000 ULP for `log`, at
  0.99999999999999978. The `f32` overloads, which round the `f64` result once
  more, are correctly rounded at 0.5000 ULP over 185,065 inputs covering every
  `f32` exponent. `pow_special`'s literal `234187180623265792` became
  `LOG_SUB_OFF`, which `log` shares for the same subnormal rescaling, and the
  sixteen constants the old scheme needed — `LN2_HI`, `LN2_LO`, `INV_LN2`,
  `EXP_HALF_LN2`, `EXP_P1`-`EXP_P5`, and `LOG_L1`-`LOG_L7` — are gone with it.
  `exp`'s hot-path guard admits powers down to -1022, where the scale stops
  being a normal double, rather than reusing `pow_extreme`'s -1000: the two are
  output-identical over 18,139 inputs in the underflow band, because between
  them the result is still normal and the two-step path's closing multiply by
  2^-1000 is an exact shift rather than a second rounding, so the wider bound
  was taken only for stating the guard's own limit. Spelling that guard as one
  unsigned range test instead of two signed compares was measured and rejected:
  0.7 per cent on the chain and 0.6 per cent against on throughput, a wash
  either way. The constant block was regrouped to match: `POW_` named only one
  of three callers once `exp` and `log` joined `pow` on the tables — nine of the
  twenty-nine were pow-only against nineteen already shared — so the prefix now
  states purpose instead. `LOG_*` covers the logarithm reduction and series,
  `EXP_*` the exponential reduction, `TWO_52` and `TWO_M60` are bare powers of
  two, and `POW_ABS` and `POW_SIGN` are all that pow alone still needs. The
  rename is behaviour-preserving, checked rather than assumed: an
  order-sensitive checksum over `exp`, `log`, `pow(f64)`, and `pow(f32)` across
  four million arguments each is unchanged.
- Folded every hot-path call out of `sin` and `cos`. Each call was reaching
  `isnan`, `isinf`, `reduce_medium` — which called `exponent_field` two or three
  times of its own — and then `kernel_sin` or `kernel_cos`: five or six
  branch-with-links, each storing and reloading `d24-d31`, against one in `exp`
  and two in `log`. Splitting the cost by band first showed where a table would
  and would not have helped: below pi/4, where only the kernel runs, `sin`'s
  dependent chain was 19.8 ns, while in the Cody-Waite band it was 64.7 ns, so
  the reduction was about seventy per cent of it. A table shrinks the kernel
  polynomial and needs a finer reduction to do it, which is the smaller half and
  the wrong direction; the calls were the larger half. `reduce_medium`,
  `kernel_sin`, and `kernel_cos` are now inlined at each use and deleted,
  `exponent_field`'s body is spelled out in the folded reduction and the helper
  is left to its one remaining caller in `reduce_large`, and classification
  collapses to a single `(raw & ABS_BITS) >= INF_BITS`, which covers NaN and
  infinity together and yields the magnitude without a branch. `cos`'s
  below-pi/4 path is specialized rather than sharing the folded kernel: the
  unreduced argument has no tail, so every `y1` term drops out. That
  specialization mattered — sharing the general kernel there had pushed `cos` to
  456 instructions and regressed its small-angle chain from 28.1 to 32.0 ns,
  promotion being whole-function, and specializing turned that into 24.6 ns.
  Throughput fell from 31.8 to 11.4 ns for `sin` and 29.6 to 12.2 ns for `cos`
  in the Cody-Waite band, and from 7.2 to 4.0 and 17.3 to 4.7 ns below pi/4; the
  dependent chains fell from 64.7 to 50.7 and 63.0 to 51.8 ns in the band, and
  19.8 to 11.7 and 28.1 to 24.6 ns below pi/4. Beyond 1.6e6 nothing moves, at
  about 590 ns, since `reduce_large` is untouched and still the only call either
  function makes. The cost is code size: `sin` grows from 180 to 392
  instructions and `cos` from 164 to 432, because both kernels and the reduction
  now appear in each. Every result is bit-identical to the previous
  implementation over 48 million comparisons spanning the finite domain in both
  signs, so the documented 0.7871 and 0.7884 ULP bounds carry over unchanged
  rather than needing a fresh reference sweep. `POW_ABS` became `ABS_BITS` and
  gained `INF_BITS` beside it, since the magnitude mask is no longer pow's
  alone.
- Gave the `f32` `sin` and `cos` their own kernels in place of rounding the
  `f64` result: musl's four-term `f64` polynomials, after an inline reduction
  by pi/2 in a 25-bit head and a tail that stays exact below 2^28 * pi/2, and
  Payne-Hanek beyond it. They fell from 7.9 to 4.8 ns (`sin`) and 7.1 to 4.9 ns
  (`cos`) on `bench/std/run-math.sh`, with unchanged checksums; every finite
  input below 1.6e6 and a 1-in-997 sample above it stays within 1 ULP of the
  rounded `f64` result.
- Widened float register promotion past the callee-saved eight: `d24-d31`,
  which nothing else reserves between statements, now take up to eight more
  promoted locals. They are caller-saved and the shared byte routines clobber
  them too, so `fp_leaves` stores and reloads them inside `emit_bl_internal`,
  `emit_bl_extern`, and the indirect-call path — every branch-with-link the
  emitter produces, so no call site can be missed. A function whose loop
  carries a call, or a value of aggregate type over 32 bytes that copies
  through `__ace$mem_copy`, keeps frame slots rather than pay that per
  iteration. `pow(f64)` fell from 28.5 to 23.2 ns and its dependent chain from
  69.3 to 59.0 ns, with slot address computations on its hot path down from 43
  to 17; the `calls` and `hotloop` benchmarks moved within +-0.6% best-of-five,
  and `pow` results are identical on 24M operand pairs and on 11,024 exact
  cases. A promoted local held across a call had no test: one is added, and it
  fails on a build that omits the save.
- Rewrote `pow(f64)`'s extended-precision product on `math.fma`: `e_hi` is now
  `y * log_hi` and its rounding error comes back exactly from
  `fma(y, log_hi, -e_hi)`, which deletes the 26-bit splitting of both operands
  along with `POW_HIGH26`, `y_hi`, and `l_hi` — the split existed only to make
  the leading product exact without a fused multiply-add. 31 ns fell to 29 ns
  and the dependent chain from 78.7 to 69.3 ns. Against an exact 80-digit
  reference over 11,024 inputs covering near-1 bases, log-table entry edges,
  subnormal results, and `y*log(x)` across -745..710, the worst error is
  unchanged at 0.5031 ULP for normal results and 0.7325 in the subnormal range.
  The two forms agree on all but 33 of 24M random operand pairs, each by one
  step, and on those the fused form is the closer one 20 times to 13, with the
  worst case improving from 0.5098 to 0.5071 ULP. Mechanically fusing the
  remaining multiply-add pairs was measured separately at 3% with no effect on
  the chain, and fusing the `f32` kernel at 1%; neither was kept.
- Restructured both `math.pow` kernels around the emitter's register promotion,
  which gives callee-saved float registers only to named locals used at least
  twice: a local that misses out costs a store where it is set and a load at
  every mention. Single-use values are now nested in the expression that
  consumes them, cheap ones (`kd`, `zhi`, the `-r^2/2` term, the 26-bit half of
  `y`, `er^2`, the exponential's correction) are recomputed instead of named,
  the power-of-two entry the extreme path needs moved into that branch, and the
  `pow_signed` call became a sign-bit xor, since `flip` already holds that bit.
  Every step preserves each rounding, so the results are bit-identical: 24M
  `f64` and 6M `f32` random bit patterns plus all 576 special-value pairs agree
  exactly, in checked and release mode alike. `pow(f64)` fell from 49.8 to
  31.3 ns and `pow(f32)` from 13.7 to 11.6 ns; dependent-chain latency fell from
  94.8 to 78.4 ns and 48.1 to 44.1 ns. Rebalancing the correction sums into
  shallower trees, binding the log-table entry through a pointer, and reaching
  the exact fast paths through the base's bits were all measured and gained
  nothing.
- Fixed `pow` returning a NaN for a negative zero or negative infinite base with
  a fractional exponent (`pow(-0, 0.5)`, `pow(-inf, 0.5)`): `pow_special` now
  settles zero and infinite bases before the fractional-exponent NaN, which
  applies only to finite negative bases, so they give `+0` or `+inf` per Annex F.
- Fixed compound untyped constants under a float cast (`f32(-1.0)`,
  `f32(0.0 - 1.0)`, `f32(-3)`, `f32(2 * 3)`), which stored a wrong value or hit
  "untyped operand reached codegen": the cast check now settles the operand
  through `coerce` (float math at the target type, integer math as `i64`).
- Fixed global initializers with a negated float literal (`G: f32 = -2.5`,
  `f32(-2.5)`, struct and array elements), which were rejected as not
  compile-time constants: the constant-initializer folder now folds a negated
  float literal and admits it under a same-family float cast.
- Replaced generated float formatting's `ecvt`/`fcvt`, parse-back, `floor`,
  floating-environment, and memory-helper dependencies with a shared native
  exact binary-to-decimal engine under `std/@`. Fixed-capacity base-2^32
  arithmetic produces shortest round-trip `f32`/`f64` text and fixed `%f`
  rounding through precision 324 without allocation or platform conversion.
- Replaced shortest `f32`/`f64` formatting's exact bigint search with an
  Apache-2.0-attributed Ace adaptation of Ulf Adams' Ryu algorithm. Fixed-width
  products and cached powers now select the shortest ties-to-even coefficient;
  fixed `%f` retains the bigint path through precision 324.
- Replaced `str.parse[f32/f64]` platform parsing with native exact
  decimal-to-IEEE conversion. Fixed-capacity bigint comparisons select adjacent
  values and round exact midpoints to even, including subnormal and overflow
  boundaries, without allocation, locale state, `strtod`, or `strtof`.
- Added a three-tier decimal-to-float parser: exact small integers, a
  fast_float-attributed Eisel-Lemire fixed-width path for ordinary decimals,
  and the existing bigint comparison path for long exact fallback cases. The
  tier-selecting scan also performs the complete syntax validation once.
  Specialized the f32/f64 fast converters after generated-AArch64 profiling:
  fixed-step leading-zero normalization, scalar out-parameters, integrated
  special-value classification, and fused cached-power multiplication avoid
  the generic call chain and its aggregate spill/copy overhead.
- Added a compiler-shipped internal native-endian unaligned word load and used
  it to scan finite float digit runs eight bytes at a time. A branchless SWAR
  digit mask, fixed-step trailing-zero count, packed eight-digit accumulation,
  and scalar short-tail path preserve the shared three-tier parser validation.
- Added short and long-mantissa float-parse benchmark cohorts as supporting
  diagnostics and made an explicit 80%-short/20%-long blend the canonical
  comparison, keeping scan-path performance visible without conflating it with
  the greater-than-19-digit bigint conversion fallback.
- Added `sys.pipe` so descriptor-polling fixtures also use the public OS facade
  instead of declaring a test-local C binding.
- Exposed the target-defined `sys.EAGAIN` and `sys.EWOULDBLOCK` errno constants
  so nonblocking callers do not embed platform numbers.
- Standardized OS-specific stdlib organization as a public `module.ace` facade
  selecting `os/<target>.ace`; `fmt`, `net`, `poll`, and `sys` use the layout,
  and each target file implements its facade operations directly. Flattened
  `str` back to `std/str.ace` after its final OS dependency was removed.
- `math` currently contains only its minimal implemented integer/generic
  helpers; floating libm and SIMD remain open work.
- Added `.acefmt` `single-statement-body = same-line-if-fits` (bodies of functions, `if`, `for`, `while` holding one
  statement stay on the opener's line within 120 columns) and applied it across `std/`, `pkg/`, `demo/`, `bench/`, and
  `tests/`; `else` branches, every branch of an `if`/`else` chain, and a body whose statement is itself a body stay
  multi-line. Fixtures whose line layout is under test (`tests/emit/basics/main.ace` for DWARF rows,
  `tests/modules/location` for `@line`) keep their layout.
- Added `.acefmt` `function-body = multi-line`: a non-empty function, method, or associated-function body always opens
  on its own line, even for one statement; `single-statement-body` now governs only `if`, `for`, and `while`. Applied
  across `std/`, `pkg/`, `demo/`, `bench/`, and `tests/`, with a blank line between each now multi-line declaration.
- Applied the one-word naming rule to std: `ascii.control`/`lower`/`equal`,
  `utf8.valid`, `mem.usable`, `poll.FOREVER`; `bits.popcount`/`clz`/`ctz`/
  `bswap`/`rotl`/`rotr`/`wradd`/`wrsub`/`wrmul`, and one `bits.cast` overload
  set whose declared parameter type (float or unsigned) selects the existing
  read or write intrinsic; the POSIX spellings `sys.opendir`/`readdir`/
  `closedir` and `net.reuseaddr`/`reuseport`, with their error domains and
  private `os/` twins. `tcp`'s `*_nonblock`, `progress_*`, and `*_all` forms
  and `net.pending_error` keep their identifying word.
- Applied the same rule to `pkg/http`: the method constants are the method
  tokens themselves (`http.GET`, `http.POST`, …), and the internal helpers are
  `message.tchar` (RFC 9110's term), `transport.connect`/`get`, and
  `redirection.redirects`. Removed `redirection`'s unused method-constant
  duplicates and `transport`'s private `GET` alias.

## Significant implementation corrections

- A resolved method call returning nothing is no longer queued for
  end-of-file resolution: its `NULL` result type read as "not declared yet",
  so every void method call was checked twice and reported its errors twice.
  Deferral now also requires the call to have no resolved symbol.
- Added `.acefmt` `result-group = adjacent` and `after-result-group = always`:
  a statement binding a call's result, its `defer`, and the checks of that
  result form one group with no blank line inside, even before a multi-line
  check body, and a blank line follows the group unless its block closes.
  Applied across `std/`, `pkg/`, `demo/`, and `bench/`; `file.read` now
  places its descriptor `defer` before the check, as the `AGENTS.md` release
  rule requires.
- Added `.acefmt` list and literal layout rules and applied them across
  `std/`, `pkg/`, `demo/`, `bench/`, and `tests/` outside the lexer and parser
  fixtures. `struct-literal = field-per-line`: a struct literal with two or
  more fields puts one field per line with a trailing comma, and a one-line
  body holding one becomes a block. `struct-literal-in-bare-list =
  named-local`: because a bare comma list stays on one line, such a literal in
  a multi-value `return` is first bound to a local; the six empty
  `Request {method = null, target = null}` failures became `blank(Request)`.
  `dense-list = whole-items-per-row`: an array literal of plain values or
  struct literals of plain values packs the same number of whole elements on
  every row, the most that fit in 120 columns, and `dense-list-columns =
  aligned` pads each element, or each struct-row field but the last, to its
  column's widest so the columns line up. The `std/@` tables follow it, with
  their exact-power runs deliberately grouped (six per row in
  `float_parse_tables`, two per row in `float_shortest_tables`). `multiline-trailing-comma = always` now holds
  for every multi-line delimited list, and struct literals take the space
  before `{`; a one-line nonempty `{ }` has inner spaces while `()` and `[]`
  stay tight. `nested-overlong-list = named-local`: an overlong call nested
  inside a larger expression is bound to a local rather than broken
  mid-expression, as `sin` and `cos` now do with their polynomial (results
  bit-identical). The parser accepts a trailing comma in `extern` parameter lists,
  the one list that rejected it (`tests/parser/extern_trailing_comma_ok.ace`).
- Extended global constant emission to carry pointer relocations recursively
  through arrays, structs, repeated elements, aliases, and union payloads, so
  string-bearing aggregates remain static data without runtime initialization.
- Repaired the manual memory benchmark for the current zero-valid slice model,
  made checked/release costs explicit, and added a checksum-matched integer CPU
  comparison against equivalent C `-O0` and `-O2` builds.
- Corrected AArch64 aggregate argument/return layout, deep-frame loads, floating
  load/store scaling, integer-to-floating temporary separation, remainder
  lowering, short-circuit normalization, variadic stack slots, and large
  subobject copies as their feature fixtures exposed them.
- Widened constant folding to constructor-shaped same-family primitive, alias,
  distinct, enum, and qualified casts while retaining semantic fit and
  cross-family checks.
- Canonicalized equivalent module paths before assigning nominal Error identities,
  preventing duplicate module loads and domain identities.
- Derived private symbol prefixes for directory-module entries from their
  directory names rather than the repeated `module.ace` filename.
- Fixed cross-module identifier renaming (`module.c`'s `rename_type_refs`) to
  walk a fixed array type's length expression. It previously recursed only
  into an array type's element (`t->a`), so a module-private `const` used to
  back a `[N]T` array length inside that same imported module's own function
  bodies, struct fields, or parameter/return types kept its bare name instead
  of gaining the module's private prefix, producing a spurious "unknown name".
  Latent since array lengths were always integer literals; surfaced once
  Core language widened array lengths to constant expressions (above).

- Expanded `sys` with allocation-free filesystem metadata and mutation
  (`stat`, `mkdir`, `rename`, and `unlink`) plus caller-owned buffered directory
  iteration (`open_dir`, `read_dir`, and `close_dir`). The Darwin layer parses
  private `dirent64` batches into exact caller-provided name buffers, keeps
  undersized entries retryable, and exposes only stable file kind and size.
  The portable `mkdir(path)` form delegates effective permissions to the
  process umask, while explicit POSIX creation modes use named `S_I*` bits.
- Allowed global constant initializers to forward recursively evaluable
  constants from imported modules. The `sys` facade now re-exports its
  target-selected `O_*` and `S_I*` values without duplicating their numbers.
- Fixed homing of a trailing stack-passed scalar parameter to copy only its own
  width instead of the ABI-padded stack-slot width (Darwin pads scalar slots to
  4 bytes), which had let a narrow trailing argument (e.g. `bool`) clobber the
  low bytes of the previously homed adjacent stack argument. This was the root
  cause of the owning HTTPS client's response-dependent heap corruption during
  deferred `client_wipe`.
- Moved `http`'s implementation files (`client.ace`, `format.ace`, `parse.ace`,
  `redirect.ace`, `server.ace`, `transport.ace`) under `std/http/inc/` by
  convention, keeping `std/http/mod.ace` as the sole external surface. Each of
  `client.ace` and `server.ace` now declares its own plain `Context` struct and
  `context_new`/`context_wipe` pair, since directory-module privacy already
  keeps the two from colliding internally; `mod.ace` aliases them as public
  `ClientContext`/`ServerContext` and re-exposes disambiguated constructors
  `client_context_new`/`client_context_wipe` alongside the existing
  `server_context_new`/`server_context_wipe`, keeping the facade uniform.
- Added `tcp.listen_nonblock` and `tcp.accept_nonblock`, mirroring
  `connect_nonblock`, so a listener and its accepted connections can join the
  same nonblocking, `poll`-driven composition as outbound connections.
- Implemented `http.serve`: a caller-driven, nonblocking HTTP/1.1 server core
  (`std/http/server.ace`) built directly on the existing incremental
  `RequestParser` and `Response.format`, following `tcp`'s externally-driven
  connection-progress model rather than owning a blocking loop. `ServerContext`
  binds a nonblocking listener and poll queue and allocates a caller-sized
  array of fixed-capacity `Session` slots (separate input/header/body/output
  buffers per session, no internal resizing); `serve` performs at most one
  `poll.wait` per exhausted batch and drives exactly one ready listener or
  session per call, returning one `Event`
  (`accepted`/`request`/`sent`/`disconnected`/`failed`) — the caller's own loop
  supplies the bound on how long to run. `respond` formats a handler's
  `Response` into the session's output, synthesizing exactly one
  `Connection: close` header and rejecting a handler-supplied one; each
  session closes and recycles once its response is fully sent. A forked-client
  loopback fixture (`tests/emit/http/serve.ace`) proves accept, parse,
  respond, close, and slot reuse across two sequential connections.
- Renamed the server context's per-buffer parameters from `*_cap` to `*_size`
  (`input_size`, `header_size`, `body_size`, `output_size`), matching the
  `_size` naming already used elsewhere (`mem.realloc`'s `new_size`,
  `tls.context_size`/`record_size`) instead of introducing a one-off `_cap`
  spelling.
- Added `server_context_new(endpoint): (ServerContext, Error)`, a low-effort
  overload beside the fully-parameterized form, for a caller with no capacity
  preference. Unlike the client's `Storage`, session capacities are
  deliberately fixed for the context's lifetime rather than grown on demand
  (so one session overflowing never disturbs another live session's buffer
  mid-poll), so the low-effort form supplies fixed built-in defaults instead —
  512 backlog, 128 sessions, 8 KiB input/output, 64 headers, and 256 KiB body
  per session — rather than growing storage. The defaults live in
  underscore-prefixed module constants (`_backlog`, `_session_count`, …)
  rather than uppercase names, following the project's established lowercase
  `const` style (e.g. `tls`'s `closed`, `send_record`); a same-named local
  parameter in the explicit overload ordinarily shadows a same-named constant
  without conflict, so no artificial `default_`-style prefix was needed to
  avoid a collision that was never actually possible.
- Added a complete HTTP server proof of concept
  (`demo/http-complete-server/main.ace`) built on the low-effort
  `server_context_new(endpoint)` overload, and a small `http.get` usage
  example to `docs/standard-library.md`.
- Added a blocking `serve(endpoint, handle): Error` overload alongside the
  nonblocking core: no `ServerContext`, poll queue, session table, or
  caller-visible backlog/capacity tuning, just an endpoint and a
  `(Request): Response` handler. It binds, then loops forever, serially —
  accept, read one request, call `handle`, write the response with a
  synthesized `Connection: close`, close, repeat — using fixed internal
  buffers sized like the low-effort `server_context_new(endpoint)`
  defaults. A per-connection failure just drops that connection; `serve`
  itself only returns if the listener cannot be created. Added a matching
  minimal demo (`demo/http-simple-server/main.ace`).
- Fixed `SPEC.md`'s stale claim that `parse_url` rejects `https://`: the
  parser has accepted both schemes since the TLS-capable client overload was
  added, since `get(url, mut client)` needs `https://` to parse and both
  `get` overloads share one parser (`get(url, mut context)` still rejects
  `https://` before connecting, but through its own scheme-forced
  `parse_url_for(url, false)` call, not through `parse_url` itself). Updated
  the stale test assertion in `tests/emit/http/main.ace` to match.
- Removed the caller-driven nonblocking HTTP server core
  (`ServerContext`/`Session`/`Event`, `server_context_new`,
  `server_context_wipe`, `serve(mut context, timeout_ns)`, `respond`) and its
  `demo/http-complete-server` proof of concept: it had been implemented
  directly, without going through the Decision Queue carve process, and the
  owner does not want to keep that design. `std/http` now exposes exactly one
  server convenience, the blocking `serve(endpoint, handle): Error` overload,
  with `demo/http-simple-server` as its sole demo. Trimmed the matching
  fixtures (`tests/emit/http/serve.ace` removed; `tests/emit/http/main.ace`
  and `tests/sema/stdlib/http_ok.ace` no longer exercise the removed surface).
  A fresh carve of how much server mechanism belongs in the language versus a
  framework built on top is tracked in `TODO.md` before anything like the
  nonblocking core returns.
- Implemented `http.Router` (`std/http/inc/router.ace`), the fresh carve for
  the nonblocking-server item above's routing half: a growable route table
  (`transport.mem_adjust`/`mem.release`, the same pattern `Client`'s
  `Context`/`Storage` already uses) with `router_new`/`router_wipe`,
  `mut Router.handle(method, path, handler)`, and `pub Router.dispatch`
  matching on exact method plus segment-boundary longest path-prefix
  (`/home` matches `/home` and `/home/about` but not `/homework`; any
  `?query` on the request target is stripped before matching). Ace has no
  closures or first-class method values, so a stateful `Router` cannot be
  handed to `serve` as a bare `(Request): Response`; instead `serve` gained a
  second overload, `serve(endpoint: string, router: Router): Error`, that
  dispatches through the router directly. `server.ace`'s single-shot
  `serve_one` was split into shared `read_request`/`respond_close` helpers
  reused by both the handler-based and router-based per-connection paths.
  Added `tests/emit/http/router.ace` covering registration, prefix/segment
  matching, method mismatch, query stripping, and the built-in 404 fallback.
- Moved the grow-or-allocate helper formerly named `transport.mem_adjust`
  into `std/mem.ace` as `mem.adjust[T]`: `router.ace` and `client.ace` both
  called it through `transport` for a purely generic slice-growth operation,
  not anything HTTP-specific. `transport.ace`, `router.ace`, and `client.ace`
  now call `mem.adjust` directly; `router.ace` no longer imports `transport`.
- Renamed module-level `const` declarations from a leading-underscore
  lowercase spelling to all uppercase (`_backlog` → `BACKLOG`, `_input_size` →
  `INPUT_SIZE`, and the rest of `server.ace`'s session defaults), replacing
  the project's earlier lowercase `const` convention.
- Collapsed `server.ace`'s two `serve` overloads back to one,
  `serve(endpoint: string, mut context: Context, router: Router): Error`,
  removing the bare-`handle` overload now that `Router` is the sole
  dispatch surface. Added `server.Context` (`input`/`headers`/`body`/`output`,
  each starting empty, plus a `backlog` field defaulting to 512) with
  `context_new`/`context_wipe`, replacing `server.ace`'s fixed
  `BACKLOG`/`INPUT_SIZE`/`MAX_HEADERS`/`BODY_SIZE`/`OUTPUT_SIZE` module
  constants; `read_request` and `respond_close` now grow each buffer through
  `mem.adjust` off the same capacity signals the parser and formatter
  already reported (`RequestResult.capacity`, `FormatErr.capacity`, and the
  `input` buffer filling before a request completes), with no size cap.
  Growing `input` resets and reparses the retained bytes, mirroring
  `transport.exchange`'s existing handling of a moving reallocation, since
  the parser's header/target views point into it. `backlog` is the one
  non-growing field, read once by `listen_endpoint` when `serve` binds and
  never touched by `mem.adjust`. `demo/http-simple-server` and `http.mod.ace`
  (`ServerContext`, `server_context_new`/`server_context_wipe`) were updated
  to match.
- Renamed `std/http/inc`'s `format.ace`, `parse.ace`, and `redirect.ace` to
  `message.ace`, `parsing.ace`, and `redirection.ace` so every file in the
  directory is a noun, matching its own import alias exactly; `parsing` and
  `message` were already used at every import site, and the `redirect` alias
  at its one call site in `client.ace` was renamed to `redirection` to match.
- Added a minimal `http.get` client demo (`demo/http-simple-client/main.ace`):
  one `ClientContext`-backed GET against a fixed URL, and the response
  status/body printed with no redirects or TLS.
- Removed TLS entirely: `std/tls.ace`, `http`'s HTTPS overloads
  (`get(url, mut client)`, `get_redirecting`'s TLS overload) and its `Client`/
  `TlsConfig` types, the Darwin BearSSL adapter and vendored sources
  (`vendor/bearssl/`), the distributed `external/` link artifacts, and every
  TLS-only test and driver fixture. `http` is now plaintext-only; `Storage`
  no longer carries `tls_context`/`tls_records`. Removed the compiler-owned
  `@extern("component")` attribute and its supporting mechanism along with
  it, since TLS was its only consumer: the `Node.extern_component` field, its
  parsing (`@extern("tls")`), the sema reachability walk
  (`extern_requirements`) that conditionally omitted component-carrying
  functions from emission, the emit-time gating, the `--tls-backend` CLI flag
  and pkg-build equivalent, and the `(component, target, backend)` link-recipe
  table in `main.c` (`ExternRecipe`, `TLS_BEARSSL_DARWIN_ARM64`) that
  resolved and appended bundled TLS link inputs. `-c`/`-o` and `ace pkg
  build`/`run` no longer have any external-component linking to perform.
  This was a deliberate reset, not a deprecation: the prior TLS approach is
  discarded outright rather than migrated, and its Decision Queue entry and
  `TODO.md` survey now record a fresh carve, unconstrained by the removed
  backend, adapter shape, or `@extern` mechanism.
- Removed `client.ace`'s `redirect_next` `secure_transport` parameter and
  `current_secure`/`RedirectErr.downgrade` branch, dead since the TLS removal
  above left the plaintext client as `redirect_next`'s only caller, always
  passing a constant `secure_transport = false`: the current URL can never be
  HTTPS, so the downgrade branch could no longer trigger. The remaining
  HTTPS-redirect-target rejection (`UrlErr.invalid`) is now unconditional.
  Removed `RedirectErr.downgrade` from `redirection.ace`'s error domain, now
  unreachable.
- Relocated `http` out of `std` to the standalone package `pkg/http`
  (`ace.pkg`: `kind: lib`, `root: mod.ace`), independent of the separate
  TLS/HTTPS std-vs-package question, since TLS's removal above left `http`
  plaintext-only and pure Ace with no compiler-owned mechanism tying it to
  the `std` tree. `pkg/http` is Package System v1's first real payload,
  proving its local-path dependency mechanism end to end. `demo/http-simple-
  client` and `demo/http-simple-server` became `app` packages (`ace.pkg`
  each declaring `http: ../../pkg/http`) since a bare `import http` no longer
  resolves under plain manifest-free compilation; `docs/std/http.md` moved to
  `docs/http.md`.
- Gave `pkg/http` its own behavioral test suite, decoupled from the core
  compiler suite: `pkg/http/tests/` holds 7 cases (`framing`, `client`,
  `redirect`, `request`, `router`, `plaintext_only`, `api_surface`), one
  directory per case, each an `app` package depending on the sibling `http`
  package (`http: ../..`), matching `tests/package_cli`'s convention —
  `// expect-exit: N`/`// expect-check-ok` markers as `ace.pkg` comments, run
  by the new `pkg/http/tests/run.sh`. `make test-packages` runs every
  `pkg/*/tests/run.sh`, kept separate from `make test`'s `tests/run.sh`:
  package content tests shouldn't gate core compiler iteration. Content
  carried over unchanged from the fixtures the relocation above left broken
  (`tests/emit/http`'s 5 fixtures, `tests/sema/stdlib/http_ok.ace`,
  `tests/driver/http_plain_autolink.ace`), which are removed from the core
  suite now that they're superseded; `tests/run.sh`'s corresponding
  `http_plain_autolink.ace` driver block is removed. `http_plain_autolink`'s
  duplicate "standalone object" invocation (recompiling the same file with
  `-c` alone) is dropped rather than ported: it existed to check that a
  plaintext build didn't drag in TLS link inputs at the object level, and
  with `@extern(component)` removed entirely there is nothing left to
  isolate from; the case's behavioral half (https scheme rejection, DNS-
  failure propagation) is preserved as `plaintext_only`.
- Removed `ace pkg build`/`run`'s stale `--tls-backend=NAME` mention from
  `package_usage()`'s printed help: the flag itself was already removed with
  `@extern(component)`, so the argument parser has rejected it as unknown
  since then, but the help text kept advertising it.
- Implemented `ace pkg add NAME PATH`, `ace pkg remove NAME`, and
  `ace pkg list` (SPEC.md Decision Queue, "Package system v1"), completing
  v1's local-path-only manifest-editing surface. `add`/`remove` are line-
  oriented text surgery in `package.c` (`package_manifest_add_dep`/
  `package_manifest_remove_dep`) mirroring `parse_manifest`'s own comment/
  section/key scan: `add` inserts a `NAME: PATH` line right after the
  `[dependencies]` header (creating the section, preceded by exactly one
  blank line normalized from however many trailing blank lines the file
  already had, if absent); `remove` deletes the one matching entry line and,
  if it was the section's last entry, the header line too, along with a
  blank separator line immediately before it — keeping repeated add/remove
  cycles byte-for-byte idempotent. Neither function validates the graph
  itself: `main.c`'s `package_cmd_add`/`package_cmd_remove` back up the
  manifest's original bytes first, perform the edit, then re-run the same
  `package_resolve` the other subcommands already use, restoring the backup
  verbatim and reporting the failing package_resolve diagnostic if the edit
  produced an invalid graph (duplicate name, alias not matching the target's
  declared name, stdlib-name collision, cycle, mismatched `ace` version) — no
  separate validation logic, and a failed `add`/`remove` never touches the
  file. `list` is read-only, resolving and printing the graph's name/kind/
  direct-or-transitive/directory. Added `tests/package_cli/pkg_add_cross_dep`
  (`add` then `pkg build`/`run` prove a real cross-package import),
  `pkg_add_err_rollback` (mismatched alias, manifest unchanged),
  `pkg_remove_dep` (add then remove round-trips to the original bytes),
  `pkg_remove_err_missing`, and `pkg_list_transitive` (three-package chain
  distinguishing direct from transitive); `tests/run.sh` gained the
  `pkg-add-args`/`expect-add-err`/`expect-remove-ok`/`expect-remove-err`/
  `expect-list-has` markers, with `pkg-add-args` cases auto-cleaned by a
  trailing `pkg remove` so the checked-in fixture manifest stays unmutated
  across repeated runs.
- Fixed `switch` dispatch (`emit_union_switch` and the scalar `N_SWITCH` case,
  both in `emit.c`) falling through into whichever case was emitted first when
  the scrutinee matched no explicit pattern and `else` was not the last case,
  instead of running `else`. For a union switch this ran the wrong variant's
  case body against a mismatched payload, misreading raw bytes (e.g. an `f64`)
  as a slice descriptor or other pointer-bearing binder; this was the actual
  cause of both the growable-array-of-unions corruption and the non-tail
  recursive-call-with-union-parameter crash recorded in `TODO.md`, which
  turned out to be a single defect rather than two. Both switch forms now
  branch unconditionally to `else`'s label (or past the switch with no
  `else`) after the pattern comparisons, never relying on fallthrough. Added
  `tests/emit/control_flow/switch_else_position.ace` and
  `tests/emit/unions/else_not_last.ace`.

- Fixed a root import alias (`import name: "./path.ace"`) being declared into
  the single flat scope semantic checking used for the entire merged
  compilation unit, so it collided with any same-named local anywhere else in
  the unit, including inside `std` modules the importing package never
  directly names. Root import aliases now declare into their own `Scope`,
  parented to that shared scope, spliced in only while checking the root
  file's own function bodies, global-variable initializers, and top-level
  statements; declarations of every kind still land in the one shared scope
  as before, so cross-file name resolution is unaffected. Added
  `tests/modules/import_alias_scope`.

- Generic function stamping now checks each body under the scope it was
  declared in: `Sym.decl_scope` records the root file's import-alias scope for
  root-declared generics and the shared file scope for every other generic, and
  both `instantiate_generic_func_args` and `sema_stamp_body` use it instead of
  the caller's scope or the bare shared scope. Added
  `tests/modules/import_alias_generic`.

- Added the standalone package `pkg/json` (`kind: lib`, `root: mod.ace`): a
  whole-document RFC 8259 parser. `json.parse(text): (Json, Error)` builds a
  `Json` union (`none`, `bool`, `number: f64`, `string`, `array: []Json`,
  `object: []Pair`) with heap strings and backing storage, reports failures
  through the `JsonErr` domain, and enforces a limit of 512 nested
  containers (`MAX_DEPTH`). Leading-zero numbers, unpaired `\u` surrogates, unescaped
  control bytes, and trailing data are rejected. `json.wipe` recursively releases every
  allocation, keys included. The parser walk is `Parser` methods, with
  `parse_value` and `parse_object`/`parse_array` calling each other; only
  helpers that ignore parser state stay free functions. `\uXXXX` decoding uses
  a private `encode_utf8`. `pkg/json/tests/basics` runs under
  `make test-packages`, `demo/json-parse-file` parses a file and prints every
  key and value, and `docs/json.md` documents the package.
- Renamed the `ace.pkg` manifest field `kind` to `type` and its value `app` to
  `exe` (`lib` unchanged). The parser reads keys as plain text, so `type` is
  not affected by the reserved keyword. `kind:` and `type: app` are now
  errors with no alias; `ace pkg init`, `list`, and `--pkg-graph` use `exe`.
- Dropped the dependency-free `demo/darwin-packet-sniffer/ace.pkg`; the demo
  builds with plain `ace`. Demos, benches, and tests carry a manifest only to
  depend on a `pkg/` package or to test the package system.
- Added the single-file std module `file`: `file.read` composes
  `sys.open`/`stat`/`read` into one `mem.alloc` buffer released by
  `mem.release`, and `file.write` composes `open`/`write`/`close`, creating
  with `0o666` less the umask; both retry `EINTR`, and `FileErr` covers
  allocation failure and a shrinking file. It is a module of its own because
  every imported function is linked, so an allocating `sys` function would
  pull `malloc` into every `sys` user. `demo/json-parse-file` and
  `demo/json-format-file` use it in place of their own read and write loops.
- Gave `pkg/regex`'s `Regex` a convenience surface over a now-public core:
  `Matcher` is exported, and `Regex` uses only public core operations.
  `Regex::new` returns `(Regex, Error)`, leaving the pattern offset to
  `Syntax::parse`; `is_match` is `match`; `find` returns `bool` and records
  the match with its searched text; `next(text, mut at)` finds and advances
  past the match, stepping over an empty one; `count` loops `next`; `group(k)`
  and `span(k)` read the recorded match, replacing `captures`. `bench/regex`
  checksums are unchanged and every workload is within 1.2% of the previous
  build, inside run-to-run spread.
- Marked `pkg/regex`'s per-byte helpers `inline`: `Queue.has`,
  `Queue.insert`, `is_word`, `is_boundary`, and `Class.has`. `bench/regex`
  runs 7-16% faster per byte (`literal` 32.2 to 28.4 ns, `pathological` 175.6
  to 146.7), retires 6.3% fewer instructions, and grows its text by 1,024
  bytes; checksums are unchanged.
- Split `pkg/regex`'s `Matcher.add` into `add_current` and `add_next`, one
  per queue, removing the `into_next` test from its membership check, insert,
  and per-slot capture copy. `bench/regex` runs 4-17% faster per byte
  (`captures` 59.7 to 49.3 ns, `word-ing` 52.2 to 46.9) with `pathological`
  unchanged, and retires 5.2% fewer instructions. Marking `add` `inline`
  instead cost `pathological` 5% and 2.8% more instructions, and passing the
  queue as a `mut Queue` parameter left `captures` 5% slower, though it
  retired as few instructions.
- Marked per-byte and per-element helpers `inline` where a release
  microbenchmark measured a gain: `ascii.control`/`lower`, `utf8.width`,
  `str`'s `space_byte`, `math.isnan`/`isinf`/`isfinite`, the `f32`
  `exp`/`log`/`sin`/`cos` forwarders, `math`'s `bit_at`, Ryu's `log10_pow2`/
  `log10_pow5`/`pow5_bits`/`shift_wide`/`multiple_power2`/`multiple_power5`,
  `Array.set`, `pkg/json`'s `is_ws`/`is_digit`/`Parser.skip_ws`, and
  `pkg/http`'s `tchar`. `Array.push` became an inline capacity check that
  calls `reserve` only when full, instead of calling `mem.grow` every time.
  Per operation: case-folding `ascii.equal` on 64 bytes 48% faster, `str.trim`
  37%, float classification 52%, `f32` math 17%, `sin` past 2^20·π/2 28%,
  `utf8.encode` 20%, `push` 58%, `set` 12%, float formatting 1-4%,
  `pkg/json` parsing 6%, and `pkg/http` request parsing 4.5%; checksums are
  unchanged and text grows 1.3-2.7%. Ryu's `multiply_shift64`/
  `multiply_shift32` made formatting 3-8% slower and stay out of line;
  `Map.has`, `Array.clear`/`truncate`, `math`'s `exponent_field`, the float
  parser's `exact64`/`exact32`, `pkg/json`'s `Json` forwarders and
  `Formatter.object`/`array`, and `pkg/http`'s `skip_ows` gained at most
  about 1% and stay unmarked.
- Made `str`'s integer-parse helpers scalar and `inline`: `digit_value`
  returns 36 for a non-digit, so the existing `< radix` test rejects both
  cases, and `accumulate` updates a `mut` magnitude and returns only the
  overflow flag. Decimal `parse[int]`/`parse[uint]` run 50% faster, radix-16
  44%, and rejected inputs 58%; dropping the tuple results alone gave 29-44%.
- Replaced `str`'s `accumulate(mut mag, …)` with the check-only
  `fits(mag, d, radix, limit)`, the loop updating `mag` itself. The `mut`
  argument had pinned `mag` to its frame slot, so every digit loaded and
  stored it; in a register, decimal parsing runs 21-25% faster, radix-16 12%,
  and rejected inputs 24%. Hoisting the per-digit overflow division into a
  per-parse cutoff and remainder gained nothing: the division depends only on
  the digit and overlaps the multiply-add chain.
- Grouped `bench/` into four sections, each a directory with its own runners:
  `compiler/` (compile time), `core/` (`calls`, `cpu`, `hotloop`), `std/`
  (`string`, `memory`), and `pkg/` (`regex`). `bench/README.md` follows the
  same grouping; recorded results are unchanged.
- Added standard-library and package benchmarks. `bench/std/string` gains
  integer parsing, trimming, case-insensitive comparison, and UTF-8 validation
  against `strtoll`, `strtoull`, `strncasecmp`, and hand-written C, with one
  checksum check covering every row that shares a contract.
  `bench/std/collections` runs `Array` and `Map` against the same containers
  in C, and `bench/std/math` runs `math` against libm; both build C without
  auto-vectorization. `bench/pkg/json` measures writing, parsing, and pointer
  lookup, Ace-only. The first baselines put UTF-8 validation at 6.3x C,
  large-argument `sin` at 47x, `Array` access at 4-5.5x, and the other rows at
  1.1-3.5x.
- Gave `pkg/json` a convenience layer over its public core. The tagged union
  is now `Value` (in `inc/value.ace`), and `Json` (in `inc/json.ace`, matching
  `regex.Regex`) holds a `Parser`, a `Formatter`, and `root`: the input tree.
  `Json::parse(text)` returns `(Json, Error)` and wipes itself on failure;
  `get`, `string`, `number`, and `bool` read `root` by RFC 6901 JSON Pointer,
  the typed ones returning the zero value when it is absent or of another
  type. `Json::new()` makes an empty `Json` for writing: every `Formatter`
  write call is forwarded one to one, `format()` is `finish`, and `reset`
  starts the next text. `format` returns only what was written, never `root`
  implicitly; writing a parsed tree back is `value(root)`. Forwarding costs
  one extra call per write, about 3.5% on a 648-byte document. The pointer
  walk is the new core function `json.get(tree, path)`, which compares escaped
  tokens in place and allocates nothing. `demo/json-parse-file` reads four
  fields by pointer instead of walking the tree (87 to 32 lines), and
  `demo/json-format-file` writes through `Json` inline (66 to 42 lines).
- Replaced `pkg/json`'s `Json::parse(text): (Json, Error)` with the method
  `mut Json.parse(text): Error`, so parsing and writing both start from
  `Json::new()`. Each parse resets the parser and reuses its storage, which
  a fresh `Json` per document could not: about 36% less time per document at
  114 bytes and 21% at 460. On failure `root` is `Value.none`.
- Added `demo/regex-search-file`, an `exe` package over `pkg/regex` that reads
  `SPEC.md` with `file.read` and prints every `Decision:` line through a
  `next` loop.
- Renamed `sys.FileKind` and `net.SocketKind` to `sys.FileType` and
  `net.SocketType`, matching POSIX file/socket-type vocabulary. Because `type`
  is a keyword, the `FileInfo` field, the `socket` parameter, and the
  `read_dir` local are `file_type`/`socket_type`, and the `ResolveErr` member
  `socket_kind` is `socket_type`. The remaining Ace-side `kind` names follow:
  the private float `decimal.Kind` is `decimal.Type` with `number_type` fields,
  and the `pkg/http` capacity parameter is `capacity_type`. The C compiler
  keeps its internal `kind` names, since `Sym` already has a `type` member.
- Renamed the `switch` catch-all case from `else` to `default` and required it
  to be the last case. `default` is a new keyword and `F_ELSE` is now
  `F_DEFAULT`. One sema check (`check_default_last`) reports a case after
  `default`, which also covers a repeated `default`, replacing the old
  duplicate-`else` check. `else` as a case is a parse error naming `default`.
  The two dispatch regression tests now put `default` last and live as
  `switch_default_dispatch.ace` and `default_dispatch.ace`; new error fixtures
  cover both diagnostics. Every switch arm in `std`, `pkg`, `docs`, tests, and
  the VS Code grammar was migrated.
- The `pkg/http` request and response parsers now validate a header's framing
  (`Content-Length`, `Transfer-Encoding`) before checking header-slot capacity,
  so a framing conflict reports `framing` even on a full header buffer. State
  changes follow the capacity check, so a `capacity(headers)` retry is clean.
- Added a streaming writer to `pkg/json` (`inc/writing.ace`): `json.Writer`
  emits compact RFC 8259 text into caller storage with no allocation or tree.
  `object`/`array`/`end` open and close containers (`end` picks `}` or `]`
  from a one-bit-per-level kind set sized to the parser's 512-level limit),
  `key` plus `value`/`none` or the `field` shorthand write members, and
  `value(Json)` walks a parsed tree, which `json.format(value, output)`
  wraps. The first failure sticks and later calls are no-ops; `finish`
  returns the byte count or a `json.FormatErr` (`capacity`, `depth`,
  non-finite `number`, `structure`). Numbers reuse `str.format`; strings
  escape `"`, `\` and control bytes and pass other bytes through.
  `pkg/json/tests/writing` covers output, round trip, and every error.
- Added the allocation-free `utf8` module (`decode`, `encode`, `width`,
  `is_valid`) over plain `u32` code points with strict RFC 3629 validation;
  a failed decode returns width 1 so loops always advance. `pkg/json` now
  uses `utf8.encode` for `\uXXXX` escapes instead of a private encoder.
  Dropped the planned `rune` and `yarn` types: a transparent `u32` alias adds
  no behavior, and a code-point sequence costs 4x storage for indexing that
  code points do not make meaningful.
- Comparing two untyped constants (`3 == 3`, `2 + 3 > 4`) now yields `bool`;
  both operands materialize at their natural type (`int` or `f64`) through
  `coerce`. A comparison chain unifies on its first typed operand and also
  materializes untyped float operands, so `x < 2.0` checks for `x: f64`.
  `bool_const` evaluates chains pairwise, so `static_assert` accepts ordering
  comparisons. Fixed the `f64` `fcmp` encoding, which carried a stray `Rm` bit
  and read the wrong register when the right operand sat in an even temp ≥ 2.
- Added `@file` and `@line` (Errors #16): the parser lowers them to a string
  literal and an untyped integer literal for the location where they are
  written. Paths are build-relative (`std/...`, or relative to the entry
  file's directory or root package, with `..` outside it) via one
  `build_path` helper, which `assert` diagnostics now use too, replacing the
  absolute paths they embedded for imported modules. Closed the package v1
  Decision Queue item, whose slice is fully implemented.
- Float `<`, `<=`, `>`, and `>=` are now false when either operand is NaN:
  `tok_cond` gives floats `LO`/`LS`/`GT`/`GE`, which all fail on an
  unordered `fcmp`, instead of the unsigned `HI`/`HS` that held there.
- The loader's module-prefix walk (`rename_refs`) now visits struct field
  defaults and field/struct `@align` expressions, and selected-import lowering
  visits the `@align` slots it skipped. Previously such an expression naming
  its own module's enum or `const` stayed unprefixed, so the struct failed to
  resolve once imported.
- Brought `pkg/http`, `pkg/json`, their tests, and demos under the
  `defer`-after-acquisition discipline. `transport.get_with_storage` defers
  the connection close instead of `exchange` closing a passed `raw` handle on
  every exit; `server.serve_one` now accepts and defers its own connection
  close, and `serve` defers the listener close.
- `tcp.Listener` and `tcp.Conn` gained an `active` flag and canonical-zero
  field defaults, making `Listener {}`/`Conn {}` the empty handle rather than
  a live-looking descriptor zero; `close` on an inactive handle is a
  syscall-free no-op returning null, so `defer tcp.close` precedes the error
  check like `mem.release`. `pkg/http` now spells its empty handles that way.
- Range `for` loops test their 64-bit remaining trip count with a 64-bit
  `cbz`; the 32-bit test skipped or truncated ranges whose count reached 2^32.
- `mem.copy`, `move`, `fill`, and `zero` call shared per-object routines
  (`__ace$mem_copy`/`move`/`fill`) instead of emitting byte loops. A private
  convention (x15 destination, x16 source or byte, x17 count; clobbers only
  x8, x15-x17, x30, v24-v31) keeps call sites spill-free. Ranges up to 64
  bytes use overlapping NEON chunks loaded before any store, which also makes
  them overlap-safe; larger ranges use 32-byte loops with the far-end chunk
  preloaded, and forward runs align the destination to 32 bytes. Release
  4096-byte copy fell from 1995 to 82 ns (1.10x libSystem).
- `mem.compare` and `mem.equal` share `__ace$mem_compare` (x15/x16 operands,
  x17 common length in, -1/0/1 out) instead of byte loops. NEON `eor`/`umaxp`
  finds a differing 16- or 32-byte chunk, and byte-reversed 64-bit words order
  its first differing byte; lengths below 16 load overlapping or first/middle/
  last chunks into vector lanes. `__text` is now 64-byte aligned and helper
  loop heads are padded to 16 bytes, since an unaligned copy loop measured
  135 ns instead of 82. Release 4096-byte compare fell from 2976 to 107 ns
  (0.67x libSystem `memcmp`).
- Integer `/`, `%`, `/=`, and `%=` by a literal or named-constant divisor
  lower without `udiv`/`sdiv` (`emit_const_divide`): powers of two become
  shifts, masks, or a biased signed shift, and other divisors a
  `umulh`/`smulh` (64-bit) or `umull`/`smull` (narrower) reciprocal multiply
  with libdivide's Granlund-Montgomery magic generator, computed in portable
  C. Constant nonzero divisors other than signed -1 need no checked guards.
  The generator was validated against hardware division on 30.9M cases. The
  CPU recurrence fell from about 45.5 to 42.5 ms in release (C `-O2` 34 ms).
- Function entries and source `for` loop heads are padded with `nop` to
  16-byte boundaries (`align_code`), so identical code no longer changes speed
  with its offset in a fetch block. Entry padding never executes; loop-head
  padding runs once per loop entry. Current benchmarks are unchanged within
  noise, since loop bodies are still too large for head placement to
  dominate; `__text` grows about 0.4%.
- Runtime repeat initializers `[value : count]` no longer emit one copy per
  element. A constant whose encoding repeats one byte, or a runtime one-byte
  integer, fills the array through `__ace$mem_fill`; other values store element
  0 and double the filled prefix through disjoint `__ace$mem_copy` calls, so
  code grows with log2(count). `bench/string.ace` `__text` fell from 358 KB to
  106 KB and its `main` from about 66,000 to 3,400 instructions.
- Aggregate copies above 32 bytes call `__ace$mem_copy` instead of an 8-byte
  word loop, with operand moves ordered so no source register is overwritten
  first; up to 32 bytes stays inline. `StaticMap` string-key checks call
  `__ace$mem_compare` instead of a byte loop. Release copies of 48-byte
  sequence elements fell about 22%, 256-byte array assignment about 50%, and
  48-byte struct returns about 8%.
- Scalar locals are read and written directly in their frame slots: one
  `ldur`/`stur` (including `ldursb`/`ldursh` and FP `s`/`d` forms) within
  x29 - 256 instead of an address temp, load, and `mov`; `mut` slots load the
  pointer and then the value into one register. Register parameters are homed
  the same way. Call spill slots are allocated per temp index on first use
  rather than as a fixed 240-byte area, so later locals and range-loop state
  stay within direct reach. The CPU recurrence loop fell from 50 to 37
  instructions per iteration and 42.5 to 39.5 ms in release (C `-O2` 33.9 ms).
- Float negation and float register moves encode their source in `Rn`
  (bits 5-9) rather than `Rm`. `fneg` previously always negated v0, so
  `a * -b` on floats miscompiled whenever `-b` sat in a later temp; `fmov`'s
  sole caller only ever moved v0 to itself, so it never misbehaved.
- Untyped constant expressions beside a typed operand (`b != 1 + 2`,
  `b + (1 << 3)`, `-(1 + 2)`) now type every node through `coerce`, which
  also recurses into unary `-`/`~`; previously only the top node was typed and
  codegen rejected its untyped children. Unary `-` accepts untyped floats, so
  `-0.5` is valid; a negated literal keeps the plain-literal path, so its
  magnitude is judged through the negation (`-9223372036854775808` for i64).
  A compound untyped integer beside a float operand is rejected with the
  declaration's explicit-cast error rather than computed in float.
- Compiler loop state lives in callee-saved registers x19-x28: a range loop's
  cursor, count, and step, and a sequence loop's cursor and count (plus its
  index binder). A statement-only pre-pass sizes each loop nest and grants
  registers innermost-first while the nest fits the ten-register pool; outer
  loops that do not fit keep frame slots. Used registers are saved right after
  the prologue and restored in the epilogue. Functions containing `asm` get no
  loop registers. String `find-4096` fell from 9.2x to 5.8x C and
  `compare-4096` from 0.85x to 0.70x. The CPU recurrence regressed from 36.1
  to 41.7 ms although its loop shrank from 37 to 30 instructions: with loop
  state gone, `state`'s store-to-next-load forwarding is the loop's only
  memory traffic and costs about 3.7 cycles per iteration on Apple cores;
  promoting `state` itself removes it.
- Integer-class scalar locals and by-value parameters whose address never
  escapes live in the callee-saved registers loop state leaves free. The
  loop pre-pass also walks expressions, weights each use 8^loop-depth
  (straight-line uses weigh nothing, so loop-free functions save no extra
  registers), and marks escapes: `&x`, volatile access, `mut` arguments,
  field/index/slice bases, tuple assignment targets, and sequence iterables.
  Writes normalize narrow values exactly as a slot store and reload would;
  taking a promoted local's address is an internal compiler error. The CPU
  recurrence fell from 41.7 to 36.0 ms (C `-O2` 34.2 ms).
- Float locals and by-value float parameters are promoted the same way into
  d8-d15, a separate eight-register pool ranked by the same weights; `f32`
  values live in the low half. The pool has its own save area, saved with
  `stp`/`ldp d` pairs. Compiler runtime routines use only v24-v31, so the
  registers survive their calls. No current benchmark has a hot float loop;
  string timings are unchanged.
- Loop binders are promotion candidates: a range binder, a sequence index
  binder, and a sequence value binder (not an `in mut` alias) take each
  iteration's value in their own register — a width-normalizing move from
  the cursor, or a direct element load — instead of a slot store and reload
  per use. They stay per-iteration copies, so assigning one never moves the
  cursor. The CPU loop fell from 31 to 30 instructions with no memory
  traffic (time unchanged: `i` is off the recurrence); `find-4096` improved
  about 1.5%.
- `if`, general `for`, and checked `assert` conditions branch on the flags:
  `==`/`!=` and ordering chains on scalars emit `cmp`/`fcmp` + `b.cond`
  (condition-code inversion is exact on unordered floats), `!` swaps the
  branch sense, and `&&`/`||` chain branches without materializing a bool;
  other conditions still test a bool with `cbz`/`cbnz`. The string
  benchmark's code fell from 21,951 to 20,963 instructions (`cset` 436 to
  37) and `find-4096` about 3%.
- Loads land in their destination: a scalar load through an address temp
  names that temp as its own destination instead of loading a new temp and
  moving it back. Scalar fields (through struct-field chains), `.len`, and the
  indexed data pointer of a plain local slot load directly at `x29 - depth`,
  and scalar members of tuple, struct, and array literals store there; deep
  slots keep the computed-address fallback, and by-reference slots and
  pointer bases keep their address loads. The string benchmark's code fell
  from 20,963 to 20,211 instructions and `str.index_from`'s inner loop from
  20 to 16.
- Promoted integer locals are operands in their own registers: arithmetic,
  compound assignment, comparisons (including chains and bool condition
  tests), and 64-bit indexes read x19-x28 directly through `src_reg` instead
  of copying into a temp, and write results to a temp, so a promoted local
  never changes by accident. Only those audited read-only consumers borrow;
  narrower indexes, which widen in place, and every other consumer still
  copy. The arithmetic emitters take physical registers so the destination
  can differ from a borrowed source. Static copies in the string benchmark
  fell from 715 to 156 and its code from 20,211 to 19,623 instructions;
  the CPU recurrence fell from 37.0 to 36.0 ms.
- `str` searches reject a start position past the last possible match before
  iterating: a range whose first endpoint exceeds its second runs downward,
  so `split` with a multi-byte separator ending the text read past the end
  (a checked bounds abort).
- `str` search is word-at-a-time: byte search tests eight bytes per
  `@byte_load` load with the exact carry-free zero-byte test
  `~(((x & 0x7F…) + 0x7F…) | x | 0x7F…)` and `bits.trail_zeros`; substring
  search loads the words at each start and at start + len - 1, keeps
  positions matching both the first and last needle byte, and confirms each
  candidate with `mem.equal`. `find-4096` fell from 9,860 to 1,450 ns
  (0.73x C `strstr`).
- Straight-line uses count toward promotion: each weighs 1 (loop uses keep
  8^depth), and a local needs a total weight of at least 2, so a single
  straight-line use, which costs about what a register save and restore
  would, keeps its slot. Loop-free arithmetic such as the float parser's
  Eisel-Lemire step no longer stores and reloads every local. `parse-f64`
  fell from 141 to 101 ns and `format-f64-shortest` from 367 to 313 ns;
  weighting a single use as enough measured 301 ns on format with no parse
  difference, but saved registers in one-use functions.
- Float parsing tries a Clinger tier first: a coefficient of at most 2^53
  with |exponent10| <= 22 (`f64`), or 2^24 with <= 10 (`f32`), is one
  multiply or divide by an exact power-of-ten table at the requested width,
  assuming round-to-nearest. The scanner and Eisel-Lemire step use
  `bits.trail_zeros`/`lead_zeros` instead of software cascades. `parse-f64`
  fell from 100 to 87 ns (3.2x C `strtod`).
- Call-free functions promote into caller-saved x14 down to x9 before
  x19-x28, needing no save. The promotion scan marks any call that is not an
  inline form (cast, constructor, compile-time builtin, assert/panic, `bits`,
  `@byte_load`, or `mem` byte operation, whose helpers clobber only x8 and
  x15-x17); `emit_call` rejects a call in such a function. An operand-count
  temp estimate plus a margin of 3 (largest shortfall seen: 2) caps the temps
  below the promoted registers, with `tmp_int`'s cap as backstop. Union
  payload binders no longer use x14 as scratch. `parse-f64` fell from 87 to
  61 ns (2.2x C `strtod`), `format-f64-shortest` from 314 to 303 ns, and
  string-bench code by 1.3%; promoting `text.len` measured no gain.
- The exact-small float tier declines exponents past 19, where any nonzero
  coefficient overflows u64, instead of multiplying until overflow; zero goes
  to the Eisel-Lemire tier's zero check. Parsing `0e9999` fell from 14,429 to
  41 ns and `parse-f64` from 61 to 60 ns.
- Integer constants 0..4095 become add/sub/compare immediates in binary
  expressions, branches, the last operand of a comparison chain, and compound
  assignment; checked 32/64-bit forms keep the flag-based overflow branch,
  while checked narrow `+`/`-` keep the register path and its range check.
  A promoted 32/64-bit integer local takes compound results (except `/`, `%`),
  constants (masked to its width), and same-width `a op b` results directly
  in its register instead of through a temp. `const_int` no longer folds an
  error-domain constructor, which packs its domain id above the code.
  `find-4096` fell from 1,456 to 1,300 ns, `parse-f64-long` from 79 to 75 ns,
  and `format-f64-shortest` from 303 to 297 ns; CPU was unchanged.
- `emit_call` places a scalar or by-address argument whose temp already is its
  argument register without a spill or reload, and spills only argument temps
  not placed that way (temps below the call's entry depth were spilled at
  entry). A scalar result moves from x0/v0 into its temp's register before
  `reload_temps` instead of round-tripping a slot. An unguarded constant index
  (at most 4095 bytes of offset) is one add or none, and a plain store to a
  scalar field of a frame-resident local struct is a direct `stur`.
  `parse-f64` fell from 59 to 52 ns (1.9x C `strtod`), `format-f64-shortest`
  from 296 to 262 ns, and string-bench code by 5.3%; CPU was unchanged.
- Renamed `std/@/pow_tables.ace` to `math_tables.ace` and moved the `sin`/`cos`
  Payne-Hanek `2/pi` limb table into it as `two_over_pi`, so it is the single
  home for `math`'s constant tables. Results and timings are unchanged.
- Removed the streaming `json.Writer` from `pkg/json`, along with
  `json.writer` and `FormatErr.structure`; the non-streaming `json.format`
  stays as the only writer. `format` is now a recursive tree writer whose
  helpers take and return the write offset, so the first error unwinds without
  shared state. Output, escaping, and the `capacity`, `depth`, and `number`
  errors are unchanged. `pkg/json/tests/writing` now goes through `format`,
  building a 513-deep tree by hand for the depth case. A streaming
  `encode`/`decode` API will be designed separately.
- Reworked `pkg/json` ownership around a `Parser`: `json.parser_new()` makes
  one, `mut parser.parse(text)` returns a `Json` borrowed from it, and one
  `json.parser_wipe(mut parser)` releases every tree it produced. Each parse
  takes one block per element type (string bytes, array elements, object
  members) sized from the input length, since n bytes hold at most n / 2
  elements and n / 4 members, and builds containers by pushing finished
  children on parser-owned scratch stacks and copying them into the block at
  the closing bracket. This replaces the free `json.parse`, the recursive
  `json.wipe`, and every per-node allocation and unwind path; a failed parse
  needs no cleanup. Parse plus release of a 280 KB document fell from about
  6.3 to 4.1 ms (1.5x), with the same ratio at 1.4 KB and 28 KB. `JsonErr` is
  now `ParseErr`, beside `FormatErr`, with `depth_exceeded` shortened to
  `depth`; it and the parser live in `inc/parsing.ace`, and `inc/value.ace`
  became `inc/document.ace`, holding only `Json` and `Pair`.
- Added `json.Formatter` to `pkg/json` and removed the free `json.format`,
  which could only serialize a finished `Json` tree and so could not produce
  JSON from runtime data. `json.formatter_new()` and `json.formatter_wipe(mut
  formatter)` pair like `parser_new` and `parser_wipe`. The formatter is the
  earlier streaming state machine (one kind bit per open container, sticky
  first error) writing into a buffer it owns and grows from 256 bytes, with
  `value` and `field` overloads for `bool`, `int`, `uint`, `usize`, `f64`,
  `string`, and `Json`, so integers keep their exact digits. `finish()`
  returns the borrowed text. `FormatErr` is now `allocation`, `depth`,
  `number`, and `structure`; `capacity` is gone because the buffer grows. A
  formatter holds one document at a time and `reset` starts the next, keeping
  the buffer. `Parser.reset` likewise drops every tree but keeps its blocks,
  reusing any that is large enough, so results of both types are valid until
  `reset` or `wipe`. On 139-byte documents reuse cut parse time from 2.73 to
  2.07 us (24%) and format time from 1.68 to 1.55 us (8%). Added
  `demo/json-format-file`, which formats runtime-computed records and writes
  them to a file.
- Folded constant arithmetic in global initializers (`fold_int`/`fold_float` in
  `emit.c`, reached from `eval_const`): integers evaluate in 128-bit values
  range-checked against the destination width, floats in `double` with `f32`
  rounded per operation. Overflow, zero divisors, and bad shift counts are
  emit errors; casts over foldable expressions, including int-to-float, fold.
- Renamed `mem.adjust[T]` to `mem.grow[T]`, which states what it does; the
  signature and behavior are unchanged, and every `std`, `pkg`, and demo caller
  now uses the new name.
- Added the `blank(T)` builtin, the canonical zero of any type including a
  generic type parameter. Sema resolves the operand as a type (parsed with the
  full type grammar like `sizeof`) and types the call as `T`; the emitter
  zero-fills a register or frame temp, so it costs nothing beyond the fill.
- Added `std/array.ace`: a generic `Array[T]` with `push`, `reserve`, `pop`,
  `get`, `set`, `view`, and `clear` methods plus `array_new`/`array_wipe`,
  growing through `mem.grow`. `get`/`set` are checked against `len` by slicing
  the live prefix, so no new bounds machinery was added; `pop` on an empty array
  returns `blank(T)` and `ArrayErr.empty`.
- Added `bits.wrap_add`, `wrap_sub`, and `wrap_mul` for `u8`..`u64`: bits
  intrinsics (ids 33-35) emitting one plain `add`/`sub`/`mul`, re-truncated for
  the narrow widths, so they wrap in checked builds where the operators abort.
  `is_bits_integer_op` in `emit.c` now names the integer-intrinsic id set the
  direct-call, callable-symbol, and inline-eligibility sites share.
- Added `mem.zalloc[T](count)`, typed allocation with zero-filled storage: it
  reuses `gen_alloc_typed_builtin` (same overflow, zero-size, and registry
  rules) and calls `calloc(bytes, 1)` in place of `malloc(bytes)`. Darwin's
  allocator never returned dirty recycled blocks in trials, so the suite checks
  the `_calloc` reference in the object rather than block contents.
- Added `std/map.ace`: a generic `Map[K, V]` with `get`, `has`, `set`, `unset`,
  `reserve`, and `clear` methods plus `map_new`/`map_wipe`. Open addressing
  with linear probing over a power-of-two slot array from `mem.zalloc`, grown
  by rehash at 3/4 load, with backward-shift `unset`. Keys hash and compare
  through private `specialized` `hash[K]`/`same[K]` for the fixed-width
  integers, `usize`, `bool`, and `string`; the finalizer and FNV-1a step use
  `bits.wrap_mul`, since ordinary `*` aborts on overflow in checked builds.
- Added type-owned associated functions, `Type::name(...)`: a new
  `T_COLONCOLON` token and an `F_ASSOC` flag parallel to `F_METHOD`, with
  `N_FIELD_E` postfix parsing for `::` mirroring `.`. Declaration and lookup
  reuse `declare_func`/`method_key`'s existing owner-namespaced-key path, so
  a method and an associated function can never share owner+name (rejected
  at declaration). A non-generic owner resolves through the ordinary
  direct-call machinery once found under its namespaced key; a generic
  owner (`Map[K, V]::new(...)`) goes through a new
  `check_generic_assoc_call`, mirroring `check_method_call`'s generic
  branch minus the receiver-shaped logic (no `rmut`/`cmut`, no embedding
  promotion), substituting the owner's own type arguments — read from the
  call-site type expression, not a receiver parameter — into each overload
  candidate before matching. An unresolved call defers through the same
  end-of-file `Pending` fixup plain and method calls already use, so
  associated functions may call each other in any declaration order. The
  emitter reads a generic call's type arguments from the resolved owner
  type instead of a receiver (`gen_call`), and treats `F_ASSOC` like
  `F_METHOD` everywhere a receiver argument would otherwise be assumed
  (defer capture, `main` detection). `::` is compile-time-only: it acquires
  no vtable slot and cannot be reached through a value (`m::new()` is a
  compile error, since `::` only follows a type expression). An associated
  function's own type parameters beyond the owner's are not yet supported.
- Migrated `array`, `map`, and `pkg/http`'s `Router`, `ClientContext`, and
  `ServerContext` from `_new`/`_wipe` free functions to type-owned form:
  `Array[T]::new`/`Map[K, V]::new`/`Router::new`/`Context::new` (in
  `pkg/http/inc/client.ace` and `pkg/http/inc/server.ace`) as associated
  functions, `wipe()` as an ordinary mutating method, every call site
  updated. `pkg/http/mod.ace` drops its `router_new`/`router_wipe`,
  `client_context_new`/`client_context_wipe`, and
  `server_context_new`/`server_context_wipe` forwarding wrappers, since
  `Router::new()`, `ClientContext::new()`, `ServerContext::new()`, and
  `mut value.wipe()` now reach each type through its re-exported alias
  exactly as `Router.handle`/`Router.dispatch` already did. Fixed a
  cross-module gap this surfaced: `rename_decls`/`collect_members` in
  `module.c` exempted `F_METHOD` declarations from the module-name-mangling
  pass (a method's identity comes from its owner type, not a standalone
  name) but were never extended to `F_ASSOC` when associated functions
  landed, so an associated function's name was mangled with its declaring
  module's prefix while callers built the lookup key from the unmangled
  name — `Type::new()` resolved only within its declaring file. Both flags
  are now exempted together. Also migrated `pkg/http`'s `parse_url`/
  `parse_url_for` free functions in `parsing.ace` to two `Url::parse`
  associated-function overloads (`Url::parse(url)` autodetecting the
  scheme, `Url::parse(url, secure)` forcing it), dropping `mod.ace`'s
  `parse_url` wrapper; this makes the previously package-internal
  scheme-forcing overload part of `http`'s public surface, since an
  associated function's visibility is gated by `pub`/receiver type, not by
  `mod.ace`'s re-export list.
- Migrated `pkg/json`'s `Parser` and `Formatter` from `_new`/`_wipe` free
  functions to type-owned form, the same as `pkg/http`: `Parser::new`/
  `Formatter::new` as associated functions, `wipe()` as an ordinary mutating
  method, every call site updated. `pkg/json/mod.ace` drops its
  `parser_new`/`parser_wipe` and `formatter_new`/`formatter_wipe` forwarding
  wrappers, since `Parser::new()`, `Formatter::new()`, and `mut value.wipe()`
  now reach each type through its re-exported alias.
- Replaced seven hand-rolled zero-value constructor functions predating
  `blank(T)` with direct `blank(T)` calls at each call site: `zero_addr4`
  (duplicated in `std/net`'s facade and its Darwin target), `zero_dir`,
  `zero_timespec`, and `zero_stat` in `std/sys`'s Darwin target, `zero_kevent`
  in `std/poll`'s Darwin target, and `zero_url` in `pkg/http`. Every one built
  an all-zero-field struct literal by hand; no behavior change.
- Backed `pkg/http`'s `Router.routes` with `array.Array[Route]` in place of a
  hand-rolled `mem.grow`/index/increment loop, using `push`/`view`/`get`.
  Dropped the private `RouterErr` domain, which only ever duplicated
  `array.ArrayErr.allocation`.
- Added `Array[T].truncate(count)` to `std/array.ace`: drops every live
  element from `count` onward while keeping storage, bounds-checked against
  `len` the same way as `get`/`set` (checked builds abort past `len`, release
  assumes it). Converted `pkg/json`'s `Parser.stack`/`pstack` value/pair
  stacks from hand-rolled `mem.grow` pairs (`sat`/`psat`) to
  `array.Array[Json]`/`array.Array[Pair]`, using `push`/`get`/`truncate` for
  the parse_array/parse_object step that rewinds a closed container's
  temporary elements back to its watermark after copying them into the
  block's permanent storage.
- Added the standalone package `pkg/sdl` (`type: lib`), a minimal SDL3
  binding whose manifest links `SDL3` with Homebrew's `/opt/homebrew/lib` as a
  `[[link]] path` hint. `Window::new`/`wipe` and `Renderer::new`/`wipe` wrap
  SDL's opaque pointers in value structs, `Renderer.color(c)`/`color()`
  overloads pair `SDL_SetRenderDrawColor`/`SDL_GetRenderDrawColor` over a
  `Color` struct, and `error()` views `SDL_GetError`'s NUL-terminated text as
  a string without libc. `Event` mirrors the `SDL_Event` union as an
  overlapping struct (`kind`, `keyboard`, `quit`, `[128]u8` padding, all
  `@offset(0)`) whose layout `static_assert`s pin against the 3.4 headers.
  `pkg/sdl/tests/events` round-trips a key event through `SDL_PushEvent` and
  `SDL_PollEvent` headlessly and is skipped where that library is missing;
  `demo/sdl3-window` opens a window and renderer and runs a classic
  `SDL_PollEvent` loop, quitting on window close or Escape.
- Extended `pkg/sdl` with untextured colored triangles:
  `Renderer.geometry(vertices, indices)` over `SDL_RenderGeometry` with
  `Vertex`/`FPoint`/`FColor` mirroring SDL's layout, `Renderer.size()` over
  `SDL_GetCurrentRenderOutputSize`, `ticks()`/`ticks_ns()` over
  `SDL_GetTicks`/`SDL_GetTicksNS`,
  `Renderer.vsync`, `Renderer.scale`, and `Renderer.debug_text` over SDL's
  built-in 8x8 font.
  `demo/sdl3-rgb-cube` spins a cube with one solid color per face, culling
  back faces against the camera and drawing each visible face as two
  triangles over four vertices of its own. It presents under vsync and
  overlays FPS, frame time, CPU draw time excluding the wait in `present`,
  visible faces and triangles, and output size,
  formatted with `str.format` into fixed line buffers.
- Added the standalone package `pkg/regex` (`type: lib`), a byte-oriented
  regular-expression engine. `Syntax::parse(pattern): (Syntax,
  Error, usize)` builds a syntax tree or returns a `ParseErr` with the byte
  offset where parsing failed (an unclosed group or class reports its opening
  delimiter). Nodes sit in one `Array[Node]` and name children by `u32`
  index; concatenation and alternation are n-ary runs of a shared `children`
  array, so tree depth follows group nesting (capped at 512), not pattern
  length. Classes are 256-bit `[4]u64` bitmaps; `{m,n}` is capped at 1000.
  Escapes must name a metacharacter, `\n \t \r \xHH`, `\d \w \s` and
  their complements, or `\b \B`; anything else, including backreference
  digits, is rejected. `Program::compile(tree): (Program, Error)` lowers the
  tree by Thompson construction to a flat, anchored `Array[Inst]` for a Pike
  VM, encoding leftmost-first priority in `split` order (lazy forms swap it);
  `{m,n}` becomes m copies plus n-m nested optional copies. Forward targets
  are patched in place, and runs of placeholders sharing one target are
  threaded as a list through their own target fields. A subtree that emits
  nothing stops its copy loop, and programs cap at 65,536 instructions
  (`CompileErr.too_large`). `Syntax.format` and `Program.format` write
  canonical dumps through a shared `Output` sink. `Regex::new(pattern)`
  parses and compiles, and `is_match`, `find(text, after)`, and
  `captures(text, after, spans)` run a Pike VM: threads step in lockstep per
  input byte in a sparse-set queue, a new lowest-priority thread is seeded at
  each position until a match is found (leftmost-first in one pass), and
  `jmp`/`split`/`save`/assertions are followed with an explicit stack whose
  restore frames undo a `save` before a deferred sibling branch runs. Every
  buffer is sized from the program in `Regex::new`, so searches take `mut
  self` but never allocate or fail. Searches take the whole text and a start
  byte, so assertions see the true context; unset groups are `UNSET` spans.
  `pkg/regex/tests/parsing`, `compiling`, and `matching` run under `make
  test-packages`, and `docs/regex.md` documents the package.
  `bench/run-regex.sh` times four find-all and pathological workloads
  against a structure-for-structure C port of the matcher running the same
  programs (Ace/C 2.9-4.1x, just ahead of C `-O0`) with PCRE2 as an optional
  reference; `bench/README.md` records the baseline.
- Closed the definite-initialization hole through views of a local: a slice
  of a local array and a read-only `&` created no read, so an uninitialized
  array passed as `first(spans[0..2])` compiled. A view is now judged once at
  creation (`init_array_view`, the `&` case in `init_expr`): a read-only view
  requires its constant path, or the whole array for a slice or dynamic index;
  a writable view marks its constant path, or the whole array for a
  full-range slice, as a trusted promise to fill it. Slice bounds are checked
  before the base, so `buffer[0..render(mut buffer[0..n])]` holds. Per-element
  slice precision was prototyped and left out: an unsorted `InitPart` list made
  each branch join quadratic (a 4096-element partial range across 50 branches
  took 1.85 s against 3.3 ms). Buffers filled by dynamic index in `std/fmt`,
  `std/net`, the packet-sniffer demo, and `pkg/regex` tests now write through
  one full-range `mut` view of their storage.
- Definite initialization now checks specialization bodies. The pass skipped
  every function with a bracket list and no instantiations to exclude
  unstamped templates, but a specialization's list holds its type arguments
  and never gains instantiations, so its body went unchecked (this let the
  uninitialized `std/fmt` buffers compile). A specialization is recognized by
  its symbol's `spec_args` and checked like any ordinary function, called or
  not.
- Closed the unchecked-result hole of calls resolved at end of file. A forward
  call, or one whose untyped argument waits for the complete overload set
  (`f(5)` against `f(a: u64)`), returned the poison type, and only the call
  was resolved later: `y: u8 = f(300)` and `s: string = later()` compiled, and
  a cast or nested operator over such a call failed in codegen. Each function
  body (per generic stamp) that made a pending call is now re-checked in a
  final mode after resolution, and emit-time stamping resolves in that mode.
  Every `Sym` carries a declaration sequence number; the re-check sees only
  symbols up to the body's first-check point or created during the re-check,
  and overload chains, appended in declaration order, are cut at the same
  point, so name lookup, builtin shadowing, and overload selection match the
  first check. Calls that were pending (`F_PENDING`) see every declaration.
  A call is flagged when it goes pending, so `compile_time_expr` rejects it
  like a resolved call: a forward call in a field default was taken for a cast
  and reached codegen. Two checked-bounds fixtures returning `i32` from
  `main(): int` relied on the hole and now convert explicitly. The regex app's check costs 1.2% more
  instructions.
- Fixed a pending call resolving against locals declared after it. The
  end-of-file resolution looked the callee up in the call's saved block scope,
  which by then held the rest of the block, so `y: usize = get(1)` followed by
  `get: usize = y` reported `unknown function 'get'`. Each pending call now
  records the declaration sequence at the call, and inside its function's own
  scopes sees only locals up to it; the enclosing scopes stay fully visible.
- Pooled float constants per function: a float literal outside the 256
  `fmov #imm8` values, and a read of a const `f32`/`f64` global initialized by
  a literal (possibly negated, or another such const), now loads with one
  `ldr Vd, <literal>` from a deduplicated 8-byte table that `flush_fpool`
  places after the function's last instruction, patching each site's `imm19`.
  They were a `mov`/`movk` chain plus `fmov`, or `adrp`/`add`/`ldr` from the
  global's `__const` slot; `+0.0` is now one `fmov Vd, xzr`. `exp` runs 12%
  faster for `f64` and 7% for `f32`, `pow` 7%, `log` 5%; the hot-loop kernels
  are unchanged, and machine code shrinks slightly despite the tables.
- Self tail recursion: release builds lower `return f(args)` inside `f` to a
  branch back to the parameter reads instead of `bl` plus an epilogue, reusing
  the frame, as a documented optimization rather than a guarantee
  (`self_return`, `emit_call`);
  calls to other functions are untouched. `emit_func` binds the branch target
  after the prologue, callee-saved stores, and before the parameter reads only
  when sharing one frame across levels is unobservable (`self_eligible`):
  every parameter arrives in a register and is flat (a scalar, slice, or
  `string`) or by reference, every binder is flat, and nothing takes the
  address of a local or by-value parameter (`self_safe`); `main`, generic,
  exported, inline-assembly, and inline-expanding functions are excluded, and a
  pending `defer` keeps the call. Checked builds keep every call. The
  `tail-recursive` workload in `bench/core/calls` (depth 10,000) falls from
  10.58 to 0.96 ns per level, equal to C `-O2`, and ten million levels now run
  in one frame; other workloads, the 20,000-function compile time, and its
  machine code are unchanged.
- Diagnostics now render function, tuple, and pointer types in full
  (`(i32, i32): i32`, `(i32, bool)`, `*Point`) instead of the placeholders
  `(func)`, `(tuple)`, and `*T`; composite names build in a local buffer before
  taking a slot in `type_name`'s ring. Type arguments after a function with no
  generic form report that it is not generic, as a value and as a call.
- Tuple literal members now meet their declared member types as struct field
  initializers do, so `(1, 2)` initializes `(i32, i32)`: `coerce` checks each
  member against its member type, replacing the pass that fixed `int`, `f64`,
  and `u8` defaults before comparing.
- Tuple expression nodes now take the position of their first token (the
  opening parenthesis, or a bare list's first element) instead of the token
  after them, so a wrong-count tuple diagnostic names the tuple's own line.
- Rejected `%` on floats. Sema accepted it, including between two untyped
  float constants, while the emitter had no float remainder and returned the
  left operand; `%` now joins the bitwise and shift operators' integer-only
  check, placed before the untyped-constant shortcuts. Compound assignment
  now checks its operator against the target as `x = x op y` does (numeric for
  `+ - * /`, integer for `% & | ^ << >>`); it previously only coerced the right
  side, so float `%=`/`&=`/`<<=` and `bool`, struct, or `string` `+=`/`*=` were
  accepted.
- A float cast to an 8- or 16-bit integer now wraps the converted value to the
  target width. `fcvtzs`/`fcvtzu` saturate at 32 bits, so the AST emitter left
  `u8(300.0)` as 300 in its register while the stored value was 44, and
  `u8(300.0) == u8(44)` was false. The IR already wrapped.
- Slice expression nodes now take the index node's position (the first token
  inside the brackets) instead of the token after the closing `]`, so debug
  slice bounds violations and compile-time slice diagnostics (bound order,
  writable-content establishment) name the slice rather than the next line.
- The AArch64 bitmask-immediate encoder now builds its element-size prefix from
  an unsigned value; it shifted a negative `int`, which the sanitizer build
  reports on any compile that encodes a bitmask. Emitted objects are unchanged.
- Fixed a slice bound whose call pends until end of file being rejected. A
  call that cannot choose its overload before every declaration is known (an
  untyped float argument, as in `s[0..width(1.0)]`) returns the silent
  poison type until its body is re-checked, and the slice-bound check reported
  that as `slice bound must be an integer`; a poisoned bound now makes the
  slice poison too, as an index already did. `tests/ir-diff.sh` generated such
  a bound (seed 7, program 72).
- `bench/core/hotloop` adds `cube-transform`: the per-frame math of
  `demo/sdl3-rgb-cube` (8 corner rotations and projections, 6-face backface
  cull, vertex and index fill) for 1,024 cubes over 2,000 frames, headless.
  Rotations come from a 64-entry table built by an exact recurrence rather
  than `sin`/`cos`, and the C side disables `FP_CONTRACT` in the kernel, so
  checksums match bit for bit. Release Ace runs 1.15x scalar C `-O2`.
- Global value numbering now merges two `IR_GLOBAL`s only when their names
  match as well as their symbol. A static map's keys, values, and slots tables
  share the map's symbol, as do a generic's instantiations taken as function
  values (`pick[u8]`, `pick[u64]`), so release builds addressed every table
  from one of them and called every instantiation through one; e.g.
  `tests/emit/staticmap/narrow_keys.ace` exited 1 under `--release`.

## Current frontier

- Added `ace.pkg` highlighting to `editors/vscode/`: a separate `ace-pkg`
  language matched by exact filename (not `.pkg`, which macOS installers
  use), with a TextMate grammar following `GRAMMAR.md` §7. Added language
  icons for `.ace` (including `mod.ace`) and `ace.pkg`, shown when the
  active file icon theme has none.
- Unified `pkg/sdl`'s value types: one `Point` and one `Color`, both `f32`
  (SDL_FPoint/SDL_FColor layouts), replacing `FPoint`, `FColor`, and the
  `u8` `Color`; `Renderer.color` now wraps `SDL_{Set,Get}RenderDrawColorFloat`.
  Field defaults (`Color.a = 1`, zero elsewhere) make `Vertex {}` blank and
  leave `tex_coord` unwritten; `Color::hex(rgb)` builds an opaque color
  from a 24-bit `0xRRGGBB` value (e.g. `0xF44336`). Renamed
  `demo/sdl3-rgb-cube` to `demo/sdl3-cube-rotation`.
- Typed `pkg/sdl`'s flags and event values: `InitFlags` (`bits u32`) and
  `WindowFlags` (`bits u64`) replace the `INIT_*`/`WINDOW_*` constants, and
  the `u32`-backed enums `EventType` and `Keycode` replace `EVENT_*`/`KEY_*`
  as the types of `Event.kind` and `KeyboardEvent.key`.
- A `mut` method call now initializes its receiver for definite
  initialization, as `mut &receiver` does, instead of requiring it to be
  initialized first (Mutability #6). `pkg/sdl` uses it: `mut Event.poll()`
  replaces `poll_event(mut *Event)`, so loops read `for mut event.poll()`.
- `pkg/sdl` `Renderer.geometry` with empty `indices` now passes `NULL` so SDL
  draws the vertices as consecutive triangles, instead of drawing nothing;
  `indices` without `vertices` returns false.
- `pkg/sdl` `Window::new` and `Renderer.debug_text` take plain Ace strings:
  each copies the text into a `TEXT_MAX + 1` (256) byte stack buffer and
  appends the NUL SDL reads, with no allocation; longer text fails.
- `pkg/sdl` `Keycode` names `backspace`, `tab`, `enter`, `escape`, `space`,
  `digit_0`–`digit_9`, `a`–`z`, and the four arrows; other keys stay
  reachable as `Keycode(c)` with `u32(key)` for range checks.
- `pkg/sdl` `Event.keyboard(): (KeyboardEvent, bool)` returns the keyboard
  member with false unless `kind` is `key_down` or the new `key_up`; the raw
  field stays for direct access.

- Extended auto-vectorization to `f64` lanes (Compiler #6): the IR's
  `v2f64` holds two lanes in 128 bits and takes `add`, `sub`, `mul`, `div`,
  `neg`, `splat`, `load`, `store`, and `phi`, which `arm64.c` emits as NEON
  `.2d` forms (`dup .2d`, or `fmov .2d`/`movi` for an 8-bit immediate). A
  loop's lane types share one width, so `f64` never mixes with `f32`/`i32`
  lanes, and a compare on `f64` keeps a loop scalar; the vector loop counts
  `n >> 1` vectors at the same 16-byte stride. `bench/core` `saxpy-f64` drops
  from 0.508 to 0.249 ns/element (2.04x; C `-O2` scalar 0.485, vectorized
  0.182). `tests/ir/vectorize.ace` adds accepted and rejected `f64` loops, and
  `tests/vectorize/lanes.ace` `f64` counts with NaN, signed-zero, infinite,
  and subnormal lanes.
- Extended auto-vectorization to strided struct fields (Compiler #6): a
  struct array whose same-typed fields each get the same computation, every
  field stored and each reading only its own field, vectorizes as the flat
  array of its fields, running field 0's computation once per 16-byte vector
  of a group of structs. The loop's elements may be a `padd` scaled by the
  struct size or a strength-reduced pointer phi, which the remainder loop
  resumes at `base + i * size`; accesses at different field offsets of one
  struct are independent. `bench/core/run-hotloop.sh` `particle-vec3` went
  from 1.11 to 0.281 ns/particle (3.96x; C `-O2` scalar 0.735, vectorized
  with fused multiply-adds 0.274). `tests/ir/vectorize.ace` adds accepted
  `Vec3` and `Vec2` loops and rejected uneven, partial, and crossed ones;
  `tests/vectorize/lanes.ace` adds `Vec3` `f32`, two-field `f64`, and
  four-field `i32` arrays.
- Fixed auto-vectorization reading `blocks[-1]`: every two-predecessor block
  is a candidate header, and the body walk reached a return block before the
  back-edge check, so ordinary `if` joins (e.g. in `mem.grow`) read out of
  bounds and `demo/http-simple-server` crashed. The walk now rejects a
  candidate at any block that leaves through a return.

The checked-build queue is complete. Additive networking, concurrency, fuller
math/SIMD, and a second
backend remain open; their status belongs in the `SPEC.md` Decision Queue and
`TODO.md`, not in this history.
