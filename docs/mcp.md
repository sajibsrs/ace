# Agent server

`mcp/` is `ace-mcp`, a Model Context Protocol server that gives coding agents
the lookups `ace-lsp` gives editors. It reads one JSON-RPC message per line on
stdin, writes one per line on stdout, and writes diagnostics to stderr. Build
it at the repository root, beside `std/`, where its loader looks:

```
./ace -o ace-mcp mcp/main.ace
```

`.mcp.json` registers it as `./ace-mcp`, so an agent started at the
repository root picks it up. `mcp/check.sh` builds it and checks a scripted
session; it is not part of `make test`.

It answers `initialize`, `ping`, `tools/list`, and `tools/call` with these
tools. The first two take `file`, a 1-based `line`, and a 1-based byte
`column`:

- `definition`: where the name at that position is declared, as
  `file:line:column`, through the same selfhost sema as `ace-lsp`'s go to
  definition.
- `hover`: that declaration's source line and doc comment, as Markdown, the
  text `ace-lsp`'s hover shows.
- `diagnostics` (`file`): every error `ace` reports for the program holding
  the file (lex, syntax, import, and sema errors), one
  `path:line:column: message` per line with the line and byte column 1-based
  (0 for an error with no position), or `no errors`. Notes such as
  "instantiated here" are left out.
- `std_lookup` (`name`): every public declaration in std that a qualified
  name names, `module.name` (`str.format`, `mem.copy`) or
  `module.Type.method` (`fmt.Buffer.printf`), all overloads in declaration
  order. Each is shown as `file:line` and then its hover text. A bare
  `module` (`str`) lists every public declaration instead, one
  `file:line: signature` line each, the signature cut before a body's `{`.
  Paths are relative to the repository. The module
  is `std/<module>.ace` or `std/<module>/mod.ace`. A directory module's files
  are searched too, including its `inc/` and current-target `os/` files.
- `spec_section` (`ref`): the spec item a reference such as `Memory #15`
  names, or with a bare section (`Naming`), the heading's `file:line` and the
  whole section. The section is found through the table in `spec/README.md`, then
  item `N.` under that section's `## ` heading. The result is the item's
  `spec/` `file:line`, an `[open]` or `[controversial]` marker when its text
  has one, the item text, and then each `spec/queue.md` and `changelog/` line
  citing the same reference (`Memory #15`, not `Memory #150`).
- `references` (`file`, `line`, `column`): every use of the declaration
  named there, or of the declaration itself when the cursor is on it, in the
  program rooted at that file, as `path:line:column` lines sorted by place.
  A use counts when sema resolves it to that declaration, so overloads,
  same-named methods, and shadowing locals stay apart. Uses in programs that
  import the file but are not reached from it are not listed; `examples`
  covers those.
- `examples` (`file`, `line`, `column`): up to 10 uses of that declaration
  from `tests/`, `demo/`, `std/`, and `selfhost/`, in that order and by path
  within each, as `path:line:column: source line`. Each file holding the
  name's text is checked as the root of its own program and contributes the
  uses in that file that resolve to the declaration.

A position with no declaration, or a program that does not load, gives a
result with `isError` set rather than a protocol error.

## Design rationale

Agents have no prior training data on Ace, so semantic lookup replaces
recall: a name resolves through sema rather than a text search. The server
reuses `lsp/definition.ace` and `lsp/hover.ace` and differs from `ace-lsp`
only in framing and request shape, one analysis for both clients as
`gopls` and `rust-analyzer` serve every editor from one core. Positions are
Ace's own 1-based byte columns, as `ace` diagnostics print them, rather than
LSP's UTF-16 units, so a compiler error's location passes straight in. Files
are read from disk: an agent has no unsaved editor buffers. Every result is
derived on each call; nothing is indexed or stored.

The selfhost loader and sema keep each error they print as a record
(`module.Diagnostic`), so `diagnostics` reads the same errors `ace` prints
without running a second process or parsing stderr. `std_lookup` loads only
the named module, so it needs no program around the name, and it takes the
module from the name because a type name alone does not say which module
declares it.

`spec_section` matches citations textually: a range such as `Memory #15–16`
cites #15 but not #16. It finds the repository from the located `std/`, as
the compiler does, so it does not depend on the working directory.

`references` and `examples` share one walk (`lsp/references.ace`): every
named node of the checked program whose resolved declaration, by the same
rules as go to definition, is the one asked for. Resolution through sema
rather than text is what makes a short name such as `copy` or `node`
usable. `examples` finds a declaration again in each program by its file,
line, column, and node kind, since node ids differ per program; the name's
text only prefilters files. Checking every candidate costs about 1.7 s when
the name appears almost everywhere and nothing uses it, and well under a
second otherwise, since the search stops at 10 uses. The selfhost loader
and sema still print each checked program's errors on stderr.
