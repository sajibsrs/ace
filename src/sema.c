// sema.c — semantic checking (stage three): types, names, expression typing.
// Walks the AST in declaration order — single-pass semantics: declaration
// before use; forward top-level calls are deferred (like emitter fixups) and
// verified at end of file.
//
// Type model (machine truth: size/align computed for every type):
//   prims    bool i8..i64 u8..u64 f32 f64 string usize typeid
//            (`int`/`uint` = i64/u64, the platform word on arm64)
//   nominal  struct, enum (i32 layout), interface (16 bytes), distinct
//   struct.  *T (8), []T (16), [N]T, T? (ptr/slice/interface niches are
//            free; value optionals carry a visible tail tag), tuples
//            (multi-return, laid out like anonymous structs), funcs
// Literals are untyped constants: they take the expected type at use sites
// and must fit; null assigns to any T?. Everything else casts explicitly.
#include "ace.h"
#include "sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// --- type kinds (definitions shared via sema.h) ---------------------------------

static SType *T_ERR, *T_UT_INT, *T_UT_FLOAT, *T_UT_NULL;
static SType* PRIMS[14];

static SType* st_new(uint8_t kind) {
    SType* t = (SType*)arena_alloc(sizeof(SType));
    memset(t, 0, sizeof *t);
    t->kind = kind;
    return t;
}

static SType* st_prim(int p) { return PRIMS[p]; }

static int is_int(SType* t) {
    return t->kind == SK_PRIM && t->prim >= P_I8 && t->prim <= P_U64;
}
static int is_float(SType* t) {
    return t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64);
}
static int is_num(SType* t) { return is_int(t) || is_float(t); }
static int is_bool(SType* t) { return t->kind == SK_PRIM && t->prim == P_BOOL; }

static const char* type_name(SType* t) {
    switch (t->kind) {
    case SK_PRIM:
        switch (t->prim) {
        case P_BOOL: return "bool";
        case P_I8: return "i8"; case P_I16: return "i16";
        case P_I32: return "i32"; case P_I64: return "i64";
        case P_U8: return "u8"; case P_U16: return "u16";
        case P_U32: return "u32"; case P_U64: return "u64";
        case P_F32: return "f32"; case P_F64: return "f64";
        case P_STRING: return "string"; case P_USIZE: return "usize";
        case P_TYPEID: return "typeid";
        }
        return "?";
    case SK_PTR: return t->is_mut ? "mut *T" : "*T";
    case SK_SLICE: return "[]T";
    case SK_ARRAY: return "[N]T";
    case SK_OPT: return "T?";
    case SK_TUPLE: return "(tuple)";
    case SK_FUNC: return "(func)";
    case SK_STRUCT: return t->name ? t->name : "struct";
    case SK_ENUM: return t->name ? t->name : "enum";
    case SK_INTERFACE: return t->name ? t->name : "interface";
    case SK_DISTINCT: return t->name ? t->name : "distinct";
    case SK_UT: return t == T_UT_INT ? "integer literal" :
                    t == T_UT_FLOAT ? "float literal" : "null";
    default: return "?";
    }
}

// --- layout: declaration order, natural alignment -------------------------------

static uint32_t align_to(uint32_t v, uint32_t a) { return a ? (v + a - 1) / a * a : v; }

static void layout(SType* t) {
    if (t->size || t->calc) return;
    switch (t->kind) {
    case SK_PRIM:
        switch (t->prim) {
        case P_I8: case P_U8: case P_BOOL: t->size = 1; break;
        case P_I16: case P_U16: t->size = 2; break;
        case P_I32: case P_U32: case P_F32: t->size = 4; break;
        case P_I64: case P_U64: case P_F64: case P_USIZE: case P_TYPEID:
            t->size = 8; break;
        case P_STRING: t->size = 16; break;
        default: t->size = 1; break;
        }
        t->align = t->size > 8 ? 8 : t->size;
        return;
    case SK_PTR: case SK_FUNC: case SK_TYPEPARAM:
        t->size = t->align = 8;
        return;
    case SK_SLICE:
        t->size = 16; t->align = 8;
        return;
    case SK_ARRAY:
        layout(t->base);
        t->align = t->base->align;
        t->size = (uint32_t)(t->base->size * t->count);
        return;
    case SK_OPT:
        layout(t->base);
        if (t->base->kind == SK_PTR || t->base->kind == SK_SLICE ||
            t->base->kind == SK_INTERFACE ||
            (t->base->kind == SK_PRIM && t->base->prim == P_STRING)) {
            t->size = t->base->size; t->align = t->base->align; // niche
        } else {
            t->align = t->base->align ? t->base->align : 1;
            t->size = align_to(t->base->size + 1, t->align); // tail tag
        }
        return;
    case SK_STRUCT: case SK_TUPLE: {
        if (t->calc) { t->size = 1; return; }
        t->calc = 1;
        uint32_t off = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].t->kind == SK_ERR) { t->calc = 0; t->size = 1; return; }
            layout(t->fields[i].t);
            if (maxa < t->fields[i].t->align) maxa = t->fields[i].t->align;
            off = align_to(off, t->fields[i].t->align);
            off += t->fields[i].t->size;
        }
        t->align = maxa;
        t->size = align_to(off, maxa);
        t->calc = 0;
        return;
    }
    case SK_ENUM: t->size = t->align = 4; return; // int constants, i32 layout
    case SK_INTERFACE: t->size = 16; t->align = 8; return; // vtable + data
    case SK_DISTINCT:
        layout(t->base);
        t->size = t->base->size;
        t->align = t->base->align;
        return;
    default:
        t->size = t->align = 1;
        return;
    }
}

// --- symbols & scopes -------------------------------------------------------------
// (Sym and kind enums are shared via sema.h)

typedef struct Scope {
    struct Scope* parent;
    Sym*          values;
    Sym*          types;
    const char*   label;
    int           is_loop;
} Scope;

typedef struct Pending {
    Node* call;
    struct Pending* next;
} Pending;

typedef struct {
    Diag*   d;
    Scope*  scope;
    SType*  ret;
    Node*   curfn;
    // generic substitution environment (names -> types)
    const char** tnames; SType** ttypes; int nsub;
    Pending* pending;
} Ck;

static void ck_err(Ck* k, Node* n, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", k->d->path, n->line, n->col);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    k->d->errors++;
}

static Sym* sym_new(uint8_t kind, const char* name, uint32_t len) {
    Sym* s = (Sym*)arena_alloc(sizeof(Sym));
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->name = name;
    s->len = len;
    return s;
}

static void scope_push(Ck* k) {
    Scope* s = (Scope*)arena_alloc(sizeof(Scope));
    memset(s, 0, sizeof *s);
    s->parent = k->scope;
    k->scope = s;
}

static void scope_pop(Ck* k) { k->scope = k->scope->parent; }

static Sym* find_in(Sym* list, const char* name, uint32_t len) {
    for (Sym* s = list; s; s = s->next)
        if (s->len == len && memcmp(s->name, name, len) == 0) return s;
    return NULL;
}

static Sym* lookup_val(Ck* k, const char* name, uint32_t len) {
    for (Scope* s = k->scope; s; s = s->parent) {
        Sym* v = find_in(s->values, name, len);
        if (v) return v;
    }
    return NULL;
}

static Sym* lookup_type(Ck* k, const char* name, uint32_t len) {
    for (Scope* s = k->scope; s; s = s->parent) {
        Sym* v = find_in(s->types, name, len);
        if (v) return v;
    }
    return NULL;
}

static Sym* declare_val(Ck* k, Node* at, uint8_t kind, const char* name, uint32_t len) {
    if (find_in(k->scope->values, name, len)) {
        ck_err(k, at, "duplicate name '%.*s' in this scope", (int)len, name);
        return NULL;
    }
    Sym* s = sym_new(kind, name, len);
    s->next = k->scope->values;
    k->scope->values = s;
    return s;
}

static Sym* declare_type(Ck* k, Node* at, const char* name, uint32_t len) {
    if (find_in(k->scope->types, name, len)) {
        ck_err(k, at, "duplicate type '%.*s'", (int)len, name);
        return NULL;
    }
    Sym* s = sym_new(SYM_TYPE, name, len);
    s->next = k->scope->types;
    k->scope->types = s;
    return s;
}

// --- same_type / castable ----------------------------------------------------------

static int same_type(SType* a, SType* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind == SK_ERR || b->kind == SK_ERR) return 1; // poison-tolerant
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case SK_PRIM: return a->prim == b->prim;
    case SK_PTR: return a->is_mut == b->is_mut && same_type(a->base, b->base);
    case SK_SLICE: return same_type(a->base, b->base);
    case SK_ARRAY: return a->count == b->count && same_type(a->base, b->base);
    case SK_OPT: return same_type(a->base, b->base);
    case SK_TUPLE:
        if (a->nfields != b->nfields) return 0;
        for (int i = 0; i < a->nfields; i++)
            if (!same_type(a->fields[i].t, b->fields[i].t)) return 0;
        return 1;
    case SK_FUNC:
        if (a->nparams != b->nparams) return 0;
        for (int i = 0; i < a->nparams; i++)
            if (!same_type(a->params[i], b->params[i])) return 0;
        return same_type(a->ret, b->ret);
    case SK_STRUCT: case SK_ENUM: case SK_INTERFACE: case SK_DISTINCT:
        return a->decl == b->decl; // nominal identity
    case SK_TYPEPARAM:
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    default: return 0;
    }
}

