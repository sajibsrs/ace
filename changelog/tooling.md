# Tooling

- selfhost asks `sys.stat` for a path's type instead of declaring C `stat`
  and reading `st_mode` at a hard-coded offset per target.
- The self-hosted compiler lives in `selfhost/` (formerly `compiler/`), naming
  its role beside `src/`, the C bootstrap compiler.
- Manifest sections are singular, `[dependency]` and `[link]` (a plain
  section of repeatable `path:`/`framework:` lines), and a header may take one
  target or OS qualifier (`[link: darwin-arm64]`); sections not selected by
  the build target are parsed and dropped unchecked.
- A `[[link]]` record takes one `path: FILE`, an exact file (relative inside
  the package, or absolute), or a platform `framework: NAME`, replacing
  `object`/`search`/`library` and directory paths: no search, suffix order, or
  `-l` fallback. Identity is the file name; the root's path overrides.
- Split `CHANGELOG.md` into `changelog/` by area (`lang`, `compiler`, `std`,
  `pkg`, `tooling`), moving every earlier entry verbatim into
  `changelog/archive/` under its original section so sessions grep history
  rather than read it; new entries are 2–4 lines.
- Moved the rationale of implemented rules out of `spec/` (14 chapters, 20 KB)
  verbatim into a "Design rationale" section of the matching `docs/` page;
  `spec/` keeps rationale only for open decisions.
- VS Code extension: `.acefmt` gets a `{ ♠ }` icon, a section/key/value
  grammar, and `#` comments; `ace-framed.svg` is renamed `ace-pkg.svg`; the
  manifest names its publisher and author.
- Benchmarks run on both targets: the C twins read Ace's `time.awake` clock
  through one `bench/awake.h`, runners use vectorizer flags clang and gcc both
  accept and pick `size`/`time` flags by host, and the compile-time runner
  passes `--debug` where it reports debug now that release is the default.
- `tests/run.sh` picks its DWARF reader once: `llvm-dwarfdump`, else
  `dwarfdump`, whichever reports itself as LLVM, so libdwarf's same-named tool
  on Linux no longer fails the DWARF checks; with neither, they are skipped.
- `tests/run.sh` runs emit fixtures `ACE_JOBS` at a time (default: CPU count)
  and the checked, module, and driver sections in the background with their
  own scratch files, printing reports afterward in a fixed order; the suite
  drops from 2:37 to about 43 s on a 10-core Mac.
- `--tokens` prints a string token escaped as an Ace literal that lexes back
  to the same bytes, instead of raw `%.*s`, which stopped at a NUL byte and
  let an embedded newline split the line.
