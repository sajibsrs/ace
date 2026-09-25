---
paths:
  - "Makefile"
  - "src/**/*"
  - "std/**/*"
  - "pkg/**/*"
  - "tests/**/*"
---

# Testing

- Run the smallest relevant checks first; run the full suites only after
  the owner approves and reviews the change (see `AGENTS.md` Process).
- Tests are grouped under `tests/`: lexer/parser tests parse; sema tests check
  diagnostics; emit and module tests compile/link/run; stdlib-module tests use a
  synthetic std tree; emit-errors fail during `-c`; driver tests exercise
  external linking; package tests resolve an `ace.pkg` dependency graph via
  `--pkg-graph=DIR` and check cross-package imports via `--pkg-check=DIR` —
  one directory per case, root manifest at the case root (or `<case>/app`),
  dependencies under `<case>/pkg/<name>`, a `// expect-ok:`/
  `// expect-err:`/`// expect-check-ok`/`// expect-check-err:` comment naming
  the expected outcome. `tests/package_cli/` exercises the real `ace pkg`
  subcommands (`init`/`build`/`run`/`check`) rather than the debug flags —
  same one-directory-per-case/`<case>/app`+`<case>/pkg/<name>` layout, with
  `// expect-build-exit: N` (build an app, run it both via the linked binary
  and `pkg run`), `// expect-build-ok` (build a lib: succeeds, no linked
  binary), `// expect-run-err: substring` (`pkg run` on a lib must fail with
  that diagnostic), and `// expect-check-ok` (`pkg check` succeeds from a
  nested working directory, proving the walk-up-to-nearest-`ace.pkg`
  discovery).
- A change to definite initialization (`sema.c`) also runs
  `tests/definite-init-diff.sh <HEAD-ace> ./ace [count] [seed]`: random
  functions under every branch form must get byte-identical diagnostics from
  a build of `HEAD` and the change. It is not part of `make test`.
- Sandboxes may deny network and loopback sockets, causing `net`, `poll`, `tcp`,
  and HTTP fixtures to fail. Confirm environmental restrictions before treating
  those failures as regressions.
- A first-party package under `pkg/<name>/` owns its own behavioral tests
  under `pkg/<name>/tests/`, run by `make test-packages`
  (`pkg/<name>/tests/run.sh`), not `make test`/`tests/run.sh`: package content
  doesn't gate core compiler iteration. Same one-directory-per-case layout as
  `tests/package_cli/`, each case an `app` package depending on the package
  by version, found as the repository's `pkg/<name>`, with
  `// expect-exit: N`/`// expect-check-ok` markers as `ace.pkg` comments. Run `make test-packages` when touching a
  `pkg/` package; it is not covered by `make test`.
