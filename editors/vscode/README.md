# Ace

Ace syntax highlighting and basic editor configuration for Visual Studio Code and compatible editors.

The extension currently provides:

- `.ace` file recognition
- `ace.pkg` manifest highlighting (headers, known keys, dependency aliases,
  versions, `exe`/`lib`) per `GRAMMAR.md` §7
- `.acefmt` formatting-rules highlighting (sections, keys, numbers, values)
- file icons for `.ace`, `ace.pkg`, and `.acefmt`, shown when the active file
  icon theme has no icon of its own for them
- theme-compatible TextMate syntax scopes
- comment and bracket configuration
- automatic closing pairs and block indentation

It does not yet provide completion, diagnostics, formatting, navigation, or a language server.

## Maintenance

Language changes that affect syntax highlighting or basic editor behavior must update this extension when practical. Any editor support that cannot land with the language change is tracked here.

The logical keywords `and`, `or`, and `not` are colored as operators only when
unqualified; a declaration name (`pub or(...)`) and a qualified call
(`atomic.or(...)`) are colored as functions. Integer literals highlight in
decimal, `0x` hexadecimal, `0o` octal, and `0b` binary.

The highlighter colors `from` only inside a selective import, where it ends the member list; elsewhere it is an ordinary identifier.

The highlighter recognizes case-final `fallthrough` as a control keyword without reserving the same spelling in functions, variables, or calls elsewhere.

The `defer` statement keyword is reserved, so the highlighter colors it as a control keyword everywhere it appears; no contextual disambiguation is needed.

A `const self` receiver and a `p: const T` parameter need no grammar of their own: `const` is a keyword and `self` a language variable wherever they appear.

The reserved line-final `intrinsic` marker is highlighted as a keyword. The
compiler restricts it to exact compiler-provided standard-library declarations.

The predeclared `StaticMap` type is highlighted as a built-in type; its braces,
colons, and indexing use the extension's existing punctuation scopes.

The highlighter exposes distinct, stable TextMate categories:

- all types: \`entity.name.type.ace\`
- built-in types additionally: \`support.type.builtin.ace\`
- all functions: \`entity.name.function.ace\`
- built-in functions additionally: \`support.function.builtin.ace\`
- user-defined types: \`entity.name.type.ace\`
- generic type parameters: \`entity.name.type.parameter.ace\`
- imported packages: \`entity.name.namespace.ace\`
- variables and fields: \`variable.other.ace\`

TextMate grammars are lexical and do not resolve symbols. Custom type
declarations and generic syntax are recognized directly, while custom type uses
beginning with an uppercase letter receive the user-defined type scope.
Lowercase custom type uses remain ordinary identifiers. Canonical standard
packages are recognized at qualified use sites; an arbitrary import alias is
recognized as a package on its import line, but its later uses require future
semantic editor support for guaranteed classification.

A keyword used as a member name is highlighted as a name after `.`, before a
field's `:`, and before a literal's `=`. A field named `in` and keyword
members inside an enum or union list still highlight as keywords; telling
them apart needs the declaration context a TextMate grammar does not track.

## Run locally

1. Open the root of the Ace repository in Visual Studio Code.
2. Select **Run Ace Extension** in the Run and Debug view.
3. Press **F5**.

An Extension Development Host opens with `samples/showcase.ace`; `samples/ace.pkg` shows manifest highlighting. Changes to the grammar can be loaded there with **Developer: Reload Window**.

No dependencies or build step are required.
