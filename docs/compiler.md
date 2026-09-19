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

## Packages

Plain `ace file.ace` compiles one root source with no manifest. An optional
`ace.pkg` at a directory root groups a package's files under a declared
`name`, exact `version`, `ace` language version, `type` (`exe` or `lib`), and
`root` source file, with local-path-only `[dependencies]` entries (see
`SPEC.md`'s Package System section for the manifest format and dependency
rules). Optional `description`, `author`, `license`, and `repository` fields
hold free-form single-line metadata with no effect on identity, resolution, or
compilation; any of them may be left empty. Every manifest-aware operation is
grouped under `ace pkg`:

```sh
ace pkg init exe my_app
ace pkg init lib my_lib
ace pkg build
ace pkg run
ace pkg check
ace pkg add NAME PATH
ace pkg remove NAME
ace pkg list
```

`build`, `run`, and `check` take no path argument: they find the nearest
enclosing `ace.pkg` by walking up from the working directory, the same way a
nested manifest starts an independent package. `check` performs a full
semantic check with cross-package imports resolved, no codegen — the
manifest-aware counterpart to plain `ace file.ace`'s default mode. `build`
resolves the dependency graph and fully checks the root file; for a `lib`
package that is the entire result, plus ensuring `build/` exists, since v1
ships libraries as source with no reusable binary archive. For an `exe`
package, `build` additionally compiles and links into
`build/<package-name>`, accepting the same `--checked`/`--release`/
`--target=`/`--report` flags as plain compilation. `run` is
exe-only — it errors on a `lib` package — and otherwise builds, then
executes the linked binary in place, propagating its exact exit code.

`init <exe|lib> [DIR]` creates a minimal package skeleton at `DIR` (default:
the working directory), deriving the package name from `DIR`'s basename. It
refuses to overwrite an existing `ace.pkg` or root file. An exe gets
`src/main.ace` with a minimal `main(): int { return 0 }`; a lib gets an
empty `src/mod.ace`, since a library's root module needs no content to be a
valid sole import surface. The generated `ace.pkg` includes empty
`description`, `author`, `license`, and `repository` fields ready to fill in.

`add NAME PATH` and `remove NAME` edit the enclosing package's own
`[dependencies]` section for a local-path entry (`PATH` relative to that
`ace.pkg`, same convention as an existing entry): `add` validates the
complete resulting graph — no duplicate or colliding name, `PATH` holding a
`lib` package whose declared `name` exactly matches `NAME`, no cycle, one
shared `ace` language version — before writing, and `remove` re-validates
symmetrically; either restores the manifest to its exact original bytes if
validation fails. `list` is read-only: it prints the resolved graph (name,
type, direct/transitive, directory).

`--pkg-graph=DIR` and `--pkg-check=DIR` remain as lower-level debug hooks —
analogous to `--tokens`/`--ast` exposing earlier compiler stages — resolving
or checking the package at an explicit `DIR` rather than one found from the
working directory.
