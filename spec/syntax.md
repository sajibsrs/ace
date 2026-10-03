## Source
1. File extension: **.ace** (decided — `.ac` rejected: autoconf collision).
2. Source is UTF-8. Newlines terminate statements; semicolons appear only
   between clauses of a general `for` head. `//` comments end at newline and
   `/* ... */` comments do not nest.

## Syntax & Parsing
1. Fast, simple parsing is the goal — single-lookahead LL(1) is the guideline, not a requirement; small bounded lookahead is acceptable where it buys readability.
2. Recursive descent top-down parsing implementation.
3. Storage declarations state `name: type` everywhere (globals, locals, fields, parameters, returns, and multi-declarations). The only type-leading top-level forms are explicitly qualified methods (`Circle.area`) and a concrete type's canonical signing (`Circle signs Shape`); both are bounded-lookahead declarations, never statements. No `var`/`let` keywords or C typedef lexer hack is needed.
4. Function declarations are keywordless with trailing returns (`fu(a: int): int`); distinguishable from calls by bounded lookahead.
5. Function values are typed by signature — `(int, int): int` — C pointer-to-function syntax never enters the language.
6. Tuple expressions are comma-built at the top level: `return words, null` ≡ `return (words, null)` — parentheses are grouping, required only for nesting (`return (a, b), err`) or clarity. Same rule on assignment sides.
7. Bare comma-separated lists stay on one physical line. A list that crosses a newline must be enclosed by its construct's `()`, `[]`, or `{}` delimiters. Where delimiters are otherwise optional, parentheses are the multiline form: `Circle signs Shape, Color` on one line, or `Circle signs (` followed by the contract list and `)` on later lines. A single-line parenthesized contract is rejected so the surface has one canonical spelling per layout.
8. An array literal may repeat one value with `[value : count]`, as in `prefix: [12]u8 = [0 : 12]`. The surrounding context still supplies the exact array type; `count` is a compile-time constant expression (Syntax & Parsing #15) and must evaluate to that array type's length. `value` is evaluated once and copied into every element in index order. The form allocates nothing and introduces no shared element storage.
9. Every storage declaration states its type explicitly. There is no declaration-elision spelling: a local initialized from an expression is `name: Type = expr`, and every name introduced by a multi-declaration states its own type.
10. Numeric and character literals are untyped constants that take the expected
    type at use and must fit it. Casts are explicit and constructor-shaped
    (`i32(x)`, `Meters(x)`); there are no implicit numeric conversions. An
    untyped constant expression (`1 + 2`, `-0.5`, `1 << 3`) takes the expected
    type as a whole, beside a typed operand as in a declaration; an integer
    expression never becomes floating-point arithmetic (`x != 7 / 2` with `x: f64`
    needs a cast), while a lone integer literal may still meet a float operand.
    Under a float cast, an untyped floating constant expression takes the
    cast's target type (`f32(1.0 / 3.0)` computes at `f32`), and an untyped
    integer constant expression is evaluated as `int` and then converted.
11. A tuple expression may initialize or assign one explicitly stated tuple
    type of the same arity. Each member meets its corresponding member type
    exactly as a struct field initializer does: an untyped constant takes that
    type and must fit it (`pair: (i32, i32) = (1, 2)`), and a typed member must
    match it with no implicit conversion. Construction uses the existing
    positional tuple layout, performs no allocation, and introduces no new
    runtime machinery.
12. `if`, `switch`, and `for` are statements, not expressions, and their
    conditions require `bool`. Compound assignments are pure sugar with the
    left operand evaluated once; `++` and `--` do not exist.
13. A struct field may opt into a compile-time default with `name: Type =
    expression`. A named-field struct literal supplies a field value with
    `name = expression`; `:` is reserved there for declarations and type
    annotations and is rejected as a field-initializer separator. A literal may
    omit exactly the fields with
    declared defaults; every field without one remains required, and `T {}` is
    valid only when every field of `T` has one. An explicit initializer wins.
    Unknown and duplicate fields remain errors, and there is no positional,
    spread, or update literal. Each default is independently checked against
    its declared field type and may use only the existing constant-expression
    surface; it cannot call a function or refer to `self` or another instance
    field. Defaults apply only when constructing a struct literal: they do not
    initialize bare locals, globals, allocations, arrays, or canonical-zero
    values and do not affect layout or ABI. Generic defaults neither infer type
    arguments nor instantiate a generic implicitly; when generic struct
    literals are implemented, substitution follows an explicitly written
    instantiation.
14. A structured error domain keeps one entry per physical line inside its
    braces, with no commas: `code: name = "message"`. The opening brace is
    followed by a newline unless the block is empty.
15. A fixed array type's length `[N]T` is a compile-time constant expression,
    not only a bare integer literal: literals, `const` identifiers (including
    one reached through an imported module, Values & Passing #12),
    `sizeof`/`alignof`/`offsetof`, and integer/bitwise arithmetic over these —
    the same restricted surface `@align(N)` already uses (Type System,
    Carved). The evaluated length must be a positive `usize`; a runtime
    value, a non-positive result, or an expression the compiler cannot
    reduce is a compile-time error. An array literal's repetition count
    (Syntax & Parsing #8) uses the identical surface and must match the
    destination array type's length exactly.

## Names & Scoping
1. Qualification is the primary namespacing mechanism — modules provide what C fakes with prefixes: `parser.init()` instead of `ace_parser_init`; import aliasing resolves module-name collisions. Selective import is the explicit opt-in for bringing particular public spellings into their ordinary namespaces.
2. Two namespaces, separated by grammar position: **types** (type positions: after `:`, in brackets, casts) and **values** (expression positions). A type and a value may share a name — `Point: struct` + `Point(...)` constructor coexist. Position decides; decidable in one in-order check, no inference.
3. The value namespace holds functions, variables, and imports together. Variables and imports still require a unique same-scope name; functions may form an overload set under one name. A function name cannot share its scope with a variable or import.
4. Free-function overload selection is exact and static: argument count, argument types, and `mut` parameter/call-site marks must match one candidate. No implicit conversions, conversion ranking, inheritance conversions, argument-dependent lookup, or return-type-directed call selection exist. Exactly one match is required; zero matches and multiple matches are compile errors.
5. Return types do not distinguish overload declarations. Two declarations with the same parameter types and `mut` positions are duplicates even when their return types differ. `noalias` does not distinguish them because it changes optimizer facts, not the callable shape.
6. Untyped constants remain context-materialized for an ordinary single-signature call. When a real overload set requires selection, an integer/character constant has the natural `int` candidate type and a floating constant has `f64`; an explicit cast selects a narrower or unsigned overload. String and boolean literals already have fixed types. No candidate receives a private expected type during selection.
7. Generic overloads remain explicitly instantiated. `f[int](...)` considers only generic declarations named `f` with the stated type-argument count, substitutes those written types, and then applies the same exact argument rules. A bare generic is never inferred.
8. An overload used as a function value is selected by the explicitly declared function-value type, including its return type; a bare overloaded name without that context is an error. A generic function value names its type arguments (`combine[int]`); a matching specialization wins, and otherwise the same declared type selects among generic overloads with that type-argument count. This is contextual selection of a value, not return-type overloading at a call.
9. Overloading is compile-time only. The selected internal function has a parameter-signature-mangled symbol and calls it directly: no runtime tag, branch, vtable, boxing, allocation, or metadata is introduced. C-boundary `extern "c"` names cannot overload because those names are explicitly unmangled.
10. Shadowing: nearest binding wins — locals may shadow top-level names and imports; debug builds may warn on import shadowing. A local is in scope from its declarator, its own initializer included, so `get: usize = get(a)` names the uninitialized local rather than the function it shadows; this holds in every file, root or imported. A local shadowing a whole-module binding makes `binding.name` in value position a field access on the local; a type position (`binding.Type`, `binding.Type{...}`) still names the module's type. A shadowed function miscalled yields a clear "not callable" error.
11. Type-owned method overloads use the same exact static rule as free functions, with receiver type and receiver `mut` included in the parameter signature. Ambiguous method names through embedding are compile errors and can be resolved with a direct type-qualified call. Methods are not first-class values in v1.
12. Rejected: bare-name fallback resolution (`use(sort)` picking whichever function/variable exists) — meaning would depend on the declaration set, the C++ lookup disease. Same-scope non-function value collisions are a rename.
13. Future valve, if ever needed: a dedicated function-reference sigil (`&sort` = code address) enabling a separate function namespace — additive, not v1.
