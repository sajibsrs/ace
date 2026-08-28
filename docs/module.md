# Modules

Every `.ace` file is a module. A file states its imports up front, before any
declaration, and members are private unless marked `pub` on the declaration
line. Nothing from another module enters the local namespace: imported names
are always qualified with the binding you chose.

```ace
import fmt
import util: "./util.ace"

main(): int {
    fmt.print(util.twice(21)) // prints 42
    return 0
}
```

## Import forms

```ebnf
import ident                      // bare: standard library only
import ident: ident               // aliased: standard library only
import ident: "relative/path.ace" // local file, relative to the importing file
```

- A **bare** name (`import fmt`, `import math`) resolves only inside the
  standard library directory `std/` that ships beside the compiler. A local
  file can never shadow a standard module.
- The **alias** form binds a different local name to a standard module
  (`import f: fmt`); the module is still the standard one.
- The **path** form loads a local file. The path must be relative (no leading
  `/`) and must end in `.ace`; it resolves against the directory of the file
  containing the import, so a module's own imports travel with it.

One binding name may be declared only once per file. Duplicate bindings are a
compile error.

## What can be imported

A module exports any top-level declaration marked `pub`:

- functions, including exact-signature overload sets,
- globals (`x: int = 0` and `x: const int = 0`),
- types: structs, interfaces, enums, aliases, and distinct types,
- impl blocks, whose constructor becomes a qualified value.

Access is always qualified with the binding name:

```ace
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

## Visibility

`pub` lives on the declaration line and cannot drift. The compiler enforces it
at every qualified use: an unmarked member is reported as not public rather
than as unknown, so private helpers fail loudly instead of leaking.

Visibility is decided per member **name**. If any declaration of a name in a
module is not `pub`, the whole name is import-invisible; mark every overload
`pub` to export an overload set. `extern "c"` declarations are always
module-private.

## Loading, identity, and cycles

Modules are loaded depth-first in declaration order, and each dependency is
fully loaded before its importer. Module identity is the file's canonical
path, so a module reached through several paths or aliases is loaded exactly
once — a diamond dependency shares one set of globals, not copies.

Import cycles are compile errors, reported with the full chain:

```
main.ace:2:1: error: import cycle: …/a.ace -> …/b.ace -> …/a.ace -> …/main.ace
```

Missing modules, invalid paths, duplicate bindings, and private access each
have dedicated diagnostics.

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
  module member name. Rename one side.
- Standard-library modules live beside the compiler binary (`std/` next to
  `ace`); there is no user-configurable search path yet.
- Only `fmt` and the minimal `math` (`min[T]`, `max[T]`, `abs[T]`) exist in
  the standard library today.
