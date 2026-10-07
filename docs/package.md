# Packages

Plain `ace file.ace` compiles one root source with no manifest. An optional
`ace.pkg` at a directory root groups a package's files under a declared
`name`, exact `version`, `ace` language version, `type` (`exe` or `lib`), and
`root` source file, with `[dependency]` entries naming each dependency and
its exact version (`json: 0.1.0`; see `spec/modules-packages.md`'s Package System section for
the manifest format and dependency rules). Optional `description`, `author`, `license`, and `repository` fields
hold free-form single-line metadata with no effect on identity, resolution, or
compilation; any of them may be left empty. Every manifest-aware operation is
grouped under `ace pkg`:

```sh
ace pkg init exe my_app
ace pkg init lib my_lib
ace pkg build
ace pkg run [-- ARGS...]
ace pkg check
ace pkg add NAME VERSION
ace pkg remove NAME
ace pkg list
```

`build`, `run`, and `check` take no path argument: they find the nearest
enclosing `ace.pkg` by walking up from the working directory, the same way a
nested manifest starts an independent package. `--dir=DIR` on any command but
`init` starts that walk in `DIR` instead, exactly as a prior `cd DIR` would,
so `ace pkg run --dir=demo/app -- input.json` also starts the program in
`demo/app`. `init` already takes its own `[DIR]` and rejects `--dir`. `check` performs a full
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
Arguments after the first `--` go to the program unchanged, so
`ace pkg run --debug -- --debug input.json` builds in debug mode and passes
the program `--debug input.json`; without `--` it receives none.

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
unqualified `[dependency]` section: `add` validates the complete resulting graph — no
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

## Native link inputs

A `[link]` section names inputs for an `exe`'s final link, one per line:
an exact file, with the extension it has on disk, or a platform framework.
Either key may repeat:

```
[link]
path: vendor/libfoo.a
framework: CoreFoundation

[link: darwin-arm64]
path: /opt/homebrew/lib/libSDL3.dylib

[link: linux]
path: /usr/lib/libSDL3.so
```

A header may carry one target qualifier: a full target (`darwin-arm64`,
`darwin-x64`, `linux-arm64`, `linux-x64`) or an OS alone (`darwin`, `linux`),
which matches both architectures. `[dependency: linux]` works the same way.
Building target T (the host, or `--target=`) uses the unqualified section plus
those qualified T or T's OS, in file order; the rest are parsed and dropped
unchecked, so a path that exists only on another platform is no error. An
unknown qualifier, or the same header twice, is an error.

A relative path stays inside the declaring package and must exist when the
manifest loads. An absolute path is a host file. Ace never searches for a
library, guesses an extension, or falls back to the linker's own search. A
framework is the one exception: a bare name, passed as `-framework NAME`,
which the toolchain finds in its active SDK, since a platform framework's
place is fixed by the platform rather than by how it was installed.

An `exe` collects every package's active link lines, dependencies first, so an
application depending on a library links what that library declares without
naming it. Each file name and framework links once. The package being built overrides a
dependency's path by declaring the same file name at its own path; without
that, two dependencies declaring one file name at different paths are an
error naming both. The linked file must exist.

## Design rationale

Why the rules in `spec/modules-packages.md` are shaped as they are.

- Package System #15: the package layer preserves explicit imports and source
  boundaries, performs no hidden network work during compilation, admits no
  dependency code execution, and uses exact identities found in one fixed place.
  This puts simplicity and modularity ahead of convenience machinery while
  leaving the registry, shared caches, workspaces, ranges, precompiled packages,
  testing classes, and publishing additive.
