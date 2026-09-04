# Ace Language

Ace syntax highlighting and basic editor configuration for Visual Studio Code and compatible editors.

The extension currently provides:

- `.ace` file recognition
- theme-compatible TextMate syntax scopes
- comment and bracket configuration
- automatic closing pairs and block indentation

It does not yet provide completion, diagnostics, formatting, navigation, or a language server.

## Maintenance

Language changes that affect syntax highlighting or basic editor behavior must update this extension when practical. Any editor support that cannot land with the language change is tracked here.

The highlighter recognizes case-final `fallthrough` as a control keyword without reserving the same spelling in functions, variables, or calls elsewhere.

The `defer` statement keyword is reserved, so the highlighter colors it as a control keyword everywhere it appears; no contextual disambiguation is needed.

The `mut` keyword covers the parameter mark, the call-site argument mark, and the type-position view qualifier (`mut []T`, `mut string`, `mut x[lo..hi]`) alike. The grammar colors one keyword token in every role, so type-position mutability required no grammar change; no editor work remains outstanding for it.

Local declaration elision (`name := expression`) uses the ordinary assignment
operator scope and needs no indentation or bracket change.

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

## Run locally

1. Open the root of the Ace repository in Visual Studio Code.
2. Select **Run Ace Language Extension** in the Run and Debug view.
3. Press **F5**.

An Extension Development Host opens with `samples/showcase.ace`. Changes to the grammar can be loaded there with **Developer: Reload Window**.

No dependencies or build step are required.
