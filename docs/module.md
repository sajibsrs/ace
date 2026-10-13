# Modules

A module is either one `.ace` file or a directory rooted at `mod.ace`. A file
states its imports up front, before any declaration, and members are private
unless marked `pub` on the declaration line. Import a module binding for
qualified access, or explicitly select public members into their ordinary
type/value namespaces.

```ace
import fmt
import twice from "./util.ace"

main(): int {
    fmt.print(twice(21)) // prints 42
    return 0
}
```

## Import forms

```ebnf
import ident                      // bare: standard library only
import ident/ident                // bare, nested: standard library only
import ident: ident               // aliased: standard library only
import ident: ident/ident         // aliased, nested: standard library only
import ident: "relative/path.ace" // local file, relative to the importing file
import ident: "relative/path"     // local nested module, relative to the importing file
import one, two from ident        // selected public members from std
import one from "relative/path.ace"
```

- A **bare** name (`import fmt`, `import math`) resolves only inside the
  standard library directory `std/` that ships beside the compiler. A local
  file can never shadow a standard module. A single-file std module is
  `std/<name>.ace`; a directory std module is `std/<name>/mod.ace`. The entry
  and every recursively contained implementation file form one module, and
  `mod.ace` is its sole external import surface. Defining both forms for one
  std name is an ambiguity error. A missing-module diagnostic names both paths.
- The **alias** form binds a different local name to a standard module
  (`import f: fmt`); the module is still the standard one.
- The **path** form loads a local file. The path must be relative (no leading
  `/`) and must end in `.ace`; it resolves against the directory of the file
  containing the import, so a module's own imports travel with it.
- The **selective** form imports named public members without a module
  qualifier. Its source is either a bare standard-module name or a relative
  local path. `from` is contextual: outside this position it is an ordinary
  identifier, so `from: usize = 0` is valid.

A subdirectory with its own `mod.ace` is itself a **nested module**,
addressable by a `/`-separated path from any enclosing directory:

```ace
import net/tcp
import bla: net/tcp
import "../net/tcp"       // quoted relative, no .ace suffix
```

Every non-leaf segment must itself be a directory module (`mod.ace`); a
subdirectory without one stays private, swallowed by its nearest enclosing
module boundary exactly like any other implementation file. Only the **leaf**
segment's identifier becomes the bound qualifier — `import net/tcp` brings
`tcp` into scope, not `net` — mirroring single-segment `import tcp` binding
`tcp`. There is no transitivity in either direction: importing `net/tcp`
exposes nothing through `net`, and importing `net` does not expose `tcp`
unless `net`'s own `mod.ace` explicitly re-exports it.

The compiler reserves its shipped `std/@/` subtree for shared standard-library
implementation. Imports use the visibly distinct unquoted form
`import big: @bigint`; selective imports use forms such as
`import value from @shared`. Any source within the canonical `std/` tree may
use these imports, and reserved files may import one another. Application and
package sources cannot import them, even through `./`, `../`, alternate lexical
paths, or symlinks, because the boundary is checked after path canonicalization.
Compiling a reserved file directly remains valid for standard-library checks,
and equivalent source copied outside the shipped tree remains ordinary code.
The suffixless `@name` source is not a general path prefix, attribute, package
namespace, or user-definable mechanism. Writing `.ace` after it is invalid.

Bare selected lists stay on one physical line. Use parentheses for a multiline
list:

```ace
import (
    alloc,
    realloc,
    release,
) from mem
```

A binding may be declared only once per namespace in a file. A selected name
that conflicts with another import or declaration is a compile error; imports
from different modules never merge overload sets. Import the module qualified
when names collide. Nested local bindings may shadow selected values normally.
A local or parameter named like a whole-module binding shadows it the same way:
in its scope, `shape.area` reads the local's field, and `shape.Kind.circle` is
a field path too. A type position such as `shape.Box` or `shape.Box{...}` still
names the module's type. A selected name is either a type or a value, never
both, since a type and a value may not share a name in overlapping scopes.

## What can be imported

A module exports any top-level declaration marked `pub`:

- functions, each overload of a set exported on its own,
- globals (`x: int = 0` and `x: const int = 0`),
- types: structs, contracts, enums, aliases, and distinct types,
- type-owned methods and canonical signings. Each is private by
  default; `pub Type.method` permits direct external calls and
  `pub Type signs Contract` permits external contract construction.

