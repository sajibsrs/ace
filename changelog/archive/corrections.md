# Archive — Significant implementation corrections

Entries recorded before the changelog split; oldest first.


- A resolved method call returning nothing is no longer queued for
  end-of-file resolution: its `NULL` result type read as "not declared yet",
  so every void method call was checked twice and reported its errors twice.
  Deferral now also requires the call to have no resolved symbol.
- Added `.acefmt` `result-group = adjacent` and `after-result-group = always`:
  a statement binding a call's result, its `defer`, and the checks of that
  result form one group with no blank line inside, even before a multi-line
  check body, and a blank line follows the group unless its block closes.
  Applied across `std/`, `pkg/`, `demo/`, and `bench/`; `file.read` now
  places its descriptor `defer` before the check, as the `AGENTS.md` release
  rule requires.
- Added `.acefmt` list and literal layout rules and applied them across
  `std/`, `pkg/`, `demo/`, `bench/`, and `tests/` outside the lexer and parser
  fixtures. `struct-literal = field-per-line`: a struct literal with two or
  more fields puts one field per line with a trailing comma, and a one-line
  body holding one becomes a block. `struct-literal-in-bare-list =
  named-local`: because a bare comma list stays on one line, such a literal in
  a multi-value `return` is first bound to a local; the six empty
  `Request {method = null, target = null}` failures became `blank(Request)`.
  `dense-list = whole-items-per-row`: an array literal of plain values or
  struct literals of plain values packs the same number of whole elements on
  every row, the most that fit in 120 columns, and `dense-list-columns =
  aligned` pads each element, or each struct-row field but the last, to its
  column's widest so the columns line up. The `std/@` tables follow it, with
  their exact-power runs deliberately grouped (six per row in
  `float_parse_tables`, two per row in `float_shortest_tables`). `multiline-trailing-comma = always` now holds
  for every multi-line delimited list, and struct literals take the space
  before `{`; a one-line nonempty `{ }` has inner spaces while `()` and `[]`
  stay tight. `nested-overlong-list = named-local`: an overlong call nested
  inside a larger expression is bound to a local rather than broken
  mid-expression, as `sin` and `cos` now do with their polynomial (results
  bit-identical). The parser accepts a trailing comma in `extern` parameter lists,
  the one list that rejected it (`tests/parser/extern_trailing_comma_ok.ace`).
- Extended global constant emission to carry pointer relocations recursively
  through arrays, structs, repeated elements, aliases, and union payloads, so
  string-bearing aggregates remain static data without runtime initialization.
- Repaired the manual memory benchmark for the current zero-valid slice model,
  made checked/release costs explicit, and added a checksum-matched integer CPU
  comparison against equivalent C `-O0` and `-O2` builds.
- Corrected AArch64 aggregate argument/return layout, deep-frame loads, floating
  load/store scaling, integer-to-floating temporary separation, remainder
  lowering, short-circuit normalization, variadic stack slots, and large
  subobject copies as their feature fixtures exposed them.
- Widened constant folding to constructor-shaped same-family primitive, alias,
  distinct, enum, and qualified casts while retaining semantic fit and
  cross-family checks.
- Canonicalized equivalent module paths before assigning nominal Error identities,
  preventing duplicate module loads and domain identities.
- Derived private symbol prefixes for directory-module entries from their
  directory names rather than the repeated `module.ace` filename.
- Fixed cross-module identifier renaming (`module.c`'s `rename_type_refs`) to
  walk a fixed array type's length expression. It previously recursed only
  into an array type's element (`t->a`), so a module-private `const` used to
  back a `[N]T` array length inside that same imported module's own function
  bodies, struct fields, or parameter/return types kept its bare name instead
  of gaining the module's private prefix, producing a spurious "unknown name".
  Latent since array lengths were always integer literals; surfaced once
  Core language widened array lengths to constant expressions (above).

- Expanded `sys` with allocation-free filesystem metadata and mutation
  (`stat`, `mkdir`, `rename`, and `unlink`) plus caller-owned buffered directory
  iteration (`open_dir`, `read_dir`, and `close_dir`). The Darwin layer parses
  private `dirent64` batches into exact caller-provided name buffers, keeps
  undersized entries retryable, and exposes only stable file kind and size.
  The portable `mkdir(path)` form delegates effective permissions to the
  process umask, while explicit POSIX creation modes use named `S_I*` bits.
- Allowed global constant initializers to forward recursively evaluable
  constants from imported modules. The `sys` facade now re-exports its
  target-selected `O_*` and `S_I*` values without duplicating their numbers.
- Fixed homing of a trailing stack-passed scalar parameter to copy only its own
  width instead of the ABI-padded stack-slot width (Darwin pads scalar slots to
  4 bytes), which had let a narrow trailing argument (e.g. `bool`) clobber the
  low bytes of the previously homed adjacent stack argument. This was the root
  cause of the owning HTTPS client's response-dependent heap corruption during
  deferred `client_wipe`.
- Moved `http`'s implementation files (`client.ace`, `format.ace`, `parse.ace`,
  `redirect.ace`, `server.ace`, `transport.ace`) under `std/http/inc/` by
  convention, keeping `std/http/mod.ace` as the sole external surface. Each of
  `client.ace` and `server.ace` now declares its own plain `Context` struct and
  `context_new`/`context_wipe` pair, since directory-module privacy already
  keeps the two from colliding internally; `mod.ace` aliases them as public
  `ClientContext`/`ServerContext` and re-exposes disambiguated constructors
  `client_context_new`/`client_context_wipe` alongside the existing
  `server_context_new`/`server_context_wipe`, keeping the facade uniform.
- Added `tcp.listen_nonblock` and `tcp.accept_nonblock`, mirroring
  `connect_nonblock`, so a listener and its accepted connections can join the
  same nonblocking, `poll`-driven composition as outbound connections.
- Implemented `http.serve`: a caller-driven, nonblocking HTTP/1.1 server core
  (`std/http/server.ace`) built directly on the existing incremental
  `RequestParser` and `Response.format`, following `tcp`'s externally-driven
  connection-progress model rather than owning a blocking loop. `ServerContext`
  binds a nonblocking listener and poll queue and allocates a caller-sized
  array of fixed-capacity `Session` slots (separate input/header/body/output
  buffers per session, no internal resizing); `serve` performs at most one
  `poll.wait` per exhausted batch and drives exactly one ready listener or
  session per call, returning one `Event`
  (`accepted`/`request`/`sent`/`disconnected`/`failed`) — the caller's own loop
  supplies the bound on how long to run. `respond` formats a handler's
  `Response` into the session's output, synthesizing exactly one
  `Connection: close` header and rejecting a handler-supplied one; each
  session closes and recycles once its response is fully sent. A forked-client
  loopback fixture (`tests/emit/http/serve.ace`) proves accept, parse,
  respond, close, and slot reuse across two sequential connections.
- Renamed the server context's per-buffer parameters from `*_cap` to `*_size`
  (`input_size`, `header_size`, `body_size`, `output_size`), matching the
  `_size` naming already used elsewhere (`mem.realloc`'s `new_size`,
  `tls.context_size`/`record_size`) instead of introducing a one-off `_cap`
  spelling.
