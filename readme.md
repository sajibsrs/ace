# Ace

Ace is a systems programming language built around explicit machine costs,
manual memory management, fast compilation, and direct native-code emission.
The design is complete; the bootstrap compiler is implemented in dependency-free
C11 and currently targets AArch64 Mach-O on Apple Silicon.

The language rules live in `spec.md`, their rationale in `philosophy.md`, and the
formal surface grammar in `grammar.md`. `AGENTS.md` records the current
implementation status and repository workflow.

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

The lexer, parser, semantic checker, and core-language emitter are operational.
Minimal DWARF line tables, checked-build runtime diagnostics, imports, and the
standard library are the next major implementation milestones.