Whole-module access is qualified with the binding name:

```ace
import fmt
import util: "./util.ace"

main(): int {
    p: util.Point = util.origin   // imported struct type + global value
    util.zero = 5                 // imported global, written
    fmt.print(int(util.Color.blue)) // enum constant through its type
    return 0
}
```

Calls, overload selection, generic instantiation (`math.max[int](3, 9)`),
casts/constructors (`util.Point { x = 1, y = 2 }`), and function values all work
through qualification exactly as they do for local names.

A selective import places each spelling in the namespace its declaration
already occupies:

```ace
import Point, origin, score from "./util.ace"

main(): int {
    p: Point = origin
    return score(p.x)
}
```

Selecting a function imports its complete public overload set; its private
overloads stay hidden. If a module
exports a type and value with the same spelling, both are selected and remain
separate by context, just like local declarations.

Selective lists name top-level free functions, globals, and types. Public
type-owned methods and signings remain available through an imported owner or
contract type; they are not separate unqualified bindings.

## Visibility

`pub` lives on the declaration line and cannot drift. The compiler enforces it
for qualified and selective imports: an unmarked selected member is reported
as private rather than entering the importing namespace.

Imports never re-export. An import gives the importing file the right to use
the imported names; it adds nothing to the importing module's public surface.
A module's exports are exactly its own `pub` declarations, so a consumer
cannot reach a dependency's members through its importer. If `middle.ace`
imports `inner.ace` and calls `inner.ping()` inside its own `pub twice()`,
then for a consumer importing `middle`:

- `middle.twice()` works — it is `middle`'s own public declaration.
- `middle.ping()` fails with `module 'middle' has no member 'ping'`.
- `middle.inner.ping()` fails with `module 'middle' has no type 'inner'` —
  imports are not members.
- `import ping from "./middle.ace"` fails with
  `module has no public member 'ping'`.

Whoever needs `ping` imports `./inner.ace` through their own explicit line.

Within a directory module, implementation files import one another through
relative `.ace` paths. A `pub` declaration in such a file is visible only to
other files beneath that directory module, including the files of nested
modules inside it. The `mod.ace` facade exposes its chosen
surface with declarations of its own; transparent type re-exports are explicit:

```ace
import os: "./os/darwin-arm64.ace"

pub Fd: type = os.Fd
```

Code outside the directory module cannot import an implementation file, even
through `..` components or symlinks. It can import only `mod.ace`.

Visibility is decided per declaration, so each function overload carries its
own `pub`. An importer sees only the `pub` overloads of a set, qualified or
selected alike, and resolves calls and function values as if the private ones
were absent; the declaring module sees them all. A name with no `pub`
overload is not public. A call whose argument count fits no public overload
but matches a private one reports that overload as private:

```ace
// util.ace
pub scale(x: int): int { return scale(x, 10) }
scale(x: int, by: int): int { return x * by }

// main.ace
util.scale(3)    // 30
util.scale(3, 2) // error: the overload of 'util.scale' matching these arguments is private
```

A type and value sharing a spelling are
considered independently, so selecting the spelling may import either or both,
depending on which declarations are public. `extern "c"` declarations are
always module-private.

### Field visibility

A struct field follows the same rule as a declaration, measured from the file
that declares the struct. An unmarked field is private to that file. A `pub`
field is visible wherever the struct is, never wider. That means everywhere
for a `pub` struct in a single-file module or `mod.ace`, or for one a facade
re-exports through a public alias. It means the directory module's own files
for a `pub` struct left in an implementation file.

```ace
pub Array[T]: struct {
    items: mut []T = null,   // only std/array.ace reads or writes it
    pub len: usize = 0,      // every importer reads it
}
```

Outside its visibility a field cannot be read, written, named in a literal, or
set positionally (`field 'items' of 'array.Array' is private`). An empty
literal `T {}` and `blank(T)` stay legal for any visible type, since they set
no field and yield only the declared defaults and canonical zero. Embedding
promotes methods, never fields, so `x.base.f` is checked against `f`'s own
mark.

## Loading, identity, and cycles

Modules are loaded depth-first in declaration order, and each dependency is
fully loaded before its importer. Module identity is the file's canonical
path, so a module reached through several paths or aliases is loaded exactly
once — a diamond dependency shares one set of globals, not copies.

