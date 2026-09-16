---
paths:
  - "Makefile"
  - "src/**/*"
  - "std/**/*"
  - "tests/**/*"
---

# Testing

- Run the smallest relevant checks first.
- Tests are grouped under `tests/`: lexer/parser tests parse; sema tests check
  diagnostics; emit and module tests compile/link/run; stdlib-module tests use a
  synthetic std tree; emit-errors fail during `-c`; driver tests exercise
  external linking; package tests resolve an `ace.pkg` dependency graph via
  `--pkg-graph=DIR` and check cross-package imports via `--pkg-check=DIR` —
  one directory per case, root manifest at the case root (or `<case>/app`
  when a sibling dependency package is needed), a `// expect-ok:`/
  `// expect-err:`/`// expect-check-ok`/`// expect-check-err:` comment naming
  the expected outcome. `tests/package_cli/` exercises the real `ace pkg`
  subcommands (`init`/`build`/`run`/`check`) rather than the debug flags —
  same one-directory-per-case/`<case>/app`+`<case>/lib` layout, with
  `// expect-build-exit: N` (build an app, run it both via the linked binary
  and `pkg run`), `// expect-build-ok` (build a lib: succeeds, no linked
  binary), `// expect-run-err: substring` (`pkg run` on a lib must fail with
  that diagnostic), and `// expect-check-ok` (`pkg check` succeeds from a
  nested working directory, proving the walk-up-to-nearest-`ace.pkg`
  discovery).
- Sandboxes may deny network and loopback sockets, causing `net`, `poll`, `tcp`,
  and HTTP fixtures to fail. Confirm environmental restrictions before treating
  those failures as regressions.
