# Archive — Bootstrap compiler

Entries recorded before the changelog split; oldest first.


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
- Added the linux-x64 OS layer for `sys`, `fmt`, and `time` over glibc. Each
  facade selects `./os/linux-x64.ace` with `@when` beside Darwin's, and the
  `BIOC*` re-exports are gated to darwin-arm64. The `sys` error domains keep
  their variants with Linux errno numbers (no `ForkErr.process_limit`); paths
  use 4096-byte `PATH_MAX` buffers; `stat` reads the 144-byte x86-64
  `struct stat`; `readdir` parses `getdents64` records (NUL-terminated names
  at offset 19), so the Linux `Dir` has no `base`. `time.elapsed` reads
  `CLOCK_BOOTTIME` and `time.awake` `CLOCK_MONOTONIC_RAW` through
  `clock_gettime`. An Ace `main` stores its entry argc and argv in local
  `.bss` words that `sys.argc`/`sys.arg` load, and `sys.envc`/`sys.env` read
  glibc's `environ`.
- Added the linux-x64 `poll` OS layer over level-triggered epoll. `watch`
  adds, or modifies on `EEXIST`, in one atomic update; `unwatch` deletes.
  `wait` calls `epoll_wait` with the timeout rounded up to whole milliseconds
  and capped at the i32 maximum, so it needs no recent glibc or kernel. The
  event data word holds the fd and a read-interest bit, so a hangup
  (`EPOLLHUP`/`EPOLLRDHUP`) reports readable only under read interest,
  matching kqueue's EOF.
  `WatchErr` gains `unsupported` (`EPERM`) and `watch_limit` (`ENOSPC`).
- Added the linux-x64 `net` OS layer. It mirrors Darwin's operations over
  glibc with Linux errno numbers and socket constants, a `sockaddr_in` whose
  first field is a u16 family (no length byte), and glibc's `addrinfo`, whose
  address precedes the canonical name. `ResolveErr` carries glibc's negative
  `EAI_*` codes beside the local `invalid`.
- Made the ELF writer emit the constant section as writable `.data.rel.ro`
  when it carries relocations (vtables, strings in const aggregates and
  static maps), so a linux-x64 PIE links without a text relocation and the
  linker write-protects the section after relocation (RELRO); an object
  without them keeps `.rodata`.
- Made the bounds-diagnostic and debug tail-call checks in `tests/run.sh`
  pass on Linux: the former matches its exact line (dash writes its
  `Aborted` job line into the redirected stderr), and the latter reads
  `objdump` where `otool` is absent and matches x64's `call` and ELF's
  single-underscore name.
- Made the `nm` symbol checks in `tests/run.sh` host-aware: one helper lists
  an object's undefined symbols by C name (dropping GNU nm's `U` column and
  Mach-O's extra leading underscore), so the required OS boundaries (errno as
  `__error` or `__errno_location`) and the forbidden runtime dependencies are
  checked on Linux too, rather than failing or passing vacuously.
- Made the `@when` fixtures host-neutral: `tests/modules/when` binds its `os`
  alias to a darwin-arm64 or linux-x64 file and fails to resolve on any other
  target, and the duplicate-declaration fixture matches with an always-true
  `target !=` comparison.
- Made the `sys/ioctl` and `printf/float` emit fixtures run on linux-x64:
  each selects its target's constants with `@when` (BIOC requests or
  `FIOCLEX`/`FIONREAD`; `FE_DOWNWARD`), and a `// expect-link: ARGS` fixture
  line appends linker arguments, giving `printf/float` the `-lm` that glibc's
  `fesetround` needs.
- Gated the Darwin-only checks in `tests/run.sh` to a Darwin host: the SDK BPF
  layout verifier, the driver's `-framework` link, and the
  `package_cli/link_framework` case, which a new `// expect-host: OS` manifest
  comment skips unless `uname -s` prints OS.
- Made `tests/vector_ir.c` emit through the host's target: on linux-x64 it
  writes an ELF object with `x64_func` (the shared IR names drop Mach-O's
  leading underscore as symbols) and clobbers every xmm register across the
  call, so the lane-for-lane comparison with C covers x64's SSE lowering.
- Made `bench/core/run-hotloop.sh` build and run on Linux: `hotloop.c` times
  with `CLOCK_MONOTONIC_RAW` there (the clock `time.awake` reads) and keeps
  `CLOCK_UPTIME_RAW` on Darwin, and the scalar C build disables
  vectorization with the switches of whichever compiler `cc` is.
- Added x64 inline assembly. The driver now selects the target before
  semantic checking (it was set only for code generation, so checking and
  `--ir` always saw darwin-arm64), and sema checks bindings and instructions
  against it through a register table beside `ace_target`: x64 binds `rax`-
  `r15` except `rsp`/`rbp` by encoding and takes `nop`, `syscall`, and
  `int3`. x64 loads each `in` value into its register from its slot, encodes
  the instruction, and stores each `out` register narrowed to its slot;
  reserved `rbx`/`r12`-`r15` are saved below rbp at entry and restored before
  `leave`. The IR prints the target's register names. A sema or IR fixture's
  `// target: NAME` line checks it for that target; the asm emit fixtures
  select per-target functions with `@when`, and `tests/ir-diff.sh` binds a
  host register.
- Moved the target-neutral half of register allocation out of `arm64.c` into
  `src/regalloc.c` (`regalloc.h`): critical-edge splitting, block and
  emission order, use counts, liveness intervals, call/byte-routine/asm
  positions, phi and cast location sharing, and the linear scan. A target
  describes its classes in an `RaTarget` (caller- and callee-saved registers
  by class in preference order, what byte routines disturb, whether a vector
  spills across a call, which casts stay in place) and marks the values their
  user fuses; arm64 keeps its hints, fusion, and callee-saved save list. Every
  darwin-arm64 object of the emit, module, checked, bench, and demo roots is
  byte-identical in debug and release.
- Gave the x64 backend register allocation through `regalloc.c`: rbx and
  r12-r15 (saved when used), r10, r9, r8, rsi, and rdi, and xmm3-xmm15 are
  allocated; rax, rcx, rdx, r11, and xmm0-xmm2 stay selection scratch, and
  rbp the frame base. Selection is unchanged in shape: operands move from
  their locations into scratch and results back. Phi moves, call and byte
  routine arguments, parameters at entry, and asm bindings are parallel
  moves (a general-register cycle broken by `xchg`, any other through the
  frame's 16 scratch bytes), replacing the phi staging area; an aggregate
  argument copied with `rep movsb` keeps rsi and rdi across it; blocks are
  emitted in the allocator's order, loop headers rotated. The allocator now
  starts a parameter's or result address's interval at entry, where both
  targets move them (no darwin-arm64 object changes). `bench/core` hotloop
  on x64: 1.28-1.73x faster per workload (cube-transform 236.7 to 171.0
  ns), machine code about 40% smaller, compile time 4-18% higher.
