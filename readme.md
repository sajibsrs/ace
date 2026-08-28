# Ace

Ace is a systems programming language built around explicit machine costs,
manual memory management, fast compilation, and direct native-code emission.
The design is complete; the bootstrap compiler is implemented in dependency-free
C11 and currently targets AArch64 Mach-O on Apple Silicon.

The human-facing guide starts at [`docs/README.md`](docs/README.md). Normative
language rules live in `spec.md`, their rationale in `philosophy.md`, and the
formal surface grammar in `grammar.md`. `AGENTS.md` records implementation
status and repository workflow.

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
stdlib slice. Current implementation status and remaining work are tracked in
`AGENTS.md` rather than the human manual.
