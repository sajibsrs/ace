// parser.c — recursive-descent parser for Ace (stage two): tokens → AST.
//
// Node layout (a/b/c plus kids where lists are needed):
//   N_FILE      kids = imports + top decls
//   N_IMPORT    whole: str = binding (leaf segment); b = module ident (alias,
//               also the leaf segment); a = N_STR path
//               (a->op = T_AT for compiler-reserved `@name`)
//               c = full path segment chain (N_IDENT list linked through each
//               segment's own `a`), head to leaf, for bare/alias-target module
//               paths (e.g. `net/tcp`); NULL when a or F_SELECT's `a` path form
//               is used instead
//               selective (F_SELECT): kids = imported N_IDENT names;
//               b = std module ident (leaf) or a = local N_STR path
//   N_VAR       kids = binders (N_PARAM: typed declaration, N_IDENT: reassignment
//               under mixed `=`); b = init (or N_TUPLE_E); F_CONST/F_MIXED
//   N_FUNC      str = name; kids = params; a = ret; b = body; c = N_TPARAMS;
//               d = owner type for methods and associated functions;
//               F_PUB/F_EXPO/F_METHOD/F_MUT/F_ASSOC (F_METHOD: first
//               param is the receiver `self`; F_ASSOC: no receiver, called
//               only through the type, e.g. `Type.new(...)`);
//               F_SPECIALIZED/F_INTRINSIC mark bodyless declarations;
//               F_INLINE marks a function expanded at its direct calls;
//               F_COLD marks one whose calls are rare (@cold)
//   N_PARAM     str = name; a = type; F_MUT/F_NOALIAS (also used as var binder)
//   N_STRUCT    str = name; kids = N_FIELD; b = optional @align expression;
//               c = N_TPARAMS; F_PUB/F_PACKED
//   N_BITS      str = name; a = backing type; kids = N_FIELD whose ival is
//               width and F_CONST marks signed; F_PUB
//   N_FIELD     str = name; a = type; b = optional @align expression; c = optional default
//   N_UNION     str = name; kids = N_VARIANT; c = N_TPARAMS; F_PUB
//   N_VARIANT   str = name; a = payload type or NULL (payloadless variant)
//   N_CONTRACT str = name; kids = N_METHOD; c = N_TPARAMS; F_PUB
//   N_METHOD    str = name; kids = params; a = ret; F_MUT (mutating receiver)
//   N_ENUM      str = name; a = optional backing type; kids = N_IDENT members
//               whose a is an optional discriminant expression; F_PUB
//   N_ALIAS     str = name; a = type; c = N_TPARAMS; F_PUB
//   N_DISTINCT  str = name; a = base type; F_PUB
//   N_SIGNING     a = concrete type; c = owner N_TPARAMS; kids = N_CONFORMANCE
//   N_CONFORMANCE a = signed contract type; kids = resolved methods after sema
//   N_EXTERN    str = abi string; kids = N_EXTERNFN / N_EXTERNVAR
//   N_EXTERNFN  str = name; kids = params; a = ret; F_EXPO/F_VARARGS
//   N_EXTERNVAR str = name; a = type; F_MUT when writable (C Interop #6)
//   N_BLOCK     kids = statements
//   N_EXPRSTMT  a = expr
//   N_ASSIGN    kids = targets; a = rhs; op = T_ASSIGN or compound token;
//               F_VOLATILE marks the access-site volatile store
//   N_IF        a = cond; b = then-block; c = else (N_BLOCK or N_IF)
//   N_FOR       F_FORGEN (general head): a = init stmt (N_VAR/N_ASSIGN/
//               N_EXPRSTMT or NULL), b = cond expr or NULL, c = post stmt
//               (N_ASSIGN/N_EXPRSTMT or NULL), d = body; kids empty.
//               str/len = label. Typed range/sequence head (unchanged):
//               kids = one value binder, or index + value binders (N_PARAM);
//               b = iterable (expr) or N_RANGE; c = body; str/len = label.
//               F_MUT = `in mut iterable`: the value binder aliases the
//               element (sema requires writable array/slice storage)
//   N_RANGE     a = first; b = second; c = optional contextual `step` expression
//   N_SWITCH    a = scrutinee; kids = N_CASE (kids = patterns; b = block; F_DEFAULT marks `default`)
//   N_VARPAT    union-variant case pattern: a = Type.variant (N_FIELD_E);
//               b = binder N_PARAM (str, a = payload type) or NULL; ival = variant
//   N_RETURN    a = expr / N_TUPLE_E / NULL
//   N_BREAK     str = label or NULL
//   N_FALLTHROUGH contextual bare statement in a switch case block
//   N_ASM       str = instruction text; kids = N_ASMBIND (str = in/out/clobbers;
//               kids = [reg, val, reg, val, ...] for in/out, [reg, ...] for clobbers)
//   N_LABEL     str = label; a = block
//   N_DEFER     a = call expression deferred to function exit (top-level only)
//   N_NAMED     str = name; a = qualifier (N_NAMED/N_IDENT chain); kids = generic args (types)
//   N_PTR       a = pointee; F_MUT
//   N_SLICE_T   a = element
//   N_ARRAY_T   a = element; b = length expression
//   N_TUPLE_T   kids = types
//   N_FUNC_T    kids = param types; a = ret; F_CABI for extern "c" type
//   N_CONST_T   a = inner
//   N_TPARAMS   kids = N_IDENT
//   N_BINARY    op; a; b      N_CMPCHAIN kids = operands; a = N_OPS{kids = N_OP}
//   N_UNARY     op (T_STAR = dereference, T_AMP = address-of); a; F_MUT = writable `mut &`
//   N_CALL      a = callee; kids = args (N_MUTARG wraps mut-marked args); F_MUT = receiver mark
//   N_INDEX     a = base; b = index
//   N_SLICE_E   a = base; b = lo (nullable); c = hi (nullable)
//   N_FIELD_E   a = base; str = field
//   N_ARRAYLIT  kids = elements, or N_MAPENTRY (a = key, b = value) when keyed;
//               repetition: a = value, b = count expression
//   N_STRUCTLIT a = type expr or NULL (bare literal); kids = N_FIELDINIT (str or NULL; a = value)
//               F_DEFAULT on a N_FIELDINIT marks one sema added from the field's default
//   N_STATICMAP_T a = key type; b = value type
//   N_STATICMAPLIT kids = N_MAPENTRY (a = key, b = value)
//
// Contexts: struct literals are not parsed where a `{` would open a block
// (if/for/switch heads, statement starts) — Go's composite-literal rule.
#include "ace.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX_NOSLIT 1 // no struct literal at this expression level

typedef struct {
    Node** v;
    int n, cap;
} NV;

typedef struct {
    Token* toks;
    size_t n, i;
    Diag*  d;
    const char* target;
    NV exports; // functions defined inside the extern block just parsed
    sigjmp_buf* sync; // innermost statement or declaration being parsed
    sigjmp_buf stop;  // the error limit: abandon the file
    int nerr;
    Token* last;      // the latest error's token; another there is a cascade
} P;

enum { PARSE_ERROR_LIMIT = 10 };

// --- cursor helpers -----------------------------------------------------------

static Token* tk(P* p) { return &p->toks[p->i]; }

static TokKind kth(P* p, int k) {
    size_t j = p->i + (size_t)k;
    return j < p->n ? p->toks[j].kind : T_EOF;
}

static int at(P* p, TokKind k) { return tk(p)->kind == k; }

// A conversion callee begins `[]` + a type: the tokens that may start the
// element type after the brackets. A newline keeps the empty array literal.
static int slice_type_start(TokKind k) {
    return k == T_IDENT || k == T_MUT || k == T_STAR || k == T_LBRACK || k == T_LPAREN || k == T_CONST;
}

// Whether the tokens from offset k on spell an element type called as a cast,
// `T(`, `*T(`, `[2]T(`, or `m.T[A](`: after a one-element array literal they
// make it the length of an array type called as a cast ([N]T(value)).
static int array_cast_ahead(P* p, int k) {
    for (;;) {
        TokKind t = kth(p, k);
        if (t == T_STAR || t == T_MUT || t == T_CONST) {
            k++;
        } else if (t == T_LBRACK) {
            for (int depth = 0;; k++) {
                t = kth(p, k);
                if (t == T_EOF) return 0;
                if (t == T_LBRACK) depth++;
                else if (t == T_RBRACK && --depth == 0) break;
            }
            k++;
        } else {
            break;
        }
    }
    if (kth(p, k++) != T_IDENT) return 0;
    while (kth(p, k) == T_DOT && kth(p, k + 1) == T_IDENT) k += 2;
    if (kth(p, k) == T_LBRACK) {
        for (int depth = 0;; k++) {
            TokKind t = kth(p, k);
            if (t == T_EOF) return 0;
            if (t == T_LBRACK) depth++;
            else if (t == T_RBRACK && --depth == 0) break;
        }
        k++;
    }
    return kth(p, k) == T_LPAREN;
}

static int ident_at(P* p, const char* s) {
    return at(p, T_IDENT) && tk(p)->len == strlen(s) && memcmp(tk(p)->str, s, tk(p)->len) == 0;
}

static int eat(P* p, TokKind k) {
    if (at(p, k)) {
        p->i++;
        return 1;
    }
    return 0;
}

static void skipnl(P* p) {
    while (at(p, T_NL)) p->i++;
}

static const char* gdesc(P* p) {
    static char buf[IDENT_MAX + 3]; // a quoted identifier and its NUL
    Token* t = tk(p);
    switch (t->kind) {
    case T_IDENT:
        snprintf(buf, sizeof buf, "'%.*s'", (int)t->len, t->str);
        return buf;
    case T_STR:
        snprintf(buf, sizeof buf, "string literal");
        return buf;
    default:
        snprintf(buf, sizeof buf, "'%s'", tok_disp(t->kind));
        return buf;
    }
}

/* Reports a syntax error and abandons the innermost statement or declaration;
 * the parser resumes after it to report later errors, up to the limit. No
 * tree is repaired: any error blocks every later phase.
 */
static void vperr_at(P* p, Token* t, const char* fmt, va_list ap) {
    if (t != p->last) {
        fprintf(stderr, "%s:%d:%d: error: ", p->d->path, t->line, t->col);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        p->d->errors++;
        p->last = t;
        if (++p->nerr == PARSE_ERROR_LIMIT) {
            fprintf(stderr, "%s: stopped at %d errors; the rest of the file was not parsed\n", p->d->path, PARSE_ERROR_LIMIT);
            siglongjmp(p->stop, 1);
        }
    }
    siglongjmp(*p->sync, 1);
}

// Reports at t, for a check made only after the construct it names is parsed.
static void perr_at(P* p, Token* t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vperr_at(p, t, fmt, ap);
}

static void perr(P* p, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vperr_at(p, tk(p), fmt, ap);
}

static void bare_list_line(P* p, int line) {
    if (tk(p)->line != line) perr(p, "bare comma-separated lists must stay on one physical line");
}

static void signing_list_line(P* p, int line) {
    if (tk(p)->line != line)
        perr(p, "bare signed-contract lists must stay on one line; use parentheses for a multiline list");
}

static Token* expect(P* p, TokKind k, const char* what) {
    if (!at(p, k)) perr(p, "expected %s, got %s", what, gdesc(p));
    return &p->toks[p->i++];
}

// A member name (field, enum member, union variant, error entry, method) may
// be a keyword: it is declared in a member list or written after '.', never
// bare (Source #3).
static int member_kind(TokKind k) { return k == T_IDENT || (k >= T_IMPORT && k <= T_FALSE); }

static Token* expect_member(P* p, const char* what) {
    if (!member_kind(tk(p)->kind)) perr(p, "expected %s, got %s", what, gdesc(p));
    return &p->toks[p->i++];
}

// --- nodes --------------------------------------------------------------------

static Node* nd(P* p, NKind kind) {
    Node* x = (Node*)arena_alloc(sizeof(Node));
    memset(x, 0, sizeof *x);
    x->kind = (uint8_t)kind;
    x->line = tk(p)->line;
    x->col = tk(p)->col;
    x->path = p->d->path;
    return x;
}

static Node* nd_tok(P* p, NKind kind, Token* t) {
    Node* x = (Node*)arena_alloc(sizeof(Node));
    memset(x, 0, sizeof *x);
    x->kind = (uint8_t)kind;
    x->line = t->line;
    x->col = t->col;
    x->path = p->d->path;
    return x;
}

static Node* ident_node(P* p, Token* t) {
    Node* x = nd_tok(p, N_IDENT, t);
    x->str = t->str;
    x->len = t->len;
    return x;
}

static void nv_push(P* p, NV* w, Node* x) {
    (void)p;
    if (w->n == w->cap) {
        int nc = w->cap ? w->cap * 2 : 8;
        Node** nv2 = (Node**)arena_alloc((size_t)nc * sizeof(Node*));
        memcpy(nv2, w->v, (size_t)w->n * sizeof(Node*));
        w->v = nv2;
        w->cap = nc;
    }
    w->v[w->n++] = x;
}

