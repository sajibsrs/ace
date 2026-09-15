# Ace documentation

This guide documents the language behavior implemented by the current Ace
compiler. It contains working syntax, examples, rules, and current limitations;
it is not a roadmap.

The normative language definition remains [`../SPEC.md`](../SPEC.md). Where a
guide page and the specification disagree, the specification wins and the guide
must be corrected.

## Language guide

- [Primitives](primitives.md) — implemented scalar types, literals, casts,
  platform words, and strings.
- [Types and declarations](types.md) — explicit typed declarations,
  aliases, distinct types, pointers, zero values, definite initialization,
  arrays, slices, tuples, structs, enums, tagged unions, contracts, and generic
  types.
- [Memory](memory.md) — layout introspection and assertions, sequence lengths, `mem` allocation,
  byte operations, explicit release, slice aliasing, and current diagnostics.
- [Functions](function.md) — declarations, parameters, returns, mutation,
  overloads, function values, generics, and the program entry point.
- [Control flow](control-flow.md) — conditions, switches, ranges, sequence
  iteration, indices, loop exits, labeled blocks, and deferred calls.
- [Errors](errors.md) — packed `Error` values and nominal domains
  construction, `typeid` domains, exact identity matching, module boundaries,
  and the reserved `panic(message)` and checked-only `assert(condition)` builtins.
- [Modules](module.md) — qualified and selective imports, standard-library and
  relative local sources, `pub` visibility, dependency loading, and cycles.
- [Standard library](std/index.md) — the shipped `std/` modules and
  their implemented APIs.

## Tooling

- [Compiler usage](compiler.md) — checking, object emission, linking, and the
  optional compile report.
- [Editor support](../editors/vscode/README.md) — local use and scope behavior
  for the dependency-free Ace Language VS Code extension.

Repository implementation status is summarized in
[`../AGENTS.md`](../AGENTS.md), with chunk-by-chunk history in
[`../CHANGELOG.md`](../CHANGELOG.md), outside this guide.
