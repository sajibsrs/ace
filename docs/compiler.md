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
without emitting code. `-c` writes the target's object beside the root source: Mach-O for
`darwin-arm64`, ELF for `linux-x64`.
`-o NAME` emits an intermediate object and invokes the system C compiler driver
to link `NAME`. When `NAME` ends in `.ace`, it names the root source itself:
that file is compiled and the executable and intermediate object are named after
the file without the suffix, so `-o program.ace` links `program` and leaves
`program.o` beside it.

Emitted programs default to `--release`; `ace pkg build` and `ace pkg run`
follow the same default. `--debug` output dynamically validates
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
`padd` checked below the fixed length. A contract value is two leaves,
`{ptr, ptr}` (vtable, data): construction prints its vtable as a `global` and
the argument's address; dispatch loads the method slot from the vtable leaf
(a checked load in debug) and calls it with the data leaf as receiver; a
construction over a generic concrete type emits its pair's vtable with the
constructing function. A `bits` value is its backing integer: a field read
prints as a `shr` and `and`, or for a signed field a `shl`, a `cast` to the
signed type, and a `shr`; a field write or literal field merges the value with
`and`, `shl`, and `or`, after a debug range test calling the arithmetic
failure routine, and a field write loads and stores the whole value once.
`bitcast` reinterprets a float's bits
as an integer of its width, or back (`bitcast`). A call to an `inline`
function prints as the expanded body, which ends at a join block reading the
result. A `switch` on a scalar prints as a chain of `ne` comparisons, one per
pattern in source order, each branching to the next test or to its case; a
case ends at the join block or, after `fallthrough`, in the next case's block.
A `for` over a string, slice, or array prints as a loop over a pointer: the
end is `padd ptr, len * size` computed once, the head compares the pointer with
it, and each step is `padd ptr, 1 * size`, with a `u64` counter alongside when
the loop has an index binder. A value binder is a copy of the element loaded at
the pointer; under `in mut` it is the pointer itself, so its reads and writes
are loads and stores. The body has no bounds checks. A tagged union is memory: its construction stores the payload and then the
`i32` tag, and it crosses a call as `u64` words (a `u32` for a last half word)
up to 16 bytes, or by address above. `==` prints as a tag comparison and one
block per payload variant. A struct, tuple, or array holding a union is memory
too, and crosses a call as words the same way. A `switch` on a union loads the
`i32` tag once and prints as a chain of `ne` comparisons against the variant
indices; an arm's payload binder is a copy of the payload made where the arm
starts. `continue` on a labeled block jumps back to its first block, so the label
prints as a loop head whose back edges come from inside. A computed global
constant prints as a `load` of its storage, `typeid(T)` as a `u64` constant,
and an Error's `name` or `message` as a chain of comparisons of the packed
value with each declared domain (name) or entry (message), each matching one
loading its string; nothing matching leaves the empty string. `panic(message)`
prints as a `write` of the message to descriptor 2 and a call of `abort`. In
debug, a call of a function with a `noalias` parameter prints the range
checks before the call: per slice argument a representability check (`check`
of the length's bound and of the end not wrapping), then per pair with a
`noalias` member a guarded `check` that the ranges are disjoint. A `defer` prints as nothing at its statement beyond the evaluation of its
arguments; its call appears again before every `ret`, last defer first.
In release, the IR is then optimized before it prints: a value that is constant
on every path becomes a `const`, a pure operation equal to one that dominates it
(or an identity such as `x + 0`) is replaced by that value, an offset into an
array whose element size is not a power of two, indexed by a loop counter,
becomes a pointer `phi` started before the loop and stepped at its end, a branch on a constant becomes a `jmp` and its
untaken arm disappears, and blocks left empty, unreachable, or with a single
predecessor are removed or merged. Debug prints the lowered IR unchanged.
A function the IR does not cover is listed with the first
construct that kept it out, and compiling it is an error naming that
construct. An `asm` statement prints as one instruction, its
bindings' registers beside the values moved into them, the registers it
reads out after `->`, and every register it reserves as a `clobbers` mask;
each value it writes out is a `proj` of it. A binding of a local or parameter
kept in memory (address-taken, or a `mut` parameter) prints as a `load` before
the instruction and a `store` after it.

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
newline tokens. Machine code is the emitted text section (Mach-O `__text`, ELF
`.text`), while object file is the complete object including data, symbols,
relocations, and debug sections.

Compile time covers source loading through object emission. It excludes the
external linker invoked by `-o`, keeping the measurement about Ace itself. The
object-file size counts only Ace's own object; external `.o`/`.a` inputs are not
Ace output. In a check-only, `--tokens`, `--ast`, or `--ir` run, no object is emitted,
so the machine-code and object-file values are zero.

The report uses standard error so token dumps, AST dumps, and other
machine-consumable standard output remain unchanged.

## Packages

Manifest-aware building (`ace.pkg`, dependencies, native link inputs, and the
`ace pkg` commands) is described in [Packages](package.md).

## Design rationale

Why the rules in `spec/compiler.md` are shaped as they are.

- Compiler #2, reachable functions only: code generation was over half of
  compile time, most of it for imported functions a program never calls.
  Following the references compiled code already makes finds every use —
  calls, function values, vtables, `defer`, compiler-selected writers —
  without a separate list of roots, and costs no linker support. Release
  `bench/core` and `bench/std` compile 1.3–1.9x faster with 1.7–6.7x less
  machine code; the `json` and `regex` packages link 31–37% smaller.

- Compiler #4 (PHILOSOPHY.md Cautions #1): emission straight from the AST needs
  every optimization written once per source shape and repeats each in a second
  backend; the compile-time cost ranks below performance, simplicity, and
  modularity.
- Compiler #6 (PHILOSOPHY.md: performance first): the target's vector unit is a
  platform feature ordinary loops should benefit from without rewriting, as
  clang's loop vectorizer does for C; measured on `bench/core`, it takes
  `cube-transform` from 61.0 to 41.4 ns per cube (1.47x) and `saxpy-f32` from
  0.507 to 0.131 ns per element (3.9x).
