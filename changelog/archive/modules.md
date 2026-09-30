# Archive — Modules and C boundary

Entries recorded before the changelog split; oldest first.


- Made `mod.ace` the sole external facade of a standard-library directory
  module, with recursive implementation files importable only from within that
  directory boundary; facade-owned `pub` declarations, including transparent
  type aliases, explicitly define the external surface. Renamed existing
  directory facades from `module.ace` and shortened compiler-reserved imports
  from `@name.ace` to `@name`.
- Added explicit callable C function values with the distinct
  `extern "c" (parameters): return` type spelling, including zero-parameter and
  no-value shapes, contextual references to exact `export`/`extern` functions,
  positive ABI checking, null values with checked-call diagnostics, and
  C-convention indirect lowering. A clang fixture verifies the POSIX thread-entry
  shape in both C-to-Ace and Ace-to-C directions.
- Implemented canonical-path, depth-first module loading with whole-module and
  selective imports, aliases, visibility checks, cycle diagnostics, and
  declaration-before-use ordering. Modules merge into one compilation unit under
  private prefixed identities; canonical files load once.
- Made `from` contextual: the parser matches the identifier text after a
  selective import's member list, so `from` is otherwise an ordinary name.
- Added standard directory modules (`std/name/module.ace`) beside single-file
  modules, with ambiguity diagnostics. OS-layer files remain explicitly imported
  behind stable public facades.
- Represented compiler-reserved standard-library implementation as the
  canonical `std/@/` subtree and unquoted `@file.ace` import source, enforcing
  the existing boundary after path canonicalization while leaving direct
  reserved-source checks available.
- Implemented `extern "c"`, `export`, link names, and C variadics behind a
  positive ABI whitelist. Apple-arm64 lowering covers compatible scalars,
  pointers, distincts, recursive structs, nested arrays, register exhaustion,
  homogeneous floating aggregates, indirect aggregates, and structure returns;
  clang-compiled fixtures serve as the ABI oracle.
- Admitted `bool` at the C boundary as `_Bool` in every integer position,
  including C variadics (promoted to `int`); the backend already kept `bool`
  as a 0/1 byte, so only the ABI whitelist changed. The clang oracle covers
  direct values, struct fields, a `*bool` out-parameter, and variadics.
- Stated untyped storage and the plain-bits type category, and admitted C
  function values in every C-ABI position (struct fields and pointees
  included); the clang oracle calls a callback field by value and through a
  pointer in both directions.
- Added pointer reinterpretation `*U(p)`/`mut *U(p)` between plain-bits
  pointees through the existing pointer-conversion callee: sema checks plain
  bits and permission and proves alignment when `U` is no more aligned than
  `T`, so only an over-aligning view carries the debug alignment check.
- Added the reserved `bitcast(U, value)` builtin for plain-bits types of equal
  size and padding-free source, and removed the four `bits.cast` float
  intrinsics it subsumes. A float-integer pair still lowers to `IR_BITCAST`;
  other pairs go through a stack object. Callers were migrated by a sema probe
  recording each call's result type; the generated IR of the float benchmarks
  and tests is identical to before.
- Added field `@offset(N)` and overlapping structs, which complete storage
  reinterpretation. Layout places an offset field exactly and takes the size
  from the largest field end; sema rejects misaligned offsets and, where fields
  share bytes, non-plain-bits fields, defaults, literals, and by-value C use.
  Overlapping structs lower like unions (in memory, moved as words, a partial
  last word as u32/u16/u8 pieces), `field_off` reads their stored offsets, and
  definite initialization stops its path at them so any member write counts.
- Implemented statement-only AArch64 inline assembly with explicit
  input/output/clobber bindings and no constraint language or reordering.
- Implemented Package System v1's manifest and dependency-graph resolution
  (`src/package.c`): a line-based `ace.pkg` format (`key: value` fields, `//`
  comments, one `[dependencies]` section of `name: relative/path` entries)
  and a depth-first resolver mirroring the existing module loader's
  canonical-path dedup and cycle diagnostics. Validates required fields,
  exact `major.minor.patch`/`major.minor` version formats, `app`/`lib` kind
  (only `lib` may be a dependency), a `root` file confined to its package
  directory, dependency aliases that exactly match the target's declared
  name, one source per declared package name, a shared `ace` language
  version across the graph, and no collision with standard-library module
  names. The `--pkg-graph=DIR` flag exposes this stage for testing, printing
  the resolved dependency-first order with no codegen involvement. Local-path
  dependencies only; no git, lockfile, or registry. Cross-package import
  resolution and the `ace pkg` command set remain separate next steps.
- Wired Package System v1 into `module.c`'s import loader: each loaded module
  now carries its owning `Package*` (propagated from the compiled root, or
  NULL for a manifest-free compile), and a bare/aliased/selective import
  checks the importing module's declared dependencies before falling back to
  stdlib resolution. Every relative import from a package-owned module must
  stay within that package's own canonical directory, which both keeps a
  dependency's non-root files from being deep-imported and blocks a
  package's own relative imports from escaping it; a dependency's further
  dependencies stay invisible to consumers unless redeclared (no automatic
  transitivity). The `--pkg-check=DIR` flag exposes this stage: full
  semantic check of a package's root file with cross-package imports
  resolved, no codegen. The real `ace pkg` command set remains a separate
  next step.