static int castable(SType* f, SType* t) {
    if (f->kind == SK_ERR || t->kind == SK_ERR) return 1;
    if (same_type(f, t)) return 1;
    if (f == T_UT_INT)
        return is_num(t) || t->kind == SK_ENUM ||
               (t->kind == SK_DISTINCT && castable(f, t->base));
    if (f == T_UT_FLOAT)
        return is_float(t) || (t->kind == SK_DISTINCT && castable(f, t->base));
    if (t->kind == SK_DISTINCT) return castable(f, t->base);
    if (f->kind == SK_DISTINCT) return castable(f->base, t);
    if (f->kind == SK_ENUM) return is_int(t);
    if (t->kind == SK_ENUM) return is_int(f);
    if (f->kind == SK_PTR && t->kind == SK_PTR) return 1;
    return is_num(f) && is_num(t);
}

// --- untyped int constant evaluation ----------------------------------------------

static int ut_const(Node* e, int64_t* out) {
    switch (e->kind) {
    case N_INT: case N_CHAR: *out = (int64_t)e->ival; return 1;
    case N_UNARY:
        if (e->op == T_MINUS) {
            int64_t v;
            return ut_const(e->a, &v) && (*out = -v, 1);
        }
        return 0;
    case N_BINARY: {
        int64_t a, b;
        if (!ut_const(e->a, &a) || !ut_const(e->b, &b)) return 0;
        switch (e->op) {
        case T_PLUS: *out = a + b; return 1;
        case T_MINUS: *out = a - b; return 1;
        case T_STAR: *out = a * b; return 1;
        case T_SLASH: if (!b) return 0; *out = a / b; return 1;
        case T_PERCENT: if (!b) return 0; *out = a % b; return 1;
        case T_SHL: *out = a << (b & 63); return 1;
        case T_SHR: *out = a >> (b & 63); return 1;
        case T_AMP: *out = a & b; return 1;
        case T_PIPE: *out = a | b; return 1;
        case T_CARET: *out = a ^ b; return 1;
        default: return 0;
        }
    }
    default: return 0;
    }
}

// --- type building ------------------------------------------------------------------

static SType* build_type(Ck* k, Node* t);

static SType* prim_by_name(const char* s, uint32_t len) {
    struct { const char* n; int p; } M[] = {
        {"bool", P_BOOL}, {"i8", P_I8}, {"i16", P_I16}, {"i32", P_I32},
        {"i64", P_I64}, {"u8", P_U8}, {"u16", P_U16}, {"u32", P_U32},
        {"u64", P_U64}, {"f32", P_F32}, {"f64", P_F64}, {"string", P_STRING},
        {"usize", P_USIZE}, {"typeid", P_TYPEID},
        {"int", P_I64}, {"uint", P_U64}, // platform word on arm64
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if ((uint32_t)strlen(M[i].n) == len && memcmp(M[i].n, s, len) == 0)
            return st_prim(M[i].p);
    return NULL;
}

static SType* instantiate_generic_type(Ck* k, Node* decl, SType** args, int nargs);

static SType* build_named(Ck* k, Node* t) {
    if (t->a) {
        ck_err(k, t, "module-qualified types need the stdlib (not yet linked)");
        return T_ERR;
    }
    if (k->nsub) { // type-parameter substitution first (stamping)
        for (int i = 0; i < k->nsub; i++)
            if ((uint32_t)strlen(k->tnames[i]) == t->len &&
                memcmp(k->tnames[i], t->str, t->len) == 0)
                return k->ttypes[i];
    }
    SType* p = prim_by_name(t->str, t->len);
    if (p) return p;
    Sym* s = lookup_type(k, t->str, t->len);
    if (!s || !s->type) {
        ck_err(k, t, "unknown type '%.*s'", (int)t->len, t->str);
        return T_ERR;
    }
    if (t->nkids > 0) { // generic instantiation in type position: Vec[int]
        SType** args = (SType**)arena_alloc((size_t)t->nkids * sizeof(SType*));
        for (int i = 0; i < t->nkids; i++) args[i] = build_type(k, t->kids[i]);
        return instantiate_generic_type(k, s->type->decl, args, t->nkids);
    }
    if (s->is_generic) {
        ck_err(k, t, "generic type '%.*s' needs explicit type arguments",
               (int)t->len, t->str);
        return T_ERR;
    }
    return s->type;
}

static SType* build_type(Ck* k, Node* t) {
    switch (t->kind) {
    case N_NAMED: return build_named(k, t);
    case N_IDENT: { // typed struct-literal bases arrive as expression nodes
        SType* p = prim_by_name(t->str, t->len);
        if (p) return p;
        Sym* s = lookup_type(k, t->str, t->len);
        if (!s || !s->type) {
            ck_err(k, t, "unknown type '%.*s'", (int)t->len, t->str);
            return T_ERR;
        }
        if (s->is_generic) {
            ck_err(k, t, "generic type '%.*s' needs explicit type arguments",
                   (int)t->len, t->str);
            return T_ERR;
        }
        return s->type;
    }
    case N_PTR: {
        SType* p = st_new(SK_PTR);
        p->is_mut = (t->flags & F_MUT) != 0;
        p->base = build_type(k, t->a);
        return p;
    }
    case N_SLICE_T: {
        SType* s = st_new(SK_SLICE);
        s->base = build_type(k, t->a);
        return s;
    }
    case N_ARRAY_T: {
        if (t->ival == 0) {
            ck_err(k, t, "array length must be positive");
            return T_ERR;
        }
        SType* a = st_new(SK_ARRAY);
        a->count = t->ival;
        a->base = build_type(k, t->a);
        return a;
    }
    case N_OPT: {
        SType* o = st_new(SK_OPT);
        o->base = build_type(k, t->a);
        if (o->base->kind == SK_OPT) {
            ck_err(k, t, "optional of optional is not a type");
            return T_ERR;
        }
        return o;
    }
    case N_CONST_T: return build_type(k, t->a);
    case N_TUPLE_T: {
        SType* tu = st_new(SK_TUPLE);
        tu->nfields = t->nkids;
        tu->fields = (SField*)arena_alloc((size_t)(t->nkids ? t->nkids : 1) * sizeof(SField));
        for (int i = 0; i < t->nkids; i++) {
            tu->fields[i].name = NULL;
            tu->fields[i].t = build_type(k, t->kids[i]);
        }
        return tu;
    }
    case N_FUNC_T: {
        SType* f = st_new(SK_FUNC);
        f->nparams = t->nkids;
        f->params = (SType**)arena_alloc((size_t)(t->nkids ? t->nkids : 1) * sizeof(SType*));
        for (int i = 0; i < t->nkids; i++) f->params[i] = build_type(k, t->kids[i]);
        f->ret = t->a ? build_type(k, t->a) : NULL;
        return f;
    }
    default:
        ck_err(k, t, "expected a type");
        return T_ERR;
    }
}

// --- forward decls -------------------------------------------------------------------

static SType* expr_type(Ck* k, Node* e);
static int coerce(Ck* k, Node* e, SType* want);
static SType* check_call(Ck* k, Node* call);
static void check_stmt(Ck* k, Node* s);
static void check_block(Ck* k, Node* b);
static void check_structlit(Ck* k, Node* lit, SType* want);
static void check_arraylit(Ck* k, Node* lit, SType* want);
static SType* check_lvalue(Ck* k, Node* e, int write);
static void check_func_body(Ck* k, Node* f);
static void check_mut_args(Ck* k, Node* call, Node* fndecl);

// --- literal fit -------------------------------------------------------------------

static int ut_fits(Ck* k, Node* e, SType* want) {
    if (is_float(want)) {
        double d = e->kind == N_FLOAT ? e->fval :
                   e->kind == N_INT ? (double)e->ival : 0.0;
        if (want->prim == P_F32 && (d > 3.402823466e38 || d < -3.402823466e38)) {
            ck_err(k, e, "float literal out of range for f32");
            return 0;
        }
        return 1;
    }
    int64_t v;
    if (!ut_const(e, &v)) return 1;
    uint64_t u = (uint64_t)v;
    switch (want->kind == SK_ENUM ? P_I32 : want->prim) {
    case P_I8: if (v < -128 || v > 127) goto over; return 1;
    case P_I16: if (v < -32768 || v > 32767) goto over; return 1;
    case P_U8: if (u > 255) goto over; return 1;
    case P_U16: if (u > 65535) goto over; return 1;
    case P_U32: if (u > 4294967295u) goto over; return 1;
    default: return 1;
    }
over:
    ck_err(k, e, "integer literal %lld out of range for %s", (long long)v,
           type_name(want));
    return 0;
}

static int coerce(Ck* k, Node* e, SType* want) {
    SType* t = expr_type(k, e);
    if (same_type(t, want)) return 1;
    if (want->kind == SK_ERR || t->kind == SK_ERR) return 1;
    if (t == T_UT_NULL) {
        if (want->kind == SK_OPT) { e->ty = want; return 1; } // null materializes
        ck_err(k, e, "null needs an optional type, not %s", type_name(want));
        return 0;
    }
    if (t == T_UT_INT &&
        (is_int(want) || want->kind == SK_ENUM ||
         (want->kind == SK_DISTINCT && is_int(want->base)))) {
        if (ut_fits(k, e, want->kind == SK_DISTINCT ? want->base : want)) {
            if (e->kind == N_BINARY) {
                coerce(k, e->a, want);
                coerce(k, e->b, want);
            }
            e->ty = want; // untyped literal takes its expected type here
            return 1;
        }
        return 0;
    }
    if (t == T_UT_FLOAT &&
        (is_float(want) || (want->kind == SK_DISTINCT && is_float(want->base)))) {
        if (ut_fits(k, e, want->kind == SK_DISTINCT ? want->base : want)) {
            if (e->kind == N_BINARY) {
                coerce(k, e->a, want);
                coerce(k, e->b, want);
            }
            e->ty = want;
            return 1;
        }
        return 0;
    }
    ck_err(k, e, "type mismatch: %s given where %s expected (casts are explicit)",
           type_name(t), type_name(want));
    return 0;
}

// --- lvalues ------------------------------------------------------------------------

// A mut parameter grants writes through its reference even when the pointer
// type itself is unmarked (spec Mutability #2): mut lives on the binding.
static int ref_writable(Ck* k, Node* base) {
    if (base->kind != N_IDENT) return 0;
    Sym* s = lookup_val(k, base->str, base->len);
    return s && s->kind == SYM_VAR && s->is_param && s->is_mut;
}

static SType* check_lvalue(Ck* k, Node* e, int write) {
    switch (e->kind) {
    case N_IDENT: {
        Sym* s = lookup_val(k, e->str, e->len);
        if (!s) {
            ck_err(k, e, "unknown name '%.*s'", (int)e->len, e->str);
            return T_ERR;
        }
        e->sym = s; // emitter: storage key even in write-only positions
        if (s->kind != SYM_VAR) {
            ck_err(k, e, "'%.*s' is not a variable", (int)e->len, e->str);
            return T_ERR;
        }
        if (write) {
            if (s->is_const)
                ck_err(k, e, "cannot assign to const '%.*s'", (int)e->len, e->str);
            else if (s->is_param && !s->is_mut)
                ck_err(k, e, "parameter '%.*s' is read-only — mark it 'mut' in the signature",
                       (int)e->len, e->str);
        }
        return s->type;
    }
    case N_FIELD_E: {
        SType* b = check_lvalue(k, e->a, write);
        while (b->kind == SK_PTR) { // auto-deref; writes need mut pointers
            if (write && !b->is_mut && !ref_writable(k, e->a))
                ck_err(k, e, "write through read-only pointer — use 'mut *T' or a mut parameter");
            b = b->base;
        }
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind == SK_OPT) {
            ck_err(k, e, "field access through an optional — check for null first");
            return T_ERR;
        }
        if (b->kind != SK_STRUCT) {
            ck_err(k, e, "type %s has no fields", type_name(b));
            return T_ERR;
        }
        for (int i = 0; i < b->nfields; i++)
            if ((uint32_t)strlen(b->fields[i].name) == e->len &&
                memcmp(b->fields[i].name, e->str, e->len) == 0)
                return b->fields[i].t;
        ck_err(k, e, "type %s has no field '%.*s'", type_name(b), (int)e->len, e->str);
        return T_ERR;
    }
    case N_INDEX: {
        SType* b = check_lvalue(k, e->a, write);
        SType* ix = expr_type(k, e->b);
        if (ix->kind != SK_ERR && !is_int(ix) && ix != T_UT_INT)
            ck_err(k, e->b, "index must be an integer, not %s", type_name(ix));
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind != SK_SLICE && b->kind != SK_ARRAY) {
            ck_err(k, e, "cannot index %s", type_name(b));
            return T_ERR;
        }
        return b->base;
    }
    case N_UNARY:
        if (e->op == T_TILDE) {
            SType* p = expr_type(k, e->a);
            if (p->kind == SK_ERR) return T_ERR;
            if (p->kind == SK_OPT) {
                ck_err(k, e, "dereference of an optional pointer — check for null first");
                return T_ERR;
            }
            if (p->kind != SK_PTR) {
                ck_err(k, e, "'~' dereferences pointers, not %s", type_name(p));
                return T_ERR;
            }
            if (write && !p->is_mut && !ref_writable(k, e->a))
                ck_err(k, e, "write through read-only pointer — use 'mut *T' or a mut parameter");
            return p->base;
        }
        break;
    default:
        break;
    }
    ck_err(k, e, "not an assignable target");
    return T_ERR;
}

