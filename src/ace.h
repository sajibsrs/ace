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
    /* keywords (v1 inventory) */                                              \
    X(IMPORT, "import") X(PUB, "pub") X(MUT, "mut")                            \
    X(CONST, "const")                                                          \
    X(NOALIAS, "noalias") X(IF, "if") X(ELSE, "else") X(FOR, "for")            \
    X(IN, "in") X(SWITCH, "switch") X(DEFAULT, "default") X(RETURN, "return") X(BREAK, "break")      \
    X(CONTINUE, "continue") X(DEFER, "defer") X(ASM, "asm") X(EXTERN, "extern")\
    X(EXPORT, "export") X(CONTRACT, "contract") X(STRUCT, "struct")           \
    X(EMBED, "embed")                                                         \
    X(SIGNS, "signs") X(DISTINCT, "distinct") X(TYPE, "type")                \
    X(ENUM, "enum") X(UNION, "union") X(SPECIALIZED, "specialized")            \
    X(INTRINSIC, "intrinsic") X(INLINE, "inline") X(VOLATILE, "volatile")   \
    X(NULL, "null") X(TRUE, "true") X(FALSE, "false")                          \
    /* literals */                                                             \
    X(INT, "int") X(FLOAT, "float") X(STR, "string") X(CHARLIT, "char")        \
    /* assignment and compound-assignment operators */                        \
    X(ASSIGN, "=")                                                            \
    X(ADDEQ, "+=") X(SUBEQ, "-=") X(MULEQ, "*=") X(DIVEQ, "/=")                \
    X(MODEQ, "%=") X(ANDEQ, "&=") X(OREQ, "|=") X(XOREQ, "^=")                 \
    X(SHLEQ, "<<=") X(SHREQ, ">>=")                                            \
    /* operators (C set; comparison chains are a parser-level node) */         \
    X(PLUS, "+") X(MINUS, "-") X(STAR, "*") X(SLASH, "/") X(PERCENT, "%")      \
    X(EQEQ, "==") X(BANGEQ, "!=")                                              \
    X(LT, "<") X(LE, "<=") X(GT, ">") X(GE, ">=")                              \
    X(ANDAND, "&&") X(OROR, "||")                                              \
    X(AMP, "&") X(PIPE, "|") X(CARET, "^") X(SHL, "<<") X(SHR, ">>")           \
    X(TILDE, "~") X(BANG, "!")                                                \
    /* punctuation */                                                          \
    X(LPAREN, "(") X(RPAREN, ")") X(LBRACK, "[") X(RBRACK, "]")                \
    X(LBRACE, "{") X(RBRACE, "}") X(COMMA, ",") X(COLON, ":")                  \
    X(COLONCOLON, "::")                                                       \
    X(SEMI, ";") X(AT, "@")                                                  \
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

// --- Shared path/string helpers (implemented in module.c) --------------------
const char* arena_string(const char* s);
int         text_is(const char* s, uint32_t len, const char* t);
void        compiler_dir(char* out, size_t cap, const char* argv0);
void        path_dir(char* out, size_t cap, const char* path);
const char* join_path(const char* dir, const char* rel);
const char* canonicalize(const char* path); // arena storage, or NULL
int         path_beneath(const char* path, const char* dir);
// Build-relative source paths (spec/safety-errors.md Errors #16): set both roots before
// parsing, then map any source path for @file and assert diagnostics.
void        build_roots(const char* compiler_path, const char* root_dir);
const char* build_path(const char* path);

// Lex the whole source; returns a malloc'd token array terminated by T_EOF.
// *count includes the terminator. Token text storage outlives the call.
Token* lex(Source* s, Diag* d, size_t* count);

// --- Arena (implemented in lexer.c; never freed, pointers are stable) --------
char* arena_alloc(size_t n);

// Functions & ABI #9: a parameter list, a method's receiver included, has at
// most MAX_PARAMS entries.
enum { MAX_PARAMS = 64 };

