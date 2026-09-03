# AGENTS.md

Working rules for AI agents on this repository. Read this file fully before doing anything else. This file is the operating manual and index; `spec.md` is the single ledger of language rules and decision records. Where this file summarizes language content, `spec.md` is normative.

## What this repository is
Ace is a systems programming language and its bootstrap compiler. The language design is complete and the C11 compiler is in active implementation; the living design documents remain philosophy, spec, and grammar.

## Required reading, in order
1. `philosophy.md` — why Ace exists, the priority stack, the principles. **Closed**: do not change principles without explicit owner approval.
2. `spec.md` — the concrete language rules, carved incrementally. The Decision Queue at the bottom is the source of truth for status and what comes next. Use the index below to jump to a topic instead of restating one.
3. `grammar.md` — EBNF for the carved surface, with lookahead/ambiguity notes for the implementer. Where it disagrees with `spec.md`, `spec.md` wins.
4. `CHANGELOG.md` — implementation history: what was built, changed, and fixed, chunk by chunk. Consult it before assuming how something is implemented today.

## How work proceeds (owner-established process)
- One topic at a time, closed one by one. Ask the owner clarifying questions in queue order; **decisions are made by the owner, never invented by the agent**.
- Propose before implementing: when work surfaces an issue — a compiler bug, a failing or stale fixture, a stdlib gap, a design question — describe the issue, present the proposed solution briefly, and ask the owner for implementation permission before changing any code. If multiple viable options exist, present them with a recommendation; implement only what the owner approves.
- Every closed decision is recorded in `spec.md` (rule + rationale) and the Decision Queue is updated in the same edit; implementation history goes to `CHANGELOG.md` and the Current-status surface index in the same edit. Never keep a second copy of a decision or a status log here — the surface index is a directory of status words and pointers, never rule text.
- Philosophy level first, syntax carving second. Never carve syntax for a topic whose semantics are still open.
- Any contested design choice is resolved by the priority stack: **performance > simplicity > modularity > compile time > safety**.
- New sessions must resume from the Decision Queue, not restart settled debates.

## Invariants — never propose, generate, or assume these away
Digest only; the normative rules live in the linked `spec.md` topics in the index below.
- Manual memory through one fixed process-wide strategy exposed by `mem`. No allocator values/arguments, GC, borrow checker, or hidden allocation.
- Errors are values, checked with explicit branches. No exceptions, no propagation operator; panic = abort only.
- Fully explicit types, no inference. Explicit generic instantiation; no comptime, no trait solving.
- Assignment copies; parameters are by-reference — read-only unless `mut`, marked in the signature and at the call site; `noalias` params state non-overlap (debug-verifiable, release-free).
- Every type has an all-bits-zero representation. Pointer-like and handle-like
  zero values use `null`; Ace has no optional type operator. Globals are zero-initialized,
  while locals without initializers must be definitely initialized before use.
- Composition over inheritance; contract values are explicit; inheritance is rejected.
- Declarations are `name: type`; functions are keywordless with trailing returns; function values are typed by signature; no C pointer-to-function syntax.
- Single-pass compilation, recursive descent parsing, direct machine-code emission; no whole-program analysis.
- Safety is deliberately minimal: cheap local compile-time rules plus checked-build bounds, overflow, and allocator diagnostics; release strips runtime checks. No ownership, lifetime, borrow, or whole-program safety analysis.
- Control flow: no goto — labeled blocks with `break label`; `for-in` loops only
  (lowering to index loops over pointer+length); switch cases do not fall through
  implicitly, and explicit contextual `fallthrough` is implemented.

## Conventions
- Operations that establish and reverse the same relationship should use a
  recognizable lexical pair when an accurate pair exists, such as
  `watch`/`unwatch` or `add`/`remove`. Semantic accuracy takes priority over
  superficial symmetry; unrelated operations must not be named as a pair
  merely to make an API appear balanced.
- `[open]` marks undecided items and `[controversial]` contested ones. Never resolve these markers yourself — only the owner resolves them.
- Spellings marked *provisional* may change during carving; their semantics may not.
- Start strict, relax later: propose new convenience as opt-in additions, never as reductions of explicitness.
- When design is contested, cite the relevant `philosophy.md` principle or priority — not general language-design folklore.
- Ace source formatting is normative in `.acefmt` at the repo root: file bytes,
  indentation, line length, blank lines, braces, comments, and token spacing.
  It is the standard the future formatter reads and agents follow; each key's
  comment carries the rule text, so it is not restated here. Mimicking it in
  editor-specific configuration is the reader's responsibility. When a
  formatting rule is decided or changed, update `.acefmt` in the same edit.
