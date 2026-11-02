# Language server

`lsp/` is `ace-lsp`, the Ace language server, written in Ace over the
selfhost front-end. It speaks the Language Server Protocol on stdin and
stdout and writes diagnostics to stderr. Build it with:

```
./ace -o lsp/ace-lsp lsp/main.ace
```

The VS Code extension bundles it with a copy of `std/` beside it, where the
loader looks, so installing the extension is enough; definitions in `std`
open that copy.

It currently answers:

- `textDocument/documentSymbol`: the outline. Structs, bits, unions, enums,
  errors, and contracts list their members as children; methods and
  associated functions appear as `Owner.name`; each name of a `const` or
  variable declaration, aliases, distinct types, and `extern` block members
  appear on their own.

- `textDocument/definition`: go to definition. The server loads and checks
  the program holding the file with the selfhost loader and sema (inside the
  nearest `ace.pkg`'s package when there is one) and follows the name under
  the cursor: a local, global, or function (the overload a call resolved
  to), a type, a field, enum member, or error entry, or an import's module.
  It answers nothing while the file, or the file it would jump to, has
  unsaved changes, since positions come from disk.

Documents are synchronized in full on every change. A file with a syntax
error has an empty outline, since the parser keeps no declarations then.

`lsp/` imports `../selfhost/` and `../pkg/json/` by path and has no `ace.pkg`,
since a package cannot import files outside its own directory.

## Design rationale

The server reuses the selfhost front-end rather than parsing Ace itself, as
`gopls` builds on Go's own `go/parser` and `go/types`: one implementation,
so a language change reaches the editor with no second update. A separate
executable is needed while `ace` is the C compiler; once selfhost ships as
`ace`, it can become an `ace lsp` subcommand, as `deno lsp` and `gleam lsp`
are. The outline needs only the parser, so it never waits on sema.