// --- Pointer map (implemented in lexer.c) ------------------------------------
// Open-addressing map from a non-NULL pointer to a nonzero uintptr_t, for
// per-function lookups that would otherwise scan a list. A zeroed PtrMap is
// empty; get returns 0 for an absent key; put replaces an existing key.
typedef struct {
    const void** key;
    uintptr_t*   val;
    size_t       cap, n;
} PtrMap;
uintptr_t ptrmap_get(const PtrMap* m, const void* key);
void      ptrmap_put(PtrMap* m, const void* key, uintptr_t val);
void      ptrmap_clear(PtrMap* m); // keeps the storage for reuse
void      ptrmap_free(PtrMap* m);

// --- AST ---------------------------------------------------------------------
// Node kinds. X-macro: enum name, dump name. Per-kind layout is documented in
// parser.c. Types, declarations, statements, and expressions are all Nodes.
#define NODE_KINDS(X)                                                          \
    X(N_FILE, "file")                                                          \
    /* types */                                                                \
    X(N_NAMED, "named") X(N_PTR, "ptr") X(N_SLICE_T, "slice_t")                \
    X(N_ARRAY_T, "array_t") X(N_TUPLE_T, "tuple_t") X(N_FUNC_T, "func_t")      \
    X(N_CONST_T, "const_t") X(N_TPARAMS, "tparams")                            \
    X(N_STATICMAP_T, "staticmap_t")                                             \
    /* top-level declarations */                                               \
    X(N_IMPORT, "import") X(N_VAR, "var") X(N_FUNC, "func")                    \
    X(N_PARAM, "param") X(N_STRUCT, "struct") X(N_BITS, "bits")                \
    X(N_FIELD, "field")                                                        \
    X(N_UNION, "union") X(N_VARIANT, "variant")                                \
    X(N_CONTRACT, "contract") X(N_METHOD, "method") X(N_ENUM, "enum")         \
    X(N_ALIAS, "alias") X(N_DISTINCT, "distinct") X(N_ERROR_DOMAIN, "error")  \
    X(N_SIGNING, "signing")                                                   \
    X(N_CONFORMANCE, "conformance") X(N_EXTERN, "extern")                      \
    X(N_EXTERNFN, "externfn")                                                  \
    /* statements */                                                           \
    X(N_BLOCK, "block") X(N_EXPRSTMT, "exprstmt") X(N_ASSIGN, "assign")        \
    X(N_IF, "if") X(N_FOR, "for") X(N_RANGE, "range") X(N_SWITCH, "switch")    \
    X(N_CASE, "case") X(N_VARPAT, "varpat") X(N_RETURN, "return")             \
    X(N_BREAK, "break") X(N_CONTINUE, "continue")                               \
    X(N_FALLTHROUGH, "fallthrough") X(N_ASM, "asm") X(N_ASMBIND, "asmbind")   \
    X(N_LABEL, "label") X(N_DEFER, "defer")                                    \
    /* expressions */                                                          \
    X(N_INT, "int_lit") X(N_FLOAT, "float_lit") X(N_STR, "str_lit")            \
    X(N_CHAR, "char_lit") X(N_BOOL, "bool") X(N_NULL, "null")                  \
    X(N_IDENT, "ident") X(N_TUPLE_E, "tuple")                                  \
    X(N_BINARY, "binary") X(N_CMPCHAIN, "cmpchain") X(N_OPS, "ops")            \
    X(N_OP, "op") X(N_UNARY, "unary") X(N_CALL, "call")                        \
    X(N_MUTARG, "mutarg") X(N_INDEX, "index") X(N_SLICE_E, "slice")            \
    X(N_FIELD_E, "field") X(N_ARRAYLIT, "arraylit")                            \
    X(N_STRUCTLIT, "structlit") X(N_FIELDINIT, "fieldinit")                   \
    X(N_STATICMAPLIT, "staticmaplit") X(N_MAPENTRY, "mapentry")

