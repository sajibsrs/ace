# Tooling

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