// --- helpers ------------------------------------------------------------------------

static Node* iface_method(SType* iface, const char* name, uint32_t len) {
    if (!iface || iface->kind != SK_INTERFACE || !iface->decl) return NULL;
    for (int i = 0; i < iface->decl->nkids; i++) {
        Node* m = iface->decl->kids[i];
        if (m->len == len && memcmp(m->str, name, len) == 0) return m;
    }
    return NULL;
}

static void check_args(Ck* k, Node* call, SType* ft, int is_variadic) {
    int want = ft->nparams, got = call->nkids;
    if (is_variadic ? got < want : got != want) {
        ck_err(k, call, "wrong number of arguments: %d given, %d expected%s",
               got, want, is_variadic ? " (plus variadics)" : "");
        return;
    }
    for (int i = 0; i < got; i++) {
        Node* arg = call->kids[i];
        int am = arg->kind == N_MUTARG;
        Node* raw = am ? arg->a : arg;
        if (i < want) coerce(k, raw, ft->params[i]);
        else if (am) ck_err(k, arg, "'mut' marks declared parameters, not variadics");
    }
}

static void check_mut_args(Ck* k, Node* call, Node* fndecl) {
    if (!fndecl) return;
    for (int i = 0; i < call->nkids && i < fndecl->nkids; i++) {
        int am = call->kids[i]->kind == N_MUTARG;
        int pm = (fndecl->kids[i]->flags & F_MUT) != 0;
        if (am && !pm)
            ck_err(k, call->kids[i], "'mut' marks mut parameters; parameter %d is read-only", i + 1);
        else if (pm && !am)
            ck_err(k, call->kids[i], "mut parameter needs 'mut' at the call site");
        else if (am) // mut passes the caller's address — the arg must have one
            check_lvalue(k, call->kids[i]->a, 0);
    }
}

// --- expressions ----------------------------------------------------------------------

