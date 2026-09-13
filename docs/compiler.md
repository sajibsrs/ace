# Compiler usage

The Ace compiler checks one root source file and recursively loads its imports.

```sh
ace program.ace
ace -c program.ace
ace -o program program.ace
ace -o program.ace
ace --checked -o program program.ace
ace --release -o program program.ace
ace --target=darwin-arm64 program.ace
ace --tls-backend=bearssl -o program program.ace
```

The default mode performs parsing, import resolution, and semantic checking
without emitting code. `-c` writes a Mach-O arm64 object beside the root source.
`-o NAME` emits an intermediate object and invokes the system C compiler driver
to link `NAME`. When `NAME` ends in `.ace`, it names the root source itself:
that file is compiled and the executable and intermediate object are named after
the file without the suffix, so `-o program.ace` links `program` and leaves
`program.o` beside it.

Emitted programs default to `--checked`. Checked output dynamically validates
sequence bounds, integer arithmetic, call-boundary slice contracts, null use,
and allocator discipline; a violation writes a concise diagnostic to standard
error and aborts. It also enables `assert`. `--release` removes those guards,
failure paths, tracking, assertion evaluation, and diagnostic data. The flags are mutually
exclusive. Check-only, `--tokens`, and `--ast` runs accept either flag, but the
choice cannot change their result because they emit no program.

The build target defaults from the compiler host and can be selected explicitly
with `--target=NAME` (or `--target NAME`). The bootstrap compiler currently
supports only `darwin-arm64`; any other target is rejected before compilation.

Functions in the compiler-shipped standard library may carry the closed
toolchain-owned `@extern("tls")` component marker. Importing such a function is
free: Ace omits every TLS-component body unless an ordinary emitted function
actually references a marked entry point. For a required component, Ace maps
the component, target, and selected backend to an ordered link recipe. A recipe
may contain multiple objects, archives, libraries, and frameworks. Darwin-arm64
BearSSL artifacts live under `external/darwin-arm64/tls/bearssl/`; its recipe
also links Security and CoreFoundation. BearSSL is selected by default; the
explicit equivalent is `--tls-backend=bearssl`. The mechanism never downloads
or runs dependency scripts, and public `http`/`tls` APIs do not expose the
backend name.

The initial object format does not record external-component requirements. For
that reason `-c` explicitly rejects a source unit that uses TLS and directs the
caller to `-o`; plaintext-only objects, including programs that import and use
`http`, remain valid and contain no TLS adapter or backend references.

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
