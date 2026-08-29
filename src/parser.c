// parser.c — recursive-descent parser for Ace (stage two): tokens → AST.
//
// Node layout (a/b/c plus kids where lists are needed):
//   N_FILE      kids = imports + top decls
//   N_IMPORT    str = binding name; b = module ident (alias form); a = N_STR path (path form)
//   N_VAR       kids = binders (N_PARAM: str name, a type); b = init (or N_TUPLE_E); F_CONST
//   N_FUNC      str = name; kids = params; a = ret; b = body; c = N_TPARAMS;
//               d = owner type for methods; F_PUB/F_EXPO/F_METHOD/F_MUT
//   N_PARAM     str = name; a = type; F_MUT/F_NOALIAS (also used as var binder)
//   N_STRUCT    str = name; kids = N_FIELD; c = N_TPARAMS; F_PUB
//   N_FIELD     str = name; a = type
//   N_CONTRACT str = name; kids = N_METHOD; c = N_TPARAMS; F_PUB
//   N_METHOD    str = name; kids = params; a = ret; F_MUT (mutating receiver)
//   N_ENUM      str = name; kids = N_IDENT members; F_PUB
//   N_ALIAS     str = name; a = type; c = N_TPARAMS; F_PUB
//   N_DISTINCT  str = name; a = base type; F_PUB
//   N_SIGNING     a = concrete type; c = owner N_TPARAMS; kids = N_CONFORMANCE
//   N_CONFORMANCE a = signed contract type; kids = resolved methods after sema
//   N_EXTERN    str = abi string; kids = N_EXTERNFN
//   N_EXTERNFN  str = name; kids = params; a = ret; F_EXPO/F_VARARGS
//   N_BLOCK     kids = statements
//   N_EXPRSTMT  a = expr
//   N_ASSIGN    kids = targets; a = rhs; op = T_ASSIGN or compound token
//   N_IF        a = cond; b = then-block; c = else (N_BLOCK or N_IF)
//   N_FOR       str = elem name; a = elem type; b = iterable (expr) or N_RANGE; c = body
//   N_RANGE     a = lo; b = hi
//   N_SWITCH    a = scrutinee; kids = N_CASE (kids = patterns; b = block; F_ELSE)
//   N_RETURN    a = expr / N_TUPLE_E / NULL
//   N_BREAK     str = label or NULL
//   N_ASM       str = instruction text; kids = N_ASMBIND (str = in/out/clobbers;
//               kids = [reg, val, reg, val, ...] for in/out, [reg, ...] for clobbers)
//   N_LABEL     str = label; a = block
//   N_NAMED     str = name; a = qualifier (N_NAMED/N_IDENT chain); kids = generic args (types)
//   N_PTR       a = pointee; F_MUT
//   N_SLICE_T   a = element
//   N_ARRAY_T   a = element; ival = length
//   N_TUPLE_T   kids = types
//   N_FUNC_T    kids = param types; a = ret
//   N_OPT       a = inner (the `?` binds loosest: `*T?` is a nullable pointer)
//   N_CONST_T   a = inner
//   N_TPARAMS   kids = N_IDENT
//   N_BINARY    op; a; b      N_CMPCHAIN kids = operands; a = N_OPS{kids = N_OP}
//   N_UNARY     op (T_AMP = address-of); a
//   N_CALL      a = callee; kids = args (N_MUTARG wraps mut-marked args); F_MUT = receiver mark
//   N_INDEX     a = base; b = index
//   N_SLICE_E   a = base; b = lo (nullable); c = hi (nullable)
//   N_FIELD_E   a = base; str = field
//   N_ARRAYLIT  kids = elements
//   N_STRUCTLIT a = type expr or NULL (bare literal); kids = N_FIELDINIT (str or NULL; a = value)
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
    Token* toks;
    size_t n, i;
    Diag*  d;
    jmp_buf jb;
} P;

// --- cursor helpers -----------------------------------------------------------

static Token* tk(P* p) { return &p->toks[p->i]; }

static TokKind kth(P* p, int k) {
    size_t j = p->i + (size_t)k;
    return j < p->n ? p->toks[j].kind : T_EOF;
}

static int at(P* p, TokKind k) { return tk(p)->kind == k; }