static SType* expr_type_raw(Ck* k, Node* e) {
    switch (e->kind) {
    case N_INT: case N_CHAR: return T_UT_INT;
    case N_FLOAT: return T_UT_FLOAT;
    case N_STR: return st_prim(P_STRING);
    case N_BOOL: return st_prim(P_BOOL);
    case N_NULL: return T_UT_NULL;
    case N_IDENT: {
        Sym* s = lookup_val(k, e->str, e->len);
        if (!s) {
            ck_err(k, e, "unknown name '%.*s'", (int)e->len, e->str);
            return T_ERR;
        }
        if (s->kind == SYM_MODULE) {
            ck_err(k, e, "module '%.*s' used as a value", (int)e->len, e->str);
            return T_ERR;
        }
        if (s->kind == SYM_TYPE || (s->kind == SYM_FUNC && s->is_generic)) {
            ck_err(k, e, "'%.*s' is a type or generic — used bare here", (int)e->len, e->str);
            return T_ERR;
        }
        e->sym = s; // emitter: storage (slot / global / function)
        return s->type ? s->type : T_ERR;
    }
    case N_TUPLE_E: {
        SType* tu = st_new(SK_TUPLE);
        tu->nfields = e->nkids;
        tu->fields = (SField*)arena_alloc((size_t)(e->nkids ? e->nkids : 1) * sizeof(SField));
        for (int i = 0; i < e->nkids; i++) {
            tu->fields[i].name = NULL;
            tu->fields[i].t = expr_type(k, e->kids[i]);
        }
        return tu;
    }
    case N_UNARY: {
        if (e->op == T_AMP) { // &lvalue -> read-only *T
            SType* t = check_lvalue(k, e->a, 0);
            SType* p = st_new(SK_PTR);
            p->base = t;
            return p;
        }
        if (e->op == T_TILDE) return check_lvalue(k, e, 0);
        SType* t = expr_type(k, e->a);
        if (t->kind == SK_ERR) return t;
        if (t == T_UT_INT) return t;
        switch (e->op) {
        case T_BANG:
            if (is_bool(t) || is_int(t)) return t;
            break;
        case T_MINUS:
            if (is_float(t)) return t;
            if (is_int(t)) {
                if (t->prim == P_U8 || t->prim == P_U16 || t->prim == P_U32 ||
                    t->prim == P_U64) {
                    ck_err(k, e, "cannot negate unsigned %s", type_name(t));
                    return T_ERR;
                }
                return t;
            }
            break;
        default:
            break;
        }
        ck_err(k, e, "invalid operand for '%s': %s", tok_disp((TokKind)e->op), type_name(t));
        return T_ERR;
    }
    case N_BINARY: {
        TokKind op = (TokKind)e->op;
        SType* a = expr_type(k, e->a);
        SType* b = expr_type(k, e->b);
        if (a->kind == SK_ERR || b->kind == SK_ERR) return T_ERR;
        if (op == T_ANDAND || op == T_OROR) {
            if (is_bool(a) && is_bool(b)) return st_prim(P_BOOL);
            ck_err(k, e, "'%s' needs bool operands, got %s and %s",
                   tok_disp(op), type_name(a), type_name(b));
            return T_ERR;
        }
        if (a == T_UT_INT && b != T_UT_INT && b->kind != SK_UT) {
            if (!ut_fits(k, e->a, is_int(b) ? b : st_prim(P_I64))) return T_ERR;
            a = b;
            e->a->ty = a; // literal materializes to the operand type
        } else if (b == T_UT_INT && a != T_UT_INT && a->kind != SK_UT) {
            if (!ut_fits(k, e->b, is_int(a) ? a : st_prim(P_I64))) return T_ERR;
            b = a;
            e->b->ty = b;
        } else if (a == T_UT_FLOAT && is_float(b)) {
            if (!ut_fits(k, e->a, b)) return T_ERR;
            a = b;
            e->a->ty = a;
        } else if (b == T_UT_FLOAT && is_float(a)) {
            if (!ut_fits(k, e->b, a)) return T_ERR;
            b = a;
            e->b->ty = b;
        }
        if (a == T_UT_INT && b == T_UT_INT) return T_UT_INT;
        if (a == T_UT_FLOAT && b == T_UT_FLOAT) return T_UT_FLOAT;
        int cmp = op == T_EQEQ || op == T_BANGEQ || op == T_LT ||
                  op == T_LE || op == T_GT || op == T_GE;
        if (cmp) {
            if (same_type(a, b) && (is_num(a) || is_bool(a) || a->kind == SK_PTR ||
                                    a->kind == SK_ENUM || a->kind == SK_INTERFACE ||
                                    a->kind == SK_DISTINCT ||
                                    (a->kind == SK_PRIM && a->prim == P_STRING)))
                return st_prim(P_BOOL);
            if ((a == T_UT_NULL && b->kind == SK_OPT) ||
                (b == T_UT_NULL && a->kind == SK_OPT))
                return st_prim(P_BOOL);
            ck_err(k, e, "cannot compare %s and %s", type_name(a), type_name(b));
            return T_ERR;
        }
        int shift = op == T_SHL || op == T_SHR;
        int bit = op == T_AMP || op == T_PIPE || op == T_CARET;
        if (bit || shift) {
            if (is_int(a) && is_int(b) && same_type(a, b)) return a;
            ck_err(k, e, "'%s' needs integer operands of one type, got %s and %s",
                   tok_disp(op), type_name(a), type_name(b));
            return T_ERR;
        }
        if (is_num(a) && is_num(b) && same_type(a, b)) return a;
        ck_err(k, e, "type mismatch: %s %s %s — operands need one type (casts are explicit)",
               type_name(a), tok_disp(op), type_name(b));
        return T_ERR;
    }
    case N_RANGE: { // 0 .. n — element type is the unified integer type
        SType* lo = expr_type(k, e->a);
        SType* hi = expr_type(k, e->b);
        if (lo->kind == SK_ERR || hi->kind == SK_ERR) return T_ERR;
        if (lo == T_UT_INT && is_int(hi)) {
            if (!ut_fits(k, e->a, hi)) return T_ERR;
            lo = hi;
            e->a->ty = lo;
        } else if (hi == T_UT_INT && is_int(lo)) {
            if (!ut_fits(k, e->b, lo)) return T_ERR;
            hi = lo;
            e->b->ty = hi;
        }
        if (lo == T_UT_INT && hi == T_UT_INT) return st_prim(P_I64);
        if (is_int(lo) && same_type(lo, hi)) return lo;
        ck_err(k, e, "range bounds must be one integer type, got %s and %s",
               type_name(lo), type_name(hi));
        return T_ERR;
    }
    case N_CMPCHAIN: {
        SType* t = expr_type(k, e->kids[0]);
        if (t->kind == SK_ERR) return t;
        for (int i = 1; i < e->nkids; i++) {
            SType* u = expr_type(k, e->kids[i]);
            if (u->kind == SK_ERR) return T_ERR;
            if (t == T_UT_INT && is_int(u)) {
                if (!ut_fits(k, e->kids[0], u)) return T_ERR;
                t = u;
                e->kids[0]->ty = t;
            } else if (u == T_UT_INT && is_int(t)) {
                if (!ut_fits(k, e->kids[i], t)) return T_ERR;
                u = t;
                e->kids[i]->ty = u;
            }
            if (!same_type(t, u) || !is_num(t)) {
                ck_err(k, e, "comparison chain needs one numeric type, got %s and %s",
                       type_name(t), type_name(u));
                return T_ERR;
            }
        }
        return st_prim(P_BOOL);
    }
    case N_FIELD_E: {
        // enum member: Color.red
        if (e->a->kind == N_IDENT && !lookup_val(k, e->a->str, e->a->len)) {
            Sym* ts = lookup_type(k, e->a->str, e->a->len);
            if (ts && ts->type && ts->type->kind == SK_ENUM) {
                for (int i = 0; i < ts->type->decl->nkids; i++) {
                    Node* m = ts->type->decl->kids[i];
                    if (m->len == e->len && memcmp(m->str, e->str, e->len) == 0) {
                        e->a->sym = ts; // enum-constant marker for the emitter
                        e->ival = (uint64_t)i; // member index
                        return ts->type;
                    }
                }
                ck_err(k, e, "enum %s has no member '%.*s'", type_name(ts->type),
                       (int)e->len, e->str);
                return T_ERR;
            }
        }
        // module member: fmt.print (needs the stdlib; nothing to resolve yet)
        if (e->a->kind == N_IDENT) {
            Sym* ms = lookup_val(k, e->a->str, e->a->len);
            if (ms && ms->kind == SYM_MODULE) {
                ck_err(k, e, "module member '%.*s.%.*s' needs the stdlib (not yet linked)",
                       (int)e->a->len, e->a->str, (int)e->len, e->str);
                return T_ERR;
            }
        }
        SType* b = expr_type(k, e->a);
        if (b->kind == SK_ERR) return T_ERR;
        while (b->kind == SK_PTR) b = b->base; // p.x auto-derefs
        if (b->kind == SK_OPT) {
            ck_err(k, e, "field access through an optional — check for null first");
            return T_ERR;
        }
        if (b->kind == SK_INTERFACE) {
            Node* m = iface_method(b, e->str, e->len);
            if (!m) {
                ck_err(k, e, "interface %s has no method '%.*s'", type_name(b),
                       (int)e->len, e->str);
                return T_ERR;
            }
            return m->a ? build_type(k, m->a) : T_ERR; // dispatch target
        }
        if (b->kind == SK_STRUCT) {
            for (int i = 0; i < b->nfields; i++)
                if ((uint32_t)strlen(b->fields[i].name) == e->len &&
                    memcmp(b->fields[i].name, e->str, e->len) == 0)
                    return b->fields[i].t;
            ck_err(k, e, "type %s has no field '%.*s'", type_name(b), (int)e->len, e->str);
            return T_ERR;
        }
        ck_err(k, e, "type %s has no member '%.*s'", type_name(b), (int)e->len, e->str);
        return T_ERR;
    }
    case N_INDEX: {
        SType* b = expr_type(k, e->a);
        SType* ix = expr_type(k, e->b);
        if (b->kind == SK_ERR || ix->kind == SK_ERR) return T_ERR;
        if (!is_int(ix) && ix != T_UT_INT)
            ck_err(k, e->b, "index must be an integer, not %s", type_name(ix));
        if (b->kind == SK_SLICE || b->kind == SK_ARRAY) return b->base;
        ck_err(k, e, "cannot index %s", type_name(b));
        return T_ERR;
    }
    case N_SLICE_E: {
        SType* b = expr_type(k, e->a);
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind != SK_SLICE && b->kind != SK_ARRAY) {
            ck_err(k, e, "can only slice arrays and slices, not %s", type_name(b));
            return T_ERR;
        }
        if (e->b) { SType* lo = expr_type(k, e->b);
            if (!is_int(lo) && lo != T_UT_INT) ck_err(k, e->b, "slice bound must be an integer"); }
        if (e->c) { SType* hi = expr_type(k, e->c);
            if (!is_int(hi) && hi != T_UT_INT) ck_err(k, e->c, "slice bound must be an integer"); }
        SType* s = st_new(SK_SLICE);
        s->base = b->base;
        return s;
    }
    case N_STRUCTLIT: {
        if (!e->a) {
            ck_err(k, e, "bare struct literal needs its type from context");
            return T_ERR;
        }
        SType* t = build_type(k, e->a);
        if (t->kind == SK_ERR) return t;
        if (t->kind == SK_STRUCT) check_structlit(k, e, t);
        else ck_err(k, e, "%s does not take a literal", type_name(t));
        return t;
    }
    case N_ARRAYLIT:
        ck_err(k, e, "array literal needs its array type from context");
        return T_ERR;
    case N_CALL:
        return check_call(k, e);
    case N_MUTARG:
        return expr_type(k, e->a);
    default:
        ck_err(k, e, "unsupported expression in checker");
        return T_ERR;
    }
}

