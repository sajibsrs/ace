## Naming
Decision: names state what a declaration does or owns; accuracy beats cosmetic symmetry.
1. Paired operations use accurate lexical pairs (`watch`/`unwatch`, `add`/`remove`).
2. A function that only builds or releases memory is named for it. `Type.new(...)` (free form `x_new`) when the value owns heap memory that must be released, whoever allocates it (`mem` or a C library); a matching `Type.wipe(self)` (`x_wipe`) releases it. `Type.init(...)` (`x_init`) when there is nothing to release, as for a value over caller storage. A function that does work on its input keeps its verb (`parse`, `compile`), and its doc comment says what its result owns.
3. A type that has a `wipe` owns memory, and all its constructors agree on that. `wipe` is safe on any value of the type, so a result can be deferred without checking which constructor made it.
4. A property's accessors are one overloaded name, told apart by signature: `color()` reads it and `color(value)` sets it, never `get_color`/`set_color`. The getter takes `const self`. `get`/`set` remain ordinary method names for keyed access (`Map.get(key)`/`set(key, value)`).
5. Module-level `const` declarations use all uppercase (`BACKLOG`). A same-named local (e.g. a parameter) may shadow it per the ordinary nearest-binding rule (Names & Scoping); no `DEFAULT_`-style prefix.
6. File names are nouns, matching their own import alias (`parsing.ace` as `parsing`, not `parse.ace`).
7. A name that joins several nouns (`window_title_font`) is first checked for structure it flattens: when the words are owner and part, a module, type, or field path (`window.title.font`) states each owner once and lets each level be reused. Words stay joined only when together they name one concept (`line_count`, `read_all`), or where the language offers nothing to group them under: module-level constants keep a shared prefix (`MOD_SHIFT`).

## Resources
Decision: release on every exit is written once, next to the acquisition.
1. A resource that must be freed on every function exit is released with `defer`, placed immediately after its acquisition, before the error check (release is a no-op on `null`, so the failure path stays covered). Explicit release remains only where ownership transfers on success, since `defer` also runs on the success path.

## Comments
Decision: the signature carries names and types; comments carry meaning.
1. A declaration's doc comment is the comment block on its own lines directly above it; editor hover shows it. Every `pub` declaration on a `std/` module's import surface (its top-level file or `mod.ace`) and in `pkg/` has one; a comment above a run of related declarations (overloads, an ordering family, a constant table) with no blank line between covers the run, and an overload set's first doc comment covers its later overloads unless one behaves differently. It is prose that names parameters and results in backticks where they need explaining, as Go does; no `@param`-style tags.

## Layout
Decision: one place for each kind of file; `.acefmt` is normative for formatting.
1. Formatting follows `.acefmt`; a formatting-rule change updates it.
2. OS-specific standard-library code sits behind `std/<module>/mod.ace` in `std/<module>/os/<target>.ace`. `backend` is reserved for compiler/code-generation backends.
3. A directory module keeps its `mod.ace` at its root and its other implementation files under `inc/`. `mod.ace` is the module's facade and sole external import surface: it presents the public API over the `inc/` implementation, adding a thin convenience wrapper only where it makes that API clearer.
4. `demo/`, `bench/`, and `tests/` stay structurally minimal: a plain `.ace` file when `std` suffices, a directory of exact `.ace` path imports when it needs several files. An `ace.pkg` only to depend on a `pkg/` package or to test the package system, and a `mod.ace` only to test directory modules.
5. Each benchmark sits in the `bench/` section whose code it measures: `compiler/` (the compiler's own speed), `core/` (generated code for language features), `std/` (standard-library operations), or `package/` (first-party packages; not `pkg/`, which is reserved for dependency lookup). Benchmarks never live under `tests/`.
6. `selfhost/` holds the self-hosted compiler written in Ace, flat (no subdirectories); `src/` remains the C bootstrap compiler.
7. Ace programs written to find language flaws live under `experiment/<name>/`, structured like `demo/`; an experiment may carry its own check script.
