# Compiler usage

The Ace compiler checks one root source file and recursively loads its imports.

```sh
ace program.ace
ace -c program.ace
ace -o program program.ace
ace -o program.ace
ace --debug -o program program.ace
ace --release -o program program.ace
ace --ir program.ace
ace --target=darwin-arm64 program.ace
```

The default mode performs parsing, import resolution, and semantic checking
without emitting code. `-c` writes a Mach-O arm64 object beside the root source.
`-o NAME` emits an intermediate object and invokes the system C compiler driver
to link `NAME`. When `NAME` ends in `.ace`, it names the root source itself:
that file is compiled and the executable and intermediate object are named after
the file without the suffix, so `-o program.ace` links `program` and leaves
`program.o` beside it.

Emitted programs default to `--debug`. Debug output dynamically validates
sequence bounds, integer arithmetic, call-boundary slice contracts, null use,
and allocator discipline; a violation writes a concise diagnostic to standard
error and aborts. It also enables `assert`. `--release` removes those guards,
failure paths, tracking, assertion evaluation, and diagnostic data. The flags are mutually
exclusive. Check-only, `--tokens`, and `--ast` runs accept either flag, but the
choice cannot change their result because they emit no program.

`--ir` checks the program and prints the IR of each function in the root file
as the selected mode lowers it: debug marks the operations it checks with
`.chk` (integer arithmetic, pointer arithmetic, and accesses and calls through
pointer and function values), and a float constant prints as the shortest
decimal that reads back to its bits. A struct or tuple is its scalar leaves:
a parameter or result of one prints as its leaf types in braces, and a call
with one as its result prints `call {n}`, whose leaves `proj` reads. An
`Error` is a `u64`. A string or slice is two leaves, `{ptr, u64}`, and a
literal's address prints as `str` with its bytes quoted. An element of a
string or slice is addressed by `padd ptr, i * size`; in debug, each bounds
test prints as a comparison and a `check %c, line:col bounds violation`,
which aborts with that site's diagnostic when `%c` is false. An array
lives in memory: a constant index is an offset on the access, any other a
`padd` checked below the fixed length. `bitcast` reinterprets a float's bits
as an integer of its width, or back (`bits.cast`). A call to an `inline`
function prints as the expanded body, which ends at a join block reading the
result. A `switch` on a scalar prints as a chain of `ne` comparisons, one per
pattern in source order, each branching to the next test or to its case; a
case ends at the join block or, after `fallthrough`, in the next case's block.
A tagged union is memory: its construction stores the payload and then the
`i32` tag, and it crosses a call as `u64` words (a `u32` for a last half word)
up to 16 bytes, or by address above. `==` prints as a tag comparison and one
block per payload variant. A struct, tuple, or array holding a union is memory
too, and crosses a call as words the same way. A `switch` on a union loads the
`i32` tag once and prints as a chain of `ne` comparisons against the variant
indices; an arm's payload binder is a copy of the payload made where the arm
starts. A `defer` prints as nothing at its statement beyond the evaluation of its
arguments; its call appears again before every `ret`, last defer first.
A function the IR does not cover yet is listed with the first
construct that kept it out; the AST emitter compiles such functions, and the
IR's AArch64 backend compiles the rest.

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
Ace output. In a check-only, `--tokens`, `--ast`, or `--ir` run, no object is emitted,
so the machine-code and object-file values are zero.

The report uses standard error so token dumps, AST dumps, and other
machine-consumable standard output remain unchanged.

## Packages

Plain `ace file.ace` compiles one root source with no manifest. An optional
`ace.pkg` at a directory root groups a package's files under a declared
`name`, exact `version`, `ace` language version, `type` (`exe` or `lib`), and
`root` source file, with `[dependencies]` entries naming each dependency and
its exact version (`json: 0.1.0`; see `SPEC.md`'s Package System section for
the manifest format and dependency rules). Optional `description`, `author`, `license`, and `repository` fields
hold free-form single-line metadata with no effect on identity, resolution, or
compilation; any of them may be left empty. Every manifest-aware operation is
grouped under `ace pkg`:

```sh
ace pkg init exe my_app
ace pkg init lib my_lib
ace pkg build
ace pkg run
ace pkg check
ace pkg add NAME VERSION
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
`build/<package-name>`, accepting the same `--debug`/`--release`/
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

Every dependency, direct or transitive, is found from the directory of the
package being built: the first existing `pkg/NAME` in that directory or any
ancestor directory. That nearest match must be a `lib` package declaring
exactly `NAME` and the requested version; a mismatch is an error, not a
reason to keep searching, and a dependency's own `pkg/` is never read. So
sibling packages share one `pkg/` in a common ancestor, and each name has one
location per build. `pkg/` is reserved: a package's root file cannot live
under it and a relative import cannot reach into it. Nothing fetches packages
into `pkg/` yet; remote packages await a central registry.

`add NAME VERSION` and `remove NAME` edit the enclosing package's own
`[dependencies]` section: `add` validates the complete resulting graph — no
duplicate or colliding name, a matching `lib` at the nearest `pkg/NAME`, no
cycle, one shared `ace` language version — before writing, and `remove`
re-validates symmetrically; either restores the manifest to its exact
original bytes if validation fails. Neither writes or deletes anything under
`pkg/`. `list` is read-only: it prints the resolved graph (name, type,
direct/transitive, directory).

`--pkg-graph=DIR` and `--pkg-check=DIR` remain as lower-level debug hooks —
analogous to `--tokens`/`--ast` exposing earlier compiler stages — resolving
or checking the package at an explicit `DIR` rather than one found from the
working directory.