- Implemented the real `ace pkg init`/`build`/`run`/`check` CLI as a distinct
  argument-parsing mode in `main.c`, alongside (not replacing) the
  `--pkg-graph`/`--pkg-check` debug flags. `build`/`run`/`check` find the
  nearest enclosing `ace.pkg` by walking up from the working directory
  rather than taking a directory argument. `check` promotes `--pkg-check`'s
  logic verbatim. `build` resolves and fully checks the graph; a `lib`
  package stops there plus ensuring `build/` exists (v1 libraries are
  source-only, with no reusable archive), while an `app` package additionally
  compiles and links into `build/<package-name>`, reusing the direct
  compiler's TLS-recipe and linker machinery and accepting the same
  `--checked`/`--release`/`--target=`/`--tls-backend=`/`--report` flags.
  `run` is app-only and executes `build`'s linked binary in place,
  propagating its exit code. `init` creates a minimal `ace.pkg` plus root
  file (an app gets `main(): int { return 0 }`; a lib gets an empty root
  module) without overwriting an existing manifest or root file, deriving
  the package name from the target directory's basename. Added
  `tests/package_cli/` fixtures covering app/lib builds, cross-package builds,
  `run`'s app-only restriction, and nested-directory `check` discovery.
- Added `author`, `license`, and `repository` manifest fields alongside the
  existing `description` — all free-form single-line optional metadata with
  no format validation and no effect on identity, resolution, or
  compilation. `ace pkg init` now writes all four fields present but empty;
  an empty value for any of the four is treated as absent rather than the
  usual malformed-`key: value` parse error, so a freshly initialized manifest
  still builds unedited. Added `tests/package/metadata_blank` and
  `tests/package/metadata_dup_author` covering the blank-is-absent carve-out
  and that a real duplicate value is still rejected.
- Implemented native link-input declarations (SPEC.md Package System #14,
  `GRAMMAR.md` §7): a repeatable `[[link]]` manifest section, one record per
  occurrence admitting up to four optional keys once each — `object`/
  `search` (relative to the declaring package, no `..` traversal, `object`
  ending in `.o` or `.a`) and bare-name `library`/`framework` — parsed in
  `package.c` alongside the existing `[dependencies]` section (`path_is_
  contained` factored out of `path_is_local_ace` and reused for both).
  `object`/`search` are existence-checked and canonicalized at manifest-load
  time the same way `root` already is; validated entries populate a new
  `Package.links` list (`ace.h`) in file order. `ace pkg build`/`run` (`main.c`)
  collect every package's `links` across `package_resolve`'s dependency-first
  `order` into the direct driver's own flag vocabulary (`object` positional,
  `-L`/`-l`/`-framework`), de-duplicating exact matches and diagnosing a
  same-identity-different-value conflict (basename for `object`, the name
  itself for the other three kinds) as a resolution-time error naming both
  declaring packages — mirroring the existing one-version-per-package-name
  conflict handling rather than merging or renaming. A `lib` package's
  declared inputs stay dormant until some `app` in the graph reaches the
  link step. Added `tests/package/link_*` (parsing and per-package
  validation) and `tests/package_cli/link_framework`/`link_conflict`
  (end-to-end transitive linking through a real `cc` invocation, and the
  conflict diagnostic); `tests/run.sh` gained the `// expect-build-err:`
  marker since checking alone never reaches the link step.
- Added `[[link]] path: DIR` beside `library`: `package.c` resolves it to
  the exact file in the Apple linker's per-directory order (`.tbd`,
  `.dylib`, `.a`), keeping symlinks as written, and that file replaces
  `-l<name>` at the library's first position in the graph, so the path
  applies to one library, not every `-l`. A dependency's path is a hint that
  falls back to `-l<name>` where the file is missing; the root's is binding,
  overrides the hints, and a missing file is a manifest error. Dependencies
  hinting different paths for one library conflict at link collection
  whether or not either exists, so the outcome never depends on the host. A
  `path` needs a `library` in its record and must be absolute. Records now
  carry their occurrence number so a key can pair with its record's
  `library`.
- Replaced path dependencies with version-only entries resolved through a
  reserved `pkg/` directory (SPEC.md Package System #4–#6, #9–#13): a
  `[dependencies]` value is one exact `major.minor.patch` version, and every
  name in a graph resolves from the root package's directory as the nearest
  existing `<dir>/pkg/<name>` (`find_in_pkg` in `package.c`), which must
  declare that name and version; a dependency's own `pkg/` is never read.
  `package_contains` excludes `pkg/` from a package's canonical directory for
  its root file, `[[link]]` paths, and relative imports (`module.c`). `ace pkg
  add` takes `NAME VERSION`. Git sources, the lockfile, and
  `fetch`/`resolve`/`update` were dropped from the design pending a central
  registry. Package fixtures moved their dependencies under `<case>/pkg/`;
  demos, benches, and `pkg/*/tests` now reach the repository's `pkg/` by
  walking up. The `bench/pkg/` section became `bench/package/`, since a
  directory named `pkg/` is now read as a dependency directory.
- Implemented recursive nested modules: a subdirectory with its own `mod.ace`
  is itself addressable via a `/`-separated path (bare, aliased, or quoted
  relative without a `.ace` suffix). The parser accepts `ident (/ ident)*` for
  a bare/alias-target module reference, threading the full segment chain
  through the import node (`N_IMPORT.c`) while `str`/`b` keep naming the leaf
  segment, the sole bound qualifier. `module.c`'s import resolution walks
  every non-leaf segment requiring `<dir>/mod.ace` (erroring by name if
  absent — a subdirectory without `mod.ace` stays private, not a nested-module
  segment) before resolving the leaf exactly like today's flat-vs-directory
  stdname lookup; the same walk covers std, package-dependency, and quoted
  relative paths. The existing std directory-module privacy check now finds
  the *nearest* enclosing `mod.ace` rather than assuming one level, so a
  nested module's own implementation files stay private to it rather than to
  whichever outer directory module contains it.
- Moved the blocking TCP composition module from the flat `std/tcp.ace` into
  `std/net/tcp/mod.ace`, its first real use as a nested module: `import
  net/tcp` replaces `import tcp` everywhere it is used.
