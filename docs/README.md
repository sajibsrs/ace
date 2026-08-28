# Ace documentation

This guide documents the language behavior implemented by the current Ace
compiler. It contains working syntax, examples, rules, and current limitations;
it is not a roadmap.

The normative language definition remains [`../spec.md`](../spec.md). Where a
guide page and the specification disagree, the specification wins and the guide
must be corrected.

## Language guide

- [Primitive values](primitive.md) — implemented scalar types, literals, casts,
  platform words, and strings.
- [Types and declarations](type.md) — explicit declarations, aliases, distinct
  types, pointers, optionals, arrays, slices, tuples, structs, enums, interfaces,
  and generic types.
- [Functions](function.md) — declarations, parameters, returns, mutation,
  overloads, function values, generics, and the program entry point.

Repository implementation status is intentionally kept in
[`../AGENTS.md`](../AGENTS.md), outside this guide.
