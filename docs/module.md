# Modules

Every `.ace` file is a module. A file states its imports up front, before any
declaration, and members are private unless marked `pub` on the declaration
line. Import a module binding for qualified access, or explicitly select public
members into their ordinary type/value namespaces.

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
import ident: ident               // aliased: standard library only
import ident: "relative/path.ace" // local file, relative to the importing file
import one, two from ident        // selected public members from std
import one from "relative/path.ace"
```

- A **bare** name (`import fmt`, `import math`) resolves only inside the
  standard library directory `std/` that ships beside the compiler. A local
  file can never shadow a standard module. A single-file std module is
  `std/<name>.ace`; a multi-file std module is `std/<name>/module.ace` with
  its other files grouped beside that entry. Defining both forms for one std
  name is an ambiguity error. A missing-module diagnostic names both paths.
- The **alias** form binds a different local name to a standard module
  (`import f: fmt`); the module is still the standard one.
- The **path** form loads a local file. The path must be relative (no leading
  `/`) and must end in `.ace`; it resolves against the directory of the file
  containing the import, so a module's own imports travel with it.
- The **selective** form imports named public members without a module
  qualifier. Its source is either a bare standard-module name or a relative
  local path.

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

## What can be imported

A module exports any top-level declaration marked `pub`:

- functions, including exact-signature overload sets,
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
casts/constructors (`util.Point { x: 1, y: 2 }`), and function values all work
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

Selecting a function imports its complete public overload set. If a module
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

Visibility is decided per member **name and namespace**. Mark every overload
`pub` to export a value overload set. A type and value sharing a spelling are
considered independently, so selecting the spelling may import either or both,
depending on which declarations are public. `extern "c"` declarations are
always module-private.

## Loading, identity, and cycles

Modules are loaded depth-first in declaration order, and each dependency is
fully loaded before its importer. Module identity is the file's canonical
path, so a module reached through several paths or aliases is loaded exactly
once — a diamond dependency shares one set of globals, not copies.

The selected std entry is an ordinary module and its resolved canonical path
is its identity. Other files inside a multi-file std module are imported by
explicit relative `.ace` path. Internal backend files may therefore remain
directly importable, but carry no public compatibility promise; there is no
special visibility mechanism for them.

Import cycles are compile errors, reported with the full chain:

```
main.ace:2:1: error: import cycle: …/a.ace -> …/b.ace -> …/a.ace -> …/main.ace
```

Missing modules or members, invalid paths, duplicate bindings, collisions, and
private access each have dedicated diagnostics.

## Compilation model

All loaded modules are merged into one compilation unit in dependency order —
imported declarations come before the importing file's own — which preserves
Ace's declaration-before-use rule and single-pass compilation. Source file
names and line rows are preserved per file, so debugger line tables resolve
every function to the file it was written in.

## Current limitations

- There are no separate per-module objects yet: everything is recompiled in
  one merged unit on every build, and incremental rebuild granularity (the
  file, per spec) is future work.
- Import paths must name a file directly; directories, wildcard imports, and
  package lists do not exist.
- A local variable that shadows an import binding cannot be used with `.`
  member syntax in that file — the qualified use is rejected as an unknown
  module member name. Rename one side. Selectively imported values do not have
  this limitation; their nested shadowing is resolved lexically.
- Standard-library modules live beside the compiler binary (`std/` next to
  `ace`); there is no user-configurable search path yet.
- `fmt`, `mem`, and the minimal `math` exist in the standard library today — see
  [Standard library](standard-library.md).
