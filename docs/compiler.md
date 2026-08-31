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

## Linking external code

With `-o`, additional inputs and link options may be given beside the root
source. They are passed to `cc` as separate arguments — never through a
shell — in exactly the order written, after Ace's own object:

```sh
ace app.ace helper.o -L /opt/local/lib -l helper -framework CoreFoundation -o app
```

- `file.o`, `file.a` — additional object and archive inputs
- `-L DIR` — linker library search path
- `-l NAME` — linker named library
- `-framework NAME` — Apple framework

Exactly one `.ace` root source may be compiled. Link inputs and link options
are rejected with `-c` and require `-o`. Filenames and paths containing spaces
or shell metacharacters are safe: nothing interprets them. A failed link
reports `ace: linker exited with status N` (or the terminating signal) after
the linker's own diagnostics, and `ace` exits nonzero. Ace rejects an output or
generated intermediate-object path that would overwrite the root source or an
extra object/archive input, including an existing symlink or hard-link alias.
There is no raw linker escape such as `-Wl,...`; if a future need is proven, one
will be added explicitly rather than through arbitrary flags.

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
external linker invoked by `-o`, keeping the measurement about Ace itself. The
object-file size counts only Ace's own object; external `.o`/`.a` inputs are not
Ace output. In a check-only, `--tokens`, or `--ast` run, no object is emitted,
so the machine-code and object-file values are zero.

The report uses standard error so token dumps, AST dumps, and other
machine-consumable standard output remain unchanged.
