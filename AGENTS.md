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
- Every closed decision is recorded in `spec.md` (rule + rationale) and the Decision Queue is updated in the same edit; implementation history goes to `CHANGELOG.md` in the same edit. Never keep a second copy of a decision or a status log here.
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
- When writing Ace source, keep a line intact while it is at most 120 characters;
  wrap it only when it would exceed 120 characters. No Ace source line may exceed
  120 characters.
- In Ace source, put one blank line before every construct with a `{ ... }`
  body. Do not add a blank line after its closing brace unless that blank line
  is required before the next body-bearing construct. If the construct is the
  first element inside another body, do not put a blank line before it.
- Put one space before the opening `{` of every Ace body-bearing construct,
  including aggregate literals (`Value {`, never `Value{`). Write a genuinely
  empty body on one line as `{}`.
- Start every Ace source file with a block-comment header and put one blank line
  after that header. Write comments spanning multiple source lines as `/* ... */`
  blocks, not as runs of `//` comments; reserve `//` for a single source line.
- A comment attached to a body-bearing Ace construct belongs to that construct:
  put the separating blank line before the comment and no blank line between the
  comment and the construct.
- Keep a documentation comment directly adjacent to the declaration it
  documents. Put one blank line after an unrelated, section-level, or group
  comment so it cannot be mistaken for documentation of the next declaration.
- Treat a complete Ace `if` / `else if` / `else` chain as one body-bearing
  construct. Keep `else if` and `else` attached to the preceding closing brace,
  with no blank lines inside the chain; add separation only before the complete
  chain. A later independent `if` starts a new construct.
- Use `//` for single-line comments.
- For a multiline documentation block attached to a function, struct, union, or
  other declaration, put `/*` before the first content line, prefix continuation
  lines with ` *`, and put `*/` on its own final line:
  ```c
  /* First line.
   * Second line.
   */
  ```
