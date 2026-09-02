# Ace — Grammar

EBNF for the carved language surface (v1). Semantics live in `spec.md`; `[P]` marks productions whose *spelling* is provisional (semantics are final). Nothing here may contradict `spec.md` — where they disagree, `spec.md` wins and this file gets fixed.

## 1. Lexical

```ebnf
comment       := line_comment | block_comment
line_comment  := "//" (any – newline) newline
block_comment := "/*" (any – "*/")* "*/"       // non-nesting; a newline inside acts as a newline
newline       := significant — terminates statements (see §6 Notes)

keyword       := "import" | "from" | "pub" | "mut" | "const" | "noalias" | "if" | "else"
                | "for" | "in" | "switch" | "return" | "break" | "continue"
                | "asm" | "extern" | "export" | "contract" | "struct" | "embed" | "signs"
                | "specialized" | "distinct" | "type" | "enum" | "union" | "null" | "true" | "false"

ident         := letter ("letter" | digit | "_")*        [P: exact ident charset]
literal       := int_lit | float_lit | string_lit | char_lit | "null" | "true" | "false"
int_lit       := digit+ ("_" digit+)* | "0x" hex+ | "0b" bin+     [P: suffixes]
float_lit     := digit+ "." digit+ [exponent]             [P]
string_lit    := '"' (char – '"' | '\' escape)* '"'       [P: raw strings]
char_lit      := "'" (byte – "'" | '\' escape) "'"       // exactly one byte
```

## 2. Files & declarations

```ebnf
file          := import_decl* top_decl*

import_decl   := module_import | selective_import
module_import := "import" ident [":" (ident | import_path)]
                                                          // import math; import m: math
                                                          // import util: "./util.ace"
selective_import := "import" import_names "from" import_source
import_names  := ident ("," ident)*
               | "(" ident ("," ident)* [","] ")"       // parenthesized = multiline
import_source := ident | import_path                       // std name or local path
import_path   := string_lit

top_decl      := ["pub"] top_body
top_body      := var_decl | const_decl | func_decl | struct_decl
               | contract_decl | enum_decl | union_decl | alias_decl | distinct_decl
               | method_func_decl | signing_decl | extern_block

binding       := ident ":" type
var_decl      := binding ["=" expr]                       // global or local single binding
multi_decl    := binding ("," binding)+ "=" expr_list      // local typed multi-declaration; one line
const_decl    := ident ":" "const" type "=" expr

func_decl     := ident [bracket_args] "(" [params] ")" [ret_clause] (block | "specialized")
                                                          // brackets = type params or specialization args — Notes #23
method_func_decl := ["mut"] owner_type "." ident "(" [params] ")" [ret_clause] block
owner_type    := ident [type_params]                        // declares owner T names
type_params   := "[" ident ("," ident)* "]"               // max[T] — type declarations always declare params
params        := param ("," param)*
param         := param_mod* ident ":" type
param_mod     := "mut" | "noalias"
ret_clause    := ":" type | ":" "(" type ("," type)+ ")"
block         := "{" stmt* "}"

struct_decl   := ident [type_params] ":" "struct" "{" field_list "}"
field_list    := field ("," field)*
field         := ["embed"] ident ":" type

contract_decl := ident [type_params] ":" "contract" "{" method_decl* "}"
method_decl   := ["mut"] ident "(" [params] ")" [ret_clause]  // receiver implied

enum_decl     := ident ":" "enum" "{" ident ("," ident)* [","] "}"
union_decl    := ident [type_params] ":" "union" "{" union_variants "}"
union_variants := union_variant ("," union_variant)* [","]
union_variant := ident [":" type]                    // zero or one payload

alias_decl    := ident [type_params] ":" "type" "=" type
distinct_decl := ident ":" "distinct" type

signing_decl  := owner_type "signs" (type_list | "(" type_list [","] ")")
                                                          // bare = one line; parens = multiline

extern_block  := "extern" string_lit "{" extern_decl* "}"
extern_decl   := ["export"] ident "(" [extern_params] ")" [ret_clause]
                 ["=" string_lit]                        // nonempty, NUL-free link name; v1 max 510 bytes
extern_params := extern_param ("," extern_param)* ["," "..."]
extern_param  := ident ":" type
```

