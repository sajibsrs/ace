# Ace — Grammar

EBNF for the carved language surface (v1). Semantics live in `SPEC.md`; `[P]` marks productions whose *spelling* is provisional (semantics are final). Nothing here may contradict `SPEC.md` — where they disagree, `SPEC.md` wins and this file gets fixed.

## 1. Lexical

```ebnf
comment       := line_comment | block_comment
line_comment  := "//" (any – newline) newline
block_comment := "/*" (any – "*/")* "*/"       // non-nesting; a newline inside acts as a newline
newline       := significant — terminates statements (see §6 Notes)

keyword       := "import" | "from" | "pub" | "mut" | "const" | "noalias" | "if" | "else"
                | "for" | "in" | "switch" | "return" | "break" | "continue" | "defer"
                | "asm" | "extern" | "export" | "contract" | "struct" | "embed" | "signs"
                | "specialized" | "intrinsic" | "distinct" | "type" | "enum" | "union" | "null" | "true" | "false"

ident         := letter ("letter" | digit | "_")*        [P: exact ident charset]
literal       := int_lit | float_lit | string_lit | char_lit | "null" | "true" | "false"
int_lit       := digit+ ("_" digit+)* | "0x" hex+ | "0b" bin+     [P: suffixes]
float_lit     := digit+ "." digit+ [exponent]             [P]
string_lit    := '"' (char – '"' | '\' escape)* '"'       [P: raw strings]
char_lit      := "'" (byte – "'" | '\' escape) "'"       // exactly one byte
```

## 2. Files & declarations

```ebnf
file          := attributed_decl*
attributed_decl := [when_attr] (import_decl | top_decl)

when_attr     := "@" contextual_when "(" when_expr ")" newline*
contextual_when := ident                              // exact text "when"
when_expr     := when_or
when_or       := when_and (contextual_or when_and)*
when_and      := when_not (contextual_and when_not)*
when_not      := contextual_not when_not | when_primary
when_primary  := "true" | "false"
               | contextual_target ("==" | "!=") string_lit
               | "(" when_expr ")"
contextual_or := ident                                // exact text "or"
contextual_and := ident                               // exact text "and"
contextual_not := ident                               // exact text "not"
contextual_target := ident                            // exact text "target"

import_decl   := module_import | selective_import
module_import := "import" ident [":" (ident | import_path | internal_import)]
                                                          // import math; import m: math
                                                          // import util: "./util.ace"
selective_import := "import" import_names "from" import_source
import_names  := ident ("," ident)*
               | "(" ident ("," ident)* [","] ")"       // parenthesized = multiline
import_source := ident | import_path | internal_import     // std, local, or reserved
import_path   := string_lit
internal_import := "@" ident "." "ace"                  // compiler-reserved std implementation

top_decl      := ["pub"] top_body
top_body      := var_decl | const_decl | func_decl | struct_decl
               | contract_decl | enum_decl | union_decl | alias_decl | distinct_decl
               | error_domain_decl | method_func_decl | signing_decl | extern_block

binding       := ident ":" type
eq_name       := ident [":" type]                       // typed declares; bare reassigns — Note #28
var_decl      := binding ["=" expr]                       // global or local typed binding
elided_decl   := ident ":=" expr                          // local only; one binding, independently typed RHS
multi_decl    := eq_name ("," eq_name)+ "=" expr_list      // local per-name declare/assign; one line
const_decl    := ident ":" "const" type "=" expr

func_decl     := ident [bracket_args] "(" [params] ")" [ret_clause]
                 (block | "specialized" | "intrinsic")
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

enum_decl     := ident ":" "enum" [enum_backing] "{" enum_members "}"
enum_backing  := type                                  // sema: fixed-width integer or alias
enum_members  := enum_member ("," enum_member)* [","]
enum_member   := ident ["=" expr]
union_decl    := ident [type_params] ":" "union" "{" union_variants "}"
union_variants := union_variant ("," union_variant)* [","]
union_variant := ident [":" type]                    // zero or one payload

alias_decl    := ident [type_params] ":" "type" "=" type
distinct_decl := ident ":" "distinct" type
error_domain_decl := ident ":" contextual_error             // pub OpenErr: error
contextual_error := ident                                    // exact text "error"; not reserved

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
stmt          := var_decl | elided_decl | multi_decl | const_decl | assignment | expr_stmt
               | if_stmt | for_stmt | switch_stmt | return_stmt
               | break_stmt | continue_stmt | fallthrough_stmt | defer_stmt
               | asm_stmt | labeled_block | static_assert_stmt

static_assert_stmt := "static_assert" "(" expr ")"

assignment    := target ("," target)* "=" expr_list       // n, err = read()
                | target compound_op expr                 // x += y (pure sugar)
compound_op   := "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
target        := ident postfix_part*                      // lvalue: ident, .f, [i], ~x
expr_stmt     := expr

if_stmt       := "if" expr block ["else" (if_stmt | block)]
for_stmt      := "for" for_head block
for_head      := loop_binders "in" ["mut"] iterable    // sequence/range head
               | [for_init] ";" [expr] ";" [for_post]  // three-clause head
               | expr                                 // condition-only head
               |                                      // infinite head
for_init      := var_decl | elided_decl | assignment | expr_stmt
for_post      := assignment | expr_stmt
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
continue_stmt := "continue" [ident]                      // nearest loop, or an enclosing label
defer_stmt    := "defer" ["mut"] expr                   // operand must be a call (semantic rule)
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

unary         := ("!" | "~" | "-") unary | "&" lvalue | "mut" "&" lvalue | "mut" postfix | postfix
//  "!"  logical/bitwise NOT (dual role)     "~"  dereference
//  "&"  read-only address-of                "mut &" writable address-of
//  "mut p" writable-view establishment (slicing operands only — Note 25)
//  both address forms and establishment require writable lvalues   unary "-" negation

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
               | tuple_type | string_type | named_type | prim_type

ptr_type      := ["mut"] "*" type_expr                    // *Node, mut *Node
slice_type    := ["mut"] "[]" type_expr                   // []u8, mut []u8
string_type   := ["mut"] "string"                         // string, mut string
array_type    := "[" int_lit "]" type_expr                // [16][8]u8
tuple_type    := "(" type ("," type)+ ")"
func_type     := "(" [type_list] ")" ":" ret_types         // (int, int): int
named_type    := ident ("." ident)* ["[" type_list "]"]   // Vec[int], math.Vec
prim_type     := "bool" | "i8" | "i16" | "i32" | "i64"
               | "u8" | "u16" | "u32" | "u64"
               | "f32" | "f64" | "float" | "char" | "usize" | "typeid"

type_list     := type ("," type)*
ret_types     := type | "(" type ("," type)+ ")"
```