static int ate(P* p, TokKind k) {
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
    static char buf[80];
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

static void perr(P* p, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", p->d->path, tk(p)->line, tk(p)->col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    p->d->errors++;
    longjmp(p->jb, 1);
}

static void bare_list_line(P* p, int line) {
    if (tk(p)->line != line)
        perr(p, "bare comma-separated lists must stay on one physical line");
}

static void signing_list_line(P* p, int line) {
    if (tk(p)->line != line)
        perr(p, "bare signed-contract lists must stay on one line; use parentheses for a multiline list");
}

static Token* expect(P* p, TokKind k, const char* what) {
    if (!at(p, k))
        perr(p, "expected %s, got %s", what, gdesc(p));
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

typedef struct {
    Node** v;
    int n, cap;
} NV;

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

static Node* type_atom(P* p) {
    Node* t;
    if (at(p, T_CONST)) {
        t = nd(p, N_CONST_T);
        p->i++;
        t->a = parse_type(p);
        return t;
    }
    if (at(p, T_MUT)) { // `mut *T` — the only type-level mut (spec Mutability #7)
        t = nd(p, N_PTR);
        t->flags |= F_MUT;
        p->i++;
        expect(p, T_STAR, "'*' after 'mut'");
        t->a = type_atom(p); // `?` binds to the whole pointer, not the pointee
        return t;
    }
    if (at(p, T_STAR)) {
        t = nd(p, N_PTR);
        p->i++;
        t->a = type_atom(p);
        return t;
    }
    if (at(p, T_LBRACK)) {
        p->i++;
        if (ate(p, T_RBRACK)) { // []T slice
            t = nd(p, N_SLICE_T);
            t->a = type_atom(p);
            return t;
        }
        Token* n = expect(p, T_INT, "array length"); // [N]T array
        t = nd(p, N_ARRAY_T);
        t->ival = n->ival;
        expect(p, T_RBRACK, "']' after array length");
        t->a = type_atom(p);
        return t;
    }
    if (at(p, T_LPAREN)) { // tuple or function type
        t = nd(p, N_TUPLE_T);
        p->i++;
        NV w = {0};
        for (;;) {
            nv_push(p, &w, parse_type(p));
            if (!ate(p, T_COMMA)) break;
            if (at(p, T_RPAREN)) break; // trailing comma
        }
        expect(p, T_RPAREN, "')' to close type list");
        if (at(p, T_COLON)) { // (params): ret — function value type
            p->i++;
            Node* f = nd(p, N_FUNC_T);
            set_kids(f, &w);
            if (at(p, T_LPAREN)) {
                p->i++;
                Node* r = nd(p, N_TUPLE_T);
                NV rw = {0};
                for (;;) {
                    nv_push(p, &rw, parse_type(p));
                    if (!ate(p, T_COMMA)) break;
                    if (at(p, T_RPAREN)) break;
                }
                expect(p, T_RPAREN, "')' to close return list");
                f->a = rw.n == 1 ? rw.v[0] : (set_kids(r, &rw), r);
            } else {
                f->a = parse_type(p);
            }
            return f;
        }
        if (w.n == 1) return w.v[0]; // (T) ≡ T
        set_kids(t, &w);
        return t;
    }
    if (at(p, T_IDENT)) { // named, qualified, generic-instantiated
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
                nv_push(p, &w, parse_type(p));
                if (!ate(p, T_COMMA)) break;
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

// type := atom, then `?` suffixes binding loosest (spec: `*T?` = nullable ptr)
static Node* parse_type(P* p) {
    Node* t = type_atom(p);
    while (at(p, T_QUESTION)) {
        Node* o = nd(p, N_OPT);
        p->i++;
        o->a = t;
        t = o;
    }
    return t;
}

// --- expressions ----------------------------------------------------------------

static Node* expr(P* p, int ctx);

static Node* mkbin(P* p, TokKind op, Node* l, Node* r) {
    Node* b = nd(p, N_BINARY);
    b->op = (uint8_t)op;
    b->a = l;
    b->b = r;
    return b;
}

// precedence chain, C table (loosest → tightest):
// || → && → | → ^ → & → ==/!= → </<=/>/>= (chains) → <</>> → +,- → *,/,% → unary
static Node* expr_mul(P* p, int ctx);
static Node* expr_add(P* p, int ctx);
static Node* expr_shift(P* p, int ctx);
static Node* expr_cmp(P* p, int ctx);
static Node* expr_bitand(P* p, int ctx);
static Node* expr_bitxor(P* p, int ctx);
static Node* expr_bitor(P* p, int ctx);
static Node* expr_eq(P* p, int ctx);
static Node* expr_and(P* p, int ctx);
static Node* expr_orlvl(P* p, int ctx);
static Node* expr_unary(P* p, int ctx);
static Node* expr_postfix(P* p, int ctx);
static Node* expr_primary(P* p, int ctx);

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

static int is_cmp_op(TokKind k) {
    switch (k) {
    case T_LT: case T_LE: case T_GT: case T_GE:
        return 1;
    default:
        return 0;
    }
}

static Node* expr(P* p, int ctx) { return expr_orlvl(p, ctx); }

static Node* expr_orlvl(P* p, int ctx) {
    Node* l = expr_and(p, ctx);
    while (at(p, T_OROR)) {
        p->i++;
        l = mkbin(p, T_OROR, l, expr_and(p, ctx));
    }
    return l;
}

static Node* expr_and(P* p, int ctx) {
    Node* l = expr_bitor(p, ctx);
    while (at(p, T_ANDAND)) {
        p->i++;
        l = mkbin(p, T_ANDAND, l, expr_bitor(p, ctx));
    }
    return l;
}

static Node* expr_bitor(P* p, int ctx) {
    Node* l = expr_bitxor(p, ctx);
    while (at(p, T_PIPE)) {
        p->i++;
        l = mkbin(p, T_PIPE, l, expr_bitxor(p, ctx));
    }
    return l;
}

static Node* expr_bitxor(P* p, int ctx) {
    Node* l = expr_bitand(p, ctx);
    while (at(p, T_CARET)) {
        p->i++;
        l = mkbin(p, T_CARET, l, expr_bitand(p, ctx));
    }
    return l;
}

static Node* expr_bitand(P* p, int ctx) {
    Node* l = expr_eq(p, ctx);
    while (at(p, T_AMP)) {
        p->i++;
        l = mkbin(p, T_AMP, l, expr_eq(p, ctx));
    }
    return l;
}

static Node* expr_eq(P* p, int ctx) {
    Node* l = expr_cmp(p, ctx);
    while (at(p, T_EQEQ) || at(p, T_BANGEQ)) {
        TokKind op = tk(p)->kind;
        p->i++;
        l = mkbin(p, op, l, expr_cmp(p, ctx));
    }
    return l;
}
static Node* expr_cmp(P* p, int ctx) {
    Node* first = expr_shift(p, ctx);
    if (!is_cmp_op(tk(p)->kind)) return first;
    Node* ch = nd(p, N_CMPCHAIN);
    Node* ops = nd(p, N_OPS);
    NV operands = {0}, opnodes = {0};
    nv_push(p, &operands, first);
    while (is_cmp_op(tk(p)->kind)) {
        Node* on = nd(p, N_OP);
        on->op = (uint8_t)tk(p)->kind;
        nv_push(p, &opnodes, on);
        p->i++;
        nv_push(p, &operands, expr_shift(p, ctx));
    }
    set_kids(ch, &operands);
    set_kids(ops, &opnodes);
    ch->a = ops;
    return ch;
}

static Node* expr_shift(P* p, int ctx) {
    Node* l = expr_add(p, ctx);
    while (at(p, T_SHL) || at(p, T_SHR)) {
        TokKind op = tk(p)->kind;
        p->i++;
        l = mkbin(p, op, l, expr_add(p, ctx));
    }
    return l;
}

static Node* expr_add(P* p, int ctx) {
    Node* l = expr_mul(p, ctx);
    while (at(p, T_PLUS) || at(p, T_MINUS)) {
        TokKind op = tk(p)->kind;
        p->i++;
        l = mkbin(p, op, l, expr_mul(p, ctx));
    }
    return l;
}

static Node* expr_mul(P* p, int ctx) {
    Node* l = expr_unary(p, ctx);
    while (at(p, T_STAR) || at(p, T_SLASH) || at(p, T_PERCENT)) {
        TokKind op = tk(p)->kind;
        p->i++;
        l = mkbin(p, op, l, expr_unary(p, ctx));
    }
    return l;
}

static Node* expr_unary(P* p, int ctx) {
    if (at(p, T_BANG) || at(p, T_TILDE) || at(p, T_MINUS)) {
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)tk(p)->kind;
        p->i++;
        u->a = expr_unary(p, ctx);
        return u;
    }
    if (at(p, T_AMP)) { // address-of; lvalue-ness checked in semantic pass
        Node* u = nd(p, N_UNARY);
        u->op = (uint8_t)T_AMP;
        p->i++;
        u->a = expr_postfix(p, ctx);
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
            if (at(p, T_IDENT) && kth(p, 1) == T_COLON) {
                fi->str = tk(p)->str;
                fi->len = tk(p)->len;
                p->i += 2;
            }
            fi->a = expr(p, 0);
            nv_push(p, &w, fi);
            if (!ate(p, T_COMMA)) break;
            if (at(p, T_RBRACE)) break; // trailing comma
        }
    }
    expect(p, T_RBRACE, "'}' to close struct literal");
    set_kids(lit, &w);
    return lit;
}

static Node* expr_postfix(P* p, int ctx) {
    Node* base = expr_primary(p, ctx);
    for (;;) {
        switch (tk(p)->kind) {
        case T_LPAREN: { // call
            Node* call = nd(p, N_CALL);
            call->a = base;
            p->i++;
            NV args = {0};
            if (!at(p, T_RPAREN)) {
                for (;;) {
                    if (at(p, T_MUT)) {
                        Node* ma = nd(p, N_MUTARG);
                        p->i++;
                        ma->a = expr(p, 0);
                        nv_push(p, &args, ma);
                    } else {
                        nv_push(p, &args, expr(p, 0));
                    }
                    if (!ate(p, T_COMMA)) break;
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
                lo = expr(p, 0);
                if (at(p, T_DOTDOT)) {
                    is_slice = 1;
                    p->i++;
                    if (!at(p, T_RBRACK)) hi = expr(p, 0);
                } else if (at(p, T_COMMA)) { // f[int, f64] — multiple type args
                    NV w = {0};
                    nv_push(p, &w, lo);
                    while (ate(p, T_COMMA)) {
                        if (at(p, T_RBRACK)) break;
                        nv_push(p, &w, expr(p, 0));
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
            Token* f = expect(p, T_IDENT, "field name after '.'");
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
    case T_IDENT: {
        Node* x = ident_node(p, t);
        p->i++;
        return x;
    }
    case T_LPAREN: { // grouping or tuple expression
        p->i++;
        NV w = {0};
        nv_push(p, &w, expr(p, 0));
        while (ate(p, T_COMMA)) {
            if (at(p, T_RPAREN)) break; // trailing comma
            nv_push(p, &w, expr(p, 0));
        }
        expect(p, T_RPAREN, "')' to close expression");
        if (w.n == 1) return w.v[0];
        Node* tu = nd(p, N_TUPLE_E);
        set_kids(tu, &w);
        return tu;
    }
    case T_LBRACK: { // array literal
        p->i++;
        Node* al = nd(p, N_ARRAYLIT);
        NV w = {0};
        if (!at(p, T_RBRACK)) {
            for (;;) {
                nv_push(p, &w, expr(p, 0));
                if (!ate(p, T_COMMA)) break;
                if (at(p, T_RBRACK)) break;
            }
        }
        expect(p, T_RBRACK, "']' to close array literal");
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
    int line = tk(p)->line;
    nv_push(p, &w, expr(p, ctx));
    while (at(p, T_COMMA)) {
        bare_list_line(p, line);
        p->i++;
        bare_list_line(p, line);
        nv_push(p, &w, expr(p, 0));
    }
    if (w.n == 1) return w.v[0];
    Node* tu = nd(p, N_TUPLE_E);
    set_kids(tu, &w);
    return tu;
}

// --- statements -----------------------------------------------------------------

static Node* parse_block(P* p);
static Node* parse_stmt(P* p);

static void stmt_end(P* p) {
    if (!ate(p, T_NL) && !at(p, T_RBRACE) && !at(p, T_EOF))
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

static Node* parse_for(P* p) {
    Node* n = nd(p, N_FOR);
    expect(p, T_FOR, "'for'");
    Token* v = expect(p, T_IDENT, "loop variable name");
    n->str = v->str;
    n->len = v->len;
    expect(p, T_COLON, "':' after loop variable");
    n->a = parse_type(p);
    expect(p, T_IN, "'in'");
    Node* it = expr(p, CTX_NOSLIT);
    if (at(p, T_DOTDOT)) {
        p->i++;
        Node* r = nd(p, N_RANGE);
        r->a = it;
        r->b = expr(p, CTX_NOSLIT);
        it = r;
    }
    n->b = it;
    n->c = parse_block(p);
    return n;
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
        if (at(p, T_ELSE)) {
            p->i++;
            c->flags |= F_ELSE;
        } else {
            NV pats = {0};
            int line = tk(p)->line;
            nv_push(p, &pats, expr(p, CTX_NOSLIT));
            while (at(p, T_COMMA)) {
                bare_list_line(p, line);
                p->i++;
                bare_list_line(p, line);
                nv_push(p, &pats, expr(p, CTX_NOSLIT));
            }
            set_kids(c, &pats);
        }
        c->b = parse_block(p);
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
            if (!is_out && !is_clob)
                perr(p, "expected in/out/clobbers binding, got '%.*s'", (int)klen, kind);
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
                    nv_push(p, &items,
                            ident_node(p, expect(p, T_IDENT, "value name")));
                }
            }
            if (!ate(p, T_COMMA)) break;
            if (at(p, T_RBRACE) || at(p, T_NL)) break;
        }
        set_kids(bnd, &items);
        nv_push(p, &binds, bnd);
        if (!ate(p, T_NL) && !at(p, T_RBRACE))
            perr(p, "expected newline or '}' after asm binding");
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
        p->i++; // ':' of this binder
        if (at(p, T_CONST)) {
            is_const = 1;
            p->i++;
        }
        Node* binder = nd_tok(p, N_PARAM, name);
        binder->str = name->str;
        binder->len = name->len;
        binder->a = parse_type(p);
        nv_push(p, &binders, binder);
        if (!at(p, T_COMMA)) break;
        bare_list_line(p, first->line);
        p->i++;
        bare_list_line(p, first->line);
        name = expect(p, T_IDENT, "binder name after ','");
        if (!at(p, T_COLON)) perr(p, "expected ':' in destructuring declaration");
    }
    if (is_const) n->flags |= F_CONST;
    set_kids(n, &binders);
    if (ate(p, T_ASSIGN)) {
        n->b = expr_list(p, 0);
    } else if (n->nkids > 1) {
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
        if (!at(p, T_NL) && !at(p, T_RBRACE) && !at(p, T_EOF))
            n->a = expr_list(p, 0);
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
    case T_CONTINUE:
        p->i++;
        return nd(p, N_CONTINUE);
    case T_MUT: { // `mut c.scale(2.0)` — receiver-marked call statement
        p->i++;
        Node* e = expr(p, CTX_NOSLIT);
        if (e->kind != N_CALL) perr(p, "'mut' at statement level marks a call receiver");
        e->flags |= F_MUT;
        Node* n = nd(p, N_EXPRSTMT);
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
            Token* name = tk(p);
            p->i++; // ident; ':' consumed inside
            return parse_decl_stmt(p, name);
        }
        // fall through: expression / assignment statement
    default: {
        int line = tk(p)->line;
        Node* e = expr(p, CTX_NOSLIT);
        if (at(p, T_COMMA)) { // destructuring assignment: n, err = read()
            Node* a = nd(p, N_ASSIGN);
            NV targets = {0};
            nv_push(p, &targets, e);
            while (at(p, T_COMMA)) {
                bare_list_line(p, line);
                p->i++;
                bare_list_line(p, line);
                nv_push(p, &targets, expr(p, 0));
            }
            Token* op = expect(p, T_ASSIGN, "'=' in assignment");
            a->op = (uint8_t)op->kind;
            set_kids(a, &targets);
            a->a = expr_list(p, 0);
            return a;
        }
        if (is_assign_op(tk(p)->kind)) { // single-target assignment (incl. compound)
            Node* a = nd(p, N_ASSIGN);
            a->op = (uint8_t)tk(p)->kind;
            p->i++;
            NV targets = {0};
            nv_push(p, &targets, e);
            set_kids(a, &targets);
            a->a = expr(p, 0);
            return a;
        }
        Node* n = nd(p, N_EXPRSTMT);
        n->a = e;
        return n;
    }
    }
}

static Node* parse_block(P* p) {
    Node* b = nd(p, N_BLOCK);
    expect(p, T_LBRACE, "'{'");
    NV stmts = {0};
    skipnl(p);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) perr(p, "unexpected end of file, expected '}'");
        nv_push(p, &stmts, parse_stmt(p));
        stmt_end(p);
        skipnl(p);
    }
    p->i++; // '}'
    set_kids(b, &stmts);
    return b;
}

// --- declarations ---------------------------------------------------------------

static Node* parse_params(P* p, const char* closer) {
    NV params = {0};
    if (!at(p, T_RPAREN)) {
        for (;;) {
            Node* pa = nd(p, N_PARAM);
            for (;;) {
                if (ate(p, T_MUT)) pa->flags |= F_MUT;
                else if (ate(p, T_NOALIAS)) pa->flags |= F_NOALIAS;
                else break;
            }
            Token* name = expect(p, T_IDENT, "parameter name");
            pa->str = name->str;
            pa->len = name->len;
            expect(p, T_COLON, "':' after parameter name");
            pa->a = parse_type(p);
            nv_push(p, &params, pa);
            if (!ate(p, T_COMMA)) break;
            if (at(p, T_RPAREN)) break; // trailing comma
        }
    }
    expect(p, T_RPAREN, closer);
    Node* holder = nd(p, N_TPARAMS);
    set_kids(holder, &params);
    return holder;
}

static Node* parse_ret(P* p) {
    if (!ate(p, T_COLON)) return NULL;
    if (at(p, T_LPAREN)) {
        p->i++;
        Node* t = nd(p, N_TUPLE_T);
        NV w = {0};
        for (;;) {
            nv_push(p, &w, parse_type(p));
            if (!ate(p, T_COMMA)) break;
            if (at(p, T_RPAREN)) break;
        }
        expect(p, T_RPAREN, "')' to close return types");
        if (w.n == 1) return w.v[0];
        set_kids(t, &w);
        return t;
    }
    return parse_type(p);
}

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
        if (!ate(p, T_COMMA)) break;
        if (at(p, T_RBRACK)) break;
    }
    p->i++; // ']'
    set_kids(tp, &w);
    return tp;
}

static Node* parse_func_rest(P* p, Token* name, Node* tp, int pub, int expo) {
    Node* f = nd_tok(p, N_FUNC, name);
    f->str = name->str;
    f->len = name->len;
    if (pub) f->flags |= F_PUB;
    if (expo) f->flags |= F_EXPO;
    f->c = tp;
    expect(p, T_LPAREN, "'(' after function name");
    Node* ph = parse_params(p, "')' to close parameters");
    f->kids = ph->kids;
    f->nkids = ph->nkids;
    f->a = parse_ret(p);
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

static Node* parse_method_rest(P* p, Token* owner, Node* tp, int pub, int mut) {
    expect(p, T_DOT, "'.' after method owner");
    Token* name = expect(p, T_IDENT, "method name after '.'");
    Node* f = parse_func_rest(p, name, tp, pub, 0);
    f->flags |= F_METHOD;
    if (mut) f->flags |= F_MUT;
    f->d = owner_type(p, owner, tp);

    Node* self = nd_tok(p, N_PARAM, owner);
    self->str = "self";
    self->len = 4;
    if (mut) self->flags |= F_MUT;
    self->a = f->d;
    Node** params = (Node**)arena_alloc((size_t)(f->nkids + 1) * sizeof(Node*));
    params[0] = self;
    for (int i = 0; i < f->nkids; i++) params[i + 1] = f->kids[i];
    f->kids = params;
    f->nkids++;
    return f;
}

static Node* parse_signing(P* p, Token* owner, Node* tp, int pub) {
    Node* n = nd_tok(p, N_SIGNING, owner);
    n->a = owner_type(p, owner, tp);
    n->c = tp;
    if (pub) n->flags |= F_PUB;
    Token* signs = expect(p, T_SIGNS, "'signs'");
    NV contracts = {0};
    if (ate(p, T_LPAREN)) {
        if (at(p, T_RPAREN)) perr(p, "a signing needs at least one contract");
        for (;;) {
            Node* c = nd(p, N_CONFORMANCE);
            c->a = parse_type(p);
            nv_push(p, &contracts, c);
            if (!ate(p, T_COMMA)) break;
            if (at(p, T_RPAREN)) break;
        }
        if (tk(p)->line == signs->line)
            perr(p, "parentheses after 'signs' are only for multiline contract lists");
        expect(p, T_RPAREN, "')' to close signed contracts");
    } else {
        if (at(p, T_NL) || at(p, T_EOF))
            perr(p, "a signing needs at least one contract");
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
                Token* fname = expect(p, T_IDENT, "field name");
                Node* fl = nd_tok(p, N_FIELD, fname);
                fl->str = fname->str;
                fl->len = fname->len;
                expect(p, T_COLON, "':' after field name");
                fl->a = parse_type(p);
                nv_push(p, &fields, fl);
                if (!ate(p, T_COMMA)) break;
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
            if (ate(p, T_MUT)) m->flags |= F_MUT;
            Token* mname = expect(p, T_IDENT, "method name");
            m->str = mname->str;
            m->len = mname->len;
            expect(p, T_LPAREN, "'(' after method name");
            Node* ph = parse_params(p, "')' to close method parameters");
            m->kids = ph->kids;
            m->nkids = ph->nkids;
            m->a = parse_ret(p);
            nv_push(p, &methods, m);
            if (!ate(p, T_NL) && !at(p, T_RBRACE))
                perr(p, "expected newline or '}' after method");
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
        expect(p, T_LBRACE, "'{' after 'enum'");
        NV members = {0};
        if (!at(p, T_RBRACE)) {
            for (;;) {
                nv_push(p, &members, ident_node(p, expect(p, T_IDENT, "enum member")));
                if (!ate(p, T_COMMA)) break;
                if (at(p, T_RBRACE)) break; // trailing comma
            }
        }
        expect(p, T_RBRACE, "'}' to close enum");
        set_kids(n, &members);
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
    default: { // global var / const
        n = nd_tok(p, N_VAR, name);
        if (pub) n->flags |= F_PUB;
        int is_const = 0;
        if (at(p, T_CONST)) {
            is_const = 1;
            p->i++;
        }
        Node* binder = nd_tok(p, N_PARAM, name);
        binder->str = name->str;
        binder->len = name->len;
        binder->a = parse_type(p);
        NV binders = {0};
        nv_push(p, &binders, binder);
        if (is_const) n->flags |= F_CONST;
        set_kids(n, &binders);
        if (ate(p, T_ASSIGN)) n->b = expr_list(p, 0);
        else if (is_const) perr(p, "const declaration requires an initializer");
        return n;
    }
    }
}

static Node* parse_extern(P* p) {
    Node* n = nd(p, N_EXTERN);
    expect(p, T_EXTERN, "'extern'");
    Token* abi = expect(p, T_STR, "ABI string after 'extern'");
    n->str = abi->str;
    n->len = abi->len;
    expect(p, T_LBRACE, "'{' to open extern block");
    NV fns = {0};
    skipnl(p);
    while (!at(p, T_RBRACE)) {
        if (at(p, T_EOF)) perr(p, "unexpected end of file in extern block");
        Node* f = nd(p, N_EXTERNFN);
        if (ate(p, T_EXPORT)) f->flags |= F_EXPO;
        Token* name = expect(p, T_IDENT, "function name in extern block");
        f->str = name->str;
        f->len = name->len;
        expect(p, T_LPAREN, "'(' after extern function name");
        if (!at(p, T_RPAREN)) {
            NV params = {0};
            for (;;) {
                Node* pa = nd(p, N_PARAM);
                Token* pname = expect(p, T_IDENT, "parameter name");
                pa->str = pname->str;
                pa->len = pname->len;
                expect(p, T_COLON, "':' after parameter name");
                pa->a = parse_type(p);
                nv_push(p, &params, pa);
                if (!ate(p, T_COMMA)) break;
                if (at(p, T_DOTDOTDOT)) {
                    p->i++;
                    f->flags |= F_VARARGS;
                    break;
                }
            }
            set_kids(f, &params);
        }
        expect(p, T_RPAREN, "')' to close extern parameters");
        f->a = parse_ret(p);
        nv_push(p, &fns, f);
        if (!ate(p, T_NL) && !at(p, T_RBRACE))
            perr(p, "expected newline or '}' after extern declaration");
        skipnl(p);
    }
    p->i++;
    set_kids(n, &fns);
    return n;
}

static Node* parse_import(P* p) {
    Node* n = nd(p, N_IMPORT);
    expect(p, T_IMPORT, "'import'");
    Token* name = expect(p, T_IDENT, "module name after 'import'");
    n->str = name->str;
    n->len = name->len;
    if (ate(p, T_COLON)) {
        if (at(p, T_IDENT)) { // import m: math — alias binding
            n->b = ident_node(p, tk(p));
            p->i++;
        } else if (at(p, T_STR)) { // import util: "./util.ace" — path form
            n->a = nd(p, N_STR);
            n->a->str = tk(p)->str;
            n->a->len = tk(p)->len;
            p->i++;
        } else {
            perr(p, "expected module name or path after ':' in import");
        }
    }
    if (!ate(p, T_NL) && !at(p, T_EOF))
        perr(p, "expected newline after import");
    return n;
}

static Node* parse_top(P* p) {
    if (at(p, T_IMPORT)) perr(p, "imports must precede declarations");
    int pub = 0, expo = 0, mut = 0;
    for (;;) {
        if (at(p, T_PUB)) {
            if (mut) perr(p, "visibility must come before 'mut'");
            if (pub) perr(p, "duplicate 'pub' modifier");
            p->i++; pub = 1;
        }
        else if (at(p, T_EXPORT)) {
            if (expo) perr(p, "duplicate 'export' modifier");
            p->i++; expo = 1;
        }
        else if (at(p, T_MUT)) {
            if (mut) perr(p, "duplicate 'mut' modifier");
            p->i++; mut = 1;
        }
        else break;
    }
    if (at(p, T_EXTERN)) {
        if (pub || expo) perr(p, "'extern' blocks take no pub/export");
        return parse_extern(p);
    }
    if (!at(p, T_IDENT)) perr(p, "expected declaration, got %s", gdesc(p));
    Token* name = tk(p);
    p->i++;
    Node* tp = try_tparams(p);
    if (at(p, T_SIGNS)) {
        if (expo || mut) perr(p, "signings accept only the optional leading 'pub'");
        return parse_signing(p, name, tp, pub);
    }
    if (at(p, T_DOT)) {
        if (expo) perr(p, "methods cannot be exported at the C boundary");
        return parse_method_rest(p, name, tp, pub, mut);
    }
    if (mut) perr(p, "'mut' before a top-level declaration requires a method owner");
    if (at(p, T_LPAREN)) return parse_func_rest(p, name, tp, pub, expo);
    if (at(p, T_COLON)) return parse_colon_decl(p, name, tp, pub);
    perr(p, "expected '(' or ':' after top-level name '%.*s'", (int)name->len, name->str);
    return NULL;
}

Node* parse(Token* toks, size_t ntok, Diag* d) {
    P p = {0};
    p.toks = toks;
    p.n = ntok;
    p.d = d;
    Node* file = (Node*)arena_alloc(sizeof(Node));
    memset(file, 0, sizeof *file);
    file->kind = (uint8_t)N_FILE;
    file->line = 1;
    file->col = 1;
    file->path = d->path;
    if (setjmp(p.jb)) return file; // syntax error: stop at the first one
    NV decls = {0};
    skipnl(&p);
    while (at(&p, T_IMPORT)) {
        nv_push(&p, &decls, parse_import(&p));
        skipnl(&p);
    }
    while (!at(&p, T_EOF)) {
        nv_push(&p, &decls, parse_top(&p));
        if (!ate(&p, T_NL) && !at(&p, T_EOF))
            perr(&p, "expected newline after declaration, got %s", gdesc(&p));
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
    if (x->flags & F_EXPO) printf(" export");
    if (x->flags & F_MUT) printf(" mut");
    if (x->flags & F_NOALIAS) printf(" noalias");
    if (x->flags & F_CONST) printf(" const");
    if (x->flags & F_ELSE) printf(" else");
    if (x->flags & F_VARARGS) printf(" varargs");
    if (x->flags & F_METHOD) printf(" method");
}

static void pnode(Node* x, int ind) {
    for (int i = 0; i < ind; i++) putchar(' ');
    if (!x) {
        printf("nil\n");
        return;
    }
    printf("(%s", node_kind_name((NKind)x->kind));
    if (x->str) printf(" '%.*s'", (int)x->len, x->str);
    if (x->kind == N_INT || x->kind == N_CHAR || x->kind == N_BOOL ||
        x->kind == N_ARRAY_T)
        printf(" %llu", (unsigned long long)x->ival);
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