// Wrapper: every expression's computed type is annotated onto its node for
// the emitter (single pass — types land where they are checked).
static SType* expr_type(Ck* k, Node* e) {
    SType* t = expr_type_raw(k, e);
    e->ty = t;
    return t;
}

static SType* instantiate_generic_func(Ck* k, Sym* gs, Node* index_node) {
    Node* f = gs->decl;
    Node* tp = f->c;
    // type args: single index in b, or a comma list in kids (f[int, f64])
    int nargs = index_node->b ? 1 : index_node->nkids;
    if (!tp || tp->nkids != nargs) {
        ck_err(k, index_node, "generic function expects %d type arguments, got %d",
               tp ? tp->nkids : 0, nargs);
        return T_ERR;
    }
    SType** args = (SType**)arena_alloc((size_t)nargs * sizeof(SType*));
    for (int i = 0; i < nargs; i++)
        args[i] = build_type(k, index_node->b ? index_node->b : index_node->kids[i]);
    // memo (stored on the tparams node's spare pointer) before body check,
    // so self-recursive instantiations terminate
    STypeInst** head = (STypeInst**)&tp->b;
    for (STypeInst* it = *head; it; it = it->next) {
        int same = 1;
        for (int i = 0; i < nargs && same; i++) same = same_type(it->args[i], args[i]);
        if (same) return it->result;
    }
    const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
    k->tnames = (const char**)arena_alloc((size_t)nargs * sizeof(char*));
    k->ttypes = args;
    k->nsub = nargs;
    for (int i = 0; i < nargs; i++) k->tnames[i] = tp->kids[i]->str;
    SType* ft = st_new(SK_FUNC);
    ft->nparams = f->nkids;
    ft->params = (SType**)arena_alloc((size_t)(f->nkids ? f->nkids : 1) * sizeof(SType*));
    for (int i = 0; i < f->nkids; i++) ft->params[i] = build_type(k, f->kids[i]->a);
    ft->ret = f->a ? build_type(k, f->a) : NULL;
    STypeInst* inst = (STypeInst*)arena_alloc(sizeof(STypeInst));
    inst->args = args;
    inst->result = ft;
    inst->next = *head;
    *head = inst;
    check_func_body(k, f); // stamping: body checked once per instantiation
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
    return ft;
}

static SType* check_call(Ck* k, Node* call) {
    Node* callee = call->a;
    // direct name: call, cast, or forward (pending) call
    if (callee->kind == N_IDENT) {
        Sym* s = lookup_val(k, callee->str, callee->len);
        if (!s) {
            SType* prim = prim_by_name(callee->str, callee->len);
            Sym* ts = lookup_type(k, callee->str, callee->len);
            if (prim || ts) { // constructor-shaped cast: i32(x), Meters(x), Color(1)
                if (call->nkids != 1) {
                    ck_err(k, call, "a cast takes exactly one argument");
                    return T_ERR;
                }
                SType* want = prim ? prim : build_type(k, callee);
                SType* from = expr_type(k, call->kids[0]);
                if (want->kind == SK_ERR || from->kind == SK_ERR) return T_ERR;
                if (!castable(from, want)) {
                    ck_err(k, call, "cannot cast %s to %s", type_name(from), type_name(want));
                    return T_ERR;
                }
                callee->sym = ts; // named-type cast marker
                callee->ty = want; // primitive cast marker
                if (from == T_UT_INT &&
                    !ut_fits(k, call->kids[0], want->kind == SK_ENUM ? want : want->kind == SK_DISTINCT ? (is_int(want->base) ? want->base : st_prim(P_I64)) : want))
                    return T_ERR;
                return want;
            }
            if (callee->len == 7 && memcmp(callee->str, "size_of", 7) == 0) {
                if (call->nkids == 1 && call->kids[0]->kind == N_IDENT) {
                    Node* arg = call->kids[0];
                    SType* t = prim_by_name(arg->str, arg->len);
                    if (!t) {
                        Sym* s2 = lookup_type(k, arg->str, arg->len);
                        if (s2) t = s2->type;
                    }
                    if (t && t->kind != SK_ERR) {
                        layout(t);
                        call->ival = t->size; // constant for the emitter
                        return st_prim(P_USIZE);
                    }
                }
                ck_err(k, call, "size_of takes one type name");
                return T_ERR;
            }
            // forward top-level call — deferred like an emitter fixup
            Pending* pd = (Pending*)arena_alloc(sizeof(Pending));
            pd->call = call;
            pd->next = k->pending;
            k->pending = pd;
            return T_ERR; // poison; existence + args verified at end of file
        }
        if (s->kind == SYM_VAR && s->type && s->type->kind == SK_FUNC) {
            check_args(k, call, s->type, 0);
            call->sym = s; // indirect call through a function value
            return s->type->ret;
        }
        if (s->kind == SYM_IMPL) { // interface constructor: circle_shape(&c)
            check_args(k, call, s->type, 0);
            call->sym = s;
            return s->type->ret;
        }
        if (s->kind != SYM_FUNC) {
            ck_err(k, call, "'%.*s' is not callable", (int)callee->len, callee->str);
            return T_ERR;
        }
        if (s->is_generic) {
            ck_err(k, call, "generic function '%.*s' needs type arguments: %.*s[T](...)",
                   (int)callee->len, callee->str, (int)callee->len, callee->str);
            return T_ERR;
        }
        check_args(k, call, s->type, s->is_extern);
        check_mut_args(k, call, s->decl);
        call->sym = s; // direct call target
        return s->type->ret;
    }
    // generic instantiation call: max[int](3, 9)
    if (callee->kind == N_INDEX && callee->a->kind == N_IDENT) {
        Sym* s = lookup_val(k, callee->a->str, callee->a->len);
        if (s && s->kind == SYM_FUNC && s->is_generic) {
            SType* ft = instantiate_generic_func(k, s, callee);
            if (ft->kind == SK_ERR) return T_ERR;
            check_args(k, call, ft, 0);
            callee->sym = s;  // the generic declaration
            callee->ty = ft;  // the stamped signature — args recovered via the
            return ft->ret;   // instantiation memo on s->decl's tparams node
        }
        ck_err(k, call, "'%.*s' is not a generic function", (int)callee->a->len, callee->a->str);
        return T_ERR;
    }
    // method / interface dispatch
    if (callee->kind == N_FIELD_E) {
        if (callee->a->kind == N_IDENT) {
            Sym* ms = lookup_val(k, callee->a->str, callee->a->len);
            if (ms && ms->kind == SYM_MODULE) {
                ck_err(k, call, "module member '%.*s.%.*s' needs the stdlib (not yet linked)",
                       (int)callee->a->len, callee->a->str, (int)callee->len, callee->str);
                return T_ERR;
            }
        }
        SType* b = expr_type(k, callee->a);
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind == SK_INTERFACE) {
            Node* m = iface_method(b, callee->str, callee->len);
            if (!m) {
                ck_err(k, call, "interface %s has no method '%.*s'", type_name(b),
                       (int)callee->len, callee->str);
                return T_ERR;
            }
            int mmut = (m->flags & F_MUT) != 0;
            int cmut = (call->flags & F_MUT) != 0;
            if (mmut && !cmut)
                ck_err(k, call, "mutating method needs 'mut' at the call: mut x.%.*s(...)",
                       (int)callee->len, callee->str);
            if (!mmut && cmut)
                ck_err(k, call, "'mut' marks mutating methods; '%.*s' is read-only",
                       (int)callee->len, callee->str);
            SType* ft = st_new(SK_FUNC);
            ft->nparams = m->nkids;
            ft->params = (SType**)arena_alloc((size_t)(m->nkids ? m->nkids : 1) * sizeof(SType*));
            for (int i = 0; i < m->nkids; i++) ft->params[i] = build_type(k, m->kids[i]->a);
            ft->ret = m->a ? build_type(k, m->a) : NULL;
            check_args(k, call, ft, 0);
            return ft->ret;
        }
        // method sugar: x.f() ≡ f(x)
        Sym* f = lookup_val(k, callee->str, callee->len);
        if (f && f->kind == SYM_FUNC && f->type && f->type->nparams >= 1 &&
            same_type(f->type->params[0], b)) {
            if (!f->decl || f->decl->nkids < 1) return T_ERR;
            Node* p0 = f->decl->kids[0];
            int rmut = (p0->flags & F_MUT) != 0;
            int cmut = (call->flags & F_MUT) != 0;
            if (rmut && !cmut)
                ck_err(k, call, "mutating method needs 'mut' at the call site");
            if (!rmut && cmut)
                ck_err(k, call, "'mut' marks mutating methods; '%.*s' is read-only",
                       (int)callee->len, callee->str);
            if (call->nkids != f->type->nparams - 1) {
                ck_err(k, call, "wrong number of arguments: %d given, %d expected",
                       call->nkids, f->type->nparams - 1);
                return f->type->ret;
            }
            for (int i = 0; i < call->nkids; i++) {
                Node* arg = call->kids[i];
                int am = arg->kind == N_MUTARG;
                Node* pa = f->decl->kids[i + 1];
                int pm = (pa->flags & F_MUT) != 0;
                if (am && !pm)
                    ck_err(k, arg, "'mut' marks mut parameters; parameter %d is read-only", i + 2);
                else if (pm && !am)
                    ck_err(k, arg, "mut parameter needs 'mut' at the call site");
                coerce(k, am ? arg->a : arg, f->type->params[i + 1]);
            }
            call->sym = f; // method sugar: x.f() ≡ f(x) — receiver is arg 0
            return f->type->ret;
        }
        ck_err(k, call, "no method '%.*s' for %s", (int)callee->len, callee->str,
               type_name(b));
        return T_ERR;
    }
    ck_err(k, call, "unsupported callee");
    return T_ERR;
}