## 6. Parser notes

Semantics and restrictions belong in `SPEC.md`; these notes record only parsing
choices that are not evident from the productions.

1. Newlines terminate statements using lexer insertion after tokens that can end
   an expression. Incomplete expressions suppress insertion; `for`-head
   semicolons are the only semicolons.
2. Statement starts use bounded lookahead: `ident :` begins a declaration or
   label, `ident :=` an elided local declaration, and `ident =`/`ident ,`
   assignment. Function declarations are distinguished from calls by their
   trailing return clause or body.
3. Bare comma lists are single-line. Delimited lists may span lines and accept a
   trailing comma; signing and selective-import parentheses are the multiline
   form only.
4. Indexing and generic instantiation share brackets. Resolve them from the
   symbol namespace; when parsing cannot decide, parse an expression and let
   semantic analysis reclassify/check it.
5. In type position, `(...):` is a function type; otherwise a multi-element
   parenthesized type is a tuple.
6. Comparison chains form one node and evaluate each operand once.
7. Struct literals are suppressed where `{` opens a statement body; use
   parentheses to force a literal in a control-flow head.
8. Function declaration brackets contain bare identifiers and are interpreted
   after name resolution as type parameters or specialization arguments.
9. `intrinsic` is a line-final body substitute accepted syntactically on an
   ordinary function declaration. Semantic analysis restricts it to exact
   compiler-provided standard-library declarations.
9. Contextual words such as `error`, `step`, and `fallthrough` remain
   identifiers outside their exact grammatical context.
10. `mut` uses local lookahead: `mut &` starts writable address-of; in a type
    before `*`, `[]`, or `string` it qualifies that view; otherwise parsing
    admits the relevant expression/call-site form and semantic analysis enforces
    the normative permission rules.
11. Calls spelled `sizeof(` or `alignof(` parse their sole operand with the full
    type grammar.
12. `offsetof(` parses a type, comma, and dot-separated field-name path.
    `static_assert` is additionally admitted in the top-level declaration list.