## 3. Statements

```ebnf
stmt          := var_decl | multi_decl | const_decl | assignment | expr_stmt
               | if_stmt | for_stmt | switch_stmt | return_stmt
               | break_stmt | continue_stmt | fallthrough_stmt | asm_stmt | labeled_block

assignment    := target ("," target)* "=" expr_list       // n, err = read()
                | target compound_op expr                 // x += y (pure sugar)
compound_op   := "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
target        := ident postfix_part*                      // lvalue: ident, .f, [i], ~x
expr_stmt     := expr

if_stmt       := "if" expr block ["else" (if_stmt | block)]
for_stmt      := "for" loop_binders "in" iterable block
loop_binders  := loop_binder | loop_binder "," loop_binder
loop_binder   := ident ":" type
iterable      := expr | range_expr
range_expr    := expr ".." expr [contextual_step expr]
contextual_step := ident                                  // exact text "step"; not reserved
switch_stmt   := "switch" expr "{" switch_case* "}"
switch_case   := case_pat block
case_pat      := union_pattern | expr ("," expr)* | "else" [P]
union_pattern := named_type "." ident
               | named_type "." ident "(" ident ":" type ")"
return_stmt   := "return" [expr_list]
break_stmt    := "break" [ident]                         // labeled break
continue_stmt := "continue" [ident]                      // optional enclosing block label
fallthrough_stmt := contextual_fallthrough                // only as final statement of a non-final switch case
contextual_fallthrough := ident                            // exact text "fallthrough"; not reserved

labeled_block := ident ":" block                          // scan: { … break scan }

asm_stmt      := "asm" string_lit "{" asm_binding* "}"
asm_binding   := ("in" | "out" | "clobbers") ":" reg_bind ("," reg_bind)*
reg_bind      := ident "=" ident                          // x0 = fd
```

## 4. Expressions

Precedence, loosest to tightest; all binary levels left-associative **except** comparison, which is a non-associative chain (`lo <= x <= hi`, each operand evaluated once).

```ebnf
expr          := or_expr
or_expr       := and_expr ("||" and_expr)*
and_expr      := bitor_expr ("&&" bitor_expr)*
bitor_expr    := bitxor_expr ("|" bitxor_expr)*
bitxor_expr   := bitand_expr ("^" bitand_expr)*
bitand_expr   := eq_expr ("&" eq_expr)*
eq_expr       := cmp_expr (("==" | "!=") cmp_expr)*
cmp_expr      := shift_expr (("<" | "<=" | ">" | ">=") shift_expr)+
shift_expr    := add_expr (("<<" | ">>") add_expr)*
add_expr      := mul_expr (("+" | "-") mul_expr)*
mul_expr      := unary (("*" | "/" | "%") unary)*

unary         := ("!" | "~" | "-") unary | "&" lvalue | "mut" "&" lvalue | postfix
//  "!"  logical/bitwise NOT (dual role)     "~"  dereference
//  "&"  read-only address-of                "mut &" writable address-of
//  both address forms require lvalues        unary "-" negation

postfix       := primary postfix_part*
postfix_part  := "(" [arg_list] ")"                       // call; ["mut"] expr args
               | "[" expr "]"                             // index
               | "[" [expr] ".." [expr] "]"               [P] slice expression
               | "." ident                                // field
               | "[" type_list "]"                        // generic instantiation — see Notes

arg_list      := arg ("," arg)*
arg           := ["mut"] expr                             // call-site mutation mark

primary       := literal | ident | "(" expr_list ")" | array_lit | struct_lit
array_lit     := "[" [expr_list | array_repeat] "]"
array_repeat  := expr ":" int_lit                       // [u8(0) : 12]; exact contextual array length
struct_lit    := "{" field_init ("," field_init)* "}"     [P]
field_init    := ident ":" expr

expr_list     := expr ("," expr)*                         // tuple expression / multi-value
```