static void set_kids(Node* x, NV* w) {
    x->kids = w->v;
    x->nkids = w->n;
}

// --- types --------------------------------------------------------------------

static Node* parse_type(P* p);
static Node* expr(P* p, int ctx);

static int spells_staticmap(Node* t) {
    return t && t->kind == N_STATICMAP_T;
}

static int at_void(P* p) {
    return at(p, T_IDENT) && tk(p)->len == 4 && memcmp(tk(p)->str, "void", 4) == 0 && kth(p, 1) != T_DOT;
}

// A function type's return after its ':' — a type, or 'void' for no value (NULL).
static Node* func_type_ret(P* p) {
    if (at_void(p)) {
        p->i++;
        return NULL;
    }
    return parse_type(p);
}

// A type argument: a type, or 'void' (no value; checked where it is substituted).
static Node* type_arg(P* p) {
    if (!at_void(p)) return parse_type(p);
    Node* v = nd_tok(p, N_NAMED, tk(p));
    v->str = tk(p)->str;
    v->len = tk(p)->len;
    p->i++;
    return v;
}

static Node* type_atom(P* p) {
    Node* t;
    if (at(p, T_CONST)) {
        t = nd(p, N_CONST_T);
        p->i++;
        t->a = parse_type(p);
        return t;
    }
    if (at(p, T_MUT)) { // type-position mut marks writable content (Mutability #9)
        p->i++;
        if (at(p, T_STAR)) { // mut *T — writable pointee
            t = nd(p, N_PTR);
            t->flags |= F_MUT;
            p->i++;
            t->a = type_atom(p);
            return t;
        }
        if (at(p, T_LBRACK)) { // mut []T — writable elements
            if (kth(p, 1) != T_RBRACK) {
                perr(p,
                     "'mut' marks writable views ('mut *T', 'mut []T', 'mut string'); a fixed array is already writable storage; take a writable view with 'mut name[..]'");
                return NULL;
            }
            t = nd(p, N_SLICE_T);
            t->flags |= F_MUT;
            p->i += 2;
            t->a = type_atom(p);
            return t;
        }
        if (at(p, T_IDENT) && tk(p)->len == 6 && memcmp(tk(p)->str, "string", 6) == 0 && kth(p, 1) != T_DOT) {
            Token* s = tk(p); // mut string — writable bytes
            t = nd_tok(p, N_NAMED, s);
            t->str = s->str;
            t->len = s->len;
            t->flags |= F_MUT;
            p->i++;
            return t;
        }
        perr(p, "'mut' in a type marks a writable view: 'mut *T', 'mut []T', or 'mut string'");
        return NULL;
    }
    if (at(p, T_STAR)) {
        t = nd(p, N_PTR);
        p->i++;
        t->a = type_atom(p);
        return t;
    }
    if (at(p, T_LBRACK)) {
        p->i++;
        if (eat(p, T_RBRACK)) { // []T slice
            t = nd(p, N_SLICE_T);
            t->a = type_atom(p);
            return t;
        }
        t = nd(p, N_ARRAY_T); // [N]T array; N is a compile-time constant expression
        t->b = expr(p, 0);
        expect(p, T_RBRACK, "']' after array length");
        t->a = type_atom(p);
        return t;
    }
    if (at(p, T_EXTERN)) { // extern "c" ([params]) [: returns]
        Node* f = nd(p, N_FUNC_T);
        f->flags |= F_CABI;
        p->i++;
        Token* abi = expect(p, T_STR, "ABI string after 'extern'");
        if (abi->len != 1 || memcmp(abi->str, "c", 1) != 0) perr(p, "function types support only the 'c' ABI");
        expect(p, T_LPAREN, "'(' after function type ABI");
        NV w = {0};
        if (!at(p, T_RPAREN)) {
            for (;;) {
                nv_push(p, &w, parse_type(p));
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RPAREN)) break;
            }
        }
        expect(p, T_RPAREN, "')' to close function parameter types");
        set_kids(f, &w);
        expect(p, T_COLON, "':' and a return type or 'void' after C function parameter types");
        f->a = func_type_ret(p);
        return f;
    }
    if (at(p, T_LPAREN)) { // tuple or function type
        t = nd(p, N_TUPLE_T);
        p->i++;
        NV w = {0};
        if (!at(p, T_RPAREN)) {
            for (;;) {
                nv_push(p, &w, parse_type(p));
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RPAREN)) break; // trailing comma
            }
        }
        expect(p, T_RPAREN, "')' to close type list");
        if (eat(p, T_COLON)) { // (params): ret — function value type
            Node* f = nd(p, N_FUNC_T);
            set_kids(f, &w);
            f->a = func_type_ret(p);
            return f;
        }
        if (w.n < 2) { // a tuple has two or more members; a function type needs its return
            perr(p, "'(%s)' is not a type: a tuple has two or more members, and a function type needs ': R' or ': void'",
                 w.n ? "T" : "");
            return NULL;
        }
        set_kids(t, &w);
        return t;
    }
    if (at(p, T_IDENT)) { // named, qualified, generic-instantiated
        if (tk(p)->len == 9 && memcmp(tk(p)->str, "StaticMap", 9) == 0 && kth(p, 1) == T_LBRACK) {
            Node* map = nd_tok(p, N_STATICMAP_T, tk(p));
            p->i += 2;
            map->a = parse_type(p);
            if (eat(p, T_COMMA)) perr(p, "StaticMap type is spelled StaticMap[K]V, not StaticMap[K, V]");
            expect(p, T_RBRACK, "']' after StaticMap key type");
            map->b = parse_type(p);
            return map;
        }
        if (at_void(p)) {
            perr(p, "'void' is allowed only as a function type's return or a type argument");
            return NULL;
        }
        t = ident_node(p, tk(p));
        p->i++;
        Node* named = nd(p, N_NAMED);
        named->str = t->str;
        named->len = t->len;
        named->line = t->line;
        named->col = t->col;
        while (at(p, T_DOT)) {
            p->i++;
            Token* m = expect(p, T_IDENT, "type name after '.'");
            Node* outer = nd_tok(p, N_NAMED, m);
            outer->str = m->str;
            outer->len = m->len;
            outer->a = named;
            named = outer;
        }
        if (at(p, T_LBRACK)) { // generic instantiation in type position
            p->i++;
            NV w = {0};
            for (;;) {
                nv_push(p, &w, type_arg(p));
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RBRACK)) break;
            }
            expect(p, T_RBRACK, "']' to close type arguments");
            set_kids(named, &w);
        }
        return named;
    }
    perr(p, "expected type, got %s", gdesc(p));
    return NULL;
}

static Node* parse_type(P* p) { return type_atom(p); }

// --- expressions ----------------------------------------------------------------

static Node* expr(P* p, int ctx);

// Positioned at the operator, like N_UNARY, N_ASSIGN, and N_CMPCHAIN.
static Node* mkbin(P* p, Token* op, Node* l, Node* r) {
    Node* b = nd_tok(p, N_BINARY, op);
    b->op = (uint8_t)op->kind;
    b->a = l;
    b->b = r;
    return b;
}

// binary precedence, loosest → tightest (see level):
// || → && → | → ^ → & → ==/!= → </<=/>/>= (chains) → <</>> → +,- → *,/,%
static Node* expr_binary(P* p, int min, int ctx);
static Node* expr_unary(P* p, int ctx);
static Node* expr_postfix(P* p, int ctx);
static Node* expr_postfix_base(P* p, int ctx, Node* base);
static Node* expr_primary(P* p, int ctx);

/* Non-diagnostic type lookahead disambiguates `*T(address)` from unary
 * dereference.  The real type parser remains the sole AST builder. */
static int scan_type(P* p, size_t* at_i) {
    size_t i = *at_i;
    if (i >= p->n) return 0;
    TokKind k = p->toks[i].kind;
    if (k == T_CONST) {
        i++;
        if (!scan_type(p, &i)) return 0;
    } else if (k == T_MUT) {
        i++;
        if (i >= p->n || (p->toks[i].kind != T_STAR &&
                          p->toks[i].kind != T_LBRACK &&
                          p->toks[i].kind != T_IDENT)) return 0;
        if (!scan_type(p, &i)) return 0;
    } else if (k == T_STAR) {
        i++;
        if (!scan_type(p, &i)) return 0;
    } else if (k == T_LBRACK) {
        i++;
        if (i < p->n && p->toks[i].kind == T_RBRACK) i++;
        else {
            if (i >= p->n || p->toks[i].kind != T_INT) return 0;
            i++;
            if (i >= p->n || p->toks[i].kind != T_RBRACK) return 0;
            i++;
        }
        if (!scan_type(p, &i)) return 0;
    } else if (k == T_IDENT) {
        i++;
        while (i < p->n && p->toks[i].kind == T_DOT) {
            i++;
            if (i >= p->n || p->toks[i].kind != T_IDENT) return 0;
            i++;
        }
        if (i < p->n && p->toks[i].kind == T_LBRACK) {
            i++;
            for (;;) {
                if (!scan_type(p, &i)) return 0;
                if (i < p->n && p->toks[i].kind == T_COMMA) { i++; continue; }
                break;
            }
            if (i >= p->n || p->toks[i].kind != T_RBRACK) return 0;
            i++;
        }
    } else if (k == T_LPAREN) {
        i++;
        for (;;) {
            if (!scan_type(p, &i)) return 0;
            if (i < p->n && p->toks[i].kind == T_COMMA) { i++; continue; }
            break;
        }
        if (i >= p->n || p->toks[i].kind != T_RPAREN) return 0;
        i++;
        if (i < p->n && p->toks[i].kind == T_COLON) {
            i++;
            if (!scan_type(p, &i)) return 0;
        }
    } else {
        return 0;
    }
    *at_i = i;
    return 1;
}

/* A bracketed argument in expression position: a function type ('(…): R' or
   'extern "c" (…): R') can only be a type argument, never an index. */
static Node* bracket_arg(P* p) {
    if (at(p, T_EXTERN)) return parse_type(p);
    if (at(p, T_LPAREN)) {
        size_t i = p->i;
        int depth = 0;
        for (; i < p->n; i++) {
            TokKind k = p->toks[i].kind;
            if (k == T_LPAREN) depth++;
            else if (k == T_RPAREN && --depth == 0) break;
        }
        if (i + 1 < p->n && p->toks[i + 1].kind == T_COLON) return parse_type(p);
    }
    return expr(p, 0);
}

static int is_assign_op(TokKind k) {
    switch (k) {
    case T_ASSIGN: case T_ADDEQ: case T_SUBEQ: case T_MULEQ: case T_DIVEQ:
    case T_MODEQ: case T_ANDEQ: case T_OREQ: case T_XOREQ: case T_SHLEQ:
    case T_SHREQ:
        return 1;
    default:
        return 0;
    }
}

#define CMP_LEVEL 7

/* The binary precedence of operator k, 1 (loosest) to 10, or 0 for none. */
static int level(TokKind k) {
    switch (k) {
    case T_OROR: return 1;
    case T_ANDAND: return 2;
    case T_PIPE: return 3;
    case T_CARET: return 4;
    case T_AMP: return 5;
    case T_EQEQ: case T_BANGEQ: return 6;
    case T_LT: case T_LE: case T_GT: case T_GE: return CMP_LEVEL;
    case T_SHL: case T_SHR: return 8;
    case T_PLUS: case T_MINUS: return 9;
    case T_STAR: case T_SLASH: case T_PERCENT: return 10;
    default: return 0;
    }
}

static Node* expr_chain(P* p, Node* first, int ctx);

static Node* expr(P* p, int ctx) { return expr_binary(p, 1, ctx); }

/* Whether level lv is an equality or comparison's, which a bitwise
 * operator (| ^ &, levels 3-5) never takes as an operand unparenthesized. */
static int compare_level(int lv) { return lv == 6 || lv == CMP_LEVEL; }

static Node* expr_climb(P* p, int min, int ctx, int* top);

static Node* expr_binary(P* p, int min, int ctx) {
    int top;
    return expr_climb(p, min, ctx, &top);
}

/* Left-associative operators of level min and tighter, by precedence
 * climbing: one level lookup per operator, not one call per level. top
 * receives the level of the result's outermost operator, 0 for none. */
static Node* expr_climb(P* p, int min, int ctx, int* top) {
    Node* l = expr_unary(p, ctx);
    int ltop = 0;
    for (;;) {
        int lv = level(tk(p)->kind);
        if (lv < min || lv == 0) break;
        if (lv == CMP_LEVEL) {
            l = expr_chain(p, l, ctx);
            ltop = lv;
            continue;
        }
        Token* op = tk(p);
        p->i++;
        int rtop;
        Node* r = expr_climb(p, lv + 1, ctx, &rtop);
        if (lv >= 3 && lv <= 5 && (compare_level(ltop) || compare_level(rtop)))
            perr_at(p, op, "a bitwise operator and a comparison need parentheses to mix");
        l = mkbin(p, op, l, r);
        ltop = lv;
    }
    *top = ltop;
    return l;
}