- For a multiline block comment inside code or otherwise not attached to a
  declaration, use the same continuation prefix but put `*/` after the final
  content line:
  ```c
  /* First line.
   * Second line. */
  ```
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
- Philosophy and the initial carving phases are complete. The C11 bootstrap compiler (clang, no dependencies; tcc-style direct emission; v1 target AArch64 / Apple arm64) implements the lexer, parser, typecheck, full carved-surface codegen, and minimal DWARF line tables. The additive tagged-union, `fmt.printf`, and blocking IPv4 TCP networking topics are implemented. Chunk-by-chunk history: `CHANGELOG.md`.
- The general `poll` readiness queue and std directory-module mechanism are implemented ([networking carving](spec.md#initial-blocking-tcp-networking-carving-closed) in the Decision Queue). `poll` accepts general `sys.Fd`; `net` is only a consumer. The next networking proof is nonblocking echo followed by multi-client chat; carve any additional semantics that proof exposes before implementation. Multi-target backend selection remains open until a second target is genuinely supported.
- Human documentation: `docs/README.md` indexes implemented-only guides (primitives, types/declarations, functions, modules, errors, memory); examples are compiler-checked by `tests/sema/docs_*.ok.ace` and the `tests/modules/` executables. Expand the guide only alongside landed behavior.
- Editor support: `editors/vscode/` contains the dependency-free **Ace Language** extension (TextMate scopes, basic language configuration, compiler-checked syntax showcase; `.vscode/launch.json` runs an Extension Development Host). Outstanding editor work is tracked in `editors/vscode/README.md`.
- Open follow-ups: checked/release build modes, then checked allocation tracking; module loader known limits (`CHANGELOG.md`, chunk 11); stdlib gaps — `str`/`simd` not implemented, `math` minimal (no floats/libm), IPv6/UDP/address queries/nonblocking connection completion/half-close/extra TCP options additive after carving, `fmt.print(Error)` deferred by design (Stdlib #9), input lower priority than output.
- Later milestone hints — **unconfirmed, not owner decisions and not queue commitments**. Revisit and approve each scope before carving or implementation. Current priority-shaped direction: (1) nonblocking echo/chat over the implemented readiness queue, then threads/atomics; (2) executable C-comparison benchmarks followed only by measured emitter optimizations; (3) checked/release modes and checked-only allocation diagnostics; (4) window/input/audio integration through a C boundary; (5) math/SIMD and a C-facing graphics abstraction or small Metal shim. Later proof milestones are a nonblocking chat server, window/input frame, 2D sprite+audio demo, textured 3D cube, then a small networked game; the blocking TCP echo pair is implemented.
- Pre-Implementation Checklist items: **all decided** and recorded in spec.md; nothing left gating the emitter.

## Index into spec.md
`spec.md` is the single ledger. Jump to the topic instead of restating it here; nothing in this file overrides the spec.

Rules:

[Source](spec.md#source) · [Syntax & Parsing](spec.md#syntax--parsing) · [Compiler](spec.md#compiler) · [Memory](spec.md#memory) · [Values & Passing](spec.md#values--passing) · [Mutability](spec.md#mutability) · [Zero, Null & Absence](spec.md#zero-null--absence) · [Safety & Checked Builds](spec.md#safety--checked-builds) · [Errors](spec.md#errors) · [Type System](spec.md#type-system) · [Modules](spec.md#modules) · [Package System](spec.md#package-system-future-work) · [C Interop](spec.md#c-interop) · [Abstraction](spec.md#abstraction) · [Tagged Unions](spec.md#tagged-unions) · [Generics](spec.md#generics) · [Functions & ABI](spec.md#functions--abi) · [Names & Scoping](spec.md#names--scoping) · [Inline Assembly](spec.md#inline-assembly) · [Stdlib](spec.md#stdlib) · [Conditions](spec.md#conditions) · [Iterations](spec.md#iterations) · [Pre-Implementation Checklist](spec.md#pre-implementation-checklist)

Decision Queue — source of truth for status and what comes next:

- Closed: [philosophy phase](spec.md#closed-philosophy-phase) · [carving phase](spec.md#closed-carving-phase)
- Carved/implemented topics: [Explicit switch fallthrough](spec.md#explicit-switch-fallthrough-implemented) · [Recursive layouts and pointer-only type fixups](spec.md#recursive-layouts-and-pointer-only-type-fixups-implemented) · [Additive tagged-union carving](spec.md#additive-tagged-union-carving-closed) · [Formatted output (`fmt.printf`)](spec.md#formatted-output-carving-and-implementation-closed) · [Blocking TCP, DNS, general descriptor polling, and std directory modules](spec.md#initial-blocking-tcp-networking-carving-closed).
- [Implementation phase: closed so far](spec.md#implementation-phase-closed-so-far) — networking and DNS implementations, lexical/layout rules, overloading, `fmt.print`/`println` overloads, module loader, methods and signings, compile report, contract terminology, Error layer, panic, allocation and `mem`, pointer permissions, byte-sequence equality, canonical zero, ranges/steps/continuation, `sys` file I/O and time, driver linker controls, C ABI whitelist.
- [Provisional spellings](spec.md#provisional-spellings) · [Syntax details surfaced by the first code sample](spec.md#syntax-details-surfaced-by-the-first-code-sample)

## Build & test
`make` (produces `./ace`). Test: `make test` (fixtures: `tests/lexer/` + `tests/parser/` parse-only via `--ast`, `tests/sema/` full check, `tests/emit` compile+link+run via `// expect-exit: N` lines, `tests/modules/<case>/` same convention run in place so companion modules stay beside `main.ace`, `tests/stdlib_module/` std-resolution fixtures run against a copied compiler and synthetic `std/` tree under `/tmp` (directory-module resolution plus ambiguous/missing-name diagnostics), `tests/driver/` external-link fixtures for `.o`/`.a`/`-L`/`-l`/`-framework` linking and driver diagnostics; `*.ok.ace` must pass, `*.err.ace` must fail). Modes: `./ace file.ace` = full check; `--ast` = parse only; `--tokens` = lex only; `--report` = successful-run compilation statistics on stderr; `-c` = emit object `<base>.o`; `-o NAME` = emit + link via `cc`, with optional ordered `.o`/`.a` inputs, `-L <dir>`, `-l <name>`, and `-framework <name>` passed to `cc` as separate arguments (no shell); `-o FILE.ace` compiles that file in place, naming the executable and object after the file stem.