## 5. Types

```ebnf
type          := ["const"] type_expr                      // limit: const int = 1024
type_expr     := ptr_type | slice_type | array_type | func_type
               | tuple_type | named_type | prim_type

ptr_type      := ["mut"] "*" type_expr                    // *Node, mut *Node
slice_type    := "[]" type_expr                           // []u8
array_type    := "[" int_lit "]" type_expr                // [16][8]u8
tuple_type    := "(" type ("," type)+ ")"
func_type     := "(" [type_list] ")" ":" ret_types         // (int, int): int
named_type    := ident ("." ident)* ["[" type_list "]"]   // Vec[int], math.Vec
prim_type     := "bool" | "i8" | "i16" | "i32" | "i64"
               | "u8" | "u16" | "u32" | "u64"
               | "f32" | "f64" | "string" | "char" | "usize" | "typeid"

type_list     := type ("," type)*
ret_types     := type | "(" type ("," type)+ ")"
```

## 6. Notes for the implementer

1. **Newlines terminate statements.** No semicolons. A statement ends at a newline unless the expression is syntactically incomplete. The BNF above abstracts this as statement separation. Implemented rule (lexer): a newline token is emitted only when the *previous* token can end an expression — identifier, any literal (`null`/`true`/`false` included), `)`, `]`, `}`, `return`, `break`, `continue` (Go-style semicolon insertion, made explicit). The parser additionally rejects any bare comma-separated list that crosses a physical line: multiline lists must live inside `()`, `[]`, or `{}` and carry continuation/trailing commas as needed. `else` stays on the `}` line.
2. **Statement-start disambiguation is bounded lookahead (LL(2)), not backtracking:**
   - `ident ":"` → declaration; `ident "="` or `ident ","` → assignment/destructuring; otherwise → expression statement.
   - `ident "(" … ")" ":"` or `… "{"` → function declaration; otherwise → call expression.
3. **`foo[bar]` index vs instantiation:** index arguments are expressions, instantiation arguments are types. Both start with `ident`. Resolve by the name's namespace (generics live in the type/value namespace as declared) — one symbol-table lookup, no backtracking. Where truly ambiguous at parse time, parse as expression and re-check in semantic pass. [P]
4. **Tuple vs function type:** in type position, `"(" … ")"` followed by `":"` is a function type; otherwise a tuple.
5. **Comparison chaining is a distinct production** (`cmp_expr` with `+`): `a < b < c` is one grammar node, not nested binaries; each operand evaluates once.
6. **Writable address-of is `mut &q`.** It produces `mut *T` from a writable lvalue; `&q` produces read-only `*T`. In a call argument, `mut &q` is the unary address expression, not an `N_MUTARG` wrapper; an address expression is not itself a valid mut argument. Each pointer level owns its `mut`: `mut **T`, `*mut *T`, and `mut *mut *T` grant different immediate writes.
7. **Keywords reserved but unused in v1 grammar:** `noalias` appears only as a parameter modifier; `export` only in extern blocks and on declarations (`pub export`).
8. **Bare comma lists are single-line.** Delimited lists (params, fields, enum members, call args, array/struct literals, type lists) may span lines and allow trailing commas. Signing and selective-import lists use their bare forms on one line (`Circle signs Shape, Color`; `import min, max from math`) and parenthesized forms across lines; single-line parentheses are rejected as redundant.
   Typed multi-declarations are also bare lists and stay on one physical line:
   `number: int, message: string = 1, "old"`.
