# Ace — Grammar

EBNF for the carved language surface (v1) and the separate `ace.pkg` package-manifest format (§7). Semantics live in `spec/`; `[P]` marks productions whose *spelling* is provisional (semantics are final). Nothing here may contradict `spec/` — where they disagree, `spec/` wins and this file gets fixed.

## 1. Lexical

```ebnf
comment       := line_comment | block_comment
line_comment  := "//" (any – newline) newline
block_comment := "/*" (any – "*/")* "*/"       // non-nesting; a newline inside acts as a newline
newline       := significant — terminates statements (see §6 Notes)

keyword       := "import" | "pub" | "mut" | "const" | "noalias" | "if" | "else"
                | "for" | "in" | "switch" | "default" | "return" | "break" | "continue" | "defer"
                | "asm" | "extern" | "contract" | "struct" | "embed" | "signs"
                | "specialized" | "intrinsic" | "inline" | "distinct" | "type" | "enum" | "union" | "null" | "true" | "false"
                | "volatile"

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
attributed_decl := [when_attr] (layout_attr | cold_attr)* (import_decl | top_decl)

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
layout_attr   := packed_attr | align_attr
packed_attr   := "@" contextual_packed newline*
align_attr    := "@" contextual_align "(" expr ")" newline*
offset_attr   := "@" contextual_offset "(" expr ")" newline*
cold_attr     := "@" contextual_cold newline*           // function forms only (spec Functions & ABI #10)
contextual_packed := ident                            // exact text "packed"
contextual_align := ident                             // exact text "align"
contextual_offset := ident                            // exact text "offset"
contextual_cold := ident                              // exact text "cold"

import_decl   := module_import | selective_import
module_import := "import" module_path
               | "import" ident ":" (module_path | import_path | internal_import)
                                                          // import math; import net/tcp
                                                          // import m: math; import bla: net/tcp
                                                          // import util: "./util.ace"
selective_import := "import" import_names contextual_from import_source
contextual_from := ident                                  // exact text "from"; not reserved
import_names  := ident ("," ident)*
               | "(" ident ("," ident)* [","] ")"       // parenthesized = multiline
import_source := module_path | import_path | internal_import     // std, local, or reserved
module_path   := ident ("/" ident)*             // net/tcp; only the leaf binds
import_path   := string_lit                     // "./util.ace" or "../A/B" (no .ace = nested)
internal_import := "@" ident                              // compiler-reserved std implementation

top_decl      := ["pub"] ["inline"] top_body                 // "inline": function forms only (spec Functions & ABI #8)
top_body      := var_decl | const_decl | func_decl | struct_decl
               | static_map_decl
               | bits_decl | contract_decl | enum_decl | union_decl | alias_decl | distinct_decl
               | error_domain_decl | method_func_decl | assoc_func_decl | signing_decl | extern_block
               | extern_def

binding       := ident ":" type
eq_name       := ident [":" type]                       // typed declares; bare reassigns — Note #28;
                                                          // bare `_` drops its value (Errors #12)
var_decl      := binding ["=" expr]                       // global or local typed binding
static_map_decl := ident ":" static_map_type "=" static_map_lit // module-level only
multi_decl    := eq_name ("," eq_name)+ "=" expr_list      // local per-name declare/assign; one line
const_decl    := ident ":" "const" type "=" expr

func_decl     := ident [bracket_args] "(" [params] ")" [ret_clause]
                 (block | "specialized" | "intrinsic")
                                                          // brackets = type params or specialization args — Notes #23
method_func_decl := ["mut"] owner_type "." ident "(" [params] ")" [ret_clause] block
assoc_func_decl := owner_type "::" ident "(" [params] ")" [ret_clause] block
                                                          // no implicit self; no leading "mut" —
                                                          // called only as Type::name(...), never through a value
owner_type    := ident [type_params]                        // declares owner T names
type_params   := "[" ident ("," ident)* "]"               // max[T] — type declarations always declare params
params        := param ("," param)* [","]
param         := param_mod* ident ":" type
param_mod     := "mut" | "noalias"
ret_clause    := ":" type                                  // omitted for no value
block         := "{" stmt* "}"

struct_decl   := ident [type_params] ":" "struct" "{" field_list "}"
field_list    := field ("," field)*
field         := [offset_attr] [align_attr] ["embed"] ident ":" type ["=" expr]

bits_decl     := ident ":" contextual_bits bits_backing "{" bits_fields "}"
contextual_bits := ident                                // exact text "bits"; not reserved
bits_backing  := "u8" | "u16" | "u32" | "u64"
bits_fields   := bits_field ("," bits_field)* [","]
bits_field    := ident ":" [contextual_signed] int_lit // `_` is anonymous
contextual_signed := ident                              // exact text "signed"; not reserved

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
error_domain_decl := ident ":" contextual_error ["{" newline* error_entry* "}"]
error_entry   := signed_int_lit ":" ident "=" string_lit newline+
signed_int_lit := ["-"] int_lit
contextual_error := ident                                    // exact text "error"; not reserved

signing_decl  := owner_type "signs" (type_list | "(" type_list [","] ")")
                                                          // bare = one line; parens = multiline

extern_block  := "extern" string_lit "{" extern_decl* "}"
extern_decl   := extern_func | extern_var | extern_global | func_decl
                                                          // func_decl: body = defined in Ace, C-visible; link_name may follow its ret_clause
extern_def    := "extern" string_lit (func_decl | extern_global) // single definition (C Interop #2, #6)
extern_global := ["mut"] ident ":" type [link_name] "=" expr          // C global defined in Ace; read-only unless mut
extern_func   := ident "(" [extern_params] ")" [ret_clause] [link_name]
extern_var    := ["mut"] ident ":" type [link_name]       // C global; read-only unless mut (C Interop #6)
link_name     := contextual_as string_lit                 // nonempty, NUL-free; v1 max 510 bytes
contextual_as := ident                                    // exact text "as"; not reserved
extern_params := extern_param ("," extern_param)* ["," ["..."]]
extern_param  := ident ":" type
```