typedef enum {
#define N(k, _) k,
    NODE_KINDS(N)
#undef N
} NKind;

const char* node_kind_name(NKind k);

enum {
    F_PUB = 1, F_EXPO = 2, F_MUT = 4, F_NOALIAS = 8, F_CONST = 16,
    F_DEFAULT = 32, F_VARARGS = 64, F_METHOD = 128, F_SELECT = 256,
    F_EMBED = 512, F_SPECIALIZED = 1024, F_FORGEN = 2048, F_MIXED = 4096,
    F_BOUNDS_PROVEN = 8192, F_INTRINSIC = 16384, F_PACKED = 32768,
    F_VOLATILE = 65536, F_CABI = 131072, F_ASSOC = 262144,
    F_PENDING = 524288, // a call deferred to end of file
    F_INLINE = 1048576, // a function expanded at every direct call
    F_INLCALLS = 2097152, // a function body with a direct call to an inline function
};

// Compiler-backed standard-module functions. These markers live only on the
// canonical module's declaration/call nodes; names remain ordinary imports
// and are never predeclared globally.
enum {
    BI_MEM_ALLOC = 2,
    BI_MEM_REALLOC = 3,
    BI_MEM_RELEASE = 4,
    BI_MEM_COPY = 5,
    BI_MEM_MOVE = 6,
    BI_MEM_FILL = 7,
    BI_MEM_ZERO = 8,
    BI_MEM_COMPARE = 9,
    BI_FMT_PRINTF = 10,
    BI_MEM_ALLOC_T = 11,   // mem.alloc[T](count: usize): mut []T
    BI_MEM_REALLOC_T = 12, // mem.realloc[T](data: []T, new_count: usize): mut []T
    BI_MEM_RELEASE_T = 13, // mem.release[T](mut data: []T)
    BI_SYS_ARGC = 14,
    BI_SYS_ARG = 15,
    BI_SYS_ENVC = 16,
    BI_SYS_ENV = 17,
    BI_MEM_EQUAL = 18,
    BI_INTERNAL_LOAD_U64 = 21,
    BI_BITS_POP_COUNT = 22,
    BI_BITS_LEAD_ZEROS = 23,
    BI_BITS_TRAIL_ZEROS = 24,
    BI_BITS_BYTE_SWAP = 25,
    BI_BITS_ROTATE_LEFT = 26,
    BI_BITS_ROTATE_RIGHT = 27,
    BI_MEM_ALLOC_SIZE = 28, // mem.usable(data: []u8): usize
    BI_MATH_SQRT = 29,      // math.sqrt/floor/ceil: exact f32/f64 -> same type
    BI_MATH_FLOOR = 30,
    BI_MATH_CEIL = 31,
    BI_MATH_FMA = 32,       // math.fma(a, b, c): a*b + c with one rounding
    BI_BITS_WRAP_ADD = 33,  // bits.wradd/wrsub/wrmul: unchecked fixed-width arithmetic
    BI_BITS_WRAP_SUB = 34,
    BI_BITS_WRAP_MUL = 35,
    BI_MEM_ZALLOC_T = 36,   // mem.zalloc[T](count: usize): mut []T, zero-filled
    BI_STR_FORMAT = 37,     // str.format(destination, "format", ...): checked like fmt.printf
};

enum {
    PF_TEXT, PF_SIGNED, PF_UNSIGNED, PF_BINARY, PF_OCTAL,
    PF_HEX_LOWER, PF_HEX_UPPER,
    PF_FIXED, PF_STRING, PF_BYTE, PF_PERCENT,
};

typedef struct Node Node;

// Forward declarations for Package System v1 (full definitions near the end
// of this header, beside package_resolve); resolve_imports takes a Package*.
typedef struct PackageDep PackageDep;
typedef struct Package Package;

