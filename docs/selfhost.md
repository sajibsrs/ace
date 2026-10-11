# Selfhost

`selfhost/` is the Ace compiler written in Ace, beside the C bootstrap
compiler in `src/`. It is ported stage by stage from the C compiler, and a
stage counts as done when everything it produces appears in a dump that
matches the C compiler's exactly. This page is merged into the compiler
documentation once selfhost is complete.

## Stages

| Stage | File | Dump | C reference |
|---|---|---|---|
| Lex | `lexer.ace` | (no flag) tokens, as `ace --tokens` | `src/lexer.c` |
| Parse | `parser.ace` | `--ast` | `src/parser.c` |
| Load modules | `module.ace`, `path.ace` | `--modules`, compared with `ace --report` counts | `src/module.c` |
| Merge | `module.ace` | `--resolved` | `src/module.c` |
| Packages | `package.ace` | `--pkg-graph=DIR`, `--pkg-check=DIR` | `src/package.c` |
| Sema: top-level entry | `sema.ace` | `--symbols` | `src/sema.c` (`enter_top_level`) |

`--symbols` prints the scopes after every top-level name is entered
(Compiler #5) and before anything resolves: the file scope's types, then its
values, then the root's whole-module imports. Each line names the symbol, its
kind (a type's nominal kind, or `alias` for an alias not yet expanded), its
flags, and its declaration's position; a function lists each overload as a
`member` and each specialization still waiting to bind as a `spec`. On an
entry error, both compilers print only the errors.

## Checking

`selfhost/check.sh` builds selfhost with `./ace` and diffs every stage
against the C compiler over every repository `.ace` file and `ace.pkg`,
printing one agreement count per stage. It is not part of `make test`.

## Design rationale

Comparing dumps checks each stage on its own, before any later stage exists
to depend on it, and the repository's own sources and fixtures are the
corpus. A function is ported when something calls it; until then only the
data it reads is ported and dumped, so nothing goes untested.
