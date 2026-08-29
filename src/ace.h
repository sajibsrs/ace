// ace.h — shared definitions for the Ace compiler (bootstrap in C11).
#ifndef ACE_H
#define ACE_H

#include <stddef.h>
#include <stdint.h>

// Token kinds. X-macro: name, display string (used for diagnostics/dumps).
#define TOKEN_LIST(X)                                                          \
    X(EOF, "eof")                                                              \
    X(NL, "newline")                                                           \
    X(IDENT, "ident")                                                          \
    /* keywords (v1 inventory, spec.md Pre-Implementation Checklist #2) */     \
    X(IMPORT, "import") X(FROM, "from") X(PUB, "pub") X(MUT, "mut")           \
    X(CONST, "const")                                                          \
    X(NOALIAS, "noalias") X(IF, "if") X(ELSE, "else") X(FOR, "for")            \
    X(IN, "in") X(SWITCH, "switch") X(RETURN, "return") X(BREAK, "break")      \
    X(CONTINUE, "continue") X(ASM, "asm") X(EXTERN, "extern")                  \
    X(EXPORT, "export") X(CONTRACT, "contract") X(STRUCT, "struct")           \
    X(SIGNS, "signs") X(DISTINCT, "distinct") X(TYPE, "type")                \
    X(ENUM, "enum")                                                            \
    X(NULL, "null") X(TRUE, "true") X(FALSE, "false")                          \
    /* literals */                                                             \
    X(INT, "int") X(FLOAT, "float") X(STR, "string") X(CHARLIT, "char")        \
    /* assignment + compound assignment (spellings recognized; acceptance */   \
    /* is a parser decision — checklist open item) */                          \
    X(ASSIGN, "=")                                                             \
    X(ADDEQ, "+=") X(SUBEQ, "-=") X(MULEQ, "*=") X(DIVEQ, "/=")                \
    X(MODEQ, "%=") X(ANDEQ, "&=") X(OREQ, "|=") X(XOREQ, "^=")                 \
    X(SHLEQ, "<<=") X(SHREQ, ">>=")                                            \
    /* operators (C set; comparison chains are a parser-level node) */         \
    X(PLUS, "+") X(MINUS, "-") X(STAR, "*") X(SLASH, "/") X(PERCENT, "%")      \
    X(EQEQ, "==") X(BANGEQ, "!=")                                              \
    X(LT, "<") X(LE, "<=") X(GT, ">") X(GE, ">=")                              \
    X(ANDAND, "&&") X(OROR, "||")                                              \
    X(AMP, "&") X(PIPE, "|") X(CARET, "^") X(SHL, "<<") X(SHR, ">>")           \
    X(TILDE, "~") X(BANG, "!") X(QUESTION, "?")                                \
    /* punctuation */                                                          \
    X(LPAREN, "(") X(RPAREN, ")") X(LBRACK, "[") X(RBRACK, "]")                \
    X(LBRACE, "{") X(RBRACE, "}") X(COMMA, ",") X(COLON, ":")                  \
    X(DOT, ".") X(DOTDOT, "..") X(DOTDOTDOT, "...")

typedef enum {
#define T(name, _) T_##name,
    TOKEN_LIST(T)
#undef T
} TokKind;

const char* tok_disp(TokKind kind);

typedef struct {
    TokKind     kind;
    int         line, col; // 1-based position of the token start
    size_t      off;       // byte offset of the token start
    uint64_t    ival;      // INT, CHARLIT
    double      fval;      // FLOAT
    const char* str;       // IDENT: source text; STR: decoded bytes (arena)
    uint32_t    len;       // byte length of str
} Token;

typedef struct {
    const char* path;
    int         errors;
} Diag;

typedef struct {
    const char* path;
    char*       buf; // whole file, NUL-terminated
    size_t      len;
} Source;

typedef struct {
    uint64_t source_files;
    uint64_t source_bytes;
    uint64_t tokens;
    uint64_t machine_code;
    uint64_t object_file;
    uint64_t compile_ns;
} CompileReport;

int source_load(Source* s, const char* path);

// Lex the whole source; returns a malloc'd token array terminated by T_EOF.
// *count includes the terminator. Token text storage outlives the call.
Token* lex(Source* s, Diag* d, size_t* count);

// --- Arena (implemented in lexer.c; never freed, pointers are stable) --------
char* arena_alloc(size_t n);