/* The comparison chain whose first operand is first. */
static Node* expr_chain(P* p, Node* first, int ctx) {
    Node* ch = nd(p, N_CMPCHAIN);
    Node* ops = nd(p, N_OPS);
    NV operands = {0}, opnodes = {0};
    nv_push(p, &operands, first);
    while (level(tk(p)->kind) == CMP_LEVEL) {
        Node* on = nd(p, N_OP);
        on->op = (uint8_t)tk(p)->kind;
        nv_push(p, &opnodes, on);
        p->i++;
        nv_push(p, &operands, expr_binary(p, CMP_LEVEL + 1, ctx));
    }
    set_kids(ch, &operands);
    set_kids(ops, &opnodes);
    ch->a = ops;
    return ch;
}

static Node* expr_unary(P* p, int ctx) {
    if (at(p, T_BANG) || at(p, T_TILDE) || at(p, T_MINUS)) {
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)tk(p)->kind;
        p->i++;
        u->a = expr_unary(p, ctx);
        return u;
    }
    if (at(p, T_MUT) && kth(p, 1) == T_AMP) { // mut &lvalue -> writable pointer
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)T_AMP;
        u->flags |= F_MUT;
        p->i += 2;
        u->a = expr_unary(p, ctx);
        return u;
    }
    if (at(p, T_MUT)) {
        // Type-position mut in expression-position type arguments:
        // Box[mut string], g[mut []u8]. No establishment form begins with
        // '*', '[', or the prim name followed by something other than '['.
        int is_mut_string = kth(p, 1) == T_IDENT && tk(p)[1].len == 6 && memcmp(tk(p)[1].str, "string", 6) == 0;
        if (kth(p, 1) == T_STAR ||
            (kth(p, 1) == T_LBRACK && kth(p, 2) == T_RBRACK) ||
            (is_mut_string && kth(p, 2) != T_LBRACK)) {
            return expr_postfix_base(p, ctx, parse_type(p));
        }
        // A leading mut marks either a value-producing mutating method call
        // or writable-view establishment through a slice.
        p->i++;
        Node* base = expr_postfix(p, ctx);
        if (base->kind == N_SLICE_E) {
            base->flags |= F_MUT;
            return base;
        }
        if (base->kind == N_CALL) {
            base->flags |= F_MUT;
            return base;
        }
        perr(p, "'mut' in an expression marks a method receiver or establishes a writable view");
        return base;
    }
    if (at(p, T_STAR)) {
        /* `*T(address)` remains the pointer-conversion spelling.  Its type is
         * immediately called; every other leading '*' is dereference. */
        size_t after_type = p->i;
        if (scan_type(p, &after_type) && after_type < p->n &&
            p->toks[after_type].kind == T_LPAREN)
            return expr_postfix_base(p, ctx, parse_type(p));
        p->i++;
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)T_STAR;
        u->a = expr_unary(p, ctx);
        return u;
    }
    if (at(p, T_AMP)) { // address-of; lvalue-ness checked in semantic pass
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)T_AMP;
        p->i++;
        u->a = expr_unary(p, ctx);
        return u;
    }
    if (at(p, T_VOLATILE)) {
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)T_VOLATILE;
        p->i++;
        u->a = expr_unary(p, ctx);
        return u;
    }
    return expr_postfix(p, ctx);
}

static Node* parse_structlit(P* p, Node* base) {
    Node* lit = nd(p, N_STRUCTLIT);
    lit->a = base;
    p->i++; // '{'
    NV w = {0};
    if (!at(p, T_RBRACE)) {
        for (;;) {
            Node* fi = nd(p, N_FIELDINIT);
            if (member_kind(tk(p)->kind) && kth(p, 1) == T_COLON)
                perr(p, "struct literal field initialization uses '='; ':' is for declarations and type annotations");
            if (member_kind(tk(p)->kind) && kth(p, 1) == T_ASSIGN) {
                fi->str = tk(p)->str;
                fi->len = tk(p)->len;
                p->i += 2;
            }
            fi->a = expr(p, 0);
            nv_push(p, &w, fi);
            if (!eat(p, T_COMMA)) break;
            if (at(p, T_RBRACE)) break; // trailing comma
        }
    }
    expect(p, T_RBRACE, "'}' to close struct literal");
    set_kids(lit, &w);
    return lit;
}

static Node* parse_staticmaplit(P* p) {
    Node* lit = nd(p, N_STATICMAPLIT);
    expect(p, T_LBRACE, "'{' to open StaticMap literal");
    NV w = {0};
    while (!at(p, T_RBRACE)) {
        Node* entry = nd(p, N_MAPENTRY);
        entry->a = expr(p, 0);
        expect(p, T_COLON, "':' between StaticMap key and value");
        entry->b = expr(p, 0);
        nv_push(p, &w, entry);
        if (!eat(p, T_COMMA)) break;
        if (at(p, T_RBRACE)) break;
    }
    expect(p, T_RBRACE, "'}' to close StaticMap literal");
    set_kids(lit, &w);
    return lit;
}

static Node* expr_postfix(P* p, int ctx) {
    return expr_postfix_base(p, ctx, expr_primary(p, ctx));
}

static Node* expr_postfix_base(P* p, int ctx, Node* base) {
    for (;;) {
        switch (tk(p)->kind) {
        case T_LPAREN: { // call
            Node* call = nd(p, N_CALL);
            call->a = base;
            p->i++;
            if (base->kind == N_IDENT &&
                ((base->len == 6 && memcmp(base->str, "sizeof", 6) == 0) ||
                 (base->len == 7 && memcmp(base->str, "alignof", 7) == 0) ||
                 (base->len == 5 && memcmp(base->str, "blank", 5) == 0))) {
                // sizeof/alignof/blank operands are types: the full inline type
                // grammar is admitted, so slices and arrays need no named alias
                NV w = {0};
                nv_push(p, &w, parse_type(p));
                expect(p, T_RPAREN, "')' after layout type");
                set_kids(call, &w);
                base = call;
                continue;
            }
            if (base->kind == N_IDENT && base->len == 7 && memcmp(base->str, "bitcast", 7) == 0) {
                NV w = {0};
                nv_push(p, &w, parse_type(p));
                expect(p, T_COMMA, "',' after bitcast type");
                nv_push(p, &w, expr(p, 0));
                expect(p, T_RPAREN, "')' after bitcast value");
                set_kids(call, &w);
                base = call;
                continue;
            }
            if (base->kind == N_IDENT && base->len == 8 && memcmp(base->str, "offsetof", 8) == 0) {
                NV w = {0};
                nv_push(p, &w, parse_type(p));
                expect(p, T_COMMA, "',' after offsetof type");
                Token* ft = expect_member(p, "field name in offsetof");
                Node* path = ident_node(p, ft);
                while (eat(p, T_DOT)) {
                    Token* part = expect_member(p, "field name after '.' in offsetof");
                    Node* field = nd_tok(p, N_FIELD_E, part);
                    field->a = path; field->str = part->str; field->len = part->len;
                    path = field;
                }
                nv_push(p, &w, path);
                expect(p, T_RPAREN, "')' after offsetof field path");
                set_kids(call, &w);
                base = call;
                continue;
            }
            NV args = {0};
            if (!at(p, T_RPAREN)) {
                for (;;) {
                    // `mut &value` is one writable-address expression, not a
                    // mut call argument. `mut &value` could not be a valid mut
                    // argument anyway because an address expression is not an lvalue.
                    if (at(p, T_MUT) && kth(p, 1) != T_AMP) {
                        Token* mut = tk(p);
                        p->i++;
                        Node* value = expr(p, 0);
                        if (value->kind == N_SLICE_E) {
                            // `mut seq[lo..hi]` establishes the writable view;
                            // it is the argument value, not a mut-argument mark
                            value->flags |= F_MUT;
                            nv_push(p, &args, value);
                        } else if (value->kind == N_CALL) {
                            // `mut receiver.method()` marks that nested call's
                            // receiver; it is not the enclosing call's mut arg.
                            value->flags |= F_MUT;
                            nv_push(p, &args, value);
                        } else {
                            Node* ma = nd_tok(p, N_MUTARG, mut);
                            ma->a = value;
                            nv_push(p, &args, ma);
                        }
                    } else {
                        nv_push(p, &args, expr(p, 0));
                    }
                    if (!eat(p, T_COMMA)) break;
                    if (at(p, T_RPAREN)) break; // trailing comma
                }
            }
            expect(p, T_RPAREN, "')' to close call arguments");
            set_kids(call, &args);
            base = call;
            continue;
        }
        case T_LBRACK: { // index, slice, or generic instantiation in expr position
            p->i++;
            Node* ix = nd(p, N_INDEX);
            ix->a = base;
            Node* lo = NULL, *hi = NULL;
            int is_slice = 0;
            if (at(p, T_DOTDOT)) {
                is_slice = 1;
                p->i++;
                if (!at(p, T_RBRACK)) hi = expr(p, 0);
            } else {
                lo = bracket_arg(p);
                if (at(p, T_DOTDOT)) {
                    is_slice = 1;
                    p->i++;
                    if (!at(p, T_RBRACK)) hi = expr(p, 0);
                } else if (at(p, T_COMMA)) { // f[int, f64] — multiple type args
                    NV w = {0};
                    nv_push(p, &w, lo);
                    while (eat(p, T_COMMA)) {
                        if (at(p, T_RBRACK)) break;
                        nv_push(p, &w, bracket_arg(p));
                    }
                    expect(p, T_RBRACK, "']' after index/type arguments");
                    set_kids(ix, &w);
                    base = ix;
                    continue;
                }
            }
            expect(p, T_RBRACK, "']' after index/slice");
            if (is_slice) {
                Node* sl = nd(p, N_SLICE_E);
                sl->line = ix->line; // anchor inside the brackets, as for an index
                sl->col = ix->col;
                sl->a = base;
                sl->b = lo;
                sl->c = hi;
                base = sl;
            } else {
                ix->b = lo;
                base = ix;
            }
            continue;
        }
        case T_DOT: { // field access / module member
            p->i++;
            Token* f = expect_member(p, "field name after '.'");
            Node* fa = nd_tok(p, N_FIELD_E, f);
            fa->a = base;
            fa->str = f->str;
            fa->len = f->len;
            base = fa;
            continue;
        }
        case T_LBRACE: // typed struct literal (suppressed at statement/if/for heads)
            if (ctx & CTX_NOSLIT) return base;
            base = parse_structlit(p, base);
            continue;
        default:
            return base;
        }
    }
}

static Node* expr_primary(P* p, int ctx) {
    Token* t = tk(p);
    switch (t->kind) {
    case T_INT: {
        Node* x = nd(p, N_INT);
        x->ival = t->ival;
        p->i++;
        return x;
    }
    case T_FLOAT: {
        Node* x = nd(p, N_FLOAT);
        x->fval = t->fval;
        p->i++;
        return x;
    }
    case T_STR: {
        Node* x = nd(p, N_STR);
        x->str = t->str;
        x->len = t->len;
        p->i++;
        return x;
    }
    case T_CHARLIT: {
        Node* x = nd(p, N_CHAR);
        x->ival = t->ival;
        p->i++;
        return x;
    }
    case T_AT: { // @file / @line: literals for the location written (Errors #16)
        Token* name = &p->toks[p->i + 1];
        if (name->kind == T_IDENT && text_is(name->str, name->len, "line")) {
            Node* x = nd(p, N_INT);
            x->ival = (uint64_t)t->line;
            p->i += 2;
            return x;
        }
        if (name->kind == T_IDENT && text_is(name->str, name->len, "file")) {
            Node* x = nd(p, N_STR);
            x->str = build_path(p->d->path);
            x->len = (uint32_t)strlen(x->str);
            p->i += 2;
            return x;
        }
        perr(p, "expected 'file' or 'line' after '@' in an expression");
        return nd(p, N_INT);
    }
    case T_NULL:
        p->i++;
        return nd(p, N_NULL);
    case T_TRUE:
    case T_FALSE: {
        Node* x = nd(p, N_BOOL);
        x->ival = t->kind == T_TRUE;
        p->i++;
        return x;
    }
    case T_IDENT:
    case T_SELF: { // the receiver reads as an ordinary local
        Node* x = ident_node(p, t);
        p->i++;
        return x;
    }
    case T_LPAREN: { // grouping or tuple expression
        p->i++;
        NV w = {0};
        nv_push(p, &w, expr(p, 0));
        while (eat(p, T_COMMA)) {
            if (at(p, T_RPAREN)) break; // trailing comma
            nv_push(p, &w, expr(p, 0));
        }
        expect(p, T_RPAREN, "')' to close expression");
        if (w.n == 1) {
            if (w.v[0]->kind == N_FIELD_E) w.v[0]->flags |= F_GROUPED; // (x.m) is not a method call
            return w.v[0];
        }
        Node* tu = nd_tok(p, N_TUPLE_E, t); // at '(', not the token after ')
        set_kids(tu, &w);
        return tu;
    }
    case T_LBRACK: { // array literal — or an inline slice type called as a
        // slice-shaped cast syntax ([]u8(value)), admitted only as a callee
        p->i++;
        if (at(p, T_RBRACK) && slice_type_start(kth(p, 1))) {
            Node* ty = nd(p, N_SLICE_T);
            p->i++;
            ty->a = parse_type(p);
            return ty;
        }
        Node* al = nd(p, N_ARRAYLIT);
        NV w = {0};
        if (!at(p, T_RBRACK)) {
            Node* first = expr(p, 0);
            if (eat(p, T_COLON)) {
                al->a = first;
                al->b = expr(p, 0); // repetition count: a compile-time constant expression
            } else {
                int keyed = at(p, T_ASSIGN); // [E.a = x, E.b = y]
                for (Node* el = first;;) {
                    if (keyed) {
                        Node* en = nd(p, N_MAPENTRY);
                        en->a = el;
                        expect(p, T_ASSIGN, "'=' after a keyed array literal's member");
                        en->b = expr(p, 0);
                        el = en;
                    } else if (at(p, T_ASSIGN)) {
                        perr(p, "a keyed array literal keys every element, the first included");
                    }
                    nv_push(p, &w, el);
                    if (!eat(p, T_COMMA) || at(p, T_RBRACK)) break;
                    el = expr(p, 0);
                }
            }
        }
        expect(p, T_RBRACK, "']' to close array literal");
        if (w.n == 1 && !al->a && w.v[0]->kind != N_MAPENTRY && array_cast_ahead(p, 0)) {
            // [N]T(value): an array type called as a cast, admitted only as a callee
            Node* ty = nd_tok(p, N_ARRAY_T, t);
            ty->b = w.v[0];
            ty->a = type_atom(p);
            return ty;
        }
        set_kids(al, &w);
        return al;
    }
    case T_LBRACE: // bare struct literal (type from context)
        if (ctx & CTX_NOSLIT) perr(p, "unexpected '{' — struct literals need a type or parentheses here");
        return parse_structlit(p, NULL);
    default:
        perr(p, "expected expression, got %s", gdesc(p));
        return NULL;
    }
}

