# Ace — Grammar

EBNF for the carved language surface (v1). Semantics live in `spec.md`; `[P]` marks productions whose *spelling* is provisional (semantics are final). Nothing here may contradict `spec.md` — where they disagree, `spec.md` wins and this file gets fixed.

## 1. Lexical

```ebnf
comment       := line_comment | block_comment
line_comment  := "//" (any – newline) newline
block_comment := "/*" (any – "*/")* "*/"       // non-nesting; a newline inside acts as a newline
newline       := significant — terminates statements (see §6 Notes)

keyword       := "import" | "pub" | "mut" | "const" | "noalias" | "if" | "else"
               | "for" | "in" | "switch" | "return" | "break" | "continue"
               | "asm" | "extern" | "export" | "interface" | "struct" | "impl"
               | "distinct" | "type" | "enum" | "null" | "true" | "false"

ident         := letter ("letter" | digit | "_")*        [P: exact ident charset]
literal       := int_lit | float_lit | string_lit | char_lit | "null" | "true" | "false"
int_lit       := digit+ ("_" digit+)* | "0x" hex+ | "0b" bin+     [P: suffixes]
float_lit     := digit+ "." digit+ exponent?              [P]
string_lit    := '"' (char – '"' | '\' escape)* '"'       [P: raw strings]
char_lit      := "'" (char – "'" | '\' escape) "'"
```

## 2. Files & declarations

```ebnf
file          := import_decl* top_decl*

import_decl   := "import" ident (":" import_path)?        // alias: import m: math
import_path   := string_lit                               // std bare, local by path

top_decl      := "pub"? top_body
top_body      := var_decl | const_decl | func_decl | struct_decl
               | interface_decl | enum_decl | alias_decl | distinct_decl
               | impl_decl | extern_block

var_decl      := ident ":" type ("=" expr)?               // locals: same form inside blocks
const_decl    := ident ":" "const" type "=" expr

func_decl     := ident type_params? "(" params? ")" ret_clause? block
type_params   := "[" ident ("," ident)* "]"               // max[T]
params        := param ("," param)*
param         := param_mod* ident ":" type
param_mod     := "mut" | "noalias"
ret_clause    := ":" type | ":" "(" type ("," type)+ ")"
block         := "{" stmt* "}"

struct_decl   := ident type_params? ":" "struct" "{" field_list "}"
field_list    := field ("," field)*
field         := ident ":" type

interface_decl:= ident type_params? ":" "interface" "{" method_decl* "}"
method_decl   := "mut"? ident "(" params? ")" ret_clause?     // receiver implied

enum_decl     := ident ":" "enum" "{" ident ("," ident)* ","? "}"

alias_decl    := ident type_params? ":" "type" "=" type
distinct_decl := ident ":" "distinct" type

impl_decl     := ident ":" "impl" "(" ident "," ident ")" "{" impl_entry ("," impl_entry)* ","? "}"
impl_entry    := "mut"? ident ":" ident                   // method-name : function-name

extern_block  := "extern" string_lit "{" extern_decl* "}"
extern_decl   := "export"? ident "(" extern_params? ")" ret_clause?
extern_params := extern_param ("," extern_param)* ("," "...")?
extern_param  := ident ":" type
```

## 3. Statements

```ebnf
stmt          := var_decl | const_decl | assignment | expr_stmt
               | if_stmt | for_stmt | switch_stmt | return_stmt
               | break_stmt | continue_stmt | asm_stmt | labeled_block

assignment    := target ("," target)* "=" expr_list       // n, err = read()
                | target compound_op expr                 // x += y (pure sugar)
compound_op   := "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
target        := ident postfix_part*                      // lvalue: ident, .f, [i], ~x
expr_stmt     := expr

if_stmt       := "if" expr block ("else" (if_stmt | block))?
for_stmt      := "for" ident ":" type "in" iterable block
iterable      := expr | range_expr
range_expr    := expr ".." expr                           [P: step/reverse forms]
switch_stmt   := "switch" expr "{" switch_case* "}"
switch_case   := case_pat block
case_pat      := expr ("," expr)* | "else"                [P]
return_stmt   := "return" expr_list?
break_stmt    := "break" ident?                           // labeled break
continue_stmt := "continue"

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

unary         := ("!" | "~" | "-") unary | "&" lvalue | postfix
//  "!"  logical/bitwise NOT (dual role)     "~"  dereference
//  "&"  address-of (lvalues only)           unary "-" negation

postfix       := primary postfix_part*
postfix_part  := "(" arg_list? ")"                        // call; "mut"? expr args
               | "[" expr "]"                             // index
               | "[" expr? ".." expr? "]"                 [P] slice expression
               | "." ident                                // field
               | "[" type_list "]"                        // generic instantiation — see Notes

arg_list      := arg ("," arg)*
arg           := "mut"? expr                              // call-site mutation mark

primary       := literal | ident | "(" expr_list ")" | array_lit | struct_lit
array_lit     := "[" expr_list? "]"
struct_lit    := "{" field_init ("," field_init)* "}"     [P]
field_init    := ident ":" expr

expr_list     := expr ("," expr)*                         // tuple expression / multi-value
```