typedef struct {
    uint8_t  kind;
    uint16_t precision;
    uint8_t  zero;       // '0' flag: zero padding after any sign
    uint32_t width;      // minimum byte width, 0 for none
    uint32_t off, len;   // decoded static-format byte span for PF_TEXT
    int      arg;        // call->kids index; zero for text/percent
    void*    helper;     // Sym* selected by sema, consumed by the emitter
} PrintfPart;

typedef struct {
    Node*       format;  // resolved static string literal
    PrintfPart* parts;
    int         nparts;
    void*       open;    // Sym*: the call's sink opener and closer
    void*       close;
} PrintfPlan;

struct Node {
    uint8_t     kind; // NKind
    uint8_t     op;   // TokKind for binary/unary/assign/op nodes
    uint32_t    flags;
    int         line, col;
    const char* path; // source file owning this node (for diagnostics/DWARF)
    const char* str; // name / string bytes (len in len)
    uint32_t    len;
    uint64_t    ival; // int/char/bool literals; layout builtin result
    double      fval; // float literals
    Node*       a;
    Node*       b;
    Node*       c;
    Node*       d; // method owner / contract-construction signing metadata
    Node**      kids;
    int         nkids;
    const char* cname; // preserved source spelling for a renamed C boundary or
                       // error domain (module.c); extern linkage uses `_name`
    // sema annotations (filled by sema.c, consumed by emit.c):
    void*       ty;  // SType* — computed type of this expression
    void*       sym; // Sym* — resolved symbol (idents, call targets, binders;
                     //  for enum members `Color.red`: the TYPE sym on the base)
    void*       aux; // feature-specific checked metadata (currently PrintfPlan*)
};

// Parse a token stream into an N_FILE node. Aborts at the first syntax error
// (reported via d). Always returns a node (possibly partial).
Node* parse(Token* toks, size_t ntok, Diag* d, const char* target);

// Print an S-expression dump of the AST (stdout).
void parse_dump(Node* file);

// Semantic check: types, names, expression typing, in declaration order.
// Returns the number of errors (also counted in d). Annotates nodes (ty/sym)
// for the emitter; generic stamp bodies are re-annotated on demand via
// sema.h's sema_stamp_body().
int sema_check(Node* file, Diag* d);

// Load supported imports and merge their declarations into the compilation
// unit. Qualified and selectively imported references are lowered to private
// module-prefixed names before semantic checking. root_package is the package
// owning `file` (Package System v1), or NULL for a plain manifest-free
// compile; it gates dependency-alias bare imports and same-package relative
// import containment.
int resolve_imports(Node* file, Diag* d, const char* compiler_path,
                    const char* target,
                    CompileReport* report, Package* root_package);

// Direct AArch64 codegen (stage four): walks the sema-checked AST and writes
// a Mach-O arm64 object. Returns 0 on success. Sets emit_has_main.
typedef enum { EMIT_DEBUG, EMIT_RELEASE } EmitMode;

int emit_object(Node* file, Diag* d, const char* out_path, CompileReport* report, EmitMode mode);
extern int emit_has_main;
// The build target (main.c): "darwin-arm64" writes a Mach-O arm64 object,
// "linux-x64" an ELF x64 one.
extern const char* ace_target;
// Inline-assembly registers of the target (Inline Assembly #2): the number a
// binding's name selects (arm64 xN as N; x64 by encoding, rax 0 .. r15 15,
// never rsp or rbp), or -1; and a number's name.
int asm_register(const char* s, uint32_t len);
const char* asm_register_name(int r);
// The name prefix of std's internal formatting module, whose piece writers
// fmt.printf and str.format lower to; NULL until it loads.
extern const char* formatting_prefix;