// expr_list: comma list; single expression returned bare, else N_TUPLE_E
static Node* expr_list(P* p, int ctx) {
    NV w = {0};
    Token* first = tk(p);
    int line = first->line;
    nv_push(p, &w, expr(p, ctx));
    while (at(p, T_COMMA)) {
        bare_list_line(p, line);
        p->i++;
        bare_list_line(p, line);
        nv_push(p, &w, expr(p, 0));
    }
    if (w.n == 1) return w.v[0];
    Node* tu = nd_tok(p, N_TUPLE_E, first);
    set_kids(tu, &w);
    return tu;
}

// --- statements -----------------------------------------------------------------

static Node* parse_block(P* p);
static Node* parse_stmt(P* p);
static Node* parse_decl_stmt(P* p, Token* first);
static Node* parse_expr_stmt(P* p, int ctx);
static Node* parse_for_head(P* p, Node* n);

static void stmt_end(P* p) {
    if (!eat(p, T_NL) && !at(p, T_RBRACE) && !at(p, T_EOF))
        perr(p, "expected newline or '}' after statement, got %s", gdesc(p));
}

static Node* parse_if(P* p) {
    Node* n = nd(p, N_IF);
    expect(p, T_IF, "'if'");
    n->a = expr(p, CTX_NOSLIT);
    n->b = parse_block(p);
    if (at(p, T_ELSE)) { // must be on the '}' line (lexer emits NL otherwise)
        p->i++;
        if (at(p, T_IF)) n->c = parse_if(p);
        else n->c = parse_block(p);
    }
    return n;
}

/*
 * Scan a `ident ':'` for-head to decide typed binders vs declaration init:
 * the first depth-0 `in` selects the typed range/sequence path; a depth-0
 * `=` or `;` selects a three-clause declaration init. A bounded scan, never
 * backtracking (the variant_binder_at pattern).
 */
static int for_head_typed_at(P* p) {
    size_t j = p->i + 2; // past the binder name and ':'
    int depth = 0;
    while (j < p->n) {
        TokKind k = p->toks[j].kind;
        if (depth == 0) {
            if (k == T_IN) return 1;
            if (k == T_ASSIGN || k == T_SEMI || k == T_LBRACE || k == T_NL || k == T_EOF) return 0;
        }
        if (k == T_LBRACK || k == T_LPAREN || k == T_LBRACE) depth++;
        else if (k == T_RBRACK || k == T_RPAREN || k == T_RBRACE) {
            if (depth == 0) return 0;
            depth--;
        }
        j++;
    }
    return 0;
}

static void for_semi(P* p, int hline, const char* what) {
    if (tk(p)->line != hline) perr(p, "for-head clauses must stay on one physical line");
    expect(p, T_SEMI, what);
}

// Three-clause tail after the init: [cond] ';' [post] block. An omitted
// condition is always true; the post clause accepts assignments and
// expression statements only — parse_expr_stmt cannot produce a declaration.
static void for_tail(P* p, Node* n, int hline) {
    if (!at(p, T_SEMI) && !at(p, T_LBRACE)) n->b = expr(p, CTX_NOSLIT);
    for_semi(p, hline, "';' after the for-head condition");
    if (!at(p, T_LBRACE)) n->c = parse_expr_stmt(p, CTX_NOSLIT);
    if (tk(p)->line != hline) perr(p, "for-head clauses must stay on one physical line");
    n->d = parse_block(p);
}

// Head disambiguation by bounded lookahead, no backtracking: `{` → infinite;
// `;` → three-clause with an empty init; `ident ":"` → typed binders when the
// scan finds a depth-0 `in`, otherwise a declaration init; anything else is
// an expression that becomes the condition or an assignment/expression
// statement init.
static Node* parse_for_head(P* p, Node* n) {
    int hline = tk(p)->line;
    if (at(p, T_LBRACE)) { // infinite: exits only via break/return
        n->flags |= F_FORGEN;
        n->d = parse_block(p);
        return n;
    }
    if (at(p, T_SEMI)) { // three-clause with an empty init
        n->flags |= F_FORGEN;
        p->i++;
        for_tail(p, n, hline);
        return n;
    }
    if (at(p, T_IDENT) && kth(p, 1) == T_COLON && for_head_typed_at(p)) {
        NV binders = {0};
        int line = tk(p)->line;
        for (;;) {
            Token* v = expect(p, T_IDENT, "loop variable name");
            Node* binder = nd_tok(p, N_PARAM, v);
            binder->str = v->str;
            binder->len = v->len;
            expect(p, T_COLON, "':' after loop variable");
            binder->a = parse_type(p);
            nv_push(p, &binders, binder);
            if (!at(p, T_COMMA)) break;
            bare_list_line(p, line);
            p->i++;
            bare_list_line(p, line);
            if (binders.n == 2) perr(p, "for-in accepts at most index and value binders");
        }
        set_kids(n, &binders);
        expect(p, T_IN, "'in'");
        if (eat(p, T_MUT)) n->flags |= F_MUT; // `in mut iterable`: aliasing
        Node* it = expr(p, CTX_NOSLIT);
        if ((n->flags & F_MUT) && it->kind == N_SLICE_E &&
            !(it->flags & F_MUT))
            it->flags |= F_MUT; // `in mut xs[lo..hi]` — establish, then alias
        if (at(p, T_DOTDOT)) {
            Node* r = nd(p, N_RANGE); // positioned at '..', like N_BINARY
            p->i++;
            r->a = it;
            r->b = expr(p, CTX_NOSLIT);
            if (ident_at(p, "step")) {
                p->i++;
                r->c = expr(p, CTX_NOSLIT);
            }
            it = r;
        }
        n->b = it;
        n->c = parse_block(p);
        return n;
    }
    n->flags |= F_FORGEN;
    if (at(p, T_IDENT) && kth(p, 1) == T_COLON) { // declaration init
        Token* name = tk(p);
        p->i++; // ident; ':' consumed inside
        n->a = parse_decl_stmt(p, name);
    } else {
        n->a = parse_expr_stmt(p, CTX_NOSLIT);
        if (at(p, T_LBRACE)) { // condition-only: the expression is the cond
            n->b = n->a->a;
            n->a = NULL;
            n->d = parse_block(p);
            return n;
        }
    }
    for_semi(p, hline, "';' after the for-head initializer");
    for_tail(p, n, hline);
    return n;
}

static Node* parse_for(P* p) {
    Node* n = nd(p, N_FOR);
    expect(p, T_FOR, "'for'");
    return parse_for_head(p, n);
}

/*
 * Scan a union-variant case pattern `Type.variant(binder: T)` — qualified
 * (`ev.Event.key(k: u32)`) and generic (`Result[int].ok(v: T)`) bases
 * included. Returns the token offset of the '(' that opens the binder list,
 * or 0. The `ident ':'` binder shape cannot be an expression, so this stays
 * a bounded scan, never backtracking.
 */
static size_t variant_binder_at(P* p) {
    size_t j = p->i;
    if (j >= p->n || p->toks[j].kind != T_IDENT) return 0;
    j++;
    int segments = 1;
    for (;;) {
        while (j < p->n && p->toks[j].kind == T_LBRACK) { // generic arguments
            int depth = 0;
            for (; j < p->n; j++) {
                if (p->toks[j].kind == T_LBRACK) depth++;
                else if (p->toks[j].kind == T_RBRACK) {
                    depth--;
                    j++;
                    if (!depth) break;
                }
            }
        }
        if (j + 1 < p->n && p->toks[j].kind == T_DOT && member_kind(p->toks[j + 1].kind)) {
            j += 2;
            segments++;
            continue;
        }
        break;
    }
    if (segments < 2) return 0;
    if (j + 2 < p->n && p->toks[j].kind == T_LPAREN &&
        p->toks[j + 1].kind == T_IDENT &&
        (p->toks[j + 2].kind == T_COLON || p->toks[j + 2].kind == T_RPAREN))
        return j;
    return 0;
}

/*
 * Type.variant(binder: T) — union-variant case pattern with a payload
 * binder. The variant reference keeps the N_FIELD_E shape sema resolves for
 * construction; the binder rides in b. Sema stores the variant index in ival.
 */
static Node* parse_variant_pattern(P* p, size_t lparen) {
    (void)lparen;
    Node* base = NULL; // qualified chain of the segments before the variant
    Token* seg = NULL;
    for (;;) {
        int first = !base && !at(p, T_DOT);
        if (at(p, T_DOT)) p->i++; // separator before the next segment
        seg = first ? expect(p, T_IDENT, "type or variant name") : expect_member(p, "type or variant name");
        if (at(p, T_LPAREN)) break; // the variant name (no generics on it)
        Node* node = ident_node(p, seg);
        if (base) {
            Node* q = nd_tok(p, N_FIELD_E, seg);
            q->a = base;
            q->str = seg->str;
            q->len = seg->len;
            base = q;
        } else {
            base = node;
        }
        if (at(p, T_LBRACK)) { // generic arguments on this segment
            p->i++;
            Node* ix = nd(p, N_INDEX);
            ix->a = base;
            NV w = {0};
            for (;;) {
                nv_push(p, &w, parse_type(p));
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RBRACK)) break;
            }
            expect(p, T_RBRACK, "']' to close type arguments");
            set_kids(ix, &w);
            base = ix;
        }
    }
    Node* vp = nd_tok(p, N_VARPAT, seg);
    Node* ref = nd_tok(p, N_FIELD_E, seg);
    ref->a = base;
    ref->str = seg->str;
    ref->len = seg->len;
    vp->a = ref;
    p->i++; // '('
    Token* bname = expect(p, T_IDENT, "binder name");
    Node* binder = nd_tok(p, N_PARAM, bname);
    binder->str = bname->str;
    binder->len = bname->len;
    if (eat(p, T_COLON)) {
        binder->a = parse_type(p);
    } else { // `Variant(k)` — a typeless binder; sema diagnoses it
        binder->a = NULL;
    }
    vp->b = binder;
    expect(p, T_RPAREN, "')' to close variant pattern");
    return vp;
}

/* A case opening with `(` whose matching `)` encloses a top-level comma is a
 * parenthesized case list, never a tuple: no switch value is a tuple. */
static int case_list_at(P* p) {
    if (!at(p, T_LPAREN)) return 0;
    int depth = 0;
    for (size_t j = p->i; j < p->n; j++) {
        TokKind k = p->toks[j].kind;
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE) depth++;
        else if (k == T_RPAREN || k == T_RBRACK || k == T_RBRACE) {
            if (--depth == 0) return 0;
        } else if (k == T_COMMA && depth == 1) return 1;
        else if (k == T_EOF) return 0;
    }
    return 0;
}

