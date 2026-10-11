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
| Sema: type completion | `sema.ace` | `--symbols` | `src/sema.c` (`complete_types`) |

`--symbols` prints the scopes once every top-level name is entered
(Compiler #5), every type is complete, and every function signature is
resolved (signings not yet): the file scope's types, then its values, then
the root's whole-module imports. Each line names the symbol, its kind (a
type's kind; an alias shows its target's), its flags (`entered` until
something resolves it), and its declaration's position; a function lists each
overload, bound specializations included, as a `member`, flagged `hidden`
when the root file cannot see it (Modules #2) and followed by its signature
unless generic, and each specialization still waiting to bind as a `spec`. Below a completed type come its `size` and `align` (with `packed`,
`overlap`, and an explicit `@align`), an enum's, bits type's, or distinct
type's `base`, and then its parts: a struct `field` with type, size,
effective alignment, and offset; a bits `field` with bit offset and width; a
union `variant` with its payload's size and alignment; an enum `member` or an
error `entry` with its value; or an alias's `= target`. A generic type has no
layout of its own; an instance a field names shows as its declaration's name
(`Box`) with the instance's size, since instances live in no scope. On an
error, both compilers print only the errors.

Type completion stamps generic types as C does: one memoized instance per
declaration and argument list, its errors followed by an `instantiated here`
note per enclosing stamp. Its compile-time expressions (array lengths,
`@align` and `@offset`, enum values, error codes, field defaults, and the
initializers of the globals they name) go through the compile-time subset of
C's expression checker: literals, names, operators, member access, struct
and array literals, casts, `blank`, `sizeof`, `alignof`, and `offsetof`.
Selfhost stops with `does not yet support` on anything else there, such as a
call to a function, which needs overload resolution.

## Checking

`selfhost/check.sh` builds selfhost with `./ace` and diffs every stage
against the C compiler over every repository `.ace` file and `ace.pkg`,
printing one agreement count per stage. It is not part of `make test`.

## Design rationale

Comparing dumps checks each stage on its own, before any later stage exists
to depend on it, and the repository's own sources and fixtures are the
corpus. A function is ported when something calls it; until then only the
data it reads is ported and dumped, so nothing goes untested.