// --- statements ------------------------------------------------------------------------

static void narrow(Ck* k, Node* cond, int taken) {
    if (!cond || cond->kind != N_BINARY) return;
    int neq = ((TokKind)cond->op == T_BANGEQ) == taken;
    if (!neq) return;
    if (cond->b->kind != N_NULL || cond->a->kind != N_IDENT) return;
    Sym* s = lookup_val(k, cond->a->str, cond->a->len);
    if (!s || !s->type || s->type->kind != SK_OPT) return;
    Sym* ns = sym_new(SYM_VAR, s->name, s->len);
    ns->type = s->type->base;
    ns->decl = s->decl;
    ns->is_mut = s->is_mut;
    ns->is_const = s->is_const;
    ns->next = k->scope->values;
    k->scope->values = ns;
}

static void check_stmt(Ck* k, Node* s) {
    switch (s->kind) {
    case N_VAR: {
        SType** wants = (SType**)arena_alloc((size_t)(s->nkids ? s->nkids : 1) * sizeof(SType*));
        for (int i = 0; i < s->nkids; i++) {
            Node* binder = s->kids[i];
            SType* t = build_type(k, binder->a);
            wants[i] = t;
            Sym* v = declare_val(k, binder, SYM_VAR, binder->str, binder->len);
            if (v) {
                v->type = t;
                v->is_const = (s->flags & F_CONST) != 0;
                v->decl = binder;
                binder->sym = v; // emitter: slot key + declared type
            }
        }
        if (!s->b) {
            if (s->flags & F_CONST)
                ck_err(k, s, "const declaration requires an initializer");
            else if (s->nkids > 1)
                ck_err(k, s, "destructuring declaration requires an initializer");
            return;
        }
        if (s->nkids == 1) {
            Node* init = s->b;
            if (init->kind == N_STRUCTLIT && !init->a && wants[0]->kind == SK_STRUCT)
                check_structlit(k, init, wants[0]);
            else if (init->kind == N_ARRAYLIT && wants[0]->kind == SK_ARRAY)
                check_arraylit(k, init, wants[0]);
            else
                coerce(k, init, wants[0]);
            return;
        }
        SType* rt = expr_type(k, s->b);
        if (rt->kind == SK_ERR) return;
        if (rt->kind != SK_TUPLE || rt->nfields != s->nkids) {
            ck_err(k, s, "destructuring initializer returns %d values, %d names bound",
                   rt->kind == SK_TUPLE ? rt->nfields : 1, s->nkids);
            return;
        }
        if (s->b->kind == N_TUPLE_E) { // explicit tuple: coerce element-wise
            for (int i = 0; i < s->nkids; i++)
                coerce(k, s->b->kids[i], wants[i]);
        } else { // multi-return call: binder types must equal return types
            for (int i = 0; i < s->nkids; i++)
                if (!same_type(rt->fields[i].t, wants[i]))
                    ck_err(k, s->kids[i],
                           "value %d of the call is %s but '%.*s' is %s (casts are explicit)",
                           i + 1, type_name(rt->fields[i].t), (int)s->kids[i]->len,
                           s->kids[i]->str, type_name(wants[i]));
        }
        return;
    }
    case N_ASSIGN: {
        int compound = s->op != T_ASSIGN;
        if (s->nkids > 1) {
            if (compound) { ck_err(k, s, "compound assignment takes one target"); return; }
            SType* rt = expr_type(k, s->a);
            if (rt->kind == SK_ERR) return;
            if (rt->kind != SK_TUPLE || rt->nfields != s->nkids) {
                ck_err(k, s, "assignment binds %d names but the right side has %d values",
                       s->nkids, rt->kind == SK_TUPLE ? rt->nfields : 1);
                return;
            }
            if (s->a->kind == N_TUPLE_E) { // explicit tuple: coerce element-wise
                for (int i = 0; i < s->nkids; i++) {
                    SType* t = check_lvalue(k, s->kids[i], 1);
                    coerce(k, s->a->kids[i], t);
                }
            } else { // multi-return call: targets must equal return types
                for (int i = 0; i < s->nkids; i++) {
                    SType* t = check_lvalue(k, s->kids[i], 1);
                    if (!same_type(rt->fields[i].t, t))
                        ck_err(k, s->kids[i],
                               "value %d of the call is %s but the target is %s (casts are explicit)",
                               i + 1, type_name(rt->fields[i].t), type_name(t));
                }
            }
            return;
        }
        SType* t = check_lvalue(k, s->kids[0], 1);
        if (t->kind == SK_ERR) return;
        Node* rhs = s->a;
        if (rhs->kind == N_STRUCTLIT && !rhs->a && t->kind == SK_STRUCT) {
            check_structlit(k, rhs, t);
            return;
        }
        if (rhs->kind == N_ARRAYLIT && t->kind == SK_ARRAY) {
            check_arraylit(k, rhs, t);
            return;
        }
        coerce(k, rhs, t); // compound ops check as x = x op y with same type
        return;
    }
    case N_EXPRSTMT: {
        SType* t = expr_type(k, s->a);
        if (t == T_UT_INT || t == T_UT_FLOAT)
            ck_err(k, s, "untyped literal expression has no effect — bind it to a type");
        return;
    }
    case N_IF: {
        SType* c = expr_type(k, s->a);
        if (c->kind != SK_ERR && !is_bool(c))
            ck_err(k, s->a, "if condition must be bool, not %s", type_name(c));
        scope_push(k);
        narrow(k, s->a, 1);
        check_block(k, s->b);
        scope_pop(k);
        if (s->c) {
            scope_push(k);
            narrow(k, s->a, 0);
            check_stmt(k, s->c); // else-if chains are N_IF; blocks self-scope
            scope_pop(k);
        }
        return;
    }
    case N_FOR: {
        SType* elem = build_type(k, s->a);
        scope_push(k);
        k->scope->is_loop = 1;
        Sym* v = declare_val(k, s, SYM_VAR, s->str, s->len);
        if (v) {
            v->type = elem;
            v->decl = s; // emitter: the loop variable's slot key
            s->sym = v; // loop variable: its slot key is the N_FOR node
        }
        SType* it = expr_type(k, s->b);
        if (it->kind != SK_ERR) {
            if (s->b->kind == N_RANGE) {
                if (!is_int(elem))
                    ck_err(k, s, "range loop variable must be an integer type, not %s",
                           type_name(elem));
            } else if (it->kind == SK_SLICE || it->kind == SK_ARRAY) {
                if (!same_type(it->base, elem))
                    ck_err(k, s, "loop variable is %s but elements are %s",
                           type_name(elem), type_name(it->base));
            } else {
                ck_err(k, s->b, "for-in iterates slices, arrays, or ranges — not %s",
                       type_name(it));
            }
        }
        check_block(k, s->c);
        scope_pop(k);
        return;
    }
    case N_SWITCH: {
        SType* t = expr_type(k, s->a);
        int has_else = 0;
        for (int i = 0; i < s->nkids; i++) {
            Node* c = s->kids[i];
            if (c->flags & F_ELSE) {
                if (has_else) ck_err(k, c, "duplicate else case");
                has_else = 1;
            } else if (t->kind != SK_ERR) {
                for (int j = 0; j < c->nkids; j++) coerce(k, c->kids[j], t);
            }
            scope_push(k);
            check_block(k, c->b);
            scope_pop(k);
        }
        return;
    }
    case N_RETURN: {
        SType* rt = k->ret;
        int nret = rt ? (rt->kind == SK_TUPLE ? rt->nfields : 1) : 0;
        if (!s->a) {
            if (nret != 0)
                ck_err(k, s, "function returns %d values; bare return returns none", nret);
            return;
        }
        if (s->a->kind == N_TUPLE_E) {
            if (!rt || rt->kind != SK_TUPLE) {
                ck_err(k, s, "function returns %d value, return has %d", nret, s->a->nkids);
                return;
            }
            if (s->a->nkids != rt->nfields) {
                ck_err(k, s, "function returns %d values, return has %d",
                       rt->nfields, s->a->nkids);
                return;
            }
            for (int i = 0; i < s->a->nkids; i++)
                coerce(k, s->a->kids[i], rt->fields[i].t);
            s->a->ty = rt;
            return;
        }
        if (rt && rt->kind == SK_TUPLE) {
            ck_err(k, s, "function returns %d values, return has 1", rt->nfields);
            return;
        }
        if (!rt) {
            ck_err(k, s, "function returns nothing");
            return;
        }
        coerce(k, s->a, rt);
        return;
    }
    case N_BREAK: {
        if (s->str) {
            for (Scope* sc = k->scope; sc; sc = sc->parent)
                if (sc->label && (uint32_t)strlen(sc->label) == s->len &&
                    memcmp(sc->label, s->str, s->len) == 0) return;
            ck_err(k, s, "no enclosing block labeled '%.*s'", (int)s->len, s->str);
        } else {
            int loop = 0;
            for (Scope* sc = k->scope; sc; sc = sc->parent) loop |= sc->is_loop;
            if (!loop) ck_err(k, s, "break outside a loop");
        }
        return;
    }
    case N_CONTINUE: {
        int loop = 0;
        for (Scope* sc = k->scope; sc; sc = sc->parent) loop |= sc->is_loop;
        if (!loop) ck_err(k, s, "continue outside a loop");
        return;
    }
    case N_LABEL: {
        scope_push(k);
        k->scope->label = s->str;
        check_block(k, s->a);
        scope_pop(k);
        return;
    }
    case N_ASM: {
        for (int i = 0; i < s->nkids; i++) {
            Node* bnd = s->kids[i];
            int isclob = bnd->len == 8 && memcmp(bnd->str, "clobbers", 8) == 0;
            if (isclob) continue; // register names only
            for (int j = 0; j < bnd->nkids; j += 2) { // pairs: reg, value
                Node* val = bnd->kids[j + 1];
                if (val->kind == N_IDENT) {
                    Sym* vs = lookup_val(k, val->str, val->len);
                    if (!vs) {
                        ck_err(k, val, "asm binding '%.*s' is not a local or parameter",
                               (int)val->len, val->str);
                        continue;
                    }
                    val->sym = vs; // emitter: storage for in/out moves
                }
            }
        }
        return;
    }
    case N_BLOCK:
        scope_push(k);
        check_block(k, s);
        scope_pop(k);
        return;
    default:
        ck_err(k, s, "unsupported statement in checker");
    }
}

