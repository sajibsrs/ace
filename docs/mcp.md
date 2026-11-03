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
tools, each taking `file`, a 1-based `line`, and a 1-based byte `column`:

- `definition`: where the name at that position is declared, as
  `file:line:column`, through the same selfhost sema as `ace-lsp`'s go to
  definition.
- `hover`: that declaration's source line and doc comment, as Markdown, the
  text `ace-lsp`'s hover shows.

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