- Documentation-only changes do not run the test suite. Review the diff and run
  `git diff --check`; only when the documentation exposes genuine uncertainty
  about compiler behavior, run the smallest targeted check needed to resolve it,
  never the full suite by default.
- Never create a Git commit unless the owner explicitly asks for that commit. Completing an implementation does not imply permission to commit it.

## Human documentation
- Human-facing language documentation lives in `docs/`. It describes only behavior that is implemented and available in the current compiler; future work, queues, and planned APIs belong in the `spec.md` Decision Queue, not in the manual.
- `spec.md` remains normative. Documentation explains the implemented rules with practical examples and explicit current limitations, and must never contradict the spec.
- Every committed change to implemented syntax, semantics, compiler-visible behavior, command-line usage, or stdlib APIs must update the relevant `docs/` page in the same commit. Add a page only when its subject is implemented.
- Keep documentation examples valid Ace. When practical, mirror substantive examples in `tests/sema/docs_*.ok.ace` or an executable fixture so later compiler changes cannot silently stale them.

## Editor support

- The Ace Language VS Code editor plugin lives in `editors/vscode/`.
- When a language change adds or changes syntax, keywords, comments, brackets, indentation, or another editor-visible construct, update the extension in the same change when support is practical. Record any required editor work that remains in `editors/vscode/README.md` so it is not lost.

## Current status
Surface index — the fast block for "is X supported?": one row per surface, status word only, normative rules always at the linked `spec.md` section. Updated in the same edit as the Decision Queue and `CHANGELOG.md`; the prose below carries rationale and next steps, never duplicated status detail.

