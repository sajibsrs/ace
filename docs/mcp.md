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
  order. Each is shown as `file:line` and then its hover text. The module
  is `std/<module>.ace` or `std/<module>/mod.ace`. A directory module's files
  are searched too, including its `inc/` and current-target `os/` files.

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