static void check_block(Ck* k, Node* b) {
    for (int i = 0; i < b->nkids; i++) check_stmt(k, b->kids[i]);
}

// --- literals ----------------------------------------------------------------------------

static void check_structlit(Ck* k, Node* lit, SType* want) {
    lit->ty = want; // emitter: context-typed literal carries its type
    if (want->kind != SK_STRUCT) {
        ck_err(k, lit, "struct literal used where %s expected", type_name(want));
        return;
    }
    int n = lit->nkids;
    int positional = n > 0 && lit->kids[0]->str == NULL;
    for (int i = 0; i < n; i++) {
        Node* fi = lit->kids[i];
        if (positional) {
            if (fi->str) { ck_err(k, fi, "cannot mix named and positional fields"); return; }
            if (i >= want->nfields) {
                ck_err(k, fi, "too many fields: %s has %d", type_name(want), want->nfields);
                return;
            }
            coerce(k, fi->a, want->fields[i].t);
        } else {
            if (!fi->str) { ck_err(k, fi, "cannot mix named and positional fields"); return; }
            int found = -1;
            for (int j = 0; j < want->nfields; j++)
                if ((uint32_t)strlen(want->fields[j].name) == fi->len &&
                    memcmp(want->fields[j].name, fi->str, fi->len) == 0) { found = j; break; }
            if (found < 0) {
                ck_err(k, fi, "type %s has no field '%.*s'", type_name(want),
                       (int)fi->len, fi->str);
                continue;
            }
            coerce(k, fi->a, want->fields[found].t);
        }
    }
    if (positional && n < want->nfields)
        ck_err(k, lit, "missing fields: %s has %d, literal has %d",
               type_name(want), want->nfields, n);
}

static void check_arraylit(Ck* k, Node* lit, SType* want) {
    lit->ty = want; // emitter: context-typed literal carries its type
    if (want->kind != SK_ARRAY) {
        ck_err(k, lit, "array literal used where %s expected", type_name(want));
        return;
    }
    if ((uint64_t)lit->nkids != want->count) {
        ck_err(k, lit, "array length is %llu, literal has %d elements",
               (unsigned long long)want->count, lit->nkids);
        return;
    }
    for (int i = 0; i < lit->nkids; i++) coerce(k, lit->kids[i], want->base);
}

// --- functions & top level ------------------------------------------------------------------

static void check_func_body(Ck* k, Node* f) {
    SType* saved_ret = k->ret; // nested checks (generic stamping) must not clobber
    Node* saved_fn = k->curfn;
    scope_push(k);
    for (int i = 0; i < f->nkids; i++) {
        Node* pa = f->kids[i];
        Sym* s = declare_val(k, pa, SYM_VAR, pa->str, pa->len);
        if (s) {
            s->type = build_type(k, pa->a);
            s->is_mut = (pa->flags & F_MUT) != 0;
            s->is_noalias = (pa->flags & F_NOALIAS) != 0;
            s->is_param = 1;
            s->decl = pa;
            pa->sym = s;
        }
    }
    k->ret = f->a ? build_type(k, f->a) : NULL;
    k->curfn = f;
    check_block(k, f->b);
    scope_pop(k);
    k->ret = saved_ret;
    k->curfn = saved_fn;
}

