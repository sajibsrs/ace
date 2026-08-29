# Compiler usage

The Ace compiler checks one root source file and recursively loads its imports.

```sh
ace program.ace
ace -c program.ace
ace -o program program.ace
```

The default mode performs parsing, import resolution, and semantic checking
without emitting code. `-c` writes a Mach-O arm64 object beside the root source.
`-o NAME` emits an intermediate object and invokes the system C compiler driver
to link `NAME`.

## Compile report

Pass `--report` to print a compact report to standard error after a successful
run:

```text
Ace compile report:
--------------------
source files:    3
source bytes:    8,412
tokens:          2,106
machine code:    3,584 bytes
object file:     7,936 bytes
compile time:    2.14 ms
```

Source files, bytes, and tokens include the root and every uniquely loaded
import. The token count excludes the end-of-file marker and includes significant
newline tokens. Machine code is the emitted `__text` section, while object file
is the complete Mach-O object including data, symbols, relocations, and debug
sections.

Compile time covers source loading through object emission. It excludes the
external linker invoked by `-o`, keeping the measurement about Ace itself. In a
check-only, `--tokens`, or `--ast` run, no object is emitted, so the machine-code
and object-file values are zero.

The report uses standard error so token dumps, AST dumps, and other
machine-consumable standard output remain unchanged.
