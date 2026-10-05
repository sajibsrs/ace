# Ace

**A small systems language that shows you what your code costs.**

Ace compiles straight to native machine code with no runtime, no garbage
collector, and no hidden allocations. Memory is managed by hand, cleanup is
spelled with `defer`, errors are ordinary return values, and every declaration
states its type. What you read is what the machine does.

The compiler is written in dependency-free C11 and emits native objects
directly, with no assembler or LLVM in between.

> **Note:** Ace currently supports only macOS on Apple Silicon (AArch64).

## A taste of Ace

```ace
import fmt

Point: struct {
    x: int,
    y: int,
}

Point.sum(self): int {
    return self.x + self.y
}

main(): int {
    points: [3]Point = [
        Point{ x = 1, y = 2 },
        Point{ x = 3, y = 4 },
        Point{ x = 5, y = 6 },
    ]

    total: int = 0
    for i: int in 0 .. points.len {
        total += points[i].sum()
    }

    fmt.printf("total: %d\n", total)
    return 0
}
```

```
total: 21
```

## Why Ace

- **Explicit costs.** No implicit conversions, no hidden copies or allocations,
  no exceptions. Casts are constructor-shaped: `i32(x)`, `Meters(x)`.
- **Contracts, not inheritance.** A contract is a required method set; a struct
  signs the contracts it fulfills once, and dispatch goes through a const
  vtable. There is no trait solving and no runtime lookup.
- **Visible mutation.** `self` is read-only by default; a mutating call says so
  at the call site: `mut doc.parse(data)`.
- **Simple to parse, fast to compile.** Keywordless `name: type` declarations
  and a recursive-descent parser with bounded lookahead.
- **Talks to C.** Link against C libraries directly; `pkg/sdl` wraps SDL3.

## What's included

| Path | Contents |
| --- | --- |
| `std/` | Standard library: `fmt`, `mem`, `str`, `utf8`, `map`, `array`, `file`, `math`, `time`, `net`, `poll`, `sys` |
| `pkg/` | First-party packages: `json`, `regex`, `http`, `sdl` |
| `demo/` | Runnable programs: HTTP client and server, JSON and regex tools, a packet sniffer, an SDL3 rotating cube |
| `editors/vscode/` | VS Code extension with syntax highlighting |

## Build

Requires a C11 compiler.

```sh
make
```

This produces `./ace`.

## Compile and run

```sh
./ace -o hello hello.ace   # compile, link, and produce an executable
./hello
```

Other modes:

```sh
./ace file.ace             # typecheck only
./ace -c file.ace          # emit a Mach-O arm64 object
./ace --tokens file.ace    # dump tokens
./ace --ast file.ace       # dump the syntax tree
./ace --ir file.ace        # dump the intermediate representation
./ace --report file.ace    # show compile statistics
```

Generated binaries carry DWARF line information, so LLDB can step through Ace
source.

## Test

```sh
make test
```

## Editor support

The [Ace VS Code extension](editors/vscode/README.md) recognizes
`.ace` files and provides syntax highlighting, comment toggling, bracket
matching, auto-closing pairs, and indentation. Open this repository in VS Code
and press **F5** (**Run Ace Extension**) to try it; nothing needs to
be built or installed.

## Learn more

- [`docs/`](docs/README.md): the guide, starting from primitives and types
- [`spec/`](spec/README.md): the language rules
- [`PHILOSOPHY.md`](PHILOSOPHY.md): why the rules are what they are
- [`GRAMMAR.md`](GRAMMAR.md): the formal grammar
- [`changelog/`](changelog/README.md): implementation history