- Added `server_context_new(endpoint): (ServerContext, Error)`, a low-effort
  overload beside the fully-parameterized form, for a caller with no capacity
  preference. Unlike the client's `Storage`, session capacities are
  deliberately fixed for the context's lifetime rather than grown on demand
  (so one session overflowing never disturbs another live session's buffer
  mid-poll), so the low-effort form supplies fixed built-in defaults instead —
  512 backlog, 128 sessions, 8 KiB input/output, 64 headers, and 256 KiB body
  per session — rather than growing storage. The defaults live in
  underscore-prefixed module constants (`_backlog`, `_session_count`, …)
  rather than uppercase names, following the project's established lowercase
  `const` style (e.g. `tls`'s `closed`, `send_record`); a same-named local
  parameter in the explicit overload ordinarily shadows a same-named constant
  without conflict, so no artificial `default_`-style prefix was needed to
  avoid a collision that was never actually possible.
- Added a complete HTTP server proof of concept
  (`demo/http-complete-server/main.ace`) built on the low-effort
  `server_context_new(endpoint)` overload, and a small `http.get` usage
  example to `docs/standard-library.md`.
- Added a blocking `serve(endpoint, handle): Error` overload alongside the
  nonblocking core: no `ServerContext`, poll queue, session table, or
  caller-visible backlog/capacity tuning, just an endpoint and a
  `(Request): Response` handler. It binds, then loops forever, serially —
  accept, read one request, call `handle`, write the response with a
  synthesized `Connection: close`, close, repeat — using fixed internal
  buffers sized like the low-effort `server_context_new(endpoint)`
  defaults. A per-connection failure just drops that connection; `serve`
  itself only returns if the listener cannot be created. Added a matching
  minimal demo (`demo/http-simple-server/main.ace`).
- Fixed `SPEC.md`'s stale claim that `parse_url` rejects `https://`: the
  parser has accepted both schemes since the TLS-capable client overload was
  added, since `get(url, mut client)` needs `https://` to parse and both
  `get` overloads share one parser (`get(url, mut context)` still rejects
  `https://` before connecting, but through its own scheme-forced
  `parse_url_for(url, false)` call, not through `parse_url` itself). Updated
  the stale test assertion in `tests/emit/http/main.ace` to match.
- Removed the caller-driven nonblocking HTTP server core
  (`ServerContext`/`Session`/`Event`, `server_context_new`,
  `server_context_wipe`, `serve(mut context, timeout_ns)`, `respond`) and its
  `demo/http-complete-server` proof of concept: it had been implemented
  directly, without going through the Decision Queue carve process, and the
  owner does not want to keep that design. `std/http` now exposes exactly one
  server convenience, the blocking `serve(endpoint, handle): Error` overload,
  with `demo/http-simple-server` as its sole demo. Trimmed the matching
  fixtures (`tests/emit/http/serve.ace` removed; `tests/emit/http/main.ace`
  and `tests/sema/stdlib/http_ok.ace` no longer exercise the removed surface).
  A fresh carve of how much server mechanism belongs in the language versus a
  framework built on top is tracked in `TODO.md` before anything like the
  nonblocking core returns.
- Implemented `http.Router` (`std/http/inc/router.ace`), the fresh carve for
  the nonblocking-server item above's routing half: a growable route table
  (`transport.mem_adjust`/`mem.release`, the same pattern `Client`'s
  `Context`/`Storage` already uses) with `router_new`/`router_wipe`,
  `mut Router.handle(method, path, handler)`, and `pub Router.dispatch`
  matching on exact method plus segment-boundary longest path-prefix
  (`/home` matches `/home` and `/home/about` but not `/homework`; any
  `?query` on the request target is stripped before matching). Ace has no
  closures or first-class method values, so a stateful `Router` cannot be
  handed to `serve` as a bare `(Request): Response`; instead `serve` gained a
  second overload, `serve(endpoint: string, router: Router): Error`, that
  dispatches through the router directly. `server.ace`'s single-shot
  `serve_one` was split into shared `read_request`/`respond_close` helpers
  reused by both the handler-based and router-based per-connection paths.
  Added `tests/emit/http/router.ace` covering registration, prefix/segment
  matching, method mismatch, query stripping, and the built-in 404 fallback.
- Moved the grow-or-allocate helper formerly named `transport.mem_adjust`
  into `std/mem.ace` as `mem.adjust[T]`: `router.ace` and `client.ace` both
  called it through `transport` for a purely generic slice-growth operation,
  not anything HTTP-specific. `transport.ace`, `router.ace`, and `client.ace`
  now call `mem.adjust` directly; `router.ace` no longer imports `transport`.
- Renamed module-level `const` declarations from a leading-underscore
  lowercase spelling to all uppercase (`_backlog` → `BACKLOG`, `_input_size` →
  `INPUT_SIZE`, and the rest of `server.ace`'s session defaults), replacing
  the project's earlier lowercase `const` convention.
- Collapsed `server.ace`'s two `serve` overloads back to one,
  `serve(endpoint: string, mut context: Context, router: Router): Error`,
  removing the bare-`handle` overload now that `Router` is the sole
  dispatch surface. Added `server.Context` (`input`/`headers`/`body`/`output`,
  each starting empty, plus a `backlog` field defaulting to 512) with
  `context_new`/`context_wipe`, replacing `server.ace`'s fixed
  `BACKLOG`/`INPUT_SIZE`/`MAX_HEADERS`/`BODY_SIZE`/`OUTPUT_SIZE` module
  constants; `read_request` and `respond_close` now grow each buffer through
  `mem.adjust` off the same capacity signals the parser and formatter
  already reported (`RequestResult.capacity`, `FormatErr.capacity`, and the
  `input` buffer filling before a request completes), with no size cap.
  Growing `input` resets and reparses the retained bytes, mirroring
  `transport.exchange`'s existing handling of a moving reallocation, since
  the parser's header/target views point into it. `backlog` is the one
  non-growing field, read once by `listen_endpoint` when `serve` binds and
  never touched by `mem.adjust`. `demo/http-simple-server` and `http.mod.ace`
  (`ServerContext`, `server_context_new`/`server_context_wipe`) were updated
  to match.
- Renamed `std/http/inc`'s `format.ace`, `parse.ace`, and `redirect.ace` to
  `message.ace`, `parsing.ace`, and `redirection.ace` so every file in the
  directory is a noun, matching its own import alias exactly; `parsing` and
  `message` were already used at every import site, and the `redirect` alias
  at its one call site in `client.ace` was renamed to `redirection` to match.
- Added a minimal `http.get` client demo (`demo/http-simple-client/main.ace`):
  one `ClientContext`-backed GET against a fixed URL, and the response
  status/body printed with no redirects or TLS.
- Removed TLS entirely: `std/tls.ace`, `http`'s HTTPS overloads
  (`get(url, mut client)`, `get_redirecting`'s TLS overload) and its `Client`/
  `TlsConfig` types, the Darwin BearSSL adapter and vendored sources
  (`vendor/bearssl/`), the distributed `external/` link artifacts, and every
  TLS-only test and driver fixture. `http` is now plaintext-only; `Storage`
  no longer carries `tls_context`/`tls_records`. Removed the compiler-owned
  `@extern("component")` attribute and its supporting mechanism along with
  it, since TLS was its only consumer: the `Node.extern_component` field, its
  parsing (`@extern("tls")`), the sema reachability walk
  (`extern_requirements`) that conditionally omitted component-carrying
  functions from emission, the emit-time gating, the `--tls-backend` CLI flag
  and pkg-build equivalent, and the `(component, target, backend)` link-recipe
  table in `main.c` (`ExternRecipe`, `TLS_BEARSSL_DARWIN_ARM64`) that
  resolved and appended bundled TLS link inputs. `-c`/`-o` and `ace pkg
  build`/`run` no longer have any external-component linking to perform.
  This was a deliberate reset, not a deprecation: the prior TLS approach is
  discarded outright rather than migrated, and its Decision Queue entry and
  `TODO.md` survey now record a fresh carve, unconstrained by the removed
  backend, adapter shape, or `@extern` mechanism.
- Removed `client.ace`'s `redirect_next` `secure_transport` parameter and
  `current_secure`/`RedirectErr.downgrade` branch, dead since the TLS removal
  above left the plaintext client as `redirect_next`'s only caller, always
  passing a constant `secure_transport = false`: the current URL can never be
  HTTPS, so the downgrade branch could no longer trigger. The remaining
  HTTPS-redirect-target rejection (`UrlErr.invalid`) is now unconditional.
  Removed `RedirectErr.downgrade` from `redirection.ace`'s error domain, now
  unreachable.
- Relocated `http` out of `std` to the standalone package `pkg/http`
  (`ace.pkg`: `kind: lib`, `root: mod.ace`), independent of the separate
  TLS/HTTPS std-vs-package question, since TLS's removal above left `http`
  plaintext-only and pure Ace with no compiler-owned mechanism tying it to
  the `std` tree. `pkg/http` is Package System v1's first real payload,
  proving its local-path dependency mechanism end to end. `demo/http-simple-
  client` and `demo/http-simple-server` became `app` packages (`ace.pkg`
  each declaring `http: ../../pkg/http`) since a bare `import http` no longer
  resolves under plain manifest-free compilation; `docs/std/http.md` moved to
  `docs/http.md`.
- Gave `pkg/http` its own behavioral test suite, decoupled from the core
  compiler suite: `pkg/http/tests/` holds 7 cases (`framing`, `client`,
  `redirect`, `request`, `router`, `plaintext_only`, `api_surface`), one
  directory per case, each an `app` package depending on the sibling `http`
  package (`http: ../..`), matching `tests/package_cli`'s convention —
  `// expect-exit: N`/`// expect-check-ok` markers as `ace.pkg` comments, run
  by the new `pkg/http/tests/run.sh`. `make test-packages` runs every
  `pkg/*/tests/run.sh`, kept separate from `make test`'s `tests/run.sh`:
  package content tests shouldn't gate core compiler iteration. Content
  carried over unchanged from the fixtures the relocation above left broken
  (`tests/emit/http`'s 5 fixtures, `tests/sema/stdlib/http_ok.ace`,
  `tests/driver/http_plain_autolink.ace`), which are removed from the core
  suite now that they're superseded; `tests/run.sh`'s corresponding
  `http_plain_autolink.ace` driver block is removed. `http_plain_autolink`'s
  duplicate "standalone object" invocation (recompiling the same file with
  `-c` alone) is dropped rather than ported: it existed to check that a
  plaintext build didn't drag in TLS link inputs at the object level, and
  with `@extern(component)` removed entirely there is nothing left to
  isolate from; the case's behavioral half (https scheme rejection, DNS-
  failure propagation) is preserved as `plaintext_only`.
- Removed `ace pkg build`/`run`'s stale `--tls-backend=NAME` mention from
  `package_usage()`'s printed help: the flag itself was already removed with
  `@extern(component)`, so the argument parser has rejected it as unknown
  since then, but the help text kept advertising it.
- Implemented `ace pkg add NAME PATH`, `ace pkg remove NAME`, and
  `ace pkg list` (SPEC.md Decision Queue, "Package system v1"), completing
  v1's local-path-only manifest-editing surface. `add`/`remove` are line-
  oriented text surgery in `package.c` (`package_manifest_add_dep`/
  `package_manifest_remove_dep`) mirroring `parse_manifest`'s own comment/
  section/key scan: `add` inserts a `NAME: PATH` line right after the
  `[dependencies]` header (creating the section, preceded by exactly one
  blank line normalized from however many trailing blank lines the file
  already had, if absent); `remove` deletes the one matching entry line and,
  if it was the section's last entry, the header line too, along with a
  blank separator line immediately before it — keeping repeated add/remove
  cycles byte-for-byte idempotent. Neither function validates the graph
  itself: `main.c`'s `package_cmd_add`/`package_cmd_remove` back up the
  manifest's original bytes first, perform the edit, then re-run the same
  `package_resolve` the other subcommands already use, restoring the backup
  verbatim and reporting the failing package_resolve diagnostic if the edit
  produced an invalid graph (duplicate name, alias not matching the target's
  declared name, stdlib-name collision, cycle, mismatched `ace` version) — no
  separate validation logic, and a failed `add`/`remove` never touches the
  file. `list` is read-only, resolving and printing the graph's name/kind/
  direct-or-transitive/directory. Added `tests/package_cli/pkg_add_cross_dep`
  (`add` then `pkg build`/`run` prove a real cross-package import),
  `pkg_add_err_rollback` (mismatched alias, manifest unchanged),
  `pkg_remove_dep` (add then remove round-trips to the original bytes),
  `pkg_remove_err_missing`, and `pkg_list_transitive` (three-package chain
  distinguishing direct from transitive); `tests/run.sh` gained the
  `pkg-add-args`/`expect-add-err`/`expect-remove-ok`/`expect-remove-err`/
  `expect-list-has` markers, with `pkg-add-args` cases auto-cleaned by a
  trailing `pkg remove` so the checked-in fixture manifest stays unmutated
  across repeated runs.
- Fixed `switch` dispatch (`emit_union_switch` and the scalar `N_SWITCH` case,
  both in `emit.c`) falling through into whichever case was emitted first when
  the scrutinee matched no explicit pattern and `else` was not the last case,
  instead of running `else`. For a union switch this ran the wrong variant's
  case body against a mismatched payload, misreading raw bytes (e.g. an `f64`)
  as a slice descriptor or other pointer-bearing binder; this was the actual
  cause of both the growable-array-of-unions corruption and the non-tail
  recursive-call-with-union-parameter crash recorded in `TODO.md`, which
  turned out to be a single defect rather than two. Both switch forms now
  branch unconditionally to `else`'s label (or past the switch with no
  `else`) after the pattern comparisons, never relying on fallthrough. Added
  `tests/emit/control_flow/switch_else_position.ace` and
  `tests/emit/unions/else_not_last.ace`.

- Fixed a root import alias (`import name: "./path.ace"`) being declared into
  the single flat scope semantic checking used for the entire merged
  compilation unit, so it collided with any same-named local anywhere else in
  the unit, including inside `std` modules the importing package never
  directly names. Root import aliases now declare into their own `Scope`,
  parented to that shared scope, spliced in only while checking the root
  file's own function bodies, global-variable initializers, and top-level
  statements; declarations of every kind still land in the one shared scope
  as before, so cross-file name resolution is unaffected. Added
  `tests/modules/import_alias_scope`.

- Generic function stamping now checks each body under the scope it was
  declared in: `Sym.decl_scope` records the root file's import-alias scope for
  root-declared generics and the shared file scope for every other generic, and
  both `instantiate_generic_func_args` and `sema_stamp_body` use it instead of
  the caller's scope or the bare shared scope. Added
  `tests/modules/import_alias_generic`.

- Added the standalone package `pkg/json` (`kind: lib`, `root: mod.ace`): a
  whole-document RFC 8259 parser. `json.parse(text): (Json, Error)` builds a
  `Json` union (`none`, `bool`, `number: f64`, `string`, `array: []Json`,
  `object: []Pair`) with heap strings and backing storage, reports failures
  through the `JsonErr` domain, and enforces a limit of 512 nested
  containers (`MAX_DEPTH`). Leading-zero numbers, unpaired `\u` surrogates, unescaped
  control bytes, and trailing data are rejected. `json.wipe` recursively releases every
  allocation, keys included. The parser walk is `Parser` methods, with
  `parse_value` and `parse_object`/`parse_array` calling each other; only
  helpers that ignore parser state stay free functions. `\uXXXX` decoding uses
  a private `encode_utf8`. `pkg/json/tests/basics` runs under
  `make test-packages`, `demo/json-parse-file` parses a file and prints every
  key and value, and `docs/json.md` documents the package.
- Renamed the `ace.pkg` manifest field `kind` to `type` and its value `app` to
  `exe` (`lib` unchanged). The parser reads keys as plain text, so `type` is
  not affected by the reserved keyword. `kind:` and `type: app` are now
  errors with no alias; `ace pkg init`, `list`, and `--pkg-graph` use `exe`.
- Dropped the dependency-free `demo/darwin-packet-sniffer/ace.pkg`; the demo
  builds with plain `ace`. Demos, benches, and tests carry a manifest only to
  depend on a `pkg/` package or to test the package system.
- Added the single-file std module `file`: `file.read` composes
  `sys.open`/`stat`/`read` into one `mem.alloc` buffer released by
  `mem.release`, and `file.write` composes `open`/`write`/`close`, creating
  with `0o666` less the umask; both retry `EINTR`, and `FileErr` covers
  allocation failure and a shrinking file. It is a module of its own because
  every imported function is linked, so an allocating `sys` function would
  pull `malloc` into every `sys` user. `demo/json-parse-file` and
  `demo/json-format-file` use it in place of their own read and write loops.
- Gave `pkg/regex`'s `Regex` a convenience surface over a now-public core:
  `Matcher` is exported, and `Regex` uses only public core operations.
  `Regex::new` returns `(Regex, Error)`, leaving the pattern offset to
  `Syntax::parse`; `is_match` is `match`; `find` returns `bool` and records
  the match with its searched text; `next(text, mut at)` finds and advances
  past the match, stepping over an empty one; `count` loops `next`; `group(k)`
  and `span(k)` read the recorded match, replacing `captures`. `bench/regex`
  checksums are unchanged and every workload is within 1.2% of the previous
  build, inside run-to-run spread.
- Marked `pkg/regex`'s per-byte helpers `inline`: `Queue.has`,
  `Queue.insert`, `is_word`, `is_boundary`, and `Class.has`. `bench/regex`
  runs 7-16% faster per byte (`literal` 32.2 to 28.4 ns, `pathological` 175.6
  to 146.7), retires 6.3% fewer instructions, and grows its text by 1,024
  bytes; checksums are unchanged.
- Split `pkg/regex`'s `Matcher.add` into `add_current` and `add_next`, one
  per queue, removing the `into_next` test from its membership check, insert,
  and per-slot capture copy. `bench/regex` runs 4-17% faster per byte
  (`captures` 59.7 to 49.3 ns, `word-ing` 52.2 to 46.9) with `pathological`
  unchanged, and retires 5.2% fewer instructions. Marking `add` `inline`
  instead cost `pathological` 5% and 2.8% more instructions, and passing the
  queue as a `mut Queue` parameter left `captures` 5% slower, though it
  retired as few instructions.
- Marked per-byte and per-element helpers `inline` where a release
  microbenchmark measured a gain: `ascii.control`/`lower`, `utf8.width`,
  `str`'s `space_byte`, `math.isnan`/`isinf`/`isfinite`, the `f32`
  `exp`/`log`/`sin`/`cos` forwarders, `math`'s `bit_at`, Ryu's `log10_pow2`/
  `log10_pow5`/`pow5_bits`/`shift_wide`/`multiple_power2`/`multiple_power5`,
  `Array.set`, `pkg/json`'s `is_ws`/`is_digit`/`Parser.skip_ws`, and
  `pkg/http`'s `tchar`. `Array.push` became an inline capacity check that
  calls `reserve` only when full, instead of calling `mem.grow` every time.
  Per operation: case-folding `ascii.equal` on 64 bytes 48% faster, `str.trim`
  37%, float classification 52%, `f32` math 17%, `sin` past 2^20·π/2 28%,
  `utf8.encode` 20%, `push` 58%, `set` 12%, float formatting 1-4%,
  `pkg/json` parsing 6%, and `pkg/http` request parsing 4.5%; checksums are
  unchanged and text grows 1.3-2.7%. Ryu's `multiply_shift64`/
  `multiply_shift32` made formatting 3-8% slower and stay out of line;
  `Map.has`, `Array.clear`/`truncate`, `math`'s `exponent_field`, the float
  parser's `exact64`/`exact32`, `pkg/json`'s `Json` forwarders and
  `Formatter.object`/`array`, and `pkg/http`'s `skip_ows` gained at most
  about 1% and stay unmarked.
- Made `str`'s integer-parse helpers scalar and `inline`: `digit_value`
  returns 36 for a non-digit, so the existing `< radix` test rejects both
  cases, and `accumulate` updates a `mut` magnitude and returns only the
  overflow flag. Decimal `parse[int]`/`parse[uint]` run 50% faster, radix-16
  44%, and rejected inputs 58%; dropping the tuple results alone gave 29-44%.
- Replaced `str`'s `accumulate(mut mag, …)` with the check-only
  `fits(mag, d, radix, limit)`, the loop updating `mag` itself. The `mut`
  argument had pinned `mag` to its frame slot, so every digit loaded and
  stored it; in a register, decimal parsing runs 21-25% faster, radix-16 12%,
  and rejected inputs 24%. Hoisting the per-digit overflow division into a
  per-parse cutoff and remainder gained nothing: the division depends only on
  the digit and overlaps the multiply-add chain.
- Grouped `bench/` into four sections, each a directory with its own runners:
  `compiler/` (compile time), `core/` (`calls`, `cpu`, `hotloop`), `std/`
  (`string`, `memory`), and `pkg/` (`regex`). `bench/README.md` follows the
  same grouping; recorded results are unchanged.
- Added standard-library and package benchmarks. `bench/std/string` gains
  integer parsing, trimming, case-insensitive comparison, and UTF-8 validation
  against `strtoll`, `strtoull`, `strncasecmp`, and hand-written C, with one
  checksum check covering every row that shares a contract.
  `bench/std/collections` runs `Array` and `Map` against the same containers
  in C, and `bench/std/math` runs `math` against libm; both build C without
  auto-vectorization. `bench/pkg/json` measures writing, parsing, and pointer
  lookup, Ace-only. The first baselines put UTF-8 validation at 6.3x C,
  large-argument `sin` at 47x, `Array` access at 4-5.5x, and the other rows at
  1.1-3.5x.
- Gave `pkg/json` a convenience layer over its public core. The tagged union
  is now `Value` (in `inc/value.ace`), and `Json` (in `inc/json.ace`, matching
  `regex.Regex`) holds a `Parser`, a `Formatter`, and `root`: the input tree.
  `Json::parse(text)` returns `(Json, Error)` and wipes itself on failure;
  `get`, `string`, `number`, and `bool` read `root` by RFC 6901 JSON Pointer,
  the typed ones returning the zero value when it is absent or of another
  type. `Json::new()` makes an empty `Json` for writing: every `Formatter`
  write call is forwarded one to one, `format()` is `finish`, and `reset`
  starts the next text. `format` returns only what was written, never `root`
  implicitly; writing a parsed tree back is `value(root)`. Forwarding costs
  one extra call per write, about 3.5% on a 648-byte document. The pointer
  walk is the new core function `json.get(tree, path)`, which compares escaped
  tokens in place and allocates nothing. `demo/json-parse-file` reads four
  fields by pointer instead of walking the tree (87 to 32 lines), and
  `demo/json-format-file` writes through `Json` inline (66 to 42 lines).
- Replaced `pkg/json`'s `Json::parse(text): (Json, Error)` with the method
  `mut Json.parse(text): Error`, so parsing and writing both start from
  `Json::new()`. Each parse resets the parser and reuses its storage, which
  a fresh `Json` per document could not: about 36% less time per document at
  114 bytes and 21% at 460. On failure `root` is `Value.none`.
- Added `demo/regex-search-file`, an `exe` package over `pkg/regex` that reads
  `SPEC.md` with `file.read` and prints every `Decision:` line through a
  `next` loop.
- Renamed `sys.FileKind` and `net.SocketKind` to `sys.FileType` and
  `net.SocketType`, matching POSIX file/socket-type vocabulary. Because `type`
  is a keyword, the `FileInfo` field, the `socket` parameter, and the
  `read_dir` local are `file_type`/`socket_type`, and the `ResolveErr` member
  `socket_kind` is `socket_type`. The remaining Ace-side `kind` names follow:
  the private float `decimal.Kind` is `decimal.Type` with `number_type` fields,
  and the `pkg/http` capacity parameter is `capacity_type`. The C compiler
  keeps its internal `kind` names, since `Sym` already has a `type` member.
- Renamed the `switch` catch-all case from `else` to `default` and required it
  to be the last case. `default` is a new keyword and `F_ELSE` is now
  `F_DEFAULT`. One sema check (`check_default_last`) reports a case after
  `default`, which also covers a repeated `default`, replacing the old
  duplicate-`else` check. `else` as a case is a parse error naming `default`.
  The two dispatch regression tests now put `default` last and live as
  `switch_default_dispatch.ace` and `default_dispatch.ace`; new error fixtures
  cover both diagnostics. Every switch arm in `std`, `pkg`, `docs`, tests, and
  the VS Code grammar was migrated.
- The `pkg/http` request and response parsers now validate a header's framing
  (`Content-Length`, `Transfer-Encoding`) before checking header-slot capacity,
  so a framing conflict reports `framing` even on a full header buffer. State
  changes follow the capacity check, so a `capacity(headers)` retry is clean.
- Added a streaming writer to `pkg/json` (`inc/writing.ace`): `json.Writer`
  emits compact RFC 8259 text into caller storage with no allocation or tree.
  `object`/`array`/`end` open and close containers (`end` picks `}` or `]`
  from a one-bit-per-level kind set sized to the parser's 512-level limit),
  `key` plus `value`/`none` or the `field` shorthand write members, and
  `value(Json)` walks a parsed tree, which `json.format(value, output)`
  wraps. The first failure sticks and later calls are no-ops; `finish`
  returns the byte count or a `json.FormatErr` (`capacity`, `depth`,
  non-finite `number`, `structure`). Numbers reuse `str.format`; strings
  escape `"`, `\` and control bytes and pass other bytes through.
  `pkg/json/tests/writing` covers output, round trip, and every error.
- Added the allocation-free `utf8` module (`decode`, `encode`, `width`,
  `is_valid`) over plain `u32` code points with strict RFC 3629 validation;
  a failed decode returns width 1 so loops always advance. `pkg/json` now
  uses `utf8.encode` for `\uXXXX` escapes instead of a private encoder.
  Dropped the planned `rune` and `yarn` types: a transparent `u32` alias adds
  no behavior, and a code-point sequence costs 4x storage for indexing that
  code points do not make meaningful.
- Comparing two untyped constants (`3 == 3`, `2 + 3 > 4`) now yields `bool`;
  both operands materialize at their natural type (`int` or `f64`) through
  `coerce`. A comparison chain unifies on its first typed operand and also
  materializes untyped float operands, so `x < 2.0` checks for `x: f64`.
  `bool_const` evaluates chains pairwise, so `static_assert` accepts ordering
  comparisons. Fixed the `f64` `fcmp` encoding, which carried a stray `Rm` bit
  and read the wrong register when the right operand sat in an even temp ≥ 2.
- Added `@file` and `@line` (Errors #16): the parser lowers them to a string
  literal and an untyped integer literal for the location where they are
  written. Paths are build-relative (`std/...`, or relative to the entry
  file's directory or root package, with `..` outside it) via one
  `build_path` helper, which `assert` diagnostics now use too, replacing the
  absolute paths they embedded for imported modules. Closed the package v1
  Decision Queue item, whose slice is fully implemented.
- Float `<`, `<=`, `>`, and `>=` are now false when either operand is NaN:
  `tok_cond` gives floats `LO`/`LS`/`GT`/`GE`, which all fail on an
  unordered `fcmp`, instead of the unsigned `HI`/`HS` that held there.
- The loader's module-prefix walk (`rename_refs`) now visits struct field
  defaults and field/struct `@align` expressions, and selected-import lowering
  visits the `@align` slots it skipped. Previously such an expression naming
  its own module's enum or `const` stayed unprefixed, so the struct failed to
  resolve once imported.
- Brought `pkg/http`, `pkg/json`, their tests, and demos under the
  `defer`-after-acquisition discipline. `transport.get_with_storage` defers
  the connection close instead of `exchange` closing a passed `raw` handle on
  every exit; `server.serve_one` now accepts and defers its own connection
  close, and `serve` defers the listener close.
- `tcp.Listener` and `tcp.Conn` gained an `active` flag and canonical-zero
  field defaults, making `Listener {}`/`Conn {}` the empty handle rather than
  a live-looking descriptor zero; `close` on an inactive handle is a
  syscall-free no-op returning null, so `defer tcp.close` precedes the error
  check like `mem.release`. `pkg/http` now spells its empty handles that way.
- Range `for` loops test their 64-bit remaining trip count with a 64-bit
  `cbz`; the 32-bit test skipped or truncated ranges whose count reached 2^32.
- `mem.copy`, `move`, `fill`, and `zero` call shared per-object routines
  (`__ace$mem_copy`/`move`/`fill`) instead of emitting byte loops. A private
  convention (x15 destination, x16 source or byte, x17 count; clobbers only
  x8, x15-x17, x30, v24-v31) keeps call sites spill-free. Ranges up to 64
  bytes use overlapping NEON chunks loaded before any store, which also makes
  them overlap-safe; larger ranges use 32-byte loops with the far-end chunk
  preloaded, and forward runs align the destination to 32 bytes. Release
  4096-byte copy fell from 1995 to 82 ns (1.10x libSystem).
- `mem.compare` and `mem.equal` share `__ace$mem_compare` (x15/x16 operands,
  x17 common length in, -1/0/1 out) instead of byte loops. NEON `eor`/`umaxp`
  finds a differing 16- or 32-byte chunk, and byte-reversed 64-bit words order
  its first differing byte; lengths below 16 load overlapping or first/middle/
  last chunks into vector lanes. `__text` is now 64-byte aligned and helper
  loop heads are padded to 16 bytes, since an unaligned copy loop measured
  135 ns instead of 82. Release 4096-byte compare fell from 2976 to 107 ns
  (0.67x libSystem `memcmp`).
- Integer `/`, `%`, `/=`, and `%=` by a literal or named-constant divisor
  lower without `udiv`/`sdiv` (`emit_const_divide`): powers of two become
  shifts, masks, or a biased signed shift, and other divisors a
  `umulh`/`smulh` (64-bit) or `umull`/`smull` (narrower) reciprocal multiply
  with libdivide's Granlund-Montgomery magic generator, computed in portable
  C. Constant nonzero divisors other than signed -1 need no checked guards.
  The generator was validated against hardware division on 30.9M cases. The
  CPU recurrence fell from about 45.5 to 42.5 ms in release (C `-O2` 34 ms).
- Function entries and source `for` loop heads are padded with `nop` to
  16-byte boundaries (`align_code`), so identical code no longer changes speed
  with its offset in a fetch block. Entry padding never executes; loop-head
  padding runs once per loop entry. Current benchmarks are unchanged within
  noise, since loop bodies are still too large for head placement to
  dominate; `__text` grows about 0.4%.
- Runtime repeat initializers `[value : count]` no longer emit one copy per
  element. A constant whose encoding repeats one byte, or a runtime one-byte
  integer, fills the array through `__ace$mem_fill`; other values store element
  0 and double the filled prefix through disjoint `__ace$mem_copy` calls, so
  code grows with log2(count). `bench/string.ace` `__text` fell from 358 KB to
  106 KB and its `main` from about 66,000 to 3,400 instructions.
- Aggregate copies above 32 bytes call `__ace$mem_copy` instead of an 8-byte
  word loop, with operand moves ordered so no source register is overwritten
  first; up to 32 bytes stays inline. `StaticMap` string-key checks call
  `__ace$mem_compare` instead of a byte loop. Release copies of 48-byte
  sequence elements fell about 22%, 256-byte array assignment about 50%, and
  48-byte struct returns about 8%.
- Scalar locals are read and written directly in their frame slots: one
  `ldur`/`stur` (including `ldursb`/`ldursh` and FP `s`/`d` forms) within
  x29 - 256 instead of an address temp, load, and `mov`; `mut` slots load the
  pointer and then the value into one register. Register parameters are homed
  the same way. Call spill slots are allocated per temp index on first use
  rather than as a fixed 240-byte area, so later locals and range-loop state
  stay within direct reach. The CPU recurrence loop fell from 50 to 37
  instructions per iteration and 42.5 to 39.5 ms in release (C `-O2` 33.9 ms).
- Float negation and float register moves encode their source in `Rn`
  (bits 5-9) rather than `Rm`. `fneg` previously always negated v0, so
  `a * -b` on floats miscompiled whenever `-b` sat in a later temp; `fmov`'s
  sole caller only ever moved v0 to itself, so it never misbehaved.
- Untyped constant expressions beside a typed operand (`b != 1 + 2`,
  `b + (1 << 3)`, `-(1 + 2)`) now type every node through `coerce`, which
  also recurses into unary `-`/`~`; previously only the top node was typed and
  codegen rejected its untyped children. Unary `-` accepts untyped floats, so
  `-0.5` is valid; a negated literal keeps the plain-literal path, so its
  magnitude is judged through the negation (`-9223372036854775808` for i64).
  A compound untyped integer beside a float operand is rejected with the
  declaration's explicit-cast error rather than computed in float.
- Compiler loop state lives in callee-saved registers x19-x28: a range loop's
  cursor, count, and step, and a sequence loop's cursor and count (plus its
  index binder). A statement-only pre-pass sizes each loop nest and grants
  registers innermost-first while the nest fits the ten-register pool; outer
  loops that do not fit keep frame slots. Used registers are saved right after
  the prologue and restored in the epilogue. Functions containing `asm` get no
  loop registers. String `find-4096` fell from 9.2x to 5.8x C and
  `compare-4096` from 0.85x to 0.70x. The CPU recurrence regressed from 36.1
  to 41.7 ms although its loop shrank from 37 to 30 instructions: with loop
  state gone, `state`'s store-to-next-load forwarding is the loop's only
  memory traffic and costs about 3.7 cycles per iteration on Apple cores;
  promoting `state` itself removes it.
- Integer-class scalar locals and by-value parameters whose address never
  escapes live in the callee-saved registers loop state leaves free. The
  loop pre-pass also walks expressions, weights each use 8^loop-depth
  (straight-line uses weigh nothing, so loop-free functions save no extra
  registers), and marks escapes: `&x`, volatile access, `mut` arguments,
  field/index/slice bases, tuple assignment targets, and sequence iterables.
  Writes normalize narrow values exactly as a slot store and reload would;
  taking a promoted local's address is an internal compiler error. The CPU
  recurrence fell from 41.7 to 36.0 ms (C `-O2` 34.2 ms).
- Float locals and by-value float parameters are promoted the same way into
  d8-d15, a separate eight-register pool ranked by the same weights; `f32`
  values live in the low half. The pool has its own save area, saved with
  `stp`/`ldp d` pairs. Compiler runtime routines use only v24-v31, so the
  registers survive their calls. No current benchmark has a hot float loop;
  string timings are unchanged.
- Loop binders are promotion candidates: a range binder, a sequence index
  binder, and a sequence value binder (not an `in mut` alias) take each
  iteration's value in their own register — a width-normalizing move from
  the cursor, or a direct element load — instead of a slot store and reload
  per use. They stay per-iteration copies, so assigning one never moves the
  cursor. The CPU loop fell from 31 to 30 instructions with no memory
  traffic (time unchanged: `i` is off the recurrence); `find-4096` improved
  about 1.5%.
- `if`, general `for`, and checked `assert` conditions branch on the flags:
  `==`/`!=` and ordering chains on scalars emit `cmp`/`fcmp` + `b.cond`
  (condition-code inversion is exact on unordered floats), `!` swaps the
  branch sense, and `&&`/`||` chain branches without materializing a bool;
  other conditions still test a bool with `cbz`/`cbnz`. The string
  benchmark's code fell from 21,951 to 20,963 instructions (`cset` 436 to
  37) and `find-4096` about 3%.
- Loads land in their destination: a scalar load through an address temp
  names that temp as its own destination instead of loading a new temp and
  moving it back. Scalar fields (through struct-field chains), `.len`, and the
  indexed data pointer of a plain local slot load directly at `x29 - depth`,
  and scalar members of tuple, struct, and array literals store there; deep
  slots keep the computed-address fallback, and by-reference slots and
  pointer bases keep their address loads. The string benchmark's code fell
  from 20,963 to 20,211 instructions and `str.index_from`'s inner loop from
  20 to 16.
- Promoted integer locals are operands in their own registers: arithmetic,
  compound assignment, comparisons (including chains and bool condition
  tests), and 64-bit indexes read x19-x28 directly through `src_reg` instead
  of copying into a temp, and write results to a temp, so a promoted local
  never changes by accident. Only those audited read-only consumers borrow;
  narrower indexes, which widen in place, and every other consumer still
  copy. The arithmetic emitters take physical registers so the destination
  can differ from a borrowed source. Static copies in the string benchmark
  fell from 715 to 156 and its code from 20,211 to 19,623 instructions;
  the CPU recurrence fell from 37.0 to 36.0 ms.
- `str` searches reject a start position past the last possible match before
  iterating: a range whose first endpoint exceeds its second runs downward,
  so `split` with a multi-byte separator ending the text read past the end
  (a checked bounds abort).
- `str` search is word-at-a-time: byte search tests eight bytes per
  `@byte_load` load with the exact carry-free zero-byte test
  `~(((x & 0x7F…) + 0x7F…) | x | 0x7F…)` and `bits.trail_zeros`; substring
  search loads the words at each start and at start + len - 1, keeps
  positions matching both the first and last needle byte, and confirms each
  candidate with `mem.equal`. `find-4096` fell from 9,860 to 1,450 ns
  (0.73x C `strstr`).
- Straight-line uses count toward promotion: each weighs 1 (loop uses keep
  8^depth), and a local needs a total weight of at least 2, so a single
  straight-line use, which costs about what a register save and restore
  would, keeps its slot. Loop-free arithmetic such as the float parser's
  Eisel-Lemire step no longer stores and reloads every local. `parse-f64`
  fell from 141 to 101 ns and `format-f64-shortest` from 367 to 313 ns;
  weighting a single use as enough measured 301 ns on format with no parse
  difference, but saved registers in one-use functions.
- Float parsing tries a Clinger tier first: a coefficient of at most 2^53
  with |exponent10| <= 22 (`f64`), or 2^24 with <= 10 (`f32`), is one
  multiply or divide by an exact power-of-ten table at the requested width,
  assuming round-to-nearest. The scanner and Eisel-Lemire step use
  `bits.trail_zeros`/`lead_zeros` instead of software cascades. `parse-f64`
  fell from 100 to 87 ns (3.2x C `strtod`).
- Call-free functions promote into caller-saved x14 down to x9 before
  x19-x28, needing no save. The promotion scan marks any call that is not an
  inline form (cast, constructor, compile-time builtin, assert/panic, `bits`,
  `@byte_load`, or `mem` byte operation, whose helpers clobber only x8 and
  x15-x17); `emit_call` rejects a call in such a function. An operand-count
  temp estimate plus a margin of 3 (largest shortfall seen: 2) caps the temps
  below the promoted registers, with `tmp_int`'s cap as backstop. Union
  payload binders no longer use x14 as scratch. `parse-f64` fell from 87 to
  61 ns (2.2x C `strtod`), `format-f64-shortest` from 314 to 303 ns, and
  string-bench code by 1.3%; promoting `text.len` measured no gain.
- The exact-small float tier declines exponents past 19, where any nonzero
  coefficient overflows u64, instead of multiplying until overflow; zero goes
  to the Eisel-Lemire tier's zero check. Parsing `0e9999` fell from 14,429 to
  41 ns and `parse-f64` from 61 to 60 ns.
- Integer constants 0..4095 become add/sub/compare immediates in binary
  expressions, branches, the last operand of a comparison chain, and compound
  assignment; checked 32/64-bit forms keep the flag-based overflow branch,
  while checked narrow `+`/`-` keep the register path and its range check.
  A promoted 32/64-bit integer local takes compound results (except `/`, `%`),
  constants (masked to its width), and same-width `a op b` results directly
  in its register instead of through a temp. `const_int` no longer folds an
  error-domain constructor, which packs its domain id above the code.
  `find-4096` fell from 1,456 to 1,300 ns, `parse-f64-long` from 79 to 75 ns,
  and `format-f64-shortest` from 303 to 297 ns; CPU was unchanged.
- `emit_call` places a scalar or by-address argument whose temp already is its
  argument register without a spill or reload, and spills only argument temps
  not placed that way (temps below the call's entry depth were spilled at
  entry). A scalar result moves from x0/v0 into its temp's register before
  `reload_temps` instead of round-tripping a slot. An unguarded constant index
  (at most 4095 bytes of offset) is one add or none, and a plain store to a
  scalar field of a frame-resident local struct is a direct `stur`.
  `parse-f64` fell from 59 to 52 ns (1.9x C `strtod`), `format-f64-shortest`
  from 296 to 262 ns, and string-bench code by 5.3%; CPU was unchanged.
- Renamed `std/@/pow_tables.ace` to `math_tables.ace` and moved the `sin`/`cos`
  Payne-Hanek `2/pi` limb table into it as `two_over_pi`, so it is the single
  home for `math`'s constant tables. Results and timings are unchanged.
- Removed the streaming `json.Writer` from `pkg/json`, along with
  `json.writer` and `FormatErr.structure`; the non-streaming `json.format`
  stays as the only writer. `format` is now a recursive tree writer whose
  helpers take and return the write offset, so the first error unwinds without
  shared state. Output, escaping, and the `capacity`, `depth`, and `number`
  errors are unchanged. `pkg/json/tests/writing` now goes through `format`,
  building a 513-deep tree by hand for the depth case. A streaming
  `encode`/`decode` API will be designed separately.
- Reworked `pkg/json` ownership around a `Parser`: `json.parser_new()` makes
  one, `mut parser.parse(text)` returns a `Json` borrowed from it, and one
  `json.parser_wipe(mut parser)` releases every tree it produced. Each parse
  takes one block per element type (string bytes, array elements, object
  members) sized from the input length, since n bytes hold at most n / 2
  elements and n / 4 members, and builds containers by pushing finished
  children on parser-owned scratch stacks and copying them into the block at
  the closing bracket. This replaces the free `json.parse`, the recursive
  `json.wipe`, and every per-node allocation and unwind path; a failed parse
  needs no cleanup. Parse plus release of a 280 KB document fell from about
  6.3 to 4.1 ms (1.5x), with the same ratio at 1.4 KB and 28 KB. `JsonErr` is
  now `ParseErr`, beside `FormatErr`, with `depth_exceeded` shortened to
  `depth`; it and the parser live in `inc/parsing.ace`, and `inc/value.ace`
  became `inc/document.ace`, holding only `Json` and `Pair`.
- Added `json.Formatter` to `pkg/json` and removed the free `json.format`,
  which could only serialize a finished `Json` tree and so could not produce
  JSON from runtime data. `json.formatter_new()` and `json.formatter_wipe(mut
  formatter)` pair like `parser_new` and `parser_wipe`. The formatter is the
  earlier streaming state machine (one kind bit per open container, sticky
  first error) writing into a buffer it owns and grows from 256 bytes, with
  `value` and `field` overloads for `bool`, `int`, `uint`, `usize`, `f64`,
  `string`, and `Json`, so integers keep their exact digits. `finish()`
  returns the borrowed text. `FormatErr` is now `allocation`, `depth`,
  `number`, and `structure`; `capacity` is gone because the buffer grows. A
  formatter holds one document at a time and `reset` starts the next, keeping
  the buffer. `Parser.reset` likewise drops every tree but keeps its blocks,
  reusing any that is large enough, so results of both types are valid until
  `reset` or `wipe`. On 139-byte documents reuse cut parse time from 2.73 to
  2.07 us (24%) and format time from 1.68 to 1.55 us (8%). Added
  `demo/json-format-file`, which formats runtime-computed records and writes
  them to a file.
- Folded constant arithmetic in global initializers (`fold_int`/`fold_float` in
  `emit.c`, reached from `eval_const`): integers evaluate in 128-bit values
  range-checked against the destination width, floats in `double` with `f32`
  rounded per operation. Overflow, zero divisors, and bad shift counts are
  emit errors; casts over foldable expressions, including int-to-float, fold.
- Renamed `mem.adjust[T]` to `mem.grow[T]`, which states what it does; the
  signature and behavior are unchanged, and every `std`, `pkg`, and demo caller
  now uses the new name.
- Added the `blank(T)` builtin, the canonical zero of any type including a
  generic type parameter. Sema resolves the operand as a type (parsed with the
  full type grammar like `sizeof`) and types the call as `T`; the emitter
  zero-fills a register or frame temp, so it costs nothing beyond the fill.
- Added `std/array.ace`: a generic `Array[T]` with `push`, `reserve`, `pop`,
  `get`, `set`, `view`, and `clear` methods plus `array_new`/`array_wipe`,
  growing through `mem.grow`. `get`/`set` are checked against `len` by slicing
  the live prefix, so no new bounds machinery was added; `pop` on an empty array
  returns `blank(T)` and `ArrayErr.empty`.
- Added `bits.wrap_add`, `wrap_sub`, and `wrap_mul` for `u8`..`u64`: bits
  intrinsics (ids 33-35) emitting one plain `add`/`sub`/`mul`, re-truncated for
  the narrow widths, so they wrap in checked builds where the operators abort.
  `is_bits_integer_op` in `emit.c` now names the integer-intrinsic id set the
  direct-call, callable-symbol, and inline-eligibility sites share.
- Added `mem.zalloc[T](count)`, typed allocation with zero-filled storage: it
  reuses `gen_alloc_typed_builtin` (same overflow, zero-size, and registry
  rules) and calls `calloc(bytes, 1)` in place of `malloc(bytes)`. Darwin's
  allocator never returned dirty recycled blocks in trials, so the suite checks
  the `_calloc` reference in the object rather than block contents.
- Added `std/map.ace`: a generic `Map[K, V]` with `get`, `has`, `set`, `unset`,
  `reserve`, and `clear` methods plus `map_new`/`map_wipe`. Open addressing
  with linear probing over a power-of-two slot array from `mem.zalloc`, grown
  by rehash at 3/4 load, with backward-shift `unset`. Keys hash and compare
  through private `specialized` `hash[K]`/`same[K]` for the fixed-width
  integers, `usize`, `bool`, and `string`; the finalizer and FNV-1a step use
  `bits.wrap_mul`, since ordinary `*` aborts on overflow in checked builds.
- Added type-owned associated functions, `Type::name(...)`: a new
  `T_COLONCOLON` token and an `F_ASSOC` flag parallel to `F_METHOD`, with
  `N_FIELD_E` postfix parsing for `::` mirroring `.`. Declaration and lookup
  reuse `declare_func`/`method_key`'s existing owner-namespaced-key path, so
  a method and an associated function can never share owner+name (rejected
  at declaration). A non-generic owner resolves through the ordinary
  direct-call machinery once found under its namespaced key; a generic
  owner (`Map[K, V]::new(...)`) goes through a new
  `check_generic_assoc_call`, mirroring `check_method_call`'s generic
  branch minus the receiver-shaped logic (no `rmut`/`cmut`, no embedding
  promotion), substituting the owner's own type arguments — read from the
  call-site type expression, not a receiver parameter — into each overload
  candidate before matching. An unresolved call defers through the same
  end-of-file `Pending` fixup plain and method calls already use, so
  associated functions may call each other in any declaration order. The
  emitter reads a generic call's type arguments from the resolved owner
  type instead of a receiver (`gen_call`), and treats `F_ASSOC` like
  `F_METHOD` everywhere a receiver argument would otherwise be assumed
  (defer capture, `main` detection). `::` is compile-time-only: it acquires
  no vtable slot and cannot be reached through a value (`m::new()` is a
  compile error, since `::` only follows a type expression). An associated
  function's own type parameters beyond the owner's are not yet supported.
- Migrated `array`, `map`, and `pkg/http`'s `Router`, `ClientContext`, and
  `ServerContext` from `_new`/`_wipe` free functions to type-owned form:
  `Array[T]::new`/`Map[K, V]::new`/`Router::new`/`Context::new` (in
  `pkg/http/inc/client.ace` and `pkg/http/inc/server.ace`) as associated
  functions, `wipe()` as an ordinary mutating method, every call site
  updated. `pkg/http/mod.ace` drops its `router_new`/`router_wipe`,
  `client_context_new`/`client_context_wipe`, and
  `server_context_new`/`server_context_wipe` forwarding wrappers, since
  `Router::new()`, `ClientContext::new()`, `ServerContext::new()`, and
  `mut value.wipe()` now reach each type through its re-exported alias
  exactly as `Router.handle`/`Router.dispatch` already did. Fixed a
  cross-module gap this surfaced: `rename_decls`/`collect_members` in
  `module.c` exempted `F_METHOD` declarations from the module-name-mangling
  pass (a method's identity comes from its owner type, not a standalone
  name) but were never extended to `F_ASSOC` when associated functions
  landed, so an associated function's name was mangled with its declaring
  module's prefix while callers built the lookup key from the unmangled
  name — `Type::new()` resolved only within its declaring file. Both flags
  are now exempted together. Also migrated `pkg/http`'s `parse_url`/
  `parse_url_for` free functions in `parsing.ace` to two `Url::parse`
  associated-function overloads (`Url::parse(url)` autodetecting the
  scheme, `Url::parse(url, secure)` forcing it), dropping `mod.ace`'s
  `parse_url` wrapper; this makes the previously package-internal
  scheme-forcing overload part of `http`'s public surface, since an
  associated function's visibility is gated by `pub`/receiver type, not by
  `mod.ace`'s re-export list.
- Migrated `pkg/json`'s `Parser` and `Formatter` from `_new`/`_wipe` free
  functions to type-owned form, the same as `pkg/http`: `Parser::new`/
  `Formatter::new` as associated functions, `wipe()` as an ordinary mutating
  method, every call site updated. `pkg/json/mod.ace` drops its
  `parser_new`/`parser_wipe` and `formatter_new`/`formatter_wipe` forwarding
  wrappers, since `Parser::new()`, `Formatter::new()`, and `mut value.wipe()`
  now reach each type through its re-exported alias.
- Replaced seven hand-rolled zero-value constructor functions predating
  `blank(T)` with direct `blank(T)` calls at each call site: `zero_addr4`
  (duplicated in `std/net`'s facade and its Darwin target), `zero_dir`,
  `zero_timespec`, and `zero_stat` in `std/sys`'s Darwin target, `zero_kevent`
  in `std/poll`'s Darwin target, and `zero_url` in `pkg/http`. Every one built
  an all-zero-field struct literal by hand; no behavior change.
- Backed `pkg/http`'s `Router.routes` with `array.Array[Route]` in place of a
  hand-rolled `mem.grow`/index/increment loop, using `push`/`view`/`get`.
  Dropped the private `RouterErr` domain, which only ever duplicated
  `array.ArrayErr.allocation`.
- Added `Array[T].truncate(count)` to `std/array.ace`: drops every live
  element from `count` onward while keeping storage, bounds-checked against
  `len` the same way as `get`/`set` (checked builds abort past `len`, release
  assumes it). Converted `pkg/json`'s `Parser.stack`/`pstack` value/pair
  stacks from hand-rolled `mem.grow` pairs (`sat`/`psat`) to
  `array.Array[Json]`/`array.Array[Pair]`, using `push`/`get`/`truncate` for
  the parse_array/parse_object step that rewinds a closed container's
  temporary elements back to its watermark after copying them into the
  block's permanent storage.
- Added the standalone package `pkg/sdl` (`type: lib`), a minimal SDL3
  binding whose manifest links `SDL3` with Homebrew's `/opt/homebrew/lib` as a
  `[[link]] path` hint. `Window::new`/`wipe` and `Renderer::new`/`wipe` wrap
  SDL's opaque pointers in value structs, `Renderer.color(c)`/`color()`
  overloads pair `SDL_SetRenderDrawColor`/`SDL_GetRenderDrawColor` over a
  `Color` struct, and `error()` views `SDL_GetError`'s NUL-terminated text as
  a string without libc. `Event` mirrors the `SDL_Event` union as an
  overlapping struct (`kind`, `keyboard`, `quit`, `[128]u8` padding, all
  `@offset(0)`) whose layout `static_assert`s pin against the 3.4 headers.
  `pkg/sdl/tests/events` round-trips a key event through `SDL_PushEvent` and
  `SDL_PollEvent` headlessly and is skipped where that library is missing;
  `demo/sdl3-window` opens a window and renderer and runs a classic
  `SDL_PollEvent` loop, quitting on window close or Escape.
- Extended `pkg/sdl` with untextured colored triangles:
  `Renderer.geometry(vertices, indices)` over `SDL_RenderGeometry` with
  `Vertex`/`FPoint`/`FColor` mirroring SDL's layout, `Renderer.size()` over
  `SDL_GetCurrentRenderOutputSize`, `ticks()`/`ticks_ns()` over
  `SDL_GetTicks`/`SDL_GetTicksNS`,
  `Renderer.vsync`, `Renderer.scale`, and `Renderer.debug_text` over SDL's
  built-in 8x8 font.
  `demo/sdl3-rgb-cube` spins a cube with one solid color per face, culling
  back faces against the camera and drawing each visible face as two
  triangles over four vertices of its own. It presents under vsync and
  overlays FPS, frame time, CPU draw time excluding the wait in `present`,
  visible faces and triangles, and output size,
  formatted with `str.format` into fixed line buffers.
- Added the standalone package `pkg/regex` (`type: lib`), a byte-oriented
  regular-expression engine. `Syntax::parse(pattern): (Syntax,
  Error, usize)` builds a syntax tree or returns a `ParseErr` with the byte
  offset where parsing failed (an unclosed group or class reports its opening
  delimiter). Nodes sit in one `Array[Node]` and name children by `u32`
  index; concatenation and alternation are n-ary runs of a shared `children`
  array, so tree depth follows group nesting (capped at 512), not pattern
  length. Classes are 256-bit `[4]u64` bitmaps; `{m,n}` is capped at 1000.
  Escapes must name a metacharacter, `\n \t \r \xHH`, `\d \w \s` and
  their complements, or `\b \B`; anything else, including backreference
  digits, is rejected. `Program::compile(tree): (Program, Error)` lowers the
  tree by Thompson construction to a flat, anchored `Array[Inst]` for a Pike
  VM, encoding leftmost-first priority in `split` order (lazy forms swap it);
  `{m,n}` becomes m copies plus n-m nested optional copies. Forward targets
  are patched in place, and runs of placeholders sharing one target are
  threaded as a list through their own target fields. A subtree that emits
  nothing stops its copy loop, and programs cap at 65,536 instructions
  (`CompileErr.too_large`). `Syntax.format` and `Program.format` write
  canonical dumps through a shared `Output` sink. `Regex::new(pattern)`
  parses and compiles, and `is_match`, `find(text, after)`, and
  `captures(text, after, spans)` run a Pike VM: threads step in lockstep per
  input byte in a sparse-set queue, a new lowest-priority thread is seeded at
  each position until a match is found (leftmost-first in one pass), and
  `jmp`/`split`/`save`/assertions are followed with an explicit stack whose
  restore frames undo a `save` before a deferred sibling branch runs. Every
  buffer is sized from the program in `Regex::new`, so searches take `mut
  self` but never allocate or fail. Searches take the whole text and a start
  byte, so assertions see the true context; unset groups are `UNSET` spans.
  `pkg/regex/tests/parsing`, `compiling`, and `matching` run under `make
  test-packages`, and `docs/regex.md` documents the package.
  `bench/run-regex.sh` times four find-all and pathological workloads
  against a structure-for-structure C port of the matcher running the same
  programs (Ace/C 2.9-4.1x, just ahead of C `-O0`) with PCRE2 as an optional
  reference; `bench/README.md` records the baseline.
- Closed the definite-initialization hole through views of a local: a slice
  of a local array and a read-only `&` created no read, so an uninitialized
  array passed as `first(spans[0..2])` compiled. A view is now judged once at
  creation (`init_array_view`, the `&` case in `init_expr`): a read-only view
  requires its constant path, or the whole array for a slice or dynamic index;
  a writable view marks its constant path, or the whole array for a
  full-range slice, as a trusted promise to fill it. Slice bounds are checked
  before the base, so `buffer[0..render(mut buffer[0..n])]` holds. Per-element
  slice precision was prototyped and left out: an unsorted `InitPart` list made
  each branch join quadratic (a 4096-element partial range across 50 branches
  took 1.85 s against 3.3 ms). Buffers filled by dynamic index in `std/fmt`,
  `std/net`, the packet-sniffer demo, and `pkg/regex` tests now write through
  one full-range `mut` view of their storage.
- Definite initialization now checks specialization bodies. The pass skipped
  every function with a bracket list and no instantiations to exclude
  unstamped templates, but a specialization's list holds its type arguments
  and never gains instantiations, so its body went unchecked (this let the
  uninitialized `std/fmt` buffers compile). A specialization is recognized by
  its symbol's `spec_args` and checked like any ordinary function, called or
  not.
- Closed the unchecked-result hole of calls resolved at end of file. A forward
  call, or one whose untyped argument waits for the complete overload set
  (`f(5)` against `f(a: u64)`), returned the poison type, and only the call
  was resolved later: `y: u8 = f(300)` and `s: string = later()` compiled, and
  a cast or nested operator over such a call failed in codegen. Each function
  body (per generic stamp) that made a pending call is now re-checked in a
  final mode after resolution, and emit-time stamping resolves in that mode.
  Every `Sym` carries a declaration sequence number; the re-check sees only
  symbols up to the body's first-check point or created during the re-check,
  and overload chains, appended in declaration order, are cut at the same
  point, so name lookup, builtin shadowing, and overload selection match the
  first check. Calls that were pending (`F_PENDING`) see every declaration.
  A call is flagged when it goes pending, so `compile_time_expr` rejects it
  like a resolved call: a forward call in a field default was taken for a cast
  and reached codegen. Two checked-bounds fixtures returning `i32` from
  `main(): int` relied on the hole and now convert explicitly. The regex app's check costs 1.2% more
  instructions.
- Fixed a pending call resolving against locals declared after it. The
  end-of-file resolution looked the callee up in the call's saved block scope,
  which by then held the rest of the block, so `y: usize = get(1)` followed by
  `get: usize = y` reported `unknown function 'get'`. Each pending call now
  records the declaration sequence at the call, and inside its function's own
  scopes sees only locals up to it; the enclosing scopes stay fully visible.
- Pooled float constants per function: a float literal outside the 256
  `fmov #imm8` values, and a read of a const `f32`/`f64` global initialized by
  a literal (possibly negated, or another such const), now loads with one
  `ldr Vd, <literal>` from a deduplicated 8-byte table that `flush_fpool`
  places after the function's last instruction, patching each site's `imm19`.
  They were a `mov`/`movk` chain plus `fmov`, or `adrp`/`add`/`ldr` from the
  global's `__const` slot; `+0.0` is now one `fmov Vd, xzr`. `exp` runs 12%
  faster for `f64` and 7% for `f32`, `pow` 7%, `log` 5%; the hot-loop kernels
  are unchanged, and machine code shrinks slightly despite the tables.
- Self tail recursion: release builds lower `return f(args)` inside `f` to a
  branch back to the parameter reads instead of `bl` plus an epilogue, reusing
  the frame, as a documented optimization rather than a guarantee
  (`self_return`, `emit_call`);
  calls to other functions are untouched. `emit_func` binds the branch target
  after the prologue, callee-saved stores, and before the parameter reads only
  when sharing one frame across levels is unobservable (`self_eligible`):
  every parameter arrives in a register and is flat (a scalar, slice, or
  `string`) or by reference, every binder is flat, and nothing takes the
  address of a local or by-value parameter (`self_safe`); `main`, generic,
  exported, inline-assembly, and inline-expanding functions are excluded, and a
  pending `defer` keeps the call. Checked builds keep every call. The
  `tail-recursive` workload in `bench/core/calls` (depth 10,000) falls from
  10.58 to 0.96 ns per level, equal to C `-O2`, and ten million levels now run
  in one frame; other workloads, the 20,000-function compile time, and its
  machine code are unchanged.
- Diagnostics now render function, tuple, and pointer types in full
  (`(i32, i32): i32`, `(i32, bool)`, `*Point`) instead of the placeholders
  `(func)`, `(tuple)`, and `*T`; composite names build in a local buffer before
  taking a slot in `type_name`'s ring. Type arguments after a function with no
  generic form report that it is not generic, as a value and as a call.
- Tuple literal members now meet their declared member types as struct field
  initializers do, so `(1, 2)` initializes `(i32, i32)`: `coerce` checks each
  member against its member type, replacing the pass that fixed `int`, `f64`,
  and `u8` defaults before comparing.
- Tuple expression nodes now take the position of their first token (the
  opening parenthesis, or a bare list's first element) instead of the token
  after them, so a wrong-count tuple diagnostic names the tuple's own line.
- Rejected `%` on floats. Sema accepted it, including between two untyped
  float constants, while the emitter had no float remainder and returned the
  left operand; `%` now joins the bitwise and shift operators' integer-only
  check, placed before the untyped-constant shortcuts. Compound assignment
  now checks its operator against the target as `x = x op y` does (numeric for
  `+ - * /`, integer for `% & | ^ << >>`); it previously only coerced the right
  side, so float `%=`/`&=`/`<<=` and `bool`, struct, or `string` `+=`/`*=` were
  accepted.
- A float cast to an 8- or 16-bit integer now wraps the converted value to the
  target width. `fcvtzs`/`fcvtzu` saturate at 32 bits, so the AST emitter left
  `u8(300.0)` as 300 in its register while the stored value was 44, and
  `u8(300.0) == u8(44)` was false. The IR already wrapped.
- Slice expression nodes now take the index node's position (the first token
  inside the brackets) instead of the token after the closing `]`, so debug
  slice bounds violations and compile-time slice diagnostics (bound order,
  writable-content establishment) name the slice rather than the next line.
- The AArch64 bitmask-immediate encoder now builds its element-size prefix from
  an unsigned value; it shifted a negative `int`, which the sanitizer build
  reports on any compile that encodes a bitmask. Emitted objects are unchanged.
- Fixed a slice bound whose call pends until end of file being rejected. A
  call that cannot choose its overload before every declaration is known (an
  untyped float argument, as in `s[0..width(1.0)]`) returns the silent
  poison type until its body is re-checked, and the slice-bound check reported
  that as `slice bound must be an integer`; a poisoned bound now makes the
  slice poison too, as an index already did. `tests/ir-diff.sh` generated such
  a bound (seed 7, program 72).
- `bench/core/hotloop` adds `cube-transform`: the per-frame math of
  `demo/sdl3-rgb-cube` (8 corner rotations and projections, 6-face backface
  cull, vertex and index fill) for 1,024 cubes over 2,000 frames, headless.
  Rotations come from a 64-entry table built by an exact recurrence rather
  than `sin`/`cos`, and the C side disables `FP_CONTRACT` in the kernel, so
  checksums match bit for bit. Release Ace runs 1.15x scalar C `-O2`.
- Global value numbering now merges two `IR_GLOBAL`s only when their names
  match as well as their symbol. A static map's keys, values, and slots tables
  share the map's symbol, as do a generic's instantiations taken as function
  values (`pick[u8]`, `pick[u64]`), so release builds addressed every table
  from one of them and called every instantiation through one; e.g.
  `tests/emit/staticmap/narrow_keys.ace` exited 1 under `--release`.