- selfhost begins sema (`selfhost/sema.ace`, #123) with top-level entry: file
  and import scopes, `Sym` and nominal `SType` shells, and entry diagnostics,
  shown through `Loader.shown` (C's `module_display`). `--symbols`, in both
  compilers, prints the entered scopes; `check.sh` diffs it.
- selfhost completes types: struct, bits, union, enum, distinct, alias,
  contract, and error-domain declarations, with layout and its deferred
  checks, const integer evaluation, and their diagnostics. `--symbols` now
  dumps each completed type's layout and parts in both compilers; C's runs
  through the new `complete_types`, split out of `sema_check`.
- selfhost stamps generic struct, union, contract, and alias types under a
  substitution environment, memoized per declaration and argument list with
  C's `same_type`, and notes each enclosing stamp on an error; `extern "c"`
  function types check their parameters and return with `is_c_abi_type`.
- selfhost checks the compile-time expressions type completion reaches
  (array lengths, layout attributes, enum values, field defaults, and named
  globals' initializers) with C's `coerce`/`expr_type` subset, keeping each
  node's type, symbol, and constant in side tables that `ut_const` reads.
- selfhost resolves function signatures and overload sets in declaration
  order, binding specializations by substituted-signature conformance, and
  ports `decl_visible`/`decl_external`; both compilers' `--symbols` share
  C's new `resolve_signatures` and print each member's signature and `hidden`.
- selfhost resolves ordinary calls in compile-time expressions with C's
  `select_call_overload` (constant ranks, array-literal candidates, shrink
  matches, per-overload visibility via `ov_hidden`) and checks their
  arguments and `mut` marks, keeping C's `F_REPORTED` as a side table.
- selfhost selects specializations at generic calls `f[T](...)` (exact
  type-argument tuple, then the sole tuple match), then the template whose
  quietly built candidate signature fits (`candidate_func_type`), with C's
  not-generic, no-match, ambiguity, and missing-specialization diagnostics.
- selfhost registers signings (`register_signing`: owner, defining module,
  uniqueness, repeated type parameters, declared contracts) in its
  signatures pass; both compilers' `--symbols` now register them and print
  each struct's signing as `signing @line:col`.
- selfhost checks function bodies (`--check`): block-scoped locals,
  statements, lvalues, local-storage escapes, pointer arithmetic, ranges,
  indexing, slicing, and the small reserved builtins. `ace --check` stops
  `sema_check` before its end-of-file checks, and `check.sh` diffs the two.
- selfhost checks error construction `Domain(code?)` (`check_error_construct`):
  at most one code, coerced to `i32`, yielding `Error`; the runtime type
  identity the compiler records has no selfhost reader yet.
- selfhost checks extern variadic arguments (`check_args`): untyped
  literals default to `i64`/`f64`, each extra must be a C-variadic scalar,
  and `mut` is rejected on extras.
- selfhost checks `mut` arguments (`check_mut_args`): an addressable place,
  never a bit field; a contract-typed `mut` argument stops as unported until
  const-backed provenance arrives with contract construction.
- selfhost checks field visibility across modules (`field_visible`): a field
  is public only with its struct, a facade's `pub` alias re-exports a `pub`
  struct (kept in a side table, C's `F_REEXPORTED`), else `decl_visible`.
- selfhost checks payloadless union variant values on a plain or explicitly
  instantiated base (`union_base_type`, `union_variant_index`), marking the
  base with the union so they count as compile-time constants.
- selfhost checks calls through a function-typed variable: arguments against
  its type, no variadics, and `mut` rejected (`check_no_mut_args`); naming a
  function as a value still stops as unported.
- selfhost checks `switch` statements: `default` last, `fallthrough`
  placement, value cases with constant-overlap spans (`case_key`), and union
  cases with variant coverage and typed payload binders.
- selfhost checks method calls and calls through function-typed struct fields:
  pointer receivers auto-deref, receiver `mut` selects the overload, embedded
  fields promote exact methods, and private methods stay file-local; generic
  owners still stop as unported. `Sema.err` prints the shadowed-module note.
- selfhost checks memory and format builtins: byte-form and typed `mem`
  intrinsics, and `fmt.printf`/`str.format`/`Buffer.printf` with the format
  parsed at check time and each conversion's piece writer looked up by exact
  specialization in std's formatting module.
- selfhost checks associated calls `Type.f(...)` and union payload construction
  (`Event.key(k)`, `Pair[int].some(x)`) in C's dispatch order; method and
  associated calls share one `mut`-mismatch diagnostic. Associated functions of
  generic owners still stop as unported.
- selfhost checks function values: a bare name, an overloaded name selected
  by the wanted function type, and `f[T]` by specialization or generic
  overload; templates that need stamping still stop as generic function bodies.
- selfhost stamps generic functions at calls and as values: the signature is
  memoized per type-argument list before the body is checked once per
  instantiation, in the generic's own scope with the caller's locals set aside.
- selfhost checks method and associated calls on generic owners: candidates
  match under the owner's type arguments and the winner is stamped once per
  owner instantiation; instantiation notes name it `Owner[T].f`, as C does.
- selfhost checks type casts: `*T(x)` from a usize address or a plain-bits
  pointer without widening, `[N]T(x)` identity casts, and `[]T(x)` casts or
  `[]T(pointer, count)` construction with constant extent checks.
- selfhost checks contract construction and contract method calls: `T(&x)`
  and `T[A](&x)` find the struct's signing item, and const-backed provenance
  follows bindings, assignments, returns, and read-only contract parameters.
- selfhost checks StaticMap declarations: allowed key types, constant
  entries reduced to string or integer keys (`typeid` keys by type identity)
  with duplicate detection, and module-level-only construction.
- selfhost checks StaticMap lookups: the table must be a module-level
  binding, and the key must have the exact key type or be an untyped constant.
- selfhost checks `asm` statements: the target's mini-assembler
  instructions, its register names in every binding, and scalar local or
  parameter values for `in`/`out`.
- selfhost `--sema` runs the full check as `ace` does: after the body checks,
  specialized templates without a specialization, each signing's method set
  (direct and promoted methods, generic methods stamped per concrete owner),
  and inline call cycles; `selfhost/check.sh` diffs it in a `sema` stage.
- selfhost `--sema` checks definite initialization as C does: direct storage
  paths per local, an undo trail joining `if`, `switch`, and labeled-block
  paths, and scalar `asm` bindings recording their local; `selfhost/check.sh`
  now fails on any `sema`-stage difference.
- selfhost `--ir` lowers root-file functions to SSA IR as `ace --debug --ir`
  prints it (`selfhost/ir.ace`, `selfhost/lowering.ace`): scalar and simple
  aggregate locals, arithmetic, `if`, general `for`, labels, and calls;
  aggregate literals, aggregates in memory (results through RESULT/sret,
  by-copy and temporary arguments), array indexing, fields of values,
  destructuring, and bits fields; strings, slices, and their lengths,
  indexing, and slicing (bounds proofs recorded by selfhost sema), contract
  values and dispatch, tagged unions (construction, equality, union
  switches), range and sequence loops, `switch`, static maps, Error and
  enum descriptions, compiler-backed calls (memory, bits, and math builtins,
  `panic`, `assert`, `bitcast`, `*T(address)`, `[]T(pointer, count)`, and
  formatted output from the plan selfhost sema records), enum conversion
  and noalias checks, `defer` (a captured argument maps its node to the
  variable holding it), inline assembly, generic calls and function values
  (named by the callee's symbol; one with no matching instance fails as C's
  does), and inline expansion (selfhost sema copies the callee's nodes, side
  tables, and locals per expansion and re-stamps a generic body, as C's
  inline_copy does).
  `selfhost/check.sh` adds an `ir` stage, compared per function.
- selfhost `--ir` lowers as a release build, as `ace --ir` does (`--debug
  --ir` as a debug one): small-leaf auto-inlining, self tail calls as loops,
  then `selfhost/optimizer.ace`, a port of `src/optimizer.c` (SCCP, GVN, loop
  strength reduction, cleanup, vectorization); `selfhost/check.sh` compares IR
  in both modes.
- selfhost cleanup's branch threading rewrites every use in the threaded
  successor (the new phi it inserts no longer hides the block's last
  instruction); `selfhost/check.sh` checks `selfhost/` files too, counting a
  function as unported only by its own skip line.
- `selfhost/regalloc.ace` ports `src/regalloc.c` (layout, liveness,
  coalescing, linear scan) and `selfhost/arm64.ace` the arm64 inputs it needs
  (AAPCS64 locations, fusion, hints, register classes); checked against the
  C allocator per value on every repository file, not yet by `check.sh`.
- selfhost emits arm64 Mach-O objects with `-c` (`arm64.ace`, `emission.ace`,
  `macho.ace`: function bodies, runtime routines, DWARF, constant data,
  globals, static maps, vtables), byte-identical to `./ace -c` in debug and
  release on every repository file; `selfhost/check.sh` diffs them.
- selfhost links with `-o NAME` (#134): it writes `NAME.o`, refuses outputs
  that would overwrite an input, passes `.o`/`.a`/`-L`/`-l`/`-framework` to
  `cc` through `exec.spawn`, and reports as C's driver does. `check.sh` builds
  selfhost with itself and runs every check with that stage-2 binary.
- selfhost's `Loader.module_of` reads modules through a view instead of
  copying each `Module` (#144): `--check` on `selfhost/main.ace` runs 3.1x
  faster, `--release --ir` 2.2x.
- selfhost's wrapping arithmetic (static-map hashes, literal negation, arm64
  division magic) uses `bits.wradd`/`wrsub`/`wrmul`, so a debug build no
  longer traps on overflow; `check.sh` builds selfhost with `--debug` and has
  it compile itself in both modes.
- selfhost's sema sizes its per-node arrays to the tree in one `resize`,
  reserves the file scope for every top-level declaration, and finds a node's
  module by binary search (#144): `--check` sema setup 6.1 → 2.7 ms, body
  checks 35.1 → 32.7 ms.
- selfhost's lowering caches each static map's perfect-hash plan for the
  whole compile, as `emit.c`'s `map_plans` does, instead of searching for it
  on every lookup: `--release -c` on itself 1.06x.