Each file keeps a canonical compilation identity, while the enclosing
`mod.ace` root establishes a directory module's external visibility boundary.
Implementation files are loaded only through imports originating within that
boundary. OS- and kernel-specific standard-library implementations always live
at `std/<module>/os/<target>.ace` behind the module's public `mod.ace` facade.
The facade selects its target file with a top-level `@when(target == "...")`
import. The name `backend` is kept for compiler/code-generation backends, not
OS bindings.

`@when` may precede any top-level declaration. Its condition is restricted to
`target` string equality/inequality, booleans, `and`, `or`, `not`, and
parentheses. The attribute has its own line and one blank line separates it
from any preceding top-level construct. Inactive declarations must have valid
syntax but are removed before imports are loaded or names and types are checked:

```ace
@when(target == "darwin-arm64")
import os: "./os/darwin-arm64.ace"
```

There is no statement-level conditional compilation. Split target-dependent
function bodies into separately guarded top-level declarations.

Import cycles are compile errors, reported with the full chain:

```
main.ace:2:1: error: import cycle: …/a.ace -> …/b.ace -> …/a.ace -> …/main.ace
```

Missing modules or members, invalid paths, duplicate bindings, collisions, and
private access each have dedicated diagnostics.

## Compilation model

All loaded modules are merged into one compilation unit in dependency order —
imported declarations come before the importing file's own — so an imported
module is complete before its importer is checked. Within a file, top-level
declarations are order-independent, and each file sees only its own top level
and its imports: the root file's names never reach an imported module's code, nor change
whether its generic brackets declare type parameters or specialize. Source file
names and line rows are preserved per file, so debugger line tables resolve
every function to the file it was written in.

## Current limitations

- There are no separate per-module objects yet: everything is recompiled in
  one merged unit on every build, and incremental rebuild granularity (the
  file, per spec) is future work.
- Relative import paths must name a file or nested module directly; wildcard
  imports and package lists do not exist.
- Standard-library modules live beside the compiler binary (`std/` next to
  `ace`), located from the real executable path the OS reports with symlinks
  resolved, so `ace` run by bare name from `PATH`, through a symlink, or by a
  relative path finds the same `std/`; there is no user-configurable search
  path yet.
- `bits`, `fmt`, `mem`, and the minimal `math` exist in the standard library today — see
  [Standard library](std/index.md).

## Design rationale

Why the rules in `spec/modules-packages.md` are shaped as they are.

- Modules #3: field visibility reuses declaration visibility, not a separate
  boundary, so any change to rule 2 carries over to fields unchanged. Private
  by default matches Rust's per-field `pub` and its rule that a literal must
  see every field it sets. Go reaches the same split by capitalization, and
  Zig and Odin keep every field public. A type can then keep invariants (an
  `Array`'s storage, a `tcp.Conn`'s descriptor) behind its methods while
  plain data marks its fields `pub`. Like Go's `pkg.T{}`, the empty literal
  and `blank(T)` stay open, because every Ace type's canonical zero is
  already a valid value and the type's own defaults are its author's choice.

- Modules #7: visibility per overload lets a public entry point share its
  name with private variants (`stat` over a private `stat(path, follow)`)
  without inventing a helper name, as C++, Swift, C#, and Kotlin set access
  per overload. Hidden overloads drop out before resolution rather than
  failing after it (C++'s order), so private declarations never change which
  public overload an importer gets, nor make its call ambiguous.

- Modules #11: platform replacement changes the target layer without spreading
  kernel ABI, constants, structures, or link names through callers, while the
  public Ace surface remains reviewable in one place and redundant forwarding
  layers do not accumulate.
- Modules #12: one restricted declaration filter supplies zero-cost target
  selection without a textual preprocessor, hidden runtime branch, general
  compile-time execution, or mid-function source fragmentation.
- C Interop #3: a positive whitelist states exactly what the compiler has
  verified against the platform ABI; anything absent is unverified, and under
  the priority stack an explicit opt-in beats accidental shape coincidences
  (default enum width freedom, slice/string descriptor layout). A `bool`
  received from C is trusted to be `0` or `1`, as C guarantees, with no
  normalization or debug check; on Apple arm64 a direct `bool` argument or
  return is zero-extended to 32 bits by its producer, and bits 32–63 are
  unspecified.
- C Interop #5: a distinct type makes the foreign convention visible while
  retaining the existing one-word, explicit function-value model; as one pointer
  word it adds no aggregate ABI case, so callback fields in C structs cross the
  boundary like data pointers.