static Node* parse_switch(P* p) {
    Node* n = nd(p, N_SWITCH);
    expect(p, T_SWITCH, "'switch'");
    n->a = expr(p, CTX_NOSLIT);
    expect(p, T_LBRACE, "'{' to open switch body");
    NV cases = {0};
    skipnl(p);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) perr(p, "unexpected end of file in switch body");
        Node* c = nd(p, N_CASE);
        if (at(p, T_DEFAULT)) {
            p->i++;
            c->flags |= F_DEFAULT;
        } else if (at(p, T_ELSE)) {
            perr(p, "'else' is not a switch case; use 'default'");
        } else {
            NV pats = {0};
            int line = tk(p)->line;
            int multi = case_list_at(p); // `(` patterns across lines `)`
            if (multi) p->i++;
            for (;;) {
                size_t dot = variant_binder_at(p);
                if (dot) {
                    nv_push(p, &pats, parse_variant_pattern(p, dot));
                } else {
                    nv_push(p, &pats, expr(p, CTX_NOSLIT));
                }
                if (!at(p, T_COMMA)) break;
                if (!multi) bare_list_line(p, line);
                p->i++;
                if (multi && at(p, T_RPAREN)) break;
                if (!multi) bare_list_line(p, line);
            }
            if (multi) {
                if (tk(p)->line == line) perr(p, "parentheses around case values are only for multiline lists");
                expect(p, T_RPAREN, "')' to close case values");
            }
            set_kids(c, &pats);
        }
        c->b = parse_block(p);
        /* `fallthrough` is contextual only for a bare statement directly in
         * a case block. Calls, assignments, declarations, and nested uses keep
         * the ordinary identifier meaning. Position is checked by sema. */
        for (int i = 0; i < c->b->nkids; i++) {
            Node* st = c->b->kids[i];
            if (st->kind != N_EXPRSTMT || !st->a || st->a->kind != N_IDENT) continue;
            Node* id = st->a;
            if (id->len != 11 || memcmp(id->str, "fallthrough", 11) != 0) continue;
            st->kind = (uint8_t)N_FALLTHROUGH;
            st->a = NULL;
            st->str = id->str;
            st->len = id->len;
        }
        nv_push(p, &cases, c);
        skipnl(p);
    }
    p->i++; // '}'
    set_kids(n, &cases);
    return n;
}

static Node* parse_asm(P* p) {
    Node* n = nd(p, N_ASM);
    expect(p, T_ASM, "'asm'");
    Token* s = expect(p, T_STR, "instruction string after 'asm'");
    n->str = s->str;
    n->len = s->len;
    expect(p, T_LBRACE, "'{' to open asm bindings");
    NV binds = {0};
    skipnl(p);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) perr(p, "unexpected end of file in asm block");
        int is_in = 0, is_out = 0, is_clob = 0;
        const char* kind = NULL;
        uint32_t klen = 0;
        if (at(p, T_IN)) { // `in` is a keyword; accept it as the binding kind
            is_in = 1;
            kind = "in";
            klen = 2;
            p->i++;
        } else if (at(p, T_IDENT)) {
            kind = tk(p)->str;
            klen = tk(p)->len;
            is_out = klen == 3 && memcmp(kind, "out", 3) == 0;
            is_clob = klen == 8 && memcmp(kind, "clobbers", 8) == 0;
            if (!is_out && !is_clob) perr(p, "expected in/out/clobbers binding, got '%.*s'", (int)klen, kind);
            p->i++;
        } else {
            perr(p, "expected in/out/clobbers binding, got %s", gdesc(p));
        }
        Node* bnd = nd(p, N_ASMBIND);
        bnd->str = kind;
        bnd->len = klen;
        expect(p, T_COLON, "':' after binding kind");
        NV items = {0};
        for (;;) {
            Token* reg = expect(p, T_IDENT, "register name");
            nv_push(p, &items, ident_node(p, reg));
            if (is_in || is_out) {
                expect(p, T_ASSIGN, "'=' in register binding");
                if (at(p, T_INT)) { // x16 = 3 — immediate operand
                    Node* imm = nd(p, N_INT);
                    imm->ival = tk(p)->ival;
                    p->i++;
                    nv_push(p, &items, imm);
                } else {
                    nv_push(p, &items, ident_node(p, expect(p, T_IDENT, "value name")));
                }
            }
            if (!eat(p, T_COMMA)) break;
            if (at(p, T_RBRACE) || at(p, T_NL)) break;
        }
        set_kids(bnd, &items);
        nv_push(p, &binds, bnd);
        if (!eat(p, T_NL) && !at(p, T_RBRACE)) perr(p, "expected newline or '}' after asm binding");
        skipnl(p);
    }
    p->i++; // '}'
    set_kids(n, &binds);
    return n;
}

// x: T = e | x: const T = e | n: T, m: U = f() (typed destructuring) | label: { }
static Node* parse_decl_stmt(P* p, Token* first) {
    Node* n = nd_tok(p, N_VAR, first);
    NV binders = {0};
    int is_const = 0;
    Token* name = first;
    for (;;) {
        if (at(p, T_COLON)) {
            p->i++;
            if (at(p, T_CONST)) {
                is_const = 1;
                p->i++;
            }
            Node* binder = nd_tok(p, N_PARAM, name);
            binder->str = name->str;
            binder->len = name->len;
            binder->a = parse_type(p);
            nv_push(p, &binders, binder);
        } else {
            Node* binder = nd_tok(p, N_IDENT, name);
            binder->str = name->str;
            binder->len = name->len;
            nv_push(p, &binders, binder);
            n->flags |= F_MIXED;
        }
        if (!at(p, T_COMMA)) break;
        bare_list_line(p, first->line);
        p->i++;
        bare_list_line(p, first->line);
        name = expect(p, T_IDENT, "binder name after ','");
    }
    if (is_const) n->flags |= F_CONST;
    set_kids(n, &binders);
    if (eat(p, T_ASSIGN)) {
        if (n->nkids > 1) n->flags |= F_MIXED;
        n->b = n->nkids == 1 && spells_staticmap(n->kids[0]->a) && at(p, T_LBRACE)
            ? parse_staticmaplit(p) : expr_list(p, 0);
    } else if (n->nkids > 1 || (n->flags & F_MIXED)) {
        perr(p, "destructuring declaration requires an initializer");
    }
    return n;
}

static Node* parse_stmt(P* p) {
    switch (tk(p)->kind) {
    case T_IF: return parse_if(p);
    case T_FOR: return parse_for(p);
    case T_SWITCH: return parse_switch(p);
    case T_ASM: return parse_asm(p);
    case T_RETURN: {
        Node* n = nd(p, N_RETURN);
        p->i++;
        if (!at(p, T_NL) && !at(p, T_RBRACE) && !at(p, T_EOF)) n->a = expr_list(p, 0);
        return n;
    }
    case T_BREAK: {
        Node* n = nd(p, N_BREAK);
        p->i++;
        if (at(p, T_IDENT)) {
            n->str = tk(p)->str;
            n->len = tk(p)->len;
            p->i++;
        }
        return n;
    }
    case T_CONTINUE: {
        Node* n = nd(p, N_CONTINUE);
        p->i++;
        if (at(p, T_IDENT)) {
            n->str = tk(p)->str;
            n->len = tk(p)->len;
            p->i++;
        }
        return n;
    }
    case T_DEFER: {
        Node* n = nd(p, N_DEFER);
        p->i++;
        Token* mut = tk(p);
        if (eat(p, T_MUT)) { // `defer mut c.scale(2.0)` — receiver-marked call
            Node* e = expr(p, CTX_NOSLIT);
            if (e->kind != N_CALL) perr_at(p, mut, "'mut' after 'defer' marks a call receiver");
            e->flags |= F_MUT;
            n->a = e;
        } else {
            n->a = expr(p, CTX_NOSLIT);
        }
        return n;
    }
    case T_MUT: { // `mut c.scale(2.0)` — receiver-marked call statement
        Token* mut = tk(p);
        Node* n = nd_tok(p, N_EXPRSTMT, mut);
        p->i++;
        Node* e = expr(p, CTX_NOSLIT);
        if (e->kind != N_CALL) perr_at(p, mut, "'mut' at statement level marks a call receiver");
        e->flags |= F_MUT;
        n->a = e;
        return n;
    }
    case T_IDENT:
        if (kth(p, 1) == T_COLON) {
            if (kth(p, 2) == T_LBRACE) { // labeled block
                Node* n = nd(p, N_LABEL);
                n->str = tk(p)->str;
                n->len = tk(p)->len;
                p->i += 2;
                n->a = parse_block(p);
                return n;
            }
            if (kth(p, 2) == T_FOR) { // labeled for — block-label namespace
                Node* n = nd(p, N_FOR);
                n->str = tk(p)->str;
                n->len = tk(p)->len;
                p->i += 3; // label, ':', 'for'
                return parse_for_head(p, n);
            }
            Token* name = tk(p);
            p->i++; // ident; ':' consumed inside
            return parse_decl_stmt(p, name);
        }
        return parse_expr_stmt(p, 0);
    default:
        return parse_expr_stmt(p, 0);
    }
}

// Expression-led statement: assignment (single or destructuring) or a bare
// expression statement. Shared by plain statements and for-head clauses;
// for-head tails rely on it never producing a declaration. ctx carries the
// caller's struct-literal suppression — a for-head clause is followed by the
// body's '{', so head clauses parse with CTX_NOSLIT (grammar Note #9).
static Node* parse_expr_stmt(P* p, int ctx) {
    Token* start = tk(p);
    int line = start->line;
    Node* e = expr(p, ctx);
    if (at(p, T_COMMA)) { // destructuring assignment, possibly mixed under `=`
        Node* a = nd(p, N_ASSIGN);
        NV targets = {0};
        nv_push(p, &targets, e);
        while (at(p, T_COMMA)) {
            bare_list_line(p, line);
            p->i++;
            bare_list_line(p, line);
            if (at(p, T_IDENT) && kth(p, 1) == T_COLON) {
                Token* name = tk(p);
                p->i += 2;
                Node* binder = nd_tok(p, N_PARAM, name);
                binder->str = name->str;
                binder->len = name->len;
                binder->a = parse_type(p);
                nv_push(p, &targets, binder);
                a->flags |= F_MIXED;
            } else {
                nv_push(p, &targets, expr(p, ctx));
            }
        }
        if (a->flags & F_MIXED) {
            for (int i = 0; i < targets.n; i++)
                if (targets.v[i]->kind != N_IDENT && targets.v[i]->kind != N_PARAM)
                    perr(p, "mixed '=' left side accepts names only");
            a->kind = N_VAR;
        }
        Token* op = expect(p, T_ASSIGN, "'=' in assignment");
        a->op = (uint8_t)op->kind;
        set_kids(a, &targets);
        if (a->kind == N_VAR) a->b = expr_list(p, ctx);
        else a->a = expr_list(p, ctx);
        return a;
    }
    if (is_assign_op(tk(p)->kind)) { // single-target assignment (incl. compound)
        Node* a = nd(p, N_ASSIGN);
        a->op = (uint8_t)tk(p)->kind;
        if (e->kind == N_UNARY && e->op == T_VOLATILE) {
            if (a->op != T_ASSIGN) perr(p, "volatile access supports only simple '=' assignment");
            a->flags |= F_VOLATILE;
            e = e->a;
        }
        p->i++;
        NV targets = {0};
        nv_push(p, &targets, e);
        set_kids(a, &targets);
        a->a = expr(p, ctx);
        return a;
    }
    Node* n = nd_tok(p, N_EXPRSTMT, start);
    n->a = e;
    return n;
}

/* Skips the construct starting at token `start`, bracket-balanced, to the
 * newline that ends it or the unmatched '}' of its block (left for the block).
 * In formatted code a token at column 1 starts a declaration or is the '}'
 * ending one, so the skip also stops there whatever the depth: an unclosed
 * '(' costs one declaration, not the rest of the file. A declaration's skip
 * (`top`) takes such a '}' as its own end; a statement's leaves it.
 */
static void skip_from(P* p, size_t start, int top) {
    p->i = start;
    int depth = 0;
    for (;; p->i++) {
        TokKind k = tk(p)->kind;
        if (k == T_EOF) return;
        if (k != T_NL && tk(p)->col == 1 && p->i > start) {
            if (!top || k != T_RBRACE) return;
            depth = 0;
            continue;
        }
        if (k == T_LPAREN || k == T_LBRACK || k == T_LBRACE) depth++;
        else if (k == T_RPAREN || k == T_RBRACK) { if (depth > 0) depth--; }
        else if (k == T_RBRACE) {
            if (depth == 0) {
                if (p->i > start) return;
            } else depth--;
        } else if (k == T_NL && depth == 0) return;
    }
}

// One statement through its terminator, or NULL after a syntax error in it.
static Node* guarded_stmt(P* p) {
    size_t start = p->i;
    sigjmp_buf* outer = p->sync;
    sigjmp_buf here;
    if (sigsetjmp(here, 0)) {
        p->sync = outer;
        skip_from(p, start, 0);
        return NULL;
    }
    p->sync = &here;
    Node* n = parse_stmt(p);
    stmt_end(p);
    p->sync = outer;
    return n;
}

