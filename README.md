# Ace

Ace is a systems programming language built around explicit machine costs,
manual memory management, fast compilation, and direct native-code emission.
The design is complete; the bootstrap compiler is implemented in dependency-free
C11 and currently targets AArch64 Mach-O on Apple Silicon.

The human-facing guide starts at [`docs/README.md`](docs/README.md). Normative
language rules and decision records live in `SPEC.md`, their rationale in
`PHILOSOPHY.md`, and the formal surface grammar in `GRAMMAR.md`. `AGENTS.md`
records repository workflow and indexes `SPEC.md`; implementation history is
kept in `CHANGELOG.md`.

## Build and test

```sh
make
make test
```

The build produces `./ace`. Useful modes are:

```sh
./ace --tokens file.ace  # lex and dump tokens
./ace --ast file.ace     # parse and dump the AST
./ace file.ace           # typecheck
./ace -c file.ace        # emit a Mach-O arm64 object
./ace -o program file.ace # emit, link with cc, and produce an executable
```

String and platform-integer output are available:

```ace
import fmt

main(): int {
    fmt.print("Welcome Ace!\n")
    fmt.print(13)
    return 0
}
```

The lexer, parser, semantic checker, and core-language emitter are operational,
including minimal DWARF source-line information for LLDB. Bare `import fmt` and
allocation-free `fmt.print(string)` / `fmt.print(int)` form the first working
stdlib slice. Current implementation status is summarized in `AGENTS.md`, with
chunk-by-chunk history in `CHANGELOG.md`, rather than the human manual.

## Editor support

The repository includes the dependency-free
[Ace Language VS Code extension](editors/vscode/README.md). It recognizes
`.ace` files, supplies theme-compatible TextMate syntax scopes, and configures
comments, brackets, automatic closing pairs, and indentation. Open the
repository in Visual Studio Code and run **Run Ace Language Extension** with
**F5** to launch an Extension Development Host; no build or installation step
is required.