## 3. Statements

```ebnf
stmt          := var_decl | multi_decl | const_decl | assignment | expr_stmt
               | if_stmt | for_stmt | switch_stmt | return_stmt
               | break_stmt | continue_stmt | fallthrough_stmt | defer_stmt
               | asm_stmt | labeled_block | static_assert_stmt

static_assert_stmt := "static_assert" "(" expr ")"

assignment    := lvalue ("," lvalue)* "=" expr_list       // n, err = read()
                | lvalue compound_op expr                 // x += y (pure sugar)
                | "volatile" lvalue "=" expr              // exactly one final scalar store
compound_op   := "+=" | "-=" | "*=" | "/=" | "%=" | "&=" | "|=" | "^=" | "<<=" | ">>="
target        := ident postfix_part*                      // lvalue: ident, .f, [i]
lvalue        := target | "*" unary                       // semantic analysis requires addressable storage
expr_stmt     := expr

if_stmt       := "if" expr block ["else" (if_stmt | block)]
for_stmt      := "for" for_head block
for_head      := loop_binders "in" ["mut"] iterable    // sequence/range head
               | [for_init] ";" [expr] ";" [for_post]  // three-clause head
               | expr                                 // condition-only head
               |                                      // infinite head
for_init      := var_decl | assignment | expr_stmt
for_post      := assignment | expr_stmt
loop_binders  := loop_binder | loop_binder "," loop_binder
loop_binder   := ident ":" type
iterable      := expr | range_expr
range_expr    := expr ".." expr [contextual_step expr]
contextual_step := ident                                  // exact text "step"; not reserved
switch_stmt   := "switch" expr "{" switch_case* [default_case] "}"
switch_case   := case_pat block
default_case  := "default" block                          [P]
case_pat      := union_pattern | expr ("," expr)*
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

unary         := ("*" | "!" | "~" | "-") unary | "&" lvalue | "mut" "&" lvalue | "mut" postfix
               | "volatile" lvalue | postfix
//  "*"  pointer dereference                 "!"  logical negation
//  "~"  integer bitwise complement
//  "&"  read-only address-of                "mut &" writable address-of
//  "mut p" receiver-marked method call or writable-view establishment
//  (slicing operands only for establishment — Note 25)
//  "volatile place" exactly one final scalar load
//  both address forms and establishment require writable lvalues   unary "-" negation

postfix       := primary postfix_part*
postfix_part  := "(" [arg_list] ")"                       // call; ["mut"] expr args
               | "[" expr "]"                             // index
               | "[" [expr] ".." [expr] "]"               [P] slice expression
               | "." ident                                // field or built-in sequence .len
               | "::" ident                               // associated-function reference (owner must be a type)
               | "[" type_args "]"                        // generic instantiation — see Notes

arg_list      := arg ("," arg)*
arg           := ["mut"] expr                             // call-site mutation mark

primary       := literal | ident | location | "(" expr_list ")" | array_lit | struct_lit
               | ptr_type | slice_type
location      := "@" ident                                // exact text "file" or "line"
array_lit     := "[" [expr_list | array_repeat] "]"
array_repeat  := expr ":" expr                          // [0 : 12]; exact contextual array length
struct_lit    := "{" [field_init ("," field_init)*] "}"   [P]
field_init    := ident "=" expr
static_map_lit := "{" [map_entry ("," map_entry)* [","]] "}"
map_entry     := expr ":" expr

expr_list     := expr ("," expr)*                         // tuple expression / multi-value
```

## 5. Types