// --- Packages (ace.pkg manifests, spec/modules-packages.md Package System) --------------------
// A package is a directory holding one ace.pkg and one root .ace file;
// dependencies are name: version entries, each found as the nearest pkg/NAME
// from the root package's directory and resolved recursively.
// module.c's resolve_imports consumes the resolved graph for cross-package
// imports; this stage itself has no codegen involvement.
struct PackageDep {
    const char* name;    // import alias — equals the target package's own name
    Package*    package;
    PackageDep* next;
};

// Native link inputs declared through a repeatable [[link]] manifest section
// (spec/modules-packages.md Package System #14, GRAMMAR.md §7) — the same four kinds the
// direct compiler driver accepts on its command line.
typedef struct PackageLink PackageLink;
// LINK_PATH is a record attribute (the directory of the record's library),
// never a PackageLink of its own.
typedef enum { LINK_OBJECT, LINK_SEARCH, LINK_LIBRARY, LINK_FRAMEWORK, LINK_PATH } LinkKind;
struct PackageLink {
    LinkKind     kind;
    const char*  value; // canonical path for OBJECT/SEARCH; bare name for LIBRARY/FRAMEWORK
    const char*  path;  // LIBRARY: its record's path directory, else NULL
    const char*  file;  // LIBRARY: the file found in path, else NULL
    int          line;
    PackageLink* next;
};

struct Package {
    const char* dir;         // canonical package root directory
    const char* name;
    int         ver_major, ver_minor, ver_patch;
    int         ace_major, ace_minor;
    int         is_exe;      // 0 = lib, 1 = exe
    const char* root_file;   // canonical path to the root .ace file
    const char* description; // NULL if absent
    const char* author;      // NULL if absent
    const char* license;     // NULL if absent
    const char* repository;  // NULL if absent
    PackageDep*  deps;
    PackageLink* links;      // declaration order
    Package*    next;        // completion order: dependencies before dependents
};

// Parse ace.pkg at package_dir and resolve its complete dependency
// graph. Returns the root package, or NULL if any error was reported through
// d. On success, *order receives every loaded package (including the root)
// in dependency-first order and *norder its count; the caller owns the
// returned array (free) but not the Package storage (arena, stable).
Package* package_resolve(const char* package_dir, Diag* d, const char* compiler_path, Package*** order, int* norder);

// Collects every package's declared [[link]] inputs across order
// (dependency-first, from package_resolve) into direct-driver flag form —
// spec/modules-packages.md Package System #14: object path positional, "-L"/dir, "-l"/name,
// "-framework"/name — de-duplicating exact kind+value matches and
// diagnosing a same-identity/different-value conflict as an error anchored
// at each declaration's own manifest:line. Returns the argv-style array
// (caller owns the array itself, not its arena-backed string contents) and
// its count via *nargs; returns NULL with *nargs 0 if d gained any errors.
char** package_collect_links(Package** order, int norder, Diag* d, int* nargs);

// True iff canonical path canon is package_dir or beneath it, outside its
// reserved pkg/ directory (spec/modules-packages.md Package System #6).
int package_contains(const char* package_dir, const char* canon);

// True iff s is a nonempty lowercase Ace identifier, the shape a package name
// must have. Shared by manifest validation and `ace pkg init`'s derived name.
int package_valid_name(const char* s, size_t len);

// Textual-only manifest edits for `ace pkg add`/`remove` (spec/queue.md Decision
// Queue, "Package system v1"): no grammar or graph validation happens here —
// the caller re-resolves the graph afterward and restores the manifest's
// original bytes on failure. Both return 0 on a successful rewrite, 1 if the
// manifest could not be read or written back (message already printed).
// add_dep inserts "name: version\n" as a new [dependencies] entry, creating the
// section header at end-of-file if absent.
int package_manifest_add_dep(const char* manifest_path, const char* name, const char* version);
// remove_dep deletes the [dependencies] entry whose key exactly equals name,
// and the section header too if no other entry remains; *found reports
// whether such an entry existed (false leaves the manifest untouched).
int package_manifest_remove_dep(const char* manifest_path, const char* name, int* found);

#endif