static SType* build_struct_type(Ck* k, Node* decl) {
    SType* t = st_new(SK_STRUCT);
    t->name = decl->str;
    t->decl = decl;
    t->nfields = decl->nkids;
    t->fields = (SField*)arena_alloc((size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    for (int i = 0; i < decl->nkids; i++) {
        t->fields[i].name = decl->kids[i]->str;
        t->fields[i].t = build_type(k, decl->kids[i]->a);
    }
    for (int i = 0; i < t->nfields; i++)
        for (int j = i + 1; j < t->nfields; j++)
            if (strcmp(t->fields[i].name, t->fields[j].name) == 0)
                ck_err(k, decl, "duplicate field '%s' in struct %s",
                       t->fields[i].name, decl->str);
    layout(t);
    return t;
}

static SType* instantiate_generic_type(Ck* k, Node* decl, SType** args, int nargs) {
    if (!decl || !decl->c) return T_ERR;
    Node* tp = decl->c;
    if (tp->nkids != nargs) {
        ck_err(k, decl, "generic type expects %d type arguments, got %d",
               tp->nkids, nargs);
        return T_ERR;
    }
    STypeInst** head = (STypeInst**)&tp->b; // memo on the tparams node
    for (STypeInst* it = *head; it; it = it->next) {
        int same = 1;
        for (int i = 0; i < nargs && same; i++) same = same_type(it->args[i], args[i]);
        if (same) return it->result;
    }
    const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
    k->tnames = (const char**)arena_alloc((size_t)nargs * sizeof(char*));
    k->ttypes = args;
    k->nsub = nargs;
    for (int i = 0; i < nargs; i++) k->tnames[i] = tp->kids[i]->str;
    SType* t = decl->kind == N_STRUCT ? build_struct_type(k, decl) : T_ERR;
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
    STypeInst* inst = (STypeInst*)arena_alloc(sizeof(STypeInst));
    inst->args = args;
    inst->result = t;
    inst->next = *head;
    *head = inst;
    return t;
}

// impl conformance: every interface method present, signatures match
static void check_impl(Ck* k, Node* impl) {
    Sym* tsym = lookup_type(k, impl->b->str, impl->b->len);
    if (!tsym || !tsym->type || tsym->type->kind != SK_INTERFACE) {
        ck_err(k, impl->b, "'%.*s' is not an interface", (int)impl->b->len, impl->b->str);
        return;
    }
    SType* iface = tsym->type;
    impl->b->sym = tsym; // emitter: interface type symbol for vtable layout
    Sym* csym = lookup_type(k, impl->a->str, impl->a->len);
    SType* conc = csym && csym->type ? csym->type : NULL;
    if (!conc) {
        ck_err(k, impl->a, "unknown type '%.*s' in impl", (int)impl->a->len, impl->a->str);
        return;
    }
    SType* pconc = st_new(SK_PTR);
    pconc->base = conc;
    for (int i = 0; i < iface->decl->nkids; i++) {
        Node* m = iface->decl->kids[i];
        Node* entry = NULL;
        for (int j = 0; j < impl->nkids; j++)
            if (impl->kids[j]->len == m->len &&
                memcmp(impl->kids[j]->str, m->str, m->len) == 0) { entry = impl->kids[j]; break; }
        if (!entry) {
            ck_err(k, impl, "impl misses interface method '%.*s'", (int)m->len, m->str);
            continue;
        }
        int mmut = (m->flags & F_MUT) != 0;
        int emut = (entry->flags & F_MUT) != 0;
        if (mmut != emut) {
            ck_err(k, entry, "method '%.*s' mut-ness does not match the interface",
                   (int)m->len, m->str);
            continue;
        }
        Sym* fn = lookup_val(k, entry->a->str, entry->a->len);
        if (!fn || fn->kind != SYM_FUNC || !fn->type) {
            ck_err(k, entry->a, "unknown function '%.*s' in impl entry",
                   (int)entry->a->len, entry->a->str);
            continue;
        }
        SType* ft = fn->type;
        if (ft->nparams != m->nkids + 1) {
            ck_err(k, entry, "'%.*s' takes %d parameters; method '%.*s' needs %d plus the receiver",
                   (int)entry->a->len, entry->a->str, ft->nparams, (int)m->len, m->str, m->nkids);
            continue;
        }
        if (!same_type(ft->params[0], pconc)) {
            ck_err(k, entry, "receiver of '%.*s' must be %s", (int)entry->a->len,
                   entry->a->str, type_name(pconc));
            continue;
        }
        if (fn->decl && ((fn->decl->kids[0]->flags & F_MUT) != 0) != mmut) {
            ck_err(k, entry, "receiver mut-ness of '%.*s' does not match the method",
                   (int)entry->a->len, entry->a->str);
            continue;
        }
        int bad = 0;
        for (int p = 0; p < m->nkids; p++) {
            SType* want = build_type(k, m->kids[p]->a);
            if (!same_type(ft->params[p + 1], want)) {
                ck_err(k, entry, "parameter %d of '%.*s' is %s; method wants %s",
                       p + 2, (int)entry->a->len, entry->a->str,
                       type_name(ft->params[p + 1]), type_name(want));
                bad = 1;
            }
        }
        SType* mret = m->a ? build_type(k, m->a) : NULL;
        if (!same_type(ft->ret, mret)) {
            ck_err(k, entry, "'%.*s' returns %s; method returns %s",
                   (int)entry->a->len, entry->a->str,
                   ft->ret ? type_name(ft->ret) : "nothing",
                   mret ? type_name(mret) : "nothing");
            bad = 1;
        }
        (void)bad;
    }
    // register the constructor: circle_shape(data: *Point): Shape
    Sym* ctor = declare_val(k, impl, SYM_IMPL, impl->str, impl->len);
    if (ctor) {
        SType* ft = st_new(SK_FUNC);
        ft->nparams = 1;
        ft->params = (SType**)arena_alloc(sizeof(SType*));
        ft->params[0] = pconc;
        ft->ret = iface;
        ctor->type = ft;
        ctor->decl = impl;
    }
}

// --- emitter support ---------------------------------------------------------------

static Scope* g_file_scope; // survives sema_check for stamping and type building
static Diag g_dummy_diag = {"(stamp)", 0}; // re-stamps are pre-checked: silent

uint32_t sema_size(SType* t) { layout(t); return t->size; }
uint32_t sema_align(SType* t) { layout(t); return t->align; }

SType* sema_build_type(Node* t) {
    Ck k = {0};
    k.d = &g_dummy_diag;
    k.scope = g_file_scope;
    return build_type(&k, t);
}

STypeInst* sema_insts(Node* generic_decl) {
    return generic_decl && generic_decl->c ? (STypeInst*)generic_decl->c->b : NULL;
}

void sema_stamp_body(Node* f, SType** args, int nargs) {
    Node* tp = f->c;
    if (!tp || tp->nkids != nargs) return;
    Ck k = {0};
    k.d = &g_dummy_diag;
    k.scope = g_file_scope;
    k.tnames = (const char**)arena_alloc((size_t)nargs * sizeof(char*));
    k.ttypes = args;
    k.nsub = nargs;
    for (int i = 0; i < nargs; i++) k.tnames[i] = tp->kids[i]->str;
    check_func_body(&k, f); // re-annotate the body under substitution
}

int sema_check(Node* file, Diag* d) {
    static Scope file_scope; // static: outlives the call for stamp re-checks
    memset(&file_scope, 0, sizeof file_scope);
    g_file_scope = &file_scope;
    if (!PRIMS[0]) {
        T_ERR = st_new(SK_ERR);
        T_UT_INT = st_new(SK_UT);
        T_UT_FLOAT = st_new(SK_UT);
        T_UT_NULL = st_new(SK_UT);
        for (int i = 0; i < 14; i++) {
            PRIMS[i] = st_new(SK_PRIM);
            PRIMS[i]->prim = (uint8_t)i;
            layout(PRIMS[i]);
        }
    }
    Ck k = {0};
    k.d = d;
    k.scope = &file_scope;
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        switch (n->kind) {
        case N_IMPORT: {
            Sym* m = declare_val(&k, n, SYM_MODULE, n->str, n->len);
            (void)m;
            break;
        }
        case N_FUNC: {
            Sym* s = declare_val(&k, n, SYM_FUNC, n->str, n->len);
            if (!s) break;
            s->decl = n;
            s->is_generic = n->c != NULL;
            if (n->len == 4 && memcmp(n->str, "main", 4) == 0) {
                int spells_int = n->a && n->a->kind == N_NAMED &&
                    n->a->len == 3 && memcmp(n->a->str, "int", 3) == 0;
                if (s->is_generic || n->nkids != 0 || !spells_int)
                    ck_err(&k, n, "entry function must be main(): int");
            }
            if (!s->is_generic) {
                s->type = st_new(SK_FUNC);
                s->type->nparams = n->nkids;
                s->type->params = (SType**)arena_alloc(
                    (size_t)(n->nkids ? n->nkids : 1) * sizeof(SType*));
                for (int p = 0; p < n->nkids; p++)
                    s->type->params[p] = build_type(&k, n->kids[p]->a);
                s->type->ret = n->a ? build_type(&k, n->a) : NULL;
                check_func_body(&k, n); // single pass: check now, in order
            }
            break;
        }
        case N_EXTERN: {
            for (int j = 0; j < n->nkids; j++) {
                Node* f = n->kids[j];
                Sym* s = declare_val(&k, f, SYM_FUNC, f->str, f->len);
                if (!s) continue;
                s->decl = f;
                s->is_extern = 1;
                s->type = st_new(SK_FUNC);
                s->type->nparams = f->nkids;
                s->type->params = (SType**)arena_alloc(
                    (size_t)(f->nkids ? f->nkids : 1) * sizeof(SType*));
                for (int p = 0; p < f->nkids; p++)
                    s->type->params[p] = build_type(&k, f->kids[p]->a);
                s->type->ret = f->a ? build_type(&k, f->a) : NULL;
            }
            break;
        }
        case N_VAR: {
            Node* binder = n->kids[0];
            SType* t = build_type(&k, binder->a);
            Sym* s = declare_val(&k, n, SYM_VAR, binder->str, binder->len);
            if (s) {
                s->type = t;
                s->is_const = (n->flags & F_CONST) != 0;
                s->is_pub = (n->flags & F_PUB) != 0;
                s->decl = binder;
                binder->sym = s; // emitter: global storage key
            }
            if (n->b) {
                if (n->b->kind == N_STRUCTLIT && !n->b->a && t->kind == SK_STRUCT)
                    check_structlit(&k, n->b, t);
                else if (n->b->kind == N_ARRAYLIT && t->kind == SK_ARRAY)
                    check_arraylit(&k, n->b, t);
                else
                    coerce(&k, n->b, t);
            } else if (n->flags & F_CONST) {
                ck_err(&k, n, "const declaration requires an initializer");
            }
            break;
        }
        case N_STRUCT: case N_INTERFACE: case N_ENUM: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            SType* t = st_new(n->kind == N_STRUCT ? SK_STRUCT :
                              n->kind == N_ENUM ? SK_ENUM : SK_INTERFACE);
            t->name = n->str;
            t->decl = n;
            s->type = t;
            s->is_generic = n->c != NULL;
            if (!s->is_generic && n->kind == N_STRUCT) {
                // register first (self-reference via pointer), then build fields
                SType* built = build_struct_type(&k, n);
                s->type = built;
            } else if (n->kind == N_ENUM) {
                layout(t);
            }
            break;
        }
        case N_ALIAS: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            s->is_generic = n->c != NULL;
            if (!s->is_generic) s->type = build_type(&k, n->a);
            break;
        }
        case N_DISTINCT: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            SType* t = st_new(SK_DISTINCT);
            t->name = n->str;
            t->decl = n;
            t->base = build_type(&k, n->a);
            s->type = t;
            layout(t);
            break;
        }
        case N_IMPL:
            check_impl(&k, n);
            break;
        default:
            break;
        }
    }

    // forward top-level calls: verify existence and argument types
    k.scope = &file_scope;
    for (Pending* pd = k.pending; pd; pd = pd->next) {
        Node* call = pd->call;
        Node* callee = call->a;
        Sym* s = lookup_val(&k, callee->str, callee->len);
        if (!s || s->kind != SYM_FUNC || s->is_generic)
            ck_err(&k, call, "unknown function '%.*s'", (int)callee->len, callee->str);
        else {
            check_args(&k, call, s->type, s->is_extern);
            check_mut_args(&k, call, s->decl);
            call->sym = s;  // resolved now that the definition exists;
            call->ty = s->type->ret; // the emitter emits this like any direct call
        }
    }
    return d->errors;
}