static Node* parse_block(P* p) {
    Node* b = nd(p, N_BLOCK);
    expect(p, T_LBRACE, "'{'");
    NV stmts = {0};
    skipnl(p);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) perr(p, "unexpected end of file, expected '}'");
        Node* n = guarded_stmt(p);
        if (n) nv_push(p, &stmts, n);
        skipnl(p);
    }
    p->i++; // '}'
    set_kids(b, &stmts);
    return b;
}

// --- declarations ---------------------------------------------------------------

/* An explicit receiver `self` / `mut self` parses as an untyped first
 * parameter (a == NULL); only a method or contract method accepts one. */
static Node* parse_params(P* p, const char* closer, int self_ok) {
    NV params = {0};
    if (!at(p, T_RPAREN)) {
        for (;;) {
            Node* pa = nd(p, N_PARAM);
            for (;;) {
                if (eat(p, T_MUT)) pa->flags |= F_MUT;
                else if (eat(p, T_NOALIAS)) pa->flags |= F_NOALIAS;
                else break;
            }
            if (at(p, T_SELF)) {
                if (!self_ok) perr(p, "'self' is only the receiver of a method declaration");
                if (params.n) perr(p, "'self' must be the first parameter");
                if (pa->flags & F_NOALIAS) perr(p, "'noalias' does not apply to the receiver 'self'");
                p->i++;
                if (at(p, T_COLON)) perr(p, "'self' takes no type; its type is the method's owner");
                pa->str = "self";
                pa->len = 4;
                nv_push(p, &params, pa);
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RPAREN)) break;
                continue;
            }
            Token* name = expect(p, T_IDENT, "parameter name");
            pa->str = name->str;
            pa->len = name->len;
            expect(p, T_COLON, "':' after parameter name");
            pa->a = parse_type(p);
            nv_push(p, &params, pa);
            if (!eat(p, T_COMMA)) break;
            if (at(p, T_RPAREN)) break; // trailing comma
        }
    }
    expect(p, T_RPAREN, closer);
    Node* holder = nd(p, N_TPARAMS);
    set_kids(holder, &params);
    return holder;
}

static Node* parse_ret(P* p) {
    if (!eat(p, T_COLON)) return NULL;
    if (at_void(p)) {
        perr(p, "a function with no return value omits its return clause; 'void' is only a function type's return");
        return NULL;
    }
    return parse_type(p);
}

static int named_attr_at(P* p, const char* name) {
    size_t n = strlen(name);
    if (!at(p, T_AT)) return 0;
    return kth(p, 1) == T_IDENT && tk(p)[1].len == n && memcmp(tk(p)[1].str, name, n) == 0;
}

/* Parse @align(expr) or @offset(expr), including its required terminating
 * physical line. */
static Node* parse_layout_attr(P* p, const char* name) {
    expect(p, T_AT, "'@' before a layout attribute");
    if (!ident_at(p, name)) perr(p, "expected @%s attribute", name);
    p->i++;
    expect(p, T_LPAREN, "'(' after a layout attribute");
    Node* value = expr(p, 0);
    expect(p, T_RPAREN, "')' after a layout attribute expression");
    int line = p->toks[p->i - 1].line;
    skipnl(p);
    if (tk(p)->line == line) perr(p, "@%s must occupy its own line before a declaration or field", name);
    return value;
}

static Node* parse_align_attr(P* p) { return parse_layout_attr(p, "align"); }

// If toks[i] starts a type-parameter list `['T,' U']' (idents only), consume it.
static Node* try_tparams(P* p) {
    if (!at(p, T_LBRACK) || kth(p, 1) != T_IDENT) return NULL;
    size_t j = p->i + 1;
    int ok = 0;
    while (j < p->n && p->toks[j].kind == T_IDENT) {
        j++;
        if (j >= p->n) break;
        if (p->toks[j].kind == T_COMMA) {
            j++;
            continue;
        }
        if (p->toks[j].kind == T_RBRACK) ok = 1;
        break;
    }
    if (!ok) return NULL;
    Node* tp = nd(p, N_TPARAMS);
    NV w = {0};
    p->i++; // '['
    for (;;) {
        nv_push(p, &w, ident_node(p, expect(p, T_IDENT, "type parameter")));
        if (!eat(p, T_COMMA)) break;
        if (at(p, T_RBRACK)) break;
    }
    p->i++; // ']'
    set_kids(tp, &w);
    return tp;
}

// `as "name"` after a C-boundary signature or type: the linker symbol when it
// differs from the Ace-side name. `as` is contextual, like `from`.
static const char* parse_link_name(P* p) {
    if (!ident_at(p, "as")) return NULL; // an identifier fits: IDENT_MAX < 510
    p->i++;
    Token* link = expect(p, T_STR, "link name string after 'as'");
    if (link->len == 0) perr(p, "extern link name cannot be empty");
    if (memchr(link->str, 0, link->len)) perr(p, "extern link name cannot contain NUL");
    // emit.c prefixes the Mach-O underscore in a 512-byte buffer.
    if (link->len > 510) perr(p, "extern link name cannot exceed 510 bytes");
    return link->str; // STR tokens are NUL-terminated arena copies
}

static Node* parse_func_rest(P* p, Token* name, Node* tp, int pub, int expo, int self_ok) {
    Node* f = nd_tok(p, N_FUNC, name);
    f->str = name->str;
    f->len = name->len;
    if (pub) f->flags |= F_PUB;
    if (expo) f->flags |= F_EXPO;
    f->c = tp;
    expect(p, T_LPAREN, "'(' after function name");
    Node* ph = parse_params(p, "')' to close parameters", self_ok);
    f->kids = ph->kids;
    f->nkids = ph->nkids;
    f->a = parse_ret(p);
    if (expo) f->cname = parse_link_name(p); // an export's C symbol
    if (eat(p, T_SPECIALIZED)) {
        f->flags |= F_SPECIALIZED; // no-default template: no body follows
        return f;
    }
    if (eat(p, T_INTRINSIC)) {
        f->flags |= F_INTRINSIC; // compiler-owned implementation; no source body
        return f;
    }
    f->b = parse_block(p);
    return f;
}

static Node* owner_type(P* p, Token* name, Node* tp) {
    Node* t = nd_tok(p, N_NAMED, name);
    t->str = name->str;
    t->len = name->len;
    if (tp) {
        NV args = {0};
        for (int i = 0; i < tp->nkids; i++) {
            Node* a = nd(p, N_NAMED);
            a->str = tp->kids[i]->str;
            a->len = tp->kids[i]->len;
            nv_push(p, &args, a);
        }
        set_kids(t, &args);
    }
    return t;
}

/* Type.name(...): a method when its first parameter is the receiver `self`
 * (typed here as the owner), otherwise an associated function. */
static Node* parse_type_func_rest(P* p, Token* owner, Node* tp, int pub) {
    expect(p, T_DOT, "'.' after function owner");
    Token* name = expect_member(p, "function name after '.'");
    Node* f = parse_func_rest(p, name, tp, pub, 0, 1);
    f->d = owner_type(p, owner, tp);
    if (f->nkids && !f->kids[0]->a) {
        f->flags |= F_METHOD;
        if (f->kids[0]->flags & F_MUT) f->flags |= F_MUT;
        f->kids[0]->a = f->d;
    } else {
        f->flags |= F_ASSOC;
    }
    return f;
}

static Node* parse_signing(P* p, Token* owner, Node* tp, int pub) {
    Node* n = nd_tok(p, N_SIGNING, owner);
    n->a = owner_type(p, owner, tp);
    n->c = tp;
    if (pub) n->flags |= F_PUB;
    Token* signs = expect(p, T_SIGNS, "'signs'");
    NV contracts = {0};
    if (eat(p, T_LPAREN)) {
        if (at(p, T_RPAREN)) perr(p, "a signing needs at least one contract");
        for (;;) {
            Node* c = nd(p, N_CONFORMANCE);
            c->a = parse_type(p);
            nv_push(p, &contracts, c);
            if (!eat(p, T_COMMA)) break;
            if (at(p, T_RPAREN)) break;
        }
        if (tk(p)->line == signs->line) perr(p, "parentheses after 'signs' are only for multiline contract lists");
        expect(p, T_RPAREN, "')' to close signed contracts");
    } else {
        if (at(p, T_NL) || at(p, T_EOF)) perr(p, "a signing needs at least one contract");
        for (;;) {
            signing_list_line(p, signs->line);
            Node* c = nd(p, N_CONFORMANCE);
            c->a = parse_type(p);
            nv_push(p, &contracts, c);
            if (p->toks[p->i - 1].line != signs->line)
                perr(p, "bare signed-contract lists must stay on one line; use parentheses for a multiline list");
            if (!at(p, T_COMMA)) break;
            signing_list_line(p, signs->line);
            p->i++;
            signing_list_line(p, signs->line);
        }
    }
    set_kids(n, &contracts);
    return n;
}