9. **Struct literals are suppressed where `{` would open a block** — statement starts and if/for/switch heads (Go's composite-literal rule); parenthesize to use a literal there. `Type{...}` postfix and bare `{...}` in expression contexts both parse.
10. **Operator precedence is the C table**, including the bitwise/shift levels shown above; asm `in:` uses the `in` keyword as the binding kind, and asm register values may be immediates (`x16 = 3`).
11. **Method declarations are type-owned.** `self` is inserted contextually as a read-only receiver; leading `mut` grants writes. A leading `pub` must precede `mut`. In expressions, `Type.method(...)` is a direct call, but bare `Type.method` is not a method value.
12. **Intrinsic `.len` is sequence-only.** The ordinary `"." ident` postfix carries it syntactically, while semantics admit the intrinsic only for arrays (constant element count), slices (stored element count), and strings (stored byte count). Other types have no intrinsic `.len`; a struct may independently declare an ordinary field with that name.
13. **Selective imports preserve the namespaces.** Each selected spelling imports every public declaration of that name from its source module: a function's complete public overload set enters the value namespace, while types enter the type namespace. Same-namespace import/declaration collisions are errors; nested lexical bindings may shadow selected values normally.
14. **Range endpoint order selects direction.** `a .. b` captures `a` then `b` once and traverses `[min(a,b), max(a,b))` in their written direction: `0 .. 4` yields `0,1,2,3`, while `4 .. 0` yields `3,2,1,0`. Equal endpoints are empty. After the second endpoint, an identifier token whose exact text is `step` introduces the once-evaluated positive step expression; `step` remains bindable everywhere, including as that expression.
15. **Sequence binders are arity-directed.** One binder is the element value. Two binders are index then element value; the index must be `usize`, and both are fresh per-iteration locals whose mutation cannot change loop progress. Ranges accept exactly one binder. There is no discard binder: use the one-binder form when no index is wanted.
16. **Labeled continue restarts blocks.** Plain `continue` targets the nearest loop. `continue name` targets only an enclosing labeled block and branches to its beginning; `break name` branches after it. Falling through the block still exits normally. These enclosing-only transfers provide structured repetition without `goto`.
17. **Tagged-union cases are patterns, not calls.** `Event.key(code: u32)` in a union switch binds an explicitly typed copy of the active payload; the same shape in expression position constructs a value. The scrutinee's union type selects pattern parsing/semantic checking, every variant must appear once unless `else` exists, and inactive payload access has no expression form.
18. **Array repetition remains explicitly typed by context.** `[value : count]` is an array literal only; `count` is a nonnegative integer literal equal to the expected `[N]T` length. `value` is checked as `T`, evaluated once, then copied to all `N` elements. The form neither infers a declaration type nor allocates.
19. **Byte sequences compare pairwise.** `==` and `!=` admit every combination of `[N]u8`, `[]u8`, and `string` in either operand order, including `[N]u8` against `[M]u8`. They compare complete lengths and exact stored bytes without conversion, allocation, decoding, or normalization. Arrays and slices of another element type do not receive this mixed-type rule.
20. **Embedding is explicit and one-level.** `embed name: Base` is an ordinary named struct field whose owned methods may be promoted to the containing struct. Direct outer methods win, multiple exact promoted matches are ambiguous, and fields are not promoted. Contract conformance may flow from a directly embedded base to its embedder, never in reverse.
21. **`char` is byte-sized.** It is a predeclared transparent alias of `u8`, not a decoded Unicode scalar. Character literals contain exactly one byte; strings carry UTF-8 bytes and retain byte indexing and iteration. A future `rune` and explicit UTF-8 conversion are outside this grammar.
22. **`sizeof` accepts the full type grammar.** Its parenthesized operand may be an inline slice or array type (`sizeof([]u8)`, `sizeof([3]char)`) as well as a named or primitive type. The result is compile-time-only.
23. **Function brackets are interpreted, not parsed.** Brackets after a function name hold bare identifiers only (`bracket_args`), so parsing never decides their meaning. Brackets whose identifiers resolve to declared types are specialization arguments and must follow a declared generic of that name; identifiers resolving to no declared type start a fresh type-parameter list, so one name may declare several generic overloads (as `str.parse[T]` does) before any specialization. A specialization's parameter and return types must equal the substituted generic signature with all type parameters supplied positionally, and it must appear after its generic — type-naming brackets with no preceding generic are an error, never a silent new parameter list. Compound type arguments (`[]u8`, `*Node`, `Pair[int, string]`) are an additive relaxation; v1 specializes over bare named types only. `specialized` replaces the body of a template that admits no call without an exact specialization; a specialization always carries a real body.
