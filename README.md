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

Shape: contract {
    area(): f64
}

Circle: struct { radius: f64 }
Rect: struct { w: f64, h: f64 = 1.0 }

Circle signs Shape
Rect signs Shape

Circle.area(): f64 { return 3.14159 * self.radius * self.radius }
Rect.area(): f64 { return self.w * self.h }

total(shapes: []Shape): f64 {
    sum: f64 = 0.0
    for i: int in 0 .. shapes.len {
        sum += shapes[i].area()
    }
    return sum
}

main(): int {
    c: Circle = Circle{ radius = 2.0 }
    r: Rect = Rect{ w = 3.0, h = 4.0 }
    shapes: [2]Shape = [Shape(&c), Shape(&r)]

    fmt.printf("total area: %.2f\n", total(shapes[..]))
    return 0
}
```

```
total area: 24.57
```

Errors come back as values, and resources are released right where they are
acquired:

```ace
data: mut []u8, err: Error = file.read(PATH)
defer mem.release[u8](mut data)
if err != null {
    fmt.printf("failed to read %s: %s\n", PATH, err.message)
    return 1
}
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

The [Ace Language VS Code extension](editors/vscode/README.md) recognizes
`.ace` files and provides syntax highlighting, comment toggling, bracket
matching, auto-closing pairs, and indentation. Open this repository in VS Code
and press **F5** (**Run Ace Language Extension**) to try it; nothing needs to
be built or installed.

## Learn more

- [`docs/`](docs/README.md): the guide, starting from primitives and types
- [`SPEC.md`](SPEC.md): the language rules
- [`PHILOSOPHY.md`](PHILOSOPHY.md): why the rules are what they are
- [`GRAMMAR.md`](GRAMMAR.md): the formal grammar
- [`CHANGELOG.md`](CHANGELOG.md): implementation history