// After `Name:` — kind word decides; otherwise it is a global var/const.
static Node* parse_colon_decl(P* p, Token* name, Node* tp, int pub) {
    p->i++; // ':'
    Node* n;
    if (ident_at(p, "bits")) {
        if (tp) perr(p, "bits types are not generic");
        p->i++;
        n = nd_tok(p, N_BITS, name);
        n->str = name->str; n->len = name->len;
        if (pub) n->flags |= F_PUB;
        n->a = parse_type(p);
        expect(p, T_LBRACE, "'{' after bits backing type");
        NV fields = {0};
        if (!at(p, T_RBRACE)) for (;;) {
            Token* fname = expect_member(p, "bit-field name or '_'");
            Node* fl = nd_tok(p, N_FIELD, fname);
            fl->str = fname->str; fl->len = fname->len;
            expect(p, T_COLON, "':' after bit-field name");
            if (ident_at(p, "signed")) { fl->flags |= F_CONST; p->i++; }
            Token* width = expect(p, T_INT, "positive bit-field width");
            fl->ival = width->ival;
            nv_push(p, &fields, fl);
            if (!eat(p, T_COMMA)) break;
            if (at(p, T_RBRACE)) break;
        }
        expect(p, T_RBRACE, "'}' to close bits type");
        set_kids(n, &fields);
        return n;
    }
    switch (tk(p)->kind) {
    case T_STRUCT: {
        p->i++;
        n = nd_tok(p, N_STRUCT, name);
        n->str = name->str;
        n->len = name->len;
        n->c = tp;
        if (pub) n->flags |= F_PUB;
        expect(p, T_LBRACE, "'{' after 'struct'");
        NV fields = {0};
        if (!at(p, T_RBRACE)) {
            for (;;) {
                Node* field_offset = NULL;
                Node* field_align = NULL;
                if (named_attr_at(p, "offset")) {
                    field_offset = parse_layout_attr(p, "offset");
                    if (named_attr_at(p, "offset")) perr(p, "a field accepts at most one @offset attribute");
                }
                if (named_attr_at(p, "align")) {
                    field_align = parse_align_attr(p);
                    if (named_attr_at(p, "align")) perr(p, "a field accepts at most one @align attribute");
                    if (named_attr_at(p, "offset")) perr(p, "@offset precedes @align on a field");
                } else if (at(p, T_AT)) {
                    perr(p, "only @offset and @align may modify a struct field");
                }
                int field_pub = at(p, T_PUB) && kth(p, 1) != T_COLON && eat(p, T_PUB); // `pub: T` names a field
                int embed = at(p, T_EMBED) && kth(p, 1) != T_COLON && eat(p, T_EMBED); // `embed: T` names a field
                Token* fname = expect_member(p, "field name");
                Node* fl = nd_tok(p, N_FIELD, fname);
                fl->str = fname->str;
                fl->len = fname->len;
                if (embed) fl->flags |= F_EMBED;
                if (field_pub) fl->flags |= F_PUB;
                fl->b = field_align;
                fl->d = field_offset;
                expect(p, T_COLON, "':' after field name");
                fl->a = parse_type(p);
                if (eat(p, T_ASSIGN)) fl->c = expr(p, 0);
                nv_push(p, &fields, fl);
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RBRACE)) break; // trailing comma
            }
        }
        expect(p, T_RBRACE, "'}' to close struct");
        set_kids(n, &fields);
        return n;
    }
    case T_CONTRACT: {
        p->i++;
        n = nd_tok(p, N_CONTRACT, name);
        n->str = name->str;
        n->len = name->len;
        n->c = tp;
        if (pub) n->flags |= F_PUB;
        expect(p, T_LBRACE, "'{' after 'contract'");
        NV methods = {0};
        skipnl(p);
        while (!at(p, T_RBRACE)) {
            if (at(p, T_EOF)) perr(p, "unexpected end of file in contract");
            Node* m = nd(p, N_METHOD);
            if (at(p, T_MUT)) perr(p, "a contract method takes no leading 'mut'; a mutating receiver is written 'mut self'");
            Token* mname = expect_member(p, "method name");
            m->str = mname->str;
            m->len = mname->len;
            expect(p, T_LPAREN, "'(' after method name");
            Node* ph = parse_params(p, "')' to close method parameters", 1);
            if (!ph->nkids || ph->kids[0]->a)
                perr_at(p, mname, "a contract method's first parameter is its receiver 'self' or 'mut self'");
            // the receiver stays implied in the contract's method node
            if (ph->kids[0]->flags & F_MUT) m->flags |= F_MUT;
            m->kids = ph->kids + 1;
            m->nkids = ph->nkids - 1;
            m->a = parse_ret(p);
            nv_push(p, &methods, m);
            if (!eat(p, T_NL) && !at(p, T_RBRACE)) perr(p, "expected newline or '}' after method");
            skipnl(p);
        }
        p->i++;
        set_kids(n, &methods);
        return n;
    }
    case T_ENUM: {
        p->i++;
        n = nd_tok(p, N_ENUM, name);
        n->str = name->str;
        n->len = name->len;
        if (pub) n->flags |= F_PUB;
        if (!at(p, T_LBRACE)) n->a = parse_type(p);
        expect(p, T_LBRACE, "'{' after 'enum'");
        NV members = {0};
        if (!at(p, T_RBRACE)) {
            for (;;) {
                Node* m = ident_node(p, expect_member(p, "enum member"));
                if (eat(p, T_ASSIGN)) m->a = expr(p, 0);
                nv_push(p, &members, m);
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RBRACE)) break; // trailing comma
            }
        }
        expect(p, T_RBRACE, "'}' to close enum");
        set_kids(n, &members);
        return n;
    }
    case T_UNION: {
        p->i++;
        n = nd_tok(p, N_UNION, name);
        n->str = name->str;
        n->len = name->len;
        n->c = tp;
        if (pub) n->flags |= F_PUB;
        expect(p, T_LBRACE, "'{' after 'union'");
        NV variants = {0};
        if (!at(p, T_RBRACE)) {
            for (;;) {
                Token* vname = expect_member(p, "variant name");
                Node* v = nd_tok(p, N_VARIANT, vname);
                v->str = vname->str;
                v->len = vname->len;
                if (eat(p, T_COLON)) // variant payload: exactly one explicit type
                    v->a = parse_type(p);
                nv_push(p, &variants, v);
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RBRACE)) break; // trailing comma
            }
        }
        expect(p, T_RBRACE, "'}' to close union");
        set_kids(n, &variants);
        return n;
    }
    case T_DISTINCT: {
        p->i++;
        n = nd_tok(p, N_DISTINCT, name);
        n->str = name->str;
        n->len = name->len;
        if (pub) n->flags |= F_PUB;
        n->a = parse_type(p);
        return n;
    }
    case T_TYPE: { // alias: Name: type = Existing
        p->i++;
        n = nd_tok(p, N_ALIAS, name);
        n->str = name->str;
        n->len = name->len;
        n->c = tp;
        if (pub) n->flags |= F_PUB;
        expect(p, T_ASSIGN, "'=' after 'type'");
        n->a = parse_type(p);
        return n;
    }
    default: { // contextual `error` domain, or global var / const
        if (at(p, T_IDENT) && tk(p)->len == 5 && memcmp(tk(p)->str, "error", 5) == 0) {
            p->i++;
            n = nd_tok(p, N_ERROR_DOMAIN, name);
            n->str = name->str;
            n->len = name->len;
            if (pub) n->flags |= F_PUB;
            if (eat(p, T_LBRACE)) {
                NV entries = {0};
                skipnl(p);
                while (!at(p, T_RBRACE)) {
                    if (at(p, T_EOF)) perr(p, "unexpected end of file in error domain");
                    Node* entry = nd(p, N_FIELD);
                    entry->a = expr(p, 0);
                    expect(p, T_COLON, "':' after error code");
                    Token* ename = expect_member(p, "error entry name");
                    entry->str = ename->str;
                    entry->len = ename->len;
                    expect(p, T_ASSIGN, "'=' after error entry name");
                    entry->b = expr(p, 0);
                    nv_push(p, &entries, entry);
                    skipnl(p);
                    if (!eat(p, T_COMMA)) {
                        if (!at(p, T_RBRACE)) perr(p, "expected ',' after error entry");
                        break;
                    }
                    skipnl(p);
                }
                p->i++;
                set_kids(n, &entries);
            }
            return n;
        }
        n = nd_tok(p, N_VAR, name);
        if (pub) n->flags |= F_PUB;
        int is_const = 0;
        if (at(p, T_CONST)) {
            is_const = 1;
            p->i++;
        }
        if (at(p, T_IDENT) && kth(p, 1) == T_LBRACE)
            perr(p, "unknown declaration type '%.*s'", (int)tk(p)->len, tk(p)->str);
        Node* binder = nd_tok(p, N_PARAM, name);
        binder->str = name->str;
        binder->len = name->len;
        binder->a = parse_type(p);
        NV binders = {0};
        nv_push(p, &binders, binder);
        if (is_const) n->flags |= F_CONST;
        set_kids(n, &binders);
        if (eat(p, T_ASSIGN))
            n->b = spells_staticmap(binder->a) && at(p, T_LBRACE)
                ? parse_staticmaplit(p) : expr_list(p, 0);
        else if (is_const) perr(p, "const declaration requires an initializer");
        return n;
    }
    }
}

// A C-ABI definition: `name(params): R { body }`. The body is what makes it
// an export rather than an import (C Interop #2).
static Node* parse_extern_def(P* p, size_t start, int pub) {
    p->i = start;
    Token* name = tk(p);
    p->i++;
    return parse_func_rest(p, name, try_tparams(p), pub, 1, 0); // sema rejects generics
}

// An exported C global after `[mut] name: T` and `=`: an ordinary global with
// C linkage, read-only unless mut (C Interop #6).
static Node* parse_extern_global(P* p, Token* name, Node* type, const char* cname, int mut, int pub) {
    Node* n = nd_tok(p, N_VAR, name);
    Node* binder = nd_tok(p, N_PARAM, name);
    binder->str = name->str;
    binder->len = name->len;
    binder->a = type;
    NV binders = {0};
    nv_push(p, &binders, binder);
    set_kids(n, &binders);
    n->flags |= F_EXPO | (mut ? F_MUT : 0) | (pub ? F_PUB : 0);
    n->cname = binder->cname = cname;
    binder->flags |= F_EXPO | (mut ? F_MUT : 0);
    expect(p, T_ASSIGN, "'=' and an initializer: a C global defined in Ace");
    n->b = expr_list(p, 0);
    return n;
}

static Node* parse_extern(P* p) {
    Node* n = nd(p, N_EXTERN);
    expect(p, T_EXTERN, "'extern'");
    Token* abi = expect(p, T_STR, "ABI string after 'extern'");
    n->str = abi->str;
    n->len = abi->len;
    expect(p, T_LBRACE, "'{' to open extern block");
    NV fns = {0};
    p->exports = (NV){0};
    skipnl(p);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) perr(p, "unexpected end of file in extern block");
        Node* f = nd(p, N_EXTERNFN);
        size_t start = p->i;
        int def = 0; // a body follows: an Ace definition with C linkage
        if (eat(p, T_MUT)) f->flags |= F_MUT;
        Token* name = expect(p, T_IDENT, "name in extern block");
        f->str = name->str;
        f->len = name->len;
        if (eat(p, T_COLON)) { // a C global variable (C Interop #6)
            f->kind = N_EXTERNVAR;
            f->a = parse_type(p);
            f->cname = parse_link_name(p);
            if (at(p, T_ASSIGN)) {
                nv_push(p, &p->exports, parse_extern_global(p, name, f->a, f->cname, (f->flags & F_MUT) != 0, 0));
                if (!eat(p, T_NL) && !at(p, T_RBRACE)) perr(p, "expected newline or '}' after extern declaration");
                skipnl(p);
                continue;
            }
        } else {
            if (f->flags & F_MUT) perr(p, "'mut' marks an extern variable: expected ':' after its name");
            if (at(p, T_LBRACK)) def = 1; // generic: reparsed as a definition for sema to reject
            else expect(p, T_LPAREN, "'(' after extern function name");
            if (!def && !at(p, T_RPAREN)) {
                NV params = {0};
                for (;;) {
                    Node* pa = nd(p, N_PARAM);
                    if (at(p, T_MUT)) { // only a definition can say this; reparsed below
                        def = 1;
                        break;
                    }
                    Token* pname = expect(p, T_IDENT, "parameter name");
                    pa->str = pname->str;
                    pa->len = pname->len;
                    expect(p, T_COLON, "':' after parameter name");
                    pa->a = parse_type(p);
                    nv_push(p, &params, pa);
                    if (!eat(p, T_COMMA)) break;
                    if (at(p, T_DOTDOTDOT)) {
                        p->i++;
                        f->flags |= F_VARARGS;
                        break;
                    }
                    if (at(p, T_RPAREN)) break; // trailing comma
                }
                set_kids(f, &params);
            }
            if (!def) {
                expect(p, T_RPAREN, "')' to close extern parameters");
                f->a = parse_ret(p);
                f->cname = parse_link_name(p);
                def = at(p, T_LBRACE);
            }
            if (def) {
                if (f->flags & F_VARARGS) perr(p, "variadic functions cannot be defined in Ace");
                nv_push(p, &p->exports, parse_extern_def(p, start, 0));
                if (!eat(p, T_NL) && !at(p, T_RBRACE)) perr(p, "expected newline or '}' after extern declaration");
                skipnl(p);
                continue;
            }
        }
        nv_push(p, &fns, f);
        if (!eat(p, T_NL) && !at(p, T_RBRACE)) perr(p, "expected newline or '}' after extern declaration");
        skipnl(p);
    }
    p->i++;
    set_kids(n, &fns);
    return n;
}

static Node* parse_internal_import(P* p) {
    expect(p, T_AT, "'@'");
    Token* name = expect(p, T_IDENT, "reserved module name after '@'");
    if (at(p, T_DOT)) perr(p, "reserved module imports omit the .ace suffix");
    Node* path = nd(p, N_STR);
    path->str = name->str;
    path->len = name->len;
    path->op = T_AT;
    return path;
}

// A bare or alias-target module reference may name a nested module path,
// e.g. `net/tcp`. `first` is the already-consumed leading segment; this
// consumes any `/ident` continuations and returns the segment chain (head to
// leaf, linked through each segment's own `a`), with `*leaf` set to the last
// segment. A single-segment path is a one-node chain.
static Node* parse_import_path(P* p, Token* first, Node** leaf) {
    Node* head = ident_node(p, first);
    Node* tail = head;
    while (at(p, T_SLASH)) {
        p->i++;
        Token* seg = expect(p, T_IDENT, "module path segment after '/'");
        Node* next = ident_node(p, seg);
        tail->a = next;
        tail = next;
    }
    *leaf = tail;
    return head;
}

static Node* parse_import(P* p) {
    Node* n = nd(p, N_IMPORT);
    expect(p, T_IMPORT, "'import'");

    NV selected = {0};
    int parenthesized = eat(p, T_LPAREN);
    Token* first = expect(p, T_IDENT, parenthesized ? "imported member name" : "module or member name after 'import'");
    if (parenthesized || at(p, T_COMMA) || ident_at(p, "from")) {
        n->flags |= F_SELECT;
        nv_push(p, &selected, ident_node(p, first));
        if (parenthesized) {
            for (;;) {
                if (!eat(p, T_COMMA)) break;
                if (at(p, T_RPAREN)) break;
                Token* member = expect(p, T_IDENT, "imported member name");
                nv_push(p, &selected, ident_node(p, member));
            }
            expect(p, T_RPAREN, "')' after imported member list");
            if (p->toks[p->i - 1].line == n->line)
                perr(p, "parentheses in a selective import are only for a multiline member list");
        } else {
            int line = first->line;
            while (at(p, T_COMMA)) {
                bare_list_line(p, line);
                p->i++;
                bare_list_line(p, line);
                Token* member = expect(p, T_IDENT, "imported member name");
                nv_push(p, &selected, ident_node(p, member));
            }
        }
        set_kids(n, &selected);
        if (!ident_at(p, "from")) perr(p, "expected 'from' after imported member list, got %s", gdesc(p));
        p->i++;
        if (at(p, T_IDENT)) {
            Token* target = tk(p);
            p->i++;
            Node* leaf;
            n->c = parse_import_path(p, target, &leaf);
            n->b = leaf;
        } else if (at(p, T_STR)) {
            n->a = nd(p, N_STR);
            n->a->str = tk(p)->str;
            n->a->len = tk(p)->len;
            p->i++;
        } else if (at(p, T_AT)) {
            n->a = parse_internal_import(p);
        } else {
            perr(p, "expected module name, path, or internal module after 'from'");
        }
    } else {
        Node* leaf;
        n->c = parse_import_path(p, first, &leaf);
        n->str = leaf->str;
        n->len = leaf->len;
    }
    if (eat(p, T_COLON)) {
        if (n->flags & F_SELECT) perr(p, "selective imports use 'from', not ':'");
        if (at(p, T_IDENT)) { // import m: math — alias binding
            Token* target = tk(p);
            p->i++;
            Node* leaf;
            n->c = parse_import_path(p, target, &leaf);
            n->b = leaf;
        } else if (at(p, T_STR)) { // import util: "./util.ace" — path form
            n->a = nd(p, N_STR);
            n->a->str = tk(p)->str;
            n->a->len = tk(p)->len;
            p->i++;
        } else if (at(p, T_AT)) { // import big: @bigint — reserved form
            n->a = parse_internal_import(p);
        } else {
            perr(p, "expected module name, path, or internal module after ':' in import");
        }
    }
    if (!eat(p, T_NL) && !at(p, T_EOF)) perr(p, "expected newline after import");
    return n;
}