| Surface | Status | Normative rules |
|---|---|---|
| Lexer, parser, typecheck, full carved-surface AArch64 codegen, minimal DWARF line tables | implemented | [Compiler](spec.md#compiler) |
| Source and syntax/parsing rules (formatting lives in `.acefmt`) | implemented | [Source](spec.md#source), [Syntax & Parsing](spec.md#syntax--parsing) |
| Values and passing: copy assignment, by-ref `mut`/`noalias` parameters | implemented | [Values & Passing](spec.md#values--passing) |
| Mutability and per-level pointer permissions | implemented | [Mutability](spec.md#mutability) |
| Canonical zero, `null`, definite initialization | implemented | [Zero, Null & Absence](spec.md#zero-null--absence) |
| Checked builds: bounds, overflow, allocator diagnostics | open | [Safety & Checked Builds](spec.md#safety--checked-builds) |
| Error layer, panic = abort | implemented | [Errors](spec.md#errors) |
| Type system: explicit types, distinct types, enums | implemented | [Type System](spec.md#type-system) |
| Contracts: methods, signings, conformances, embedding promotion | implemented | [Abstraction](spec.md#abstraction) |
| Tagged unions | implemented | [Tagged Unions](spec.md#tagged-unions) |
| Generics: explicit instantiation, specialization | implemented | [Generics](spec.md#generics) |
| Functions & ABI, C ABI compatibility whitelist | implemented | [Functions & ABI](spec.md#functions--abi) |
| C interop: `extern` blocks, variadics, link names | implemented | [C Interop](spec.md#c-interop) |
| Names & scoping, overloading | implemented | [Names & Scoping](spec.md#names--scoping) |
| Inline assembly | implemented | [Inline Assembly](spec.md#inline-assembly) |
| Conditions: if, switch, explicit fallthrough | implemented | [Conditions](spec.md#conditions) |
| Iterations: `for-in`, endpoint-ordered ranges, steps, labeled blocks | implemented | [Iterations](spec.md#iterations) |
| Sequences: slicing, views, iteration, byte-sequence equality | implemented | [Sequences](spec.md#sequences) |
| Modules: loader, selective imports, directory modules | implemented | [Modules](spec.md#modules) |
| Package system | future | [Package System](spec.md#package-system-future-work) |
| `mem`: raw and typed allocation, `copy`/`move`/`fill`/`zero`/`compare` | implemented | [Memory](spec.md#memory) |
| `fmt`: `print`/`println` overloads, `printf` | implemented | [Stdlib](spec.md#stdlib) |
| `str`: character/string library, `parse[T]` | implemented | [Stdlib](spec.md#stdlib) |
| `sys`: file I/O, monotonic clocks, descriptor constants, `errno` | implemented | [Stdlib](spec.md#stdlib) |
| `net`: blocking IPv4 TCP client/server, `resolve4` DNS, socket options | implemented | [Stdlib](spec.md#stdlib) |
| `poll`: general `sys.Fd` readiness queue | implemented | [Stdlib](spec.md#stdlib) |
| IPv6, UDP, address queries, nonblocking completion, half-close, extra TCP options | open, additive after carving | [Stdlib](spec.md#stdlib) |
| `math` | minimal, no floats/libm | [Stdlib](spec.md#stdlib) |
| `simd` | not implemented | [Stdlib](spec.md#stdlib) |
| `fmt.print(Error)` | deferred by design | [Stdlib](spec.md#stdlib) |
| Driver: extra `.o`/`.a`, `-L`/`-l`/`-framework`, in-place `-o FILE.ace` | implemented | [Compiler](spec.md#compiler) |
| Multi-target backend selection | open | [Compiler](spec.md#compiler) |

- Philosophy and the initial carving phases are complete. The C11 bootstrap compiler (clang, no dependencies; tcc-style direct emission; v1 target AArch64 / Apple arm64) implements the lexer, parser, typecheck, full carved-surface codegen, and minimal DWARF line tables. The additive tagged-union, `fmt.printf`, and blocking IPv4 TCP networking topics are implemented. Chunk-by-chunk history: `CHANGELOG.md`.
- The general `poll` readiness queue and std directory-module mechanism are implemented ([networking carving](spec.md#initial-blocking-tcp-networking-carving-closed) in the Decision Queue). `poll` accepts general `sys.Fd`; `net` is only a consumer. The next networking proof is nonblocking echo followed by multi-client chat; carve any additional semantics that proof exposes before implementation. Multi-target backend selection remains open until a second target is genuinely supported.
- Sequence overlaps and the character/string-library surface are carved ([sequence-overlap carving](spec.md#sequence-overlap-carving-closed), [character and string-library carving](spec.md#character-and-string-library-carving-closed)) and implemented: string slicing/view conversion/iteration/bounds checks, the `char` alias, inline sequence types in `sizeof`, and the pure-Ace `str` module with compiler-backed `parse[T]` (mem-allocation precedent) land together in this chunk.
- Human documentation: `docs/README.md` indexes implemented-only guides (primitives, types/declarations, functions, modules, errors, memory, standard library); examples are compiler-checked by `tests/sema/docs_*.ok.ace` and the `tests/modules/` executables. Expand the guide only alongside landed behavior.
- Editor support: `editors/vscode/` contains the dependency-free **Ace Language** extension (TextMate scopes, basic language configuration, compiler-checked syntax showcase; `.vscode/launch.json` runs an Extension Development Host). Outstanding editor work is tracked in `editors/vscode/README.md`.
- Open follow-ups: checked/release build modes, then checked allocation tracking; module loader known limits (`CHANGELOG.md`, chunk 11); stdlib gaps — `simd` not implemented, `math` minimal (no floats/libm), IPv6/UDP/address queries/nonblocking connection completion/half-close/extra TCP options additive after carving, `fmt.print(Error)` deferred by design (Stdlib #9), input lower priority than output.
- Later milestone hints — **unconfirmed, not owner decisions and not queue commitments**. Revisit and approve each scope before carving or implementation. Current priority-shaped direction: (1) nonblocking echo/chat over the implemented readiness queue, then threads/atomics; (2) executable C-comparison benchmarks followed only by measured emitter optimizations; (3) checked/release modes and checked-only allocation diagnostics; (4) window/input/audio integration through a C boundary; (5) math/SIMD and a C-facing graphics abstraction or small Metal shim. Later proof milestones are a nonblocking chat server, window/input frame, 2D sprite+audio demo, textured 3D cube, then a small networked game; the blocking TCP echo pair is implemented.
- Pre-Implementation Checklist items: **all decided** and recorded in spec.md; nothing left gating the emitter.

## Index into spec.md
`spec.md` is the single ledger. Jump to the topic instead of restating it here; nothing in this file overrides the spec.

Rules:

[Source](spec.md#source) · [Syntax & Parsing](spec.md#syntax--parsing) · [Compiler](spec.md#compiler) · [Memory](spec.md#memory) · [Values & Passing](spec.md#values--passing) · [Mutability](spec.md#mutability) · [Zero, Null & Absence](spec.md#zero-null--absence) · [Safety & Checked Builds](spec.md#safety--checked-builds) · [Errors](spec.md#errors) · [Type System](spec.md#type-system) · [Modules](spec.md#modules) · [Package System](spec.md#package-system-future-work) · [C Interop](spec.md#c-interop) · [Abstraction](spec.md#abstraction) · [Tagged Unions](spec.md#tagged-unions) · [Generics](spec.md#generics) · [Functions & ABI](spec.md#functions--abi) · [Names & Scoping](spec.md#names--scoping) · [Inline Assembly](spec.md#inline-assembly) · [Stdlib](spec.md#stdlib) · [Conditions](spec.md#conditions) · [Iterations](spec.md#iterations) · [Pre-Implementation Checklist](spec.md#pre-implementation-checklist) · [Sequences](spec.md#sequences)

Decision Queue — source of truth for status and what comes next:

- Closed: [philosophy phase](spec.md#closed-philosophy-phase) · [carving phase](spec.md#closed-carving-phase)
- Carved/implemented topics: [Explicit switch fallthrough](spec.md#explicit-switch-fallthrough-implemented) · [Recursive layouts and pointer-only type fixups](spec.md#recursive-layouts-and-pointer-only-type-fixups-implemented) · [Additive tagged-union carving](spec.md#additive-tagged-union-carving-closed) · [Formatted output (`fmt.printf`)](spec.md#formatted-output-carving-and-implementation-closed) · [Memory representation](spec.md#memory-representation-carving-closed) · [Sequence overlaps](spec.md#sequence-overlap-carving-closed) · [Character and string library](spec.md#character-and-string-library-carving-closed) · [Generic function specialization](spec.md#generic-function-specialization-carving-closed) · [Blocking TCP, DNS, general descriptor polling, and std directory modules](spec.md#initial-blocking-tcp-networking-carving-closed).
- [Implementation phase: closed so far](spec.md#implementation-phase-closed-so-far) — networking and DNS implementations, lexical/layout rules, overloading, `fmt.print`/`println` overloads, module loader, methods and signings, compile report, contract terminology, Error layer, panic, allocation and `mem`, pointer permissions, byte-sequence equality, canonical zero, ranges/steps/continuation, `sys` file I/O and time, driver linker controls, C ABI whitelist.
- [Provisional spellings](spec.md#provisional-spellings) · [Syntax details surfaced by the first code sample](spec.md#syntax-details-surfaced-by-the-first-code-sample)

## Build & test
`make` (produces `./ace`). Test: `make test` (fixtures: `tests/lexer/` + `tests/parser/` parse-only via `--ast`, `tests/sema/` full check, `tests/emit` compile+link+run via `// expect-exit: N` lines, `tests/modules/<case>/` same convention run in place so companion modules stay beside `main.ace`, `tests/stdlib_module/` std-resolution fixtures run against a copied compiler and synthetic `std/` tree under `/tmp` (directory-module resolution plus ambiguous/missing-name diagnostics), `tests/emit_errors/` negative fixtures that must fail to compile through `-c` with the expected diagnostic documented on an `// error:` line (the constant folder only runs when code is emitted), `tests/driver/` external-link fixtures for `.o`/`.a`/`-L`/`-l`/`-framework` linking and driver diagnostics; `*.ok.ace` must pass, `*.err.ace` must fail). Modes: `./ace file.ace` = full check; `--ast` = parse only; `--tokens` = lex only; `--report` = successful-run compilation statistics on stderr; `-c` = emit object `<base>.o`; `-o NAME` = emit + link via `cc`, with optional ordered `.o`/`.a` inputs, `-L <dir>`, `-l <name>`, and `-framework <name>` passed to `cc` as separate arguments (no shell); `-o FILE.ace` compiles that file in place, naming the executable and object after the file stem.
