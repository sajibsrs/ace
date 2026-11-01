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

Every command but `init` takes no path argument: it finds the nearest
enclosing `ace.pkg` by walking up from the working directory, the same way a
nested manifest starts an independent package. Each such command also accepts
`--dir=DIR`, which starts that walk in `DIR` instead, exactly as a prior
`cd DIR` would, so `ace pkg run --dir=demo/app -- input.json` also starts the
program in `demo/app`. `--dir` is read only before `--`; words after it belong
to the program. `init` takes its own `[DIR]` and rejects `--dir`. `check` performs a full
semantic check with cross-package imports resolved, no codegen — the
manifest-aware counterpart to plain `ace file.ace`'s default mode. `build`
resolves the dependency graph and fully checks the root file; for a `lib`
package that is the entire result, plus ensuring `build/bin/` exists, since
libraries ship as source; see [Static archives](#static-archives) and
[Shared libraries](#shared-libraries) for a C consumer. For an `exe` package, `build` additionally compiles and links into
`build/bin/<package-name>`, accepting the same `--debug`/`--release`/
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
An optional `test: DIR` names the directory `ace pkg test` runs; see
[std.test](std/test.md).

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

## Static archives

A `lib` emits a static archive for a C consumer when its manifest says so, or
for one build with `ace pkg build --archive`:

```
name: codec
version: 0.1.0
ace: 0.1
type: lib
root: src/mod.ace
output: archive
```

The build writes two files to `build/bin/`:

- `libcodec.a`: one object holding what the `extern "c"` definitions and
  C-exported globals reach, across the whole graph, `std` included. Only
  `extern "c"` definitions are global symbols; everything else is local.
- `libcodec.link`: the graph's [link inputs](#native-link-inputs), one linker
  argument per line in link order, with `-framework` and its name on separate
  lines. It is empty when there are none, so a consumer can always read it.

An archive records no dependencies, so the consumer passes those arguments
itself:

```sh
cc app.c build/bin/libcodec.a $(cat build/bin/libcodec.link) -o app
```

That form splits on spaces; a build system that may see a path with spaces
reads the file line by line. The same input rules apply as for an `exe`,
including overrides and conflicts. `--archive` on an `exe` is an error, as is
`output: archive`, and an archive's root may not define `main`: the program
linking it brings its own. Its graph must define an `extern "c"` function or
C-exported global, since it would otherwise be empty. A dependency's `output` has no effect. An Ace
consumer still depends on the package as source, since mangled Ace symbols are
private to each object. Two Ace archives in one C program each carry their own
copy of what they reach of `std` and of any shared dependency, and an
`extern "c"` export they share collides at link.

## Shared libraries

A `lib` emits a dylib for a C consumer with `output: shared` in its manifest,
or for one build with `ace pkg build --shared`, which replaces a declared
`output: archive`. `--archive` replaces `output: shared` the same way, and the
two flags are mutually exclusive.

The build writes `build/bin/libcodec.dylib`, holding the code of the whole
graph, `std` included. Only `extern "c"` definitions are exported. The graph's
[link inputs](#native-link-inputs) go into the link, so the dylib records them
and no `.link` file is written.

The install name is `@rpath/libcodec.dylib`, so the program using it states
where to look:

```sh
cc app.c build/bin/libcodec.dylib -Wl,-rpath,@executable_path/lib -o app
```

A dylib's root may not define `main`, its graph must define an `extern "c"`
function or C-exported global, and `--shared` on an `exe` is an error, as is
`output: shared`. `extern "c"` signatures carry only C-ABI types, so
`string`, tuples, and `Error` do not cross; a library reports failure in C
terms. A debug-build panic aborts the whole process. Nothing runs at load,
because global initializers are constants. The library and its host each hold
their own copy of what they reach of `std` and of any shared dependency, with
separate globals; allocation goes through the platform allocator, so memory may be released on
either side. An Ace consumer still depends on the package as source.

## Native link inputs

A `[link]` section names inputs for an `exe`'s final link, one per line:
an exact file, with the extension it has on disk, a file fetched by URL and
hash, or a comma list of platform frameworks. `path` and `url` may repeat;
`framework` appears once per section:

```
[link]
path: vendor/libfoo.a
framework: CoreFoundation, Security

[link: darwin-arm64]
url: https://example.com/releases/libSDL3.a
sha256: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08

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

A fetched input pairs `url:` with the `sha256:` on its next line. The build
downloads it with `/usr/bin/curl` into `~/.ace/cache/<sha256>/<file name>`,
checks its SHA-256, and links that file. Later builds check the
cached copy and need no network; a damaged copy is downloaded again. Nothing is
fetched by `ace pkg check`, for an inactive section, or for an input the root
overrides, which it does as for a path: the same file name, here usually a
`path:` to a local build. Two dependencies naming one hash at different URLs
are one input. A static library linked this way ends up inside the executable,
so the program needs neither the cache nor the library at run time.

A dependency whose `[link]` sections all miss the target stops the link with an
error rather than undefined symbols. An empty section for that target says the
package needs nothing there.

## Design rationale

A fetched input requires its hash because the hash, not the transport, is what
pins the file: a changed or tampered download fails the build, as in Go's
`go.sum` and Nix's fixed-output fetches, which is also why `http` is allowed
for mirrors and local servers. Fetching waits for the link step so checking a
package never needs the network.

`--shared` is the spelling of gcc and clang and of Go's `c-shared`, and it names
what is asked for rather than one platform's file format. The `@rpath` install
name follows Xcode's default: it leaves placement to the program, where an
absolute name would pin the library to the directory it was built in.

The request exists both as a manifest field and a flag. The field states that
a package ships this way, so every plain build does the same thing; the flag
serves a one-off build, and `ace --archive` has no manifest to read. A file
beside the archive, like a `pkg-config` `.pc` file, keeps the exact-path rule:
Mach-O's `LC_LINKER_OPTION` autolinking (used by Clang and Swift) carries only
`-l` and `-framework`, and merging inputs into the archive cannot take a
dylib or framework.

An archive or dylib is a closed set whose only entries are its C exports, so
those replace `main` as the reachability roots, as Go's `c-archive` and
`c-shared` trim to `//export` functions. Ace trims in the compiler because the
linker cannot: its objects do not set `MH_SUBSECTIONS_VIA_SYMBOLS`, so
`-dead_strip` keeps the whole object. Plain `-c` keeps everything, since its
object may be used in other ways.

Why the rules in `spec/modules-packages.md` are shaped as they are.

- Package System #15: the package layer preserves explicit imports and source
  boundaries, performs no hidden network work during compilation, admits no
  dependency code execution, and uses exact identities found in one fixed place.
  This puts simplicity and modularity ahead of convenience machinery while
  leaving the registry, shared caches, workspaces, ranges, precompiled packages,
  testing classes, and publishing additive.