// @when has a deliberately closed evaluator: target/string comparisons,
// booleans, and/or/not, and parentheses. Inactive declarations are still
// parsed for syntax, then omitted from the file before later compiler stages.
static int when_or(P* p);

static int when_primary(P* p) {
    if (eat(p, T_TRUE)) return 1;
    if (eat(p, T_FALSE)) return 0;
    if (eat(p, T_LPAREN)) {
        int value = when_or(p);
        expect(p, T_RPAREN, "')' after @when expression");
        return value;
    }
    if (ident_at(p, "target")) {
        p->i++;
        TokKind op = tk(p)->kind;
        if (op != T_EQEQ && op != T_BANGEQ) perr(p, "expected '==' or '!=' after target in @when expression");
        p->i++;
        Token* value = expect(p, T_STR, "target string after comparison");
        int equal = strlen(p->target) == value->len && memcmp(p->target, value->str, value->len) == 0;
        return op == T_EQEQ ? equal : !equal;
    }
    if (at(p, T_IDENT)) perr(p, "unknown @when compile-time name '%.*s'", (int)tk(p)->len, tk(p)->str);
    perr(p, "expected target comparison, boolean, or parenthesized @when expression");
    return 0;
}

static int when_not(P* p) {
    if (ident_at(p, "not")) {
        p->i++;
        return !when_not(p);
    }
    return when_primary(p);
}

static int when_and(P* p) {
    int value = when_not(p);
    while (ident_at(p, "and")) {
        p->i++;
        int rhs = when_not(p); // parse even when the value is already false
        value = value && rhs;
    }
    return value;
}

static int when_or(P* p) {
    int value = when_and(p);
    while (ident_at(p, "or")) {
        p->i++;
        int rhs = when_and(p); // parse even when the value is already true
        value = value || rhs;
    }
    return value;
}

static int parse_when(P* p) {
    expect(p, T_AT, "'@'");
    if (!ident_at(p, "when")) perr(p, "only the @when attribute is supported");
    p->i++;
    expect(p, T_LPAREN, "'(' after @when");
    int active = when_or(p);
    expect(p, T_RPAREN, "')' after @when expression");
    int attribute_line = p->toks[p->i - 1].line;
    skipnl(p);
    if (tk(p)->line == attribute_line) perr(p, "@when must occupy its own line before a declaration");
    if (at(p, T_EOF)) perr(p, "@when must be followed by a top-level declaration");
    return active;
}

static void parse_layout_attrs(P* p, int* packed, Node** align, int* cold) {
    while (at(p, T_AT)) {
        if (named_attr_at(p, "cold")) {
            if (*cold) perr(p, "a declaration accepts at most one @cold attribute");
            p->i += 2;
            int line = p->toks[p->i - 1].line;
            skipnl(p);
            if (tk(p)->line == line) perr(p, "@cold must occupy its own line before a declaration");
            *cold = 1;
            continue;
        }
        if (named_attr_at(p, "packed")) {
            if (*packed) perr(p, "a declaration accepts at most one @packed attribute");
            if (*align) perr(p, "@packed must precede @align");
            p->i += 2;
            int line = p->toks[p->i - 1].line;
            skipnl(p);
            if (tk(p)->line == line) perr(p, "@packed must occupy its own line before a declaration");
            *packed = 1;
            continue;
        }
        if (named_attr_at(p, "align")) {
            if (*align) perr(p, "a declaration accepts at most one @align attribute");
            *align = parse_align_attr(p);
            continue;
        }
        if (named_attr_at(p, "when")) perr(p, "@when must precede layout attributes");
        perr(p, "unknown top-level attribute");
    }
}

static Node* parse_top_named(P* p, int pub);

static Node* parse_top(P* p) {
    if (at(p, T_IMPORT)) perr(p, "imports must precede declarations");
    int pub = 0, inl = 0;
    for (;;) {
        if (at(p, T_PUB)) {
            if (inl) perr(p, "visibility must come before 'inline'");
            if (pub) perr(p, "duplicate 'pub' modifier");
            p->i++; pub = 1;
        }
        else if (at(p, T_INLINE)) {
            if (inl) perr(p, "duplicate 'inline' modifier");
            p->i++; inl = 1;
        }
        else if (at(p, T_MUT)) {
            perr(p, "a top-level declaration takes no leading 'mut'; a mutating receiver is written 'mut self'");
        }
        else break;
    }
    if (at(p, T_EXTERN) && p->i + 2 < p->n && p->toks[p->i + 1].kind == T_STR &&
        (p->toks[p->i + 2].kind == T_IDENT || p->toks[p->i + 2].kind == T_MUT)) {
        // `extern "c" name(...) { body }` or `extern "c" [mut] name: T = value`
        p->i += 2;
        int gmut = eat(p, T_MUT);
        if (gmut || (p->i + 1 < p->n && p->toks[p->i + 1].kind == T_COLON)) {
            if (inl) perr(p, "'inline' applies only to function declarations");
            Token* name = expect(p, T_IDENT, "C global name");
            expect(p, T_COLON, "':' after C global name");
            Node* type = parse_type(p);
            const char* cname = parse_link_name(p);
            return parse_extern_global(p, name, type, cname, gmut, pub);
        }
        if (p->i + 1 < p->n && p->toks[p->i + 1].kind == T_DOT)
            perr(p, "methods and associated functions cannot use the C ABI");
        Node* f = parse_extern_def(p, p->i, pub);
        if (inl) f->flags |= F_INLINE;
        return f;
    }
    if (at(p, T_EXTERN)) {
        if (pub || inl) perr(p, "'extern' blocks take no pub/inline");
        return parse_extern(p);
    }
    if (!at(p, T_IDENT)) perr(p, "expected declaration, got %s", gdesc(p));
    size_t start = p->i;
    Node* d = parse_top_named(p, pub);
    if (inl) {
        if (d->kind != N_FUNC) {
            p->i = start; // report at the declaration's name
            perr(p, "'inline' applies only to function declarations");
        }
        d->flags |= F_INLINE;
    }
    return d;
}

static Node* parse_top_named(P* p, int pub) {
    Token* name = tk(p);
    p->i++;
    if (name->len == 13 && memcmp(name->str, "static_assert", 13) == 0 && at(p, T_LPAREN)) {
        p->i--;
        Node* n = nd_tok(p, N_EXPRSTMT, name);
        n->a = expr(p, 0);
        return n;
    }
    Node* tp = try_tparams(p);
    if (at(p, T_SIGNS)) {
        return parse_signing(p, name, tp, pub);
    }
    if (at(p, T_DOT)) return parse_type_func_rest(p, name, tp, pub);
    if (at(p, T_LPAREN)) return parse_func_rest(p, name, tp, pub, 0, 0);
    if (at(p, T_COLON)) return parse_colon_decl(p, name, tp, pub);
    perr(p, "expected '(' or ':' after top-level name '%.*s'", (int)name->len, name->str);
    return NULL;
}

// One top-level item through its newline; after a syntax error, skips it.
static void top_item(P* p, NV* decls, int* saw_declaration) {
    size_t start = p->i;
    sigjmp_buf here;
    if (sigsetjmp(here, 0)) {
        skip_from(p, start, 1);
        p->exports = (NV){0};
        return;
    }
    p->sync = &here;
    int active = 1, packed = 0, cold = 0;
    Node* explicit_align = NULL;
    if (named_attr_at(p, "when")) active = parse_when(p);
    parse_layout_attrs(p, &packed, &explicit_align, &cold);
    if (at(p, T_IMPORT)) {
        if (packed || explicit_align) perr(p, "layout attributes do not apply to imports");
        if (cold) perr(p, "@cold applies only to function declarations");
        Node* imp = parse_import(p);
        if (active) {
            if (*saw_declaration) perr(p, "imports must precede declarations");
            nv_push(p, decls, imp);
        }
    } else {
        size_t at_decl = p->i;
        Node* decl = parse_top(p);
        if (cold) {
            if (decl->kind != N_FUNC || (decl->flags & F_EXPO)) {
                p->i = at_decl;
                perr(p, "@cold applies only to Ace function declarations");
            }
            decl->flags |= F_COLD;
        }
        if ((packed || explicit_align) && decl->kind != N_STRUCT)
            perr(p, "layout attributes apply only to struct declarations");
        if (decl->kind == N_STRUCT) {
            if (packed) decl->flags |= F_PACKED;
            decl->b = explicit_align;
        }
        if (!eat(p, T_NL) && !at(p, T_EOF)) perr(p, "expected newline after declaration, got %s", gdesc(p));
        if (active) {
            nv_push(p, decls, decl);
            if (decl->kind == N_EXTERN)
                for (int j = 0; j < p->exports.n; j++) nv_push(p, decls, p->exports.v[j]);
            *saw_declaration = 1;
        }
        p->exports = (NV){0};
    }
}

Node* parse(Token* toks, size_t ntok, Diag* d, const char* target) {
    P p = {0};
    p.toks = toks;
    p.n = ntok;
    p.d = d;
    p.target = target;
    Node* file = (Node*)arena_alloc(sizeof(Node));
    memset(file, 0, sizeof *file);
    file->kind = (uint8_t)N_FILE;
    file->line = 1;
    file->col = 1;
    file->path = d->path;
    if (sigsetjmp(p.stop, 0)) return file;
    NV decls = {0};
    skipnl(&p);
    int saw_declaration = 0;
    while (!at(&p, T_EOF)) {
        top_item(&p, &decls, &saw_declaration);
        skipnl(&p);
    }
    set_kids(file, &decls);
    return file;
}

// --- dump -----------------------------------------------------------------------

const char* node_kind_name(NKind k) {
    static const char* const names[] = {
#define N(_, s) s,
        NODE_KINDS(N)
#undef N
    };
    return names[k];
}

static void pflags(Node* x) {
    if (x->flags & F_PUB) printf(" pub");
    if (x->flags & F_EXPO) printf(" extern-c");
    if (x->flags & F_MUT) printf(" mut");
    if (x->flags & F_NOALIAS) printf(" noalias");
    if (x->flags & F_CONST) printf(" const");
    if (x->flags & F_DEFAULT) printf(" default");
    if (x->flags & F_VARARGS) printf(" varargs");
    if (x->flags & F_METHOD) printf(" method");
    if (x->flags & F_ASSOC) printf(" assoc");
    if (x->flags & F_SELECT) printf(" select");
    if (x->flags & F_EMBED) printf(" embed");
    if (x->flags & F_SPECIALIZED) printf(" specialized");
    if (x->flags & F_INTRINSIC) printf(" intrinsic");
    if (x->flags & F_INLINE) printf(" inline");
    if (x->flags & F_COLD) printf(" cold");
    if (x->flags & F_PACKED) printf(" packed");
    if (x->flags & F_VOLATILE) printf(" volatile");
    if (x->flags & F_CABI) printf(" c_abi");
}

static void pnode(Node* x, int ind) {
    for (int i = 0; i < ind; i++) putchar(' ');
    if (!x) {
        printf("nil\n");
        return;
    }
    printf("(%s", node_kind_name((NKind)x->kind));
    if (x->str) printf(" '%.*s'", (int)x->len, x->str);
    if (x->kind == N_INT || x->kind == N_CHAR || x->kind == N_BOOL) printf(" %llu", (unsigned long long)x->ival);
    if (x->kind == N_FLOAT) printf(" %g", x->fval);
    if (x->kind == N_BINARY || x->kind == N_UNARY || x->kind == N_ASSIGN ||
        x->kind == N_OP)
        printf(" op='%s'", tok_disp((TokKind)x->op));
    pflags(x);
    printf(" @%d:%d", x->line, x->col);
    if (!x->a && !x->b && !x->c && !x->d && !x->kids) {
        printf(")\n");
        return;
    }
    printf("\n");
    if (x->a) pnode(x->a, ind + 1);
    if (x->b) pnode(x->b, ind + 1);
    if (x->c) pnode(x->c, ind + 1);
    if (x->d) pnode(x->d, ind + 1);
    for (int i = 0; i < x->nkids; i++) pnode(x->kids[i], ind + 1);
    for (int i = 0; i < ind; i++) putchar(' ');
    printf(")\n");
}

void parse_dump(Node* file) {
    pnode(file, 0);
}