// --- AST ---------------------------------------------------------------------
// Node kinds. X-macro: enum name, dump name. Per-kind layout is documented in
// parser.c. Types, declarations, statements, and expressions are all Nodes.
#define NODE_KINDS(X)                                                          \
    X(N_FILE, "file")                                                          \
    /* types */                                                                \
    X(N_NAMED, "named") X(N_PTR, "ptr") X(N_SLICE_T, "slice_t")                \
    X(N_ARRAY_T, "array_t") X(N_TUPLE_T, "tuple_t") X(N_FUNC_T, "func_t")      \
    X(N_OPT, "opt") X(N_CONST_T, "const_t") X(N_TPARAMS, "tparams")            \
    /* top-level declarations */                                               \
    X(N_IMPORT, "import") X(N_VAR, "var") X(N_FUNC, "func")                    \
    X(N_PARAM, "param") X(N_STRUCT, "struct") X(N_FIELD, "field")              \
    X(N_CONTRACT, "contract") X(N_METHOD, "method") X(N_ENUM, "enum")         \
    X(N_ALIAS, "alias") X(N_DISTINCT, "distinct") X(N_SIGNING, "signing")     \
    X(N_CONFORMANCE, "conformance") X(N_EXTERN, "extern")                      \
    X(N_EXTERNFN, "externfn")                                                  \
    /* statements */                                                           \
    X(N_BLOCK, "block") X(N_EXPRSTMT, "exprstmt") X(N_ASSIGN, "assign")        \
    X(N_IF, "if") X(N_FOR, "for") X(N_RANGE, "range") X(N_SWITCH, "switch")    \
    X(N_CASE, "case") X(N_RETURN, "return") X(N_BREAK, "break")                \
    X(N_CONTINUE, "continue") X(N_ASM, "asm") X(N_ASMBIND, "asmbind")          \
    X(N_LABEL, "label")                                                        \
    /* expressions */                                                          \
    X(N_INT, "int_lit") X(N_FLOAT, "float_lit") X(N_STR, "str_lit")            \
    X(N_CHAR, "char_lit") X(N_BOOL, "bool") X(N_NULL, "null")                  \
    X(N_IDENT, "ident") X(N_TUPLE_E, "tuple")                                  \
    X(N_BINARY, "binary") X(N_CMPCHAIN, "cmpchain") X(N_OPS, "ops")            \
    X(N_OP, "op") X(N_UNARY, "unary") X(N_CALL, "call")                        \
    X(N_MUTARG, "mutarg") X(N_INDEX, "index") X(N_SLICE_E, "slice")            \
    X(N_FIELD_E, "field") X(N_ARRAYLIT, "arraylit")                            \
    X(N_STRUCTLIT, "structlit") X(N_FIELDINIT, "fieldinit")

typedef enum {
#define N(k, _) k,
    NODE_KINDS(N)
#undef N
} NKind;

const char* node_kind_name(NKind k);

enum {
    F_PUB = 1, F_EXPO = 2, F_MUT = 4, F_NOALIAS = 8, F_CONST = 16,
    F_ELSE = 32, F_VARARGS = 64, F_METHOD = 128, F_SELECT = 256,
};

// Compiler-backed functions declared by std/mem.ace. These markers live only
// on that module's declaration/call nodes; the names themselves are ordinary
// imported names and are never predeclared globally.
enum {
    BI_MEM_ALLOC = 2,
    BI_MEM_REALLOC = 3,
    BI_MEM_RELEASE = 4,
};

typedef struct Node Node;
struct Node {
    uint8_t     kind; // NKind
    uint8_t     op;   // TokKind for binary/unary/assign/op nodes
    uint16_t    flags;
    int         line, col;
    const char* path; // source file owning this node (for diagnostics/DWARF)
    const char* str; // name / string bytes (len in len)
    uint32_t    len;
    uint64_t    ival; // int/char/bool literals, array length; sizeof result
    double      fval; // float literals
    Node*       a;
    Node*       b;
    Node*       c;
    Node*       d; // method owner / contract-construction signing metadata
    Node**      kids;
    int         nkids;
    const char* cname; // original C-boundary name of a module-prefixed extern
                       // declaration (module.c); the linker still sees `_name`
    // sema annotations (filled by sema.c, consumed by emit.c):
    void*       ty;  // SType* — computed type of this expression
    void*       sym; // Sym* — resolved symbol (idents, call targets, binders;
                     //  for enum members `Color.red`: the TYPE sym on the base)
};

// Parse a token stream into an N_FILE node. Aborts at the first syntax error
// (reported via d). Always returns a node (possibly partial).
Node* parse(Token* toks, size_t ntok, Diag* d);

// Print an S-expression dump of the AST (stdout).
void parse_dump(Node* file);

// Semantic check: types, names, expression typing, in declaration order.
// Returns the number of errors (also counted in d). Annotates nodes (ty/sym)
// for the emitter; generic stamp bodies are re-annotated on demand via
// sema.h's sema_stamp_body().
int sema_check(Node* file, Diag* d);

// Load supported imports and merge their declarations into the compilation
// unit. Qualified and selectively imported references are lowered to private
// module-prefixed names before semantic checking.
int resolve_imports(Node* file, Diag* d, const char* compiler_path,
                    CompileReport* report);

// Direct AArch64 codegen (stage four): walks the sema-checked AST and writes
// a Mach-O arm64 object. Returns 0 on success. Sets emit_has_main.
int emit_object(Node* file, Diag* d, const char* out_path,
                CompileReport* report);
extern int emit_has_main;

#endif
