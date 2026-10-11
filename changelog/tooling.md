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