```ebnf
type          := ["const"] type_expr                      // limit: const int = 1024
type_expr     := ptr_type | slice_type | array_type | func_type | c_func_type
               | tuple_type | string_type | static_map_type | named_type | prim_type

ptr_type      := ["mut"] "*" type_expr                    // *Node, mut *Node
slice_type    := ["mut"] "[]" type_expr                   // []u8, mut []u8
string_type   := ["mut"] "string"                         // string, mut string
array_type    := "[" expr "]" type_expr                   // [16][8]u8; N is a compile-time constant expression
tuple_type    := "(" type ("," type)+ ")"
func_type     := "(" [type_list] ")" ":" func_ret          // (int, int): int, (int): void
c_func_type   := "extern" string_lit "(" [type_list] ")" ":" func_ret
                                                          // semantic ABI string: exactly "c"
named_type    := ident ("." ident)* ["[" type_args "]"]   // Vec[int], math.Vec
static_map_type := "StaticMap" "[" type "]" type           // StaticMap[K]V
prim_type     := "bool" | "i8" | "i16" | "i32" | "i64"
               | "u8" | "u16" | "u32" | "u64"
               | "f32" | "f64" | "float" | "char" | "usize" | "typeid"

type_list     := type ("," type)*
type_args     := type_arg ("," type_arg)*
type_arg      := type | "void"                             // Task[void]
func_ret      := type | "void"                             // void: no value
```

## 6. Parser notes

Semantics and restrictions belong in `spec/`; these notes record only parsing
choices that are not evident from the productions.

1. Newlines terminate statements using lexer insertion after tokens that can end
   an expression. Incomplete expressions suppress insertion; `for`-head
   semicolons are the only semicolons.
2. Statement starts use bounded lookahead: `ident :` begins a label or typed
   declaration, and `ident =`/`ident ,` begins assignment. Function declarations
   are distinguished from calls by their trailing return clause or body.
3. Bare comma lists are single-line. Delimited lists may span lines and accept a
   trailing comma; canonical formatting keeps a list on one line while it fits
   the configured 120-column limit, then uses one item per line with a trailing
   comma. Signing and selective-import parentheses are the multiline form only.
4. Indexing and generic instantiation share brackets. Resolve them from the
   symbol namespace; when parsing cannot decide, parse an expression and let
   semantic analysis reclassify/check it. A bracket item that begins `(...):`
   or `extern` is a function type, never an index: `same[(int): void](add)`.
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
11. Calls spelled `sizeof(`, `alignof(`, or `blank(` parse their sole operand with
    the full type grammar.
12. `offsetof(` parses a type, comma, and dot-separated field-name path;
    `bitcast(` parses a type, comma, and one expression.
    `static_assert` is additionally admitted in the top-level declaration list.
13. A pointer or slice type is admitted as a primary only as the callee of its
    explicit conversion/construction call. Semantic analysis distinguishes the
    one-argument slice spelling cast from two-argument pointer-plus-count slice
    construction.
14. In expression position `@` must be followed by exactly `file` or `line`
    (`location`); any other name is a syntax error. Both words remain ordinary
    identifiers everywhere else.

## 7. Package manifest (ace.pkg)

A separate small line-based format for `ace.pkg`, parsed independently of the
Ace language grammar above (`src/package.c`). Semantics live in `spec/modules-packages.md`'s
Package System section.

```ebnf
manifest        := line*
line            := comment | blank | table_header | section_header | field

comment         := "//" (any – newline)* newline
blank           := newline

section_header  := "[" "dependencies" "]" newline
table_header    := "[[" "link" "]]" newline        // repeatable; each
                                                    // occurrence opens one
                                                    // new record

field           := key ":" value newline           // surrounding spaces/tabs
                                                    // around key and value
                                                    // are insignificant
key             := top_key | dep_key | link_key
top_key         := "name" | "version" | "ace" | "type" | "root"
                  | "description" | "author" | "license" | "repository"
dep_key         := ident                           // import alias, only
                                                    // inside [dependencies]
link_key        := "object" | "search" | "library" | "framework" | "path"
                                                    // only inside [[link]]
value           := (any – newline)*                [P: escaping/quoting]
```

Notes:

1. Context is positional: `top_key` fields are only valid before any header;
   `dep_key` fields only after `[dependencies]` and before the next header;
   `link_key` fields only after a `[[link]]` header and before the next
   header.
2. `top_key` and `dep_key` (per alias) may each appear at most once in their
   manifest; `link_key` may appear at most once per `[[link]]` record — a
   fresh occurrence opens a new record and resets `link_key` availability.
   Any repeat is an error.
3. `description`/`author`/`license`/`repository` alone admit an empty
   `value` (field present, meaning absent); every other `top_key`, every
   `dep_key`, and every `link_key` requires a nonempty `value`.
4. `[dependencies]`'s `value` is one exact `major.minor.patch` version
   (Package System #4); an `object`/`search` `link_key`'s `value` is a path
   relative to the declaring package's own directory with no `..` traversal,
   and `object` must end in `.o` or `.a` (Package System #14);
   `library`/`framework` values are bare names. A `path` value is an
   absolute directory, admitted only in a record that also has a `library`
   (Package System #14).
5. [P] The recognized header and key sets are closed and expected to grow
   (e.g. a future `authors` as `[[authors]]`); today only `[dependencies]`
   and `[[link]]` exist, and an unrecognized header or key is an error.