## 5. Types

```ebnf
type          := "const"? type_expr                       // limit: const int = 1024
type_expr     := ptr_type | slice_type | array_type | func_type
               | tuple_type | named_type | prim_type
               | type_expr "?"                            // optional postfix

ptr_type      := "mut"? "*" type_expr                     // *Node, mut *Node, *Node?
slice_type    := "[]" type_expr                           // []u8
array_type    := "[" int_lit "]" type_expr                // [16][8]u8
tuple_type    := "(" type ("," type)+ ")"
func_type     := "(" type_list? ")" ":" ret_types          // (int, int): int
named_type    := ident ("." ident)* ("[" type_list "]")?  // Vec[int], math.Vec
prim_type     := "bool" | "i8" | "i16" | "i32" | "i64"
               | "u8" | "u16" | "u32" | "u64"
               | "f32" | "f64" | "string" | "usize" | "typeid"

type_list     := type ("," type)*
ret_types     := type | "(" type ("," type)+ ")"
```

## 6. Notes for the implementer

1. **Newlines terminate statements.** No semicolons. A statement ends at a newline unless the expression is syntactically incomplete (open paren/bracket continues). The BNF above abstracts this as statement separation. Implemented rule (lexer): a newline token is emitted only when the *previous* token can end an expression — identifier, any literal (`null`/`true`/`false` included), `)`, `]`, `}`, `?`, `return`, `break`, `continue` (Go-style semicolon insertion, made explicit). Consequences: parenthesized/bracketed groups continue across lines freely; `else` stays on the `}` line; multi-line comma lists carry trailing commas (`x: f32,`).
2. **Statement-start disambiguation is bounded lookahead (LL(2)), not backtracking:**
   - `ident ":"` → declaration; `ident "="` or `ident ","` → assignment/destructuring; otherwise → expression statement.
   - `ident "(" … ")" ":"` or `… "{"` → function declaration; otherwise → call expression.
3. **`foo[bar]` index vs instantiation:** index arguments are expressions, instantiation arguments are types. Both start with `ident`. Resolve by the name's namespace (generics live in the type/value namespace as declared) — one symbol-table lookup, no backtracking. Where truly ambiguous at parse time, parse as expression and re-check in semantic pass. [P]
4. **Tuple vs function type:** in type position, `"(" … ")"` followed by `":"` is a function type; otherwise a tuple.
5. **Comparison chaining is a distinct production** (`cmp_expr` with `+`): `a < b < c` is one grammar node, not nested binaries; each operand evaluates once.
6. **`&mut q` / `mut &q`** (write-capable address-of) is still `[P]` — parked in spec.md (Names/Mutability).
7. **Keywords reserved but unused in v1 grammar:** `noalias` appears only as a parameter modifier; `export` only in extern blocks and on declarations (`pub export`).
8. **Trailing commas are allowed in every comma list** (params, fields, enum members, impl entries, call args, array/struct literals, type lists) — under the newline rule they are how lists span lines.
9. **`?` binds loosest in types:** `*T?` is a nullable pointer, `[]T?` an optional slice (spec.md Memory #6 wins). Pointer-to-optional is written `*(T?)`.
10. **Struct literals are suppressed where `{` would open a block** — statement starts and if/for/switch heads (Go's composite-literal rule); parenthesize to use a literal there. `Type{...}` postfix and bare `{...}` in expression contexts both parse.
11. **Operator precedence is the C table**, including the bitwise/shift levels shown above; asm `in:` uses the `in` keyword as the binding kind, and asm register values may be immediates (`x16 = 3`).
