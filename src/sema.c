// sema.c — semantic checking (stage three): types, names, expression typing.
// Walks the AST in declaration order — single-pass semantics: declaration
// before use; forward top-level calls are deferred (like emitter fixups), and
// aggregate fields admit a second narrow fixup for uninterrupted pointer
// paths to a later same-module type, or a bare slice at the field/variant
// root ([]Later — its ptr+count representation is equally pointee-size-
// independent, but the allowance doesn't extend through or past it). Both
// are verified at end of file.
//
// Type model (machine truth: size/align computed for every type):
//   prims    bool i8..i64 u8..u64 f32 f64 string usize typeid
//            (`int`/`uint` = i64/u64, the platform word on arm64)
//   nominal  struct, enum (default i32 or explicit fixed-int layout),
//            contract (16 bytes), distinct, Error,
//            union (i32 tag + largest payload inline)
//   struct.  *T (8), []T (16), [N]T, tuples
//            (multi-return, laid out like anonymous structs), funcs
// Literals are untyped constants: they take the expected type at use sites
// and must fit; null materializes pointer-like/handle-like zero. Everything
// else casts explicitly.
#include "ace.h"
#include "sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- type kinds (definitions shared via sema.h) ---------------------------------

static SType *T_ERR, *T_VOID, *T_UT_INT, *T_UT_FLOAT, *T_UT_NULL;
static SType *T_ERROR;
static SType* T_BYTE_SLICE;
static SType* T_MUT_BYTE_SLICE; // mut []u8 — writable byte view (Mutability #9)
static SType* T_MUT_STRING;     // mut string — writable bytes
static SType* PRIMS[14];
static uint32_t next_type_id = 1;

static SType* st_new(uint8_t kind) {
    SType* t = (SType*)arena_alloc(sizeof(SType));
    memset(t, 0, sizeof *t);
    t->kind = kind;
    return t;
}

static SType* st_prim(int p) { return PRIMS[p]; }

static int is_int(SType* t) {
    return t->kind == SK_PRIM && ((t->prim >= P_I8 && t->prim <= P_U64) || t->prim == P_USIZE);
}
static int is_float(SType* t) {
    return t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64);
}
static int is_num(SType* t) { return is_int(t) || is_float(t); }
static int is_bool(SType* t) { return t->kind == SK_PRIM && t->prim == P_BOOL; }

static int is_bits_field(Node* e, SType** owner, int* index) {
    if (!e || e->kind != N_FIELD_E) return 0;
    SType* b = node_ty(e->a);
    while (b && b->kind == SK_PTR) b = b->base;
    if (!b || b->kind != SK_BITS) return 0;
    for (int i = 0; i < b->nfields; i++)
        if (!b->fields[i].bit_anonymous && strlen(b->fields[i].name) == e->len &&
            memcmp(b->fields[i].name, e->str, e->len) == 0) {
            if (owner) *owner = b;
            if (index) *index = i;
            return 1;
        }
    return 0;
}

static int is_volatile_scalar(SType* t) {
    if (!t) return 0;
    if (t->kind == SK_DISTINCT) return is_volatile_scalar(t->base);
    if (t->kind == SK_BITS) return 1;
    if (t->kind == SK_PTR) return 1;
    if (t->kind == SK_ENUM) return t->decl && t->decl->a;
    if (t->kind != SK_PRIM) return 0;
    return (t->prim >= P_I8 && t->prim <= P_U64) || t->prim == P_F32 || t->prim == P_F64 || t->prim == P_USIZE;
}

static const char* type_name(SType* t) {
    /* slice/array names render their element type: diagnostics must name the
       exact mismatched type, not a placeholder. A small ring of static
       buffers bounds nested compositions. */
    static char bufs[8][128];
    static int bi;
    switch (t->kind) {
    case SK_PRIM:
        switch (t->prim) {
        case P_BOOL: return "bool";
        case P_I8: return "i8"; case P_I16: return "i16";
        case P_I32: return "i32"; case P_I64: return "i64";
        case P_U8: return "u8"; case P_U16: return "u16";
        case P_U32: return "u32"; case P_U64: return "u64";
        case P_F32: return "f32"; case P_F64: return "f64";
        case P_STRING: return t->is_mut ? "mut string" : "string";
        case P_USIZE: return "usize";
        case P_TYPEID: return "typeid";
        }
        return "?";
    case SK_PTR: return t->is_mut ? "mut *T" : "*T";
    case SK_SLICE: {
        char* out = bufs[bi = (bi + 1) & 7];
        snprintf(out, sizeof bufs[0], "%s[]%s", t->is_mut ? "mut " : "", t->base ? type_name(t->base) : "?");
        return out;
    }
    case SK_ARRAY: {
        char* out = bufs[bi = (bi + 1) & 7];
        snprintf(out, sizeof bufs[0], "[%llu]%s", (unsigned long long)t->count, t->base ? type_name(t->base) : "?");
        return out;
    }
    case SK_STATICMAP: {
        char* out = bufs[bi = (bi + 1) & 7];
        snprintf(out, sizeof bufs[0], "StaticMap[%s]%s",
                 t->base ? type_name(t->base) : "?",
                 t->ret ? type_name(t->ret) : "?");
        return out;
    }
    case SK_TUPLE: return "(tuple)";
    case SK_FUNC: return "(func)";
    case SK_STRUCT: return t->name ? t->name : "struct";
    case SK_ENUM: return t->name ? t->name : "enum";
    case SK_CONTRACT: return t->name ? t->name : "contract";
    case SK_UNION: return t->name ? t->name : "union";
    case SK_BITS: return t->name ? t->name : "bits";
    case SK_DISTINCT: return t->name ? t->name : "distinct";
    case SK_ERROR_DOMAIN: return t->name ? t->name : "error domain";
    case SK_ERROR: return "Error";
    case SK_VOID: return "no value";
    case SK_UT: return t == T_UT_INT ? "integer literal" :
                    t == T_UT_FLOAT ? "float literal" : "null";
    case SK_FORWARD: return t->name ? t->name : "forward type";
    default: return "?";
    }
}

// --- layout: declaration order, natural alignment -------------------------------

static uint32_t align_to(uint32_t v, uint32_t a) { return a ? (v + a - 1) / a * a : v; }

static int layout(SType* t) {
    if (t->invalid_layout) return 0;
    if (t->size) return 1;
    if (t->calc || t->building || t->kind == SK_FORWARD) return 0;
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
        return 1;
    case SK_PTR: case SK_FUNC: case SK_TYPEPARAM:
        t->size = t->align = 8;
        return 1;
    case SK_SLICE:
        t->size = 16; t->align = 8;
        return 1;
    case SK_ARRAY:
        t->calc = 1;
        if (!layout(t->base)) { t->calc = 0; return 0; }
        t->align = t->base->align;
        t->size = (uint32_t)(t->base->size * t->count);
        t->calc = 0;
        return 1;
    case SK_STATICMAP:
        /* A map binding has no runtime descriptor. Give the compiler-only
           type a harmless complete layout so generic layout walkers recover. */
        t->size = t->align = 1;
        return 1;
    case SK_STRUCT: case SK_TUPLE: {
        t->calc = 1;
        uint32_t off = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            if (t->fields[i].t->kind == SK_ERR) {
                t->calc = 0; t->size = t->align = 1; return 1;
            }
            if (!layout(t->fields[i].t)) { t->calc = 0; return 0; }
            uint32_t a = t->is_packed ? 1 : t->fields[i].t->align;
            if (t->fields[i].requested_align) a = t->fields[i].requested_align;
            t->fields[i].effective_align = a;
            if (maxa < a) maxa = a;
            off = align_to(off, a);
            t->fields[i].offset = off;
            off += t->fields[i].t->size;
        }
        if (t->requested_align > maxa) maxa = t->requested_align;
        t->align = maxa;
        t->size = align_to(off, maxa);
        t->calc = 0;
        return 1;
    }
    case SK_ENUM:
        if (!t->base) t->base = st_prim(P_I32);
        if (!layout(t->base)) return 0;
        t->size = t->base->size; t->align = t->base->align;
        return 1;
    case SK_BITS:
        if (!layout(t->base)) return 0;
        t->size = t->base->size; t->align = t->base->align;
        return 1;
    case SK_CONTRACT: t->size = 16; t->align = 8; return 1; // vtable + data
    case SK_ERROR: t->size = t->align = 8; return 1; // packed domain/code value
    case SK_UNION: {
        /*
         * i32 tag at offset 0, then inline storage for the largest payload.
         * Declaration order fixes tag values from zero; the first variant is
         * the canonical all-bits-zero value (BSS globals get it for free).
         */
        t->calc = 1;
        uint32_t maxs = 0, maxa = 1;
        for (int i = 0; i < t->nfields; i++) {
            if (!t->fields[i].t) continue; // payloadless variant
            if (t->fields[i].t->kind == SK_ERR) {
                t->calc = 0; t->size = t->align = 1; return 1;
            }
            if (!layout(t->fields[i].t)) { t->calc = 0; return 0; }
            if (maxs < t->fields[i].t->size) maxs = t->fields[i].t->size;
            if (maxa < t->fields[i].t->align) maxa = t->fields[i].t->align;
        }
        t->align = maxa > 4 ? maxa : 4;
        uint32_t off = align_to(4, maxa);
        t->size = align_to(off + maxs, t->align);
        t->calc = 0;
        return 1;
    }
    case SK_DISTINCT:
        t->calc = 1;
        if (!layout(t->base)) { t->calc = 0; return 0; }
        t->size = t->base->size;
        t->align = t->base->align;
        t->calc = 0;
        return 1;
    default:
        t->size = t->align = 1;
        return 1;
    }
}

// --- symbols & scopes -------------------------------------------------------------
// (Sym and kind enums are shared via sema.h)

// Open-addressing index over one scope list; names are unique per list.
typedef struct {
    Sym**    slots;
    uint32_t cap;
} SymIndex;

typedef struct Scope {
    struct Scope* parent;
    Sym*          values;
    Sym*          types;
    SymIndex      value_index; // built once a list passes SCOPE_INDEX_MIN
    SymIndex      type_index;
    uint32_t      nvalues, ntypes;
    const char*   label;
    int           is_loop;
    int           is_fnbody; // the function's outermost body block
} Scope;

typedef struct Pending {
    Node* call;
    Scope* scope;
    SType* recv;   // method/associated calls: struct owner type
    int qualified; // method calls: Type.method(recv, ...) spelling
    int method;    // method call awaiting its declaration
    int assoc;     // associated-function call awaiting its declaration
    struct Pending* next;
} Pending;

typedef struct TypeFix TypeFix;
struct TypeFix {
    Sym* sym;
    SType** slot;
    Node* ref;
    TypeFix* next;
};

typedef struct {
    Diag*   d;
    Scope*  scope;
    SType*  ret;
    Node*   curfn;
    // generic substitution environment (names -> types)
    const char** tnames; SType** ttypes; int nsub;
    Pending* pending;
    TypeFix* typefix;
    int type_build_depth;
} Ck;

// Arena memory is not zeroed; every Pending field must start defined.
static Pending* new_pending(Ck* k) {
    Pending* pd = (Pending*)arena_alloc(sizeof(Pending));
    memset(pd, 0, sizeof *pd);
    pd->scope = k->scope;
    pd->next = k->pending;
    k->pending = pd;
    return pd;
}

static SType* build_type(Ck* k, Node* t);
static SType* instantiate_generic_type(Ck* k, Node* decl, SType** args, int nargs);

static void ck_err(Ck* k, Node* n, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", n && n->path ? n->path : k->d->path, n ? n->line : 0, n ? n->col : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    k->d->errors++;
}

// `panic` and `assert` are reserved builtin names: no binding may take either
// in any scope. Unlike them, sizeof/alignof/len/typeid/blank are ordinary shadowable
// names.
static int is_panic_name(const char* name, uint32_t len) {
    return len == 5 && memcmp(name, "panic", 5) == 0;
}

static int is_assert_name(const char* name, uint32_t len) {
    return len == 6 && memcmp(name, "assert", 6) == 0;
}
static int is_static_assert_name(const char* name, uint32_t len) {
    return len == 13 && memcmp(name, "static_assert", 13) == 0;
}
static int is_staticmap_name(const char* name, uint32_t len) {
    return len == 9 && memcmp(name, "StaticMap", 9) == 0;
}

static void ensure_memory_types(void) {
    if (T_BYTE_SLICE) return;
    if (!T_VOID) {
        T_VOID = st_new(SK_VOID);
        T_VOID->size = T_VOID->align = 1;
    }
    T_BYTE_SLICE = st_new(SK_SLICE);
    T_BYTE_SLICE->base = st_prim(P_U8);
    layout(T_BYTE_SLICE);
    T_MUT_BYTE_SLICE = st_new(SK_SLICE);
    T_MUT_BYTE_SLICE->is_mut = 1;
    T_MUT_BYTE_SLICE->base = st_prim(P_U8);
    layout(T_MUT_BYTE_SLICE);
    T_MUT_STRING = st_new(SK_PRIM);
    T_MUT_STRING->prim = P_STRING;
    T_MUT_STRING->is_mut = 1;
    layout(T_MUT_STRING);
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
    for (Sym* s = list; s; s = s->next) if (s->len == len && memcmp(s->name, name, len) == 0) return s;
    return NULL;
}

// Small scopes (most locals) scan their list; larger ones use the index.
enum { SCOPE_INDEX_MIN = 32 };

static uint32_t name_hash(const char* name, uint32_t len) {
    uint32_t h = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)name[i]) * 16777619u;
    return h;
}

static void index_put(SymIndex* ix, Sym* s) {
    uint32_t i = name_hash(s->name, s->len) & (ix->cap - 1);
    while (ix->slots[i]) i = (i + 1) & (ix->cap - 1);
    ix->slots[i] = s;
}

static Sym* index_find(SymIndex* ix, const char* name, uint32_t len) {
    uint32_t i = name_hash(name, len) & (ix->cap - 1);
    for (Sym* s; (s = ix->slots[i]); i = (i + 1) & (ix->cap - 1))
        if (s->len == len && memcmp(s->name, name, len) == 0) return s;
    return NULL;
}

static Sym* scope_find(Scope* sc, int types, const char* name, uint32_t len) {
    Sym* list = types ? sc->types : sc->values;
    SymIndex* ix = types ? &sc->type_index : &sc->value_index;
    return ix->slots ? index_find(ix, name, len) : find_in(list, name, len);
}

static void scope_insert(Scope* sc, int types, Sym* s) {
    Sym** list = types ? &sc->types : &sc->values;
    SymIndex* ix = types ? &sc->type_index : &sc->value_index;
    uint32_t* n = types ? &sc->ntypes : &sc->nvalues;
    s->next = *list;
    *list = s;
    ++*n;
    if (*n < SCOPE_INDEX_MIN) return;
    if (ix->slots && *n * 2 <= ix->cap) {
        index_put(ix, s);
        return;
    }
    uint32_t cap = ix->cap ? ix->cap * 2 : 128; // load factor <= 1/2
    while (cap < *n * 2) cap *= 2;
    ix->slots = (Sym**)arena_alloc((size_t)cap * sizeof(Sym*));
    memset(ix->slots, 0, (size_t)cap * sizeof(Sym*));
    ix->cap = cap;
    for (Sym* it = *list; it; it = it->next) index_put(ix, it);
}

static Sym* lookup_val(Ck* k, const char* name, uint32_t len) {
    for (Scope* s = k->scope; s; s = s->parent) { Sym* v = scope_find(s, 0, name, len); if (v) return v; }
    return NULL;
}

static Sym* lookup_type(Ck* k, const char* name, uint32_t len) {
    for (Scope* s = k->scope; s; s = s->parent) { Sym* v = scope_find(s, 1, name, len); if (v) return v; }
    return NULL;
}

static Sym* declare_val(Ck* k, Node* at, uint8_t kind, const char* name, uint32_t len) {
    if (is_panic_name(name, len) || is_assert_name(name, len) || is_static_assert_name(name, len)) {
        ck_err(k, at, "'%.*s' is a reserved builtin name — no declaration may take it", (int)len, name);
        return NULL;
    }
    if (scope_find(k->scope, 0, name, len)) {
        ck_err(k, at, "duplicate name '%.*s' in this scope", (int)len, name);
        return NULL;
    }
    Sym* s = sym_new(kind, name, len);
    scope_insert(k->scope, 0, s);
    return s;
}

static Sym* declare_type(Ck* k, Node* at, const char* name, uint32_t len) {
    if (is_staticmap_name(name, len)) {
        ck_err(k, at, "'StaticMap' is a predeclared type name and cannot be redeclared");
        return NULL;
    }
    if (is_panic_name(name, len) || is_assert_name(name, len) || is_static_assert_name(name, len)) {
        ck_err(k, at, "'%.*s' is a reserved builtin name — no declaration may take it", (int)len, name);
        return NULL;
    }
    Sym* old = scope_find(k->scope, 1, name, len);
    if (old && old->is_forward) {
        if (old->decl && old->decl->path && at->path && strcmp(old->decl->path, at->path) != 0) {
            ck_err(k, at, "forward type '%.*s' must be declared in the same module", (int)len, name);
            return NULL;
        }
        old->is_forward = 0;
        return old;
    }
    if (old) {
        ck_err(k, at, "duplicate type '%.*s'", (int)len, name);
        return NULL;
    }
    Sym* s = sym_new(SYM_TYPE, name, len);
    scope_insert(k->scope, 1, s);
    return s;
}

static Scope* file_scope(Ck* k) {
    Scope* s = k->scope;
    while (s && s->parent) s = s->parent;
    return s;
}

static Sym* forward_type(Ck* k, Node* ref) {
    Scope* fs = file_scope(k);
    Sym* s = scope_find(fs, 1, ref->str, ref->len);
    if (s) return s;
    s = sym_new(SYM_TYPE, ref->str, ref->len);
    s->is_forward = 1;
    s->decl = ref; // first use: unresolved EOF diagnostic and module identity
    s->type = st_new(SK_FORWARD);
    s->type->name = ref->str;
    s->type->decl = ref;
    scope_insert(fs, 1, s);
    return s;
}

static void record_type_fix(Ck* k, Sym* s, SType** slot, Node* ref) {
    TypeFix* f = (TypeFix*)arena_alloc(sizeof(TypeFix));
    f->sym = s;
    f->slot = slot;
    f->ref = ref;
    f->next = k->typefix;
    k->typefix = f;
}

static void resolve_type_fixes(Ck* k, Sym* s, SType* actual, Node* decl) {
    s->type = actual;
    s->decl = decl;
    for (TypeFix* f = k->typefix; f; f = f->next) {
        if (f->sym != s) continue;
        int nargs = f->ref ? f->ref->nkids : 0;
        if (!s->is_generic) {
            if (nargs)
                ck_err(k, f->ref, "type '%.*s' is not generic",
                       (int)s->len, s->name);
            *f->slot = actual;
            continue;
        }
        if (!nargs) {
            ck_err(k, f->ref,
                   "generic type '%.*s' needs explicit type arguments",
                   (int)s->len, s->name);
            *f->slot = T_ERR;
            continue;
        }
        SType** args = (SType**)arena_alloc((size_t)nargs * sizeof(SType*));
        for (int i = 0; i < nargs; i++) args[i] = build_type(k, f->ref->kids[i]);
        *f->slot = instantiate_generic_type(k, decl, args, nargs);
    }
}

// --- same_type / castable ----------------------------------------------------------

static int ut_const(Node* e, int64_t* out);

static int same_type(SType* a, SType* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind == SK_ERR || b->kind == SK_ERR) return 1; // poison-tolerant
    /* `string`, `[]char`, and `[]u8` are three intent spellings of one
       read-only byte-view type. `char` already resolves to P_U8. Keep the
       parser-facing string primitive so literals retain useful diagnostics
       and direct static emission, but give it slice identity everywhere
       semantic identity is observed (overloads, generics, assignments). */
    if (a->kind != b->kind) {
        SType* string = a->kind == SK_PRIM && a->prim == P_STRING ? a :
                        b->kind == SK_PRIM && b->prim == P_STRING ? b : NULL;
        SType* slice = a->kind == SK_SLICE ? a : b->kind == SK_SLICE ? b : NULL;
        return string && slice && string->is_mut == slice->is_mut &&
               slice->base && slice->base->kind == SK_PRIM &&
               slice->base->prim == P_U8;
    }
    switch (a->kind) {
    case SK_PRIM: return a->prim == b->prim && a->is_mut == b->is_mut;
    case SK_PTR: return a->is_mut == b->is_mut && same_type(a->base, b->base);
    case SK_SLICE:
        return a->is_mut == b->is_mut && same_type(a->base, b->base);
    case SK_ARRAY: return a->count == b->count && same_type(a->base, b->base);
    case SK_STATICMAP:
        return same_type(a->base, b->base) && same_type(a->ret, b->ret);
    case SK_TUPLE:
        if (a->nfields != b->nfields) return 0;
        for (int i = 0; i < a->nfields; i++) if (!same_type(a->fields[i].t, b->fields[i].t)) return 0;
        return 1;
    case SK_FUNC:
        if (a->is_c_abi != b->is_c_abi || a->nparams != b->nparams) return 0;
        for (int i = 0; i < a->nparams; i++) if (!same_type(a->params[i], b->params[i])) return 0;
        return same_type(a->ret, b->ret);
    case SK_STRUCT: case SK_ENUM: case SK_BITS: case SK_CONTRACT: case SK_DISTINCT:
    case SK_ERROR_DOMAIN: case SK_UNION:
        if (a->decl != b->decl || a->nargs != b->nargs) return 0;
        for (int i = 0; i < a->nargs; i++) if (!same_type(a->args[i], b->args[i])) return 0;
        return 1; // nominal identity plus exact generic arguments
    case SK_ERROR: return 1;
    case SK_TYPEPARAM:
        return a->name && b->name && strcmp(a->name, b->name) == 0;
    default: return 0;
    }
}

/*
 * Implicit permission shrink (Mutability #9): a mut-qualified view converts
 * to its plain form at the same level — `mut string` -> `string`,
 * `mut []T` -> `[]T`, `mut *T` -> `*T` — never the widening direction.
 */
static int shrink_ok(SType* f, SType* t) {
    if (!f || !t) return 0;
    if (!f->is_mut || t->is_mut) return 0;
    if (f->kind != t->kind) {
        SType* string = f->kind == SK_PRIM && f->prim == P_STRING ? f :
                        t->kind == SK_PRIM && t->prim == P_STRING ? t : NULL;
        SType* slice = f->kind == SK_SLICE ? f : t->kind == SK_SLICE ? t : NULL;
        return string && slice && slice->base && slice->base->kind == SK_PRIM && slice->base->prim == P_U8;
    }
    switch (f->kind) {
    case SK_PTR: case SK_SLICE:
        return same_type(f->base, t->base);
    case SK_PRIM: // only view prims carry is_mut (string)
        return f->prim == t->prim;
    default:
        return 0;
    }
}

/* Equality/identity admission modulo one implicit shrink (Mutability #9). */
static int same_modulo_shrink(SType* a, SType* b) {
    return same_type(a, b) || shrink_ok(a, b) || shrink_ok(b, a);
}

static int same_param_signature(SType* a, Node* ad, SType* b, Node* bd) {
    if (!a || !b || a->kind != SK_FUNC || b->kind != SK_FUNC || a->nparams != b->nparams) return 0;
    for (int i = 0; i < a->nparams; i++) {
        if (!same_type(a->params[i], b->params[i])) return 0;
        int am = ad && i < ad->nkids && (ad->kids[i]->flags & F_MUT);
        int bm = bd && i < bd->nkids && (bd->kids[i]->flags & F_MUT);
        if (am != bm) return 0;
    }
    return 1;
}

static int overload_count(Sym* head) {
    int n = 0;
    for (Sym* s = head; s; s = s->overload) n++;
    return n;
}

static int castable(SType* f, SType* t) {
    if (f->kind == SK_ERR || t->kind == SK_ERR) return 1;
    if (same_type(f, t)) return 1;
    if (shrink_ok(f, t)) return 1; // mut view casts to its plain form
    if (f->kind == SK_BITS) return same_type(f->base, t);
    if (t->kind == SK_BITS) return same_type(f, t->base);
    if (f == T_UT_INT) return is_num(t) || t->kind == SK_ENUM || (t->kind == SK_DISTINCT && castable(f, t->base));
    if (f == T_UT_FLOAT) return is_float(t) || (t->kind == SK_DISTINCT && castable(f, t->base));
    if (t->kind == SK_DISTINCT) return castable(f, t->base);
    if (f->kind == SK_DISTINCT) return castable(f->base, t);
    if (f->kind == SK_ENUM) return is_int(t);
    if (t->kind == SK_ENUM) return is_int(f);
    if (f->kind == SK_PTR && t->kind == SK_PTR) return 1;
    // string/[]char/[]u8 aliases share a descriptor; fixed arrays still reach
    // that view only through explicit slicing.
    if (t->kind == SK_PRIM && t->prim == P_STRING && f->kind == SK_SLICE && f->base == st_prim(P_U8)) return 1;
    return is_num(f) && is_num(t);
}

// --- sequence constant bounds ---------------------------------------------------------

/*
 * Length of a sliced sequence when constant-provable: string literals, array
 * literals, array-typed bases, and const globals initialized directly from a
 * string literal; -1 when the length is a runtime value.
 */
static int64_t seq_const_len(Ck* k, Node* base, SType* t) {
    if (base->kind == N_STR) return (int64_t)base->len;
    if (base->kind == N_ARRAYLIT) return base->a ? (int64_t)base->ival : (int64_t)base->nkids;
    if (t->kind == SK_ARRAY) return (int64_t)t->count;
    if (base->kind == N_IDENT) {
        Sym* s = lookup_val(k, base->str, base->len);
        if (s && s->is_const && s->is_global && s->type == st_prim(P_STRING) &&
            s->init && s->init->kind == N_STR)
            return (int64_t)s->init->len;
    }
    return -1;
}

/*
 * One slice bound's compile-time value when constant-provable: untyped
 * constant expressions and constant `sequence.len` properties whose length is
 * itself provable. Returns 0 when the bound cannot be known at compile time.
 */
static int bound_const(Node* n, int64_t* out) {
    if (n->kind == N_FIELD_E && n->a && n->len == 3 && memcmp(n->str, "len", 3) == 0) {
        if (n->a->kind == N_STR) {
            *out = (int64_t)n->a->len;
            return 1;
        }
        if (n->ival) { *out = (int64_t)n->ival; return 1; }
        return 0;
    }
    return ut_const(n, out);
}

/*
 * Sequences #2: the bounds precondition 0 <= lo <= hi <= source.len is a
 * compile-time error when constants prove a violation. Unknown lengths and
 * runtime bounds are left to checked builds.
 */
static void check_slice_bounds(Ck* k, Node* e, Node* base, SType* bt) {
    enum { BP_LO_NONNEG = 1, BP_HI_NONNEG = 2, BP_ORDER = 4, BP_SOURCE = 8 };
    int64_t slen = seq_const_len(k, base, bt);
    int64_t lo = 0, hi = 0;
    int klo = e->b ? bound_const(e->b, &lo) : 1;
    int khi;
    if (e->c) khi = bound_const(e->c, &hi);
    else { khi = slen >= 0; hi = khi ? slen : 0; } // omitted hi is source.len
    if (klo && lo < 0) {
        ck_err(k, e->b, "slice lower bound %lld is negative", (long long)lo);
        klo = 0; // the negative bound already explains any ordering violation
    }
    if (khi && hi < 0) {
        ck_err(k, e->c, "slice upper bound %lld is negative", (long long)hi);
        khi = 0;
    }
    if (klo && khi && lo > hi)
        ck_err(k, e, "slice lower bound %lld exceeds upper bound %lld",
               (long long)lo, (long long)hi);
    if (slen >= 0) {
        if (khi && hi > slen)
            ck_err(k, e->c, "slice upper bound %lld exceeds source length %lld",
                   (long long)hi, (long long)slen);
        else if (klo && lo > slen)
            ck_err(k, e->b, "slice lower bound %lld exceeds source length %lld",
                   (long long)lo, (long long)slen);
    }
    uint8_t proof = 0;
    if (klo && lo >= 0) proof |= BP_LO_NONNEG;
    if (khi && hi >= 0) proof |= BP_HI_NONNEG;
    if (klo && khi && lo <= hi) proof |= BP_ORDER;
    if (khi && slen >= 0 && hi <= slen) proof |= BP_SOURCE;
    uint8_t* stored_proof = (uint8_t*)arena_alloc(1);
    *stored_proof = proof;
    e->aux = stored_proof;
    if (proof == (BP_LO_NONNEG | BP_HI_NONNEG | BP_ORDER | BP_SOURCE)) e->flags |= F_BOUNDS_PROVEN;
}

/*
 * Sequences #11: an indexed write follows the slice-write bounds contract —
 * a compile-time error when constants prove the index out of range, a
 * checked-build diagnostic otherwise, and release assumes it.
 */
static void check_index_bounds(Ck* k, Node* e, Node* base, SType* bt, int write) {
    int64_t ix;
    if (!e->b || !bound_const(e->b, &ix)) return;
    if (ix < 0) {
        ck_err(k, e->b, "%sindex %lld is negative", write ? "write " : "", (long long)ix);
        return;
    }
    int64_t slen = seq_const_len(k, base, bt);
    if (slen >= 0 && ix >= slen)
        ck_err(k, e->b, "%sindex %lld exceeds source length %lld",
               write ? "write " : "", (long long)ix, (long long)slen);
    else if (slen >= 0) e->flags |= F_BOUNDS_PROVEN;
}

// --- forward decls -------------------------------------------------------------------

static SType* expr_type(Ck* k, Node* e);
static uint32_t type_identity(SType* t);
static int coerce(Ck* k, Node* e, SType* want);
static SType* check_call(Ck* k, Node* call);
static void check_stmt(Ck* k, Node* s);
static void check_block(Ck* k, Node* b);
static void check_structlit(Ck* k, Node* lit, SType* want);
static void check_arraylit(Ck* k, Node* lit, SType* want);
static SType* check_lvalue(Ck* k, Node* e, int write);
static void check_func_body(Ck* k, Node* f);
static void check_mut_args(Ck* k, Node* call, Node* fndecl);
static Node* static_string_expr(Ck* k, Node* e);

/*
 * The three byte-sequence representations participate in the explicit
 * mem.equal operation. Transparent aliases already resolve to the same SType;
 * distinct wrappers deliberately do not enter this structural category.
 */
static int is_byte_sequence(SType* t) {
    if (!t) return 0;
    if (t->kind == SK_PRIM) return t->prim == P_STRING;
    return (t->kind == SK_ARRAY || t->kind == SK_SLICE) && t->base && t->base->kind == SK_PRIM && t->base->prim == P_U8;
}

// --- tagged unions -----------------------------------------------------------------
/*
 * A union is a closed inline sum: i32 tag at offset 0, inline storage for the
 * largest payload. Construction names the union and variant; inspection uses
 * the existing no-fallthrough switch; payload access exists only as an arm
 * binder (a fresh local copy). The numeric tag is not exposed.
 */

static SType* indexed_named_type(Ck* k, Node* ix);

/*
 * Union equality compares the tag and active payload. It exists only when
 * every payload type supports equality; sequential payloads do not.
 */
static int union_supports_equality(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    if (!t) return 0;
    switch (t->kind) {
    case SK_PRIM:
        return t->prim != P_STRING;
    case SK_PTR: case SK_FUNC: case SK_ENUM: case SK_BITS:
    case SK_CONTRACT: case SK_ERROR:
        return 1;
    case SK_ARRAY: case SK_SLICE:
        return 0;
    case SK_UNION:
        for (int i = 0; i < t->nfields; i++) if (t->fields[i].t && !union_supports_equality(t->fields[i].t)) return 0;
        return 1;
    default:
        return 0;
    }
}

/*
 * The union type named by a construction or pattern base — `Event` or
 * `Pair[int]` — or NULL when the base is not a union type. T_ERR means the
 * base names a generic union without its explicit type arguments.
 */
static SType* union_base_type(Ck* k, Node* base) {
    SType* t = NULL;
    if (base->kind == N_IDENT ||
        (base->kind == N_FIELD_E && !base->a)) { // pattern parser's plain base
        if (lookup_val(k, base->str, base->len)) return NULL; // a value shadows
        Sym* ts = lookup_type(k, base->str, base->len);
        if (!ts || !ts->type) return NULL;
        if (ts->is_generic) {
            ck_err(k, base, "generic type '%.*s' needs explicit type arguments", (int)base->len, base->str);
            return T_ERR;
        }
        t = ts->type;
        base->sym = ts; // emitter: union type symbol on the base
    } else if (base->kind == N_INDEX) {
        t = indexed_named_type(k, base);
        if (!t || t->kind == SK_ERR) return NULL;
    }
    if (!t) return NULL;
    base->ty = t;
    return t->kind == SK_UNION ? t : NULL;
}

static int union_variant_index(Ck* k, Node* at, SType* u, Node* name) {
    for (int i = 0; i < u->nfields; i++)
        if ((uint32_t)strlen(u->fields[i].name) == name->len &&
            memcmp(u->fields[i].name, name->str, name->len) == 0)
            return i;
    ck_err(k, at, "union %s has no variant '%.*s'", type_name(u), (int)name->len, name->str);
    return -1;
}

/*
 * Coerce a variant payload argument. Composite payloads take their shape
 * from the payload type exactly like variable initializers: tuple literals
 * coerce element-wise (materializing untyped literals), bare struct and
 * array literals type from the payload.
 */
static void coerce_payload(Ck* k, Node* arg, SType* pt) {
    if (pt->kind == SK_TUPLE && arg->kind == N_TUPLE_E && arg->nkids == pt->nfields) {
        for (int i = 0; i < arg->nkids; i++) coerce(k, arg->kids[i], pt->fields[i].t);
        arg->ty = pt; // emitter stores members at these widths
        return;
    }
    if (arg->kind == N_STRUCTLIT && !arg->a && (pt->kind == SK_STRUCT || pt->kind == SK_BITS)) {
        check_structlit(k, arg, pt);
        return;
    }
    if (arg->kind == N_ARRAYLIT && pt->kind == SK_ARRAY) {
        check_arraylit(k, arg, pt);
        return;
    }
    coerce(k, arg, pt);
}


// --- untyped int constant evaluation ----------------------------------------------

static int ut_const(Node* e, int64_t* out) {
    switch (e->kind) {
    case N_INT: case N_CHAR: *out = (int64_t)e->ival; return 1;
    case N_IDENT: {
        Sym* s = node_sym(e);
        return s && s->is_const && s->init ? ut_const(s->init, out) : 0;
    }
    case N_CALL:
        if (e->a && e->a->kind == N_IDENT && !e->sym &&
            ((e->a->len == 6 && memcmp(e->a->str, "sizeof", 6) == 0) ||
             (e->a->len == 7 && memcmp(e->a->str, "alignof", 7) == 0) ||
             (e->a->len == 8 && memcmp(e->a->str, "offsetof", 8) == 0))) {
            *out = (int64_t)e->ival;
            return 1;
        }
        return 0;
    case N_FIELD_E: {
        if (e->len == 3 && memcmp(e->str, "len", 3) == 0 && e->a) {
            SType* b = node_ty(e->a);
            if (b && b->kind == SK_ARRAY) { *out = (int64_t)b->count; return 1; }
            if (e->a->kind == N_STR) { *out = (int64_t)e->a->len; return 1; }
        }
        Sym* s = node_sym(e);
        return s && s->is_const && s->init ? ut_const(s->init, out) : 0;
    }
    case N_UNARY:
        if (e->op == T_MINUS) {
            int64_t v;
            if (!ut_const(e->a, &v)) return 0;
            *out = (int64_t)(0 - (uint64_t)v); // unsigned negate: defined at i64 min
            return 1;
        }
        if (e->op == T_TILDE) {
            int64_t v;
            if (!ut_const(e->a, &v)) return 0;
            *out = ~v;
            return 1;
        }
        return 0;
    case N_BINARY: {
        int64_t a, b;
        if (!ut_const(e->a, &a) || !ut_const(e->b, &b)) return 0;
        switch (e->op) {
        case T_PLUS: *out = (int64_t)((uint64_t)a + (uint64_t)b); return 1;
        case T_MINUS: *out = (int64_t)((uint64_t)a - (uint64_t)b); return 1;
        case T_STAR: *out = (int64_t)((uint64_t)a * (uint64_t)b); return 1;
        case T_SLASH:
            if (!b || (a == INT64_MIN && b == -1)) return 0;
            *out = a / b; return 1;
        case T_PERCENT:
            if (!b || (a == INT64_MIN && b == -1)) return 0;
            *out = a % b; return 1;
        case T_SHL: *out = (int64_t)((uint64_t)a << ((uint64_t)b & 63)); return 1;
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

/* Exact nonnegative constants used by pointer offsets. Keep the surface narrow:
 * literals, const aliases, and an explicit usize conversion around either. */
static int usize_const(Node* e, uint64_t* out) {
    if (!e) return 0;
    if (e->kind == N_INT || e->kind == N_CHAR) {
        *out = e->ival;
        return 1;
    }
    if (e->kind == N_IDENT) {
        Sym* s = node_sym(e);
        return s && s->is_const && s->init ? usize_const(s->init, out) : 0;
    }
    if (e->kind == N_CALL && e->a && e->a->kind == N_IDENT &&
        e->a->len == 5 && memcmp(e->a->str, "usize", 5) == 0 &&
        e->nkids == 1)
        return usize_const(e->kids[0], out);
    return 0;
}

/* Exact machine addresses visible at a local construction site. */
static int address_const(Node* e, uint64_t* out) {
    if (!e) return 0;
    if (e->kind == N_NULL) { *out = 0; return 1; }
    if (e->kind == N_CALL && e->a && e->a->kind == N_PTR && e->nkids == 1) return usize_const(e->kids[0], out);
    return 0;
}

static int bool_const(Node* e, int* out) {
    if (!e) return 0;
    if (e->kind == N_BOOL) { *out = e->ival != 0; return 1; }
    if (e->kind == N_IDENT) {
        Sym* s = node_sym(e);
        return s && s->is_const && s->init ? bool_const(s->init, out) : 0;
    }
    if (e->kind == N_UNARY && e->op == T_BANG) {
        int v;
        if (!bool_const(e->a, &v)) return 0;
        *out = !v;
        return 1;
    }
    if (e->kind == N_BINARY && (e->op == T_ANDAND || e->op == T_OROR)) {
        int a, b;
        if (!bool_const(e->a, &a) || !bool_const(e->b, &b)) return 0;
        *out = e->op == T_ANDAND ? a && b : a || b;
        return 1;
    }
    if (e->kind == N_BINARY) {
        int64_t a, b;
        if (!ut_const(e->a, &a) || !ut_const(e->b, &b)) return 0;
        switch (e->op) {
        case T_EQEQ: *out = a == b; return 1;
        case T_BANGEQ: *out = a != b; return 1;
        case T_LT: *out = a < b; return 1;
        case T_LE: *out = a <= b; return 1;
        case T_GT: *out = a > b; return 1;
        case T_GE: *out = a >= b; return 1;
        default: return 0;
        }
    }
    if (e->kind == N_CMPCHAIN && e->a) { // a < b <= c: every adjacent pair holds
        int all = 1;
        int64_t prev, next;
        if (!ut_const(e->kids[0], &prev)) return 0;
        for (int i = 1; i < e->nkids; i++, prev = next) {
            if (!ut_const(e->kids[i], &next)) return 0;
            switch (e->a->kids[i - 1]->op) {
            case T_LT: all &= prev < next; break;
            case T_LE: all &= prev <= next; break;
            case T_GT: all &= prev > next; break;
            case T_GE: all &= prev >= next; break;
            default: return 0;
            }
        }
        *out = all;
        return 1;
    }
    return 0;
}

/* Struct defaults use the language's existing compile-time expression surface.
 * This is intentionally structural: semantic resolution has already separated
 * const names, enum/union members, casts, and builtins from ordinary calls. */
static int compile_time_expr(Node* e) {
    if (!e) return 0;
    switch (e->kind) {
    case N_INT: case N_FLOAT: case N_CHAR: case N_BOOL: case N_STR: case N_NULL:
        return 1;
    case N_IDENT: {
        Sym* s = node_sym(e);
        return s && s->is_const && s->is_global && s->init && compile_time_expr(s->init);
    }
    case N_FIELD_E: {
        if (e->len == 3 && memcmp(e->str, "len", 3) == 0 && e->a) {
            SType* b = node_ty(e->a);
            if ((b && b->kind == SK_ARRAY) || e->a->kind == N_STR) return 1;
        }
        Sym* s = e->a ? node_sym(e->a) : NULL;
        return s && s->kind == SYM_TYPE;
    }
    case N_UNARY:
        return compile_time_expr(e->a);
    case N_BINARY:
        return compile_time_expr(e->a) && compile_time_expr(e->b);
    case N_CMPCHAIN:
        for (int i = 0; i < e->nkids; i++) if (!compile_time_expr(e->kids[i])) return 0;
        return 1;
    case N_TUPLE_E:
        for (int i = 0; i < e->nkids; i++) if (!compile_time_expr(e->kids[i])) return 0;
        return 1;
    case N_ARRAYLIT:
        if (e->a) return compile_time_expr(e->a);
        for (int i = 0; i < e->nkids; i++) if (!compile_time_expr(e->kids[i])) return 0;
        return 1;
    case N_STRUCTLIT:
        for (int i = 0; i < e->nkids; i++) if (!compile_time_expr(e->kids[i]->a)) return 0;
        return 1;
    case N_CALL:
        if (e->sym) return 0; // resolved ordinary function call
        if (e->a && e->a->kind == N_IDENT && e->a->len == 6 && memcmp(e->a->str, "typeid", 6) == 0 && e->ival) return 1;
        for (int i = 0; i < e->nkids; i++) if (!compile_time_expr(e->kids[i])) return 0;
        return 1; // type cast, layout/type builtin, or union construction
    default:
        return 0;
    }
}

static int static_key_int(Node* e, uint64_t* out) {
    if (!e) return 0;
    if (e->kind == N_BOOL) { *out = e->ival != 0; return 1; }
    if (e->kind == N_FIELD_E) {
        Sym* ts = e->a ? node_sym(e->a) : NULL;
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ENUM) {
            *out = e->ival;
            return 1;
        }
    }
    if (e->kind == N_CALL && !e->sym) {
        if (e->a && e->a->kind == N_IDENT && e->a->len == 6 && memcmp(e->a->str, "typeid", 6) == 0 && e->ival) {
            *out = e->ival;
            return 1;
        }
        if (e->nkids == 1) return static_key_int(e->kids[0], out);
    }
    if (e->kind == N_IDENT) {
        Sym* s = node_sym(e);
        if (s && s->is_const && s->is_global && s->init) return static_key_int(s->init, out);
    }
    int64_t v;
    if (!ut_const(e, &v)) return 0;
    *out = (uint64_t)v;
    return 1;
}

static Node* static_key_string(Node* e) {
    if (!e) return NULL;
    if (e->kind == N_STR) return e;
    if (e->kind == N_IDENT) {
        Sym* s = node_sym(e);
        if (s && s->is_const && s->is_global && s->init) return static_key_string(s->init);
    }
    if (e->kind == N_CALL && !e->sym && e->nkids == 1) return static_key_string(e->kids[0]);
    return NULL;
}

static int staticmap_key_type(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    if (!t) return 0;
    if (t->kind == SK_ENUM) return 1;
    if (t->kind != SK_PRIM) return 0;
    return t->prim == P_BOOL ||
           (t->prim >= P_I8 && t->prim <= P_U64) ||
           t->prim == P_STRING || t->prim == P_USIZE || t->prim == P_TYPEID;
}

static void check_staticmap(Ck* k, Node* lit, SType* map) {
    if (!lit || lit->kind != N_STATICMAPLIT) {
        ck_err(k, lit, "StaticMap requires a brace literal of key: value entries");
        return;
    }
    if (!staticmap_key_type(map->base)) ck_err(k, lit, "%s is not an allowed StaticMap key type", type_name(map->base));
    if (!layout(map->ret)) ck_err(k, lit, "StaticMap value type %s must have a complete layout", type_name(map->ret));
    SType* key = map->base;
    while (key && key->kind == SK_DISTINCT) key = key->base;
    int strings = key && key->kind == SK_PRIM && key->prim == P_STRING;
    for (int i = 0; i < lit->nkids; i++) {
        Node* entry = lit->kids[i];
        coerce(k, entry->a, map->base);
        if (entry->b->kind == N_STRUCTLIT && !entry->b->a &&
            (map->ret->kind == SK_STRUCT || map->ret->kind == SK_BITS))
            check_structlit(k, entry->b, map->ret);
        else if (entry->b->kind == N_ARRAYLIT && map->ret->kind == SK_ARRAY) check_arraylit(k, entry->b, map->ret);
        else coerce(k, entry->b, map->ret);
        if (!compile_time_expr(entry->a)) ck_err(k, entry->a, "StaticMap key must be a global constant expression");
        if (!compile_time_expr(entry->b)) ck_err(k, entry->b, "StaticMap value must be a global constant expression");
        uint64_t bits = 0;
        Node* text = strings ? static_key_string(entry->a) : NULL;
        if (strings ? !text : !static_key_int(entry->a, &bits)) {
            ck_err(k, entry->a, "StaticMap key must be reducible at compile time");
            continue;
        }
        entry->ival = bits;
        for (int j = 0; j < i; j++) {
            Node* prev = lit->kids[j];
            int equal = 0;
            if (strings) {
                Node* ptext = static_key_string(prev->a);
                equal = ptext && text && ptext->len == text->len && memcmp(ptext->str, text->str, text->len) == 0;
            } else {
                equal = prev->ival == bits;
            }
            if (equal) {
                ck_err(k, entry->a, "duplicate StaticMap key");
                break;
            }
        }
    }
}

static Node* clone_expr(Node* n) {
    if (!n) return NULL;
    Node* q = (Node*)arena_alloc(sizeof(Node));
    *q = *n;
    q->a = clone_expr(n->a); q->b = clone_expr(n->b);
    q->c = clone_expr(n->c); q->d = clone_expr(n->d);
    if (n->nkids) {
        q->kids = (Node**)arena_alloc((size_t)n->nkids * sizeof(Node*));
        for (int i = 0; i < n->nkids; i++) q->kids[i] = clone_expr(n->kids[i]);
    }
    return q;
}

// --- type building ------------------------------------------------------------------

static SType* build_type(Ck* k, Node* t);
static SType* build_type_ctx(Ck* k, Node* t, int path);
enum { CABI_DIRECT, CABI_FIELD, CABI_POINTEE, CABI_POINTEE_FIELD };
static int is_c_abi_type(SType* t, int pos, SType** stack, int nstack);

static SType* prim_by_name(const char* s, uint32_t len) {
    struct { const char* n; int p; } M[] = {
        {"bool", P_BOOL}, {"i8", P_I8}, {"i16", P_I16}, {"i32", P_I32},
        {"i64", P_I64}, {"u8", P_U8}, {"u16", P_U16}, {"u32", P_U32},
        {"u64", P_U64}, {"f32", P_F32}, {"f64", P_F64}, {"float", P_F64},
        {"string", P_STRING},
        {"usize", P_USIZE}, {"typeid", P_TYPEID},
        {"int", P_I64}, {"uint", P_U64}, // platform word on arm64
        {"char", P_U8}, // transparent alias: shared identity with u8 (Stdlib #15)
    };
    for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
        if ((uint32_t)strlen(M[i].n) == len && memcmp(M[i].n, s, len) == 0)
            return st_prim(M[i].p);
    return NULL;
}

static SType* instantiate_generic_type(Ck* k, Node* decl, SType** args, int nargs);
static SType* indexed_named_type(Ck* k, Node* ix);

static SType* build_named_ctx(Ck* k, Node* t, int path) {
    if (t->flags & F_MUT) { // `mut string` — the only mut-qualified named type
        if (t->a || t->nkids || !(t->len == 6 && memcmp(t->str, "string", 6) == 0)) {
            ck_err(k, t,
                   "'mut' marks writable views ('mut *T', 'mut []T', 'mut string'), not '%.*s'",
                   (int)t->len, t->str);
            return T_ERR;
        }
        return T_MUT_STRING;
    }
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
        if (path == 1) {
            s = forward_type(k, t);
            return s->type;
        }
        ck_err(k, t, "unknown type '%.*s'", (int)t->len, t->str);
        return T_ERR;
    }
    if (s->is_forward) {
        if (path == 1) return s->type;
        ck_err(k, t,
               "type '%.*s' is not declared yet; only a pointer chain or a root-level slice field reference may be fixed up",
               (int)t->len, t->str);
        return T_ERR;
    }
    if (t->nkids > 0) { // generic instantiation in type position: Vec[int]
        SType** args = (SType**)arena_alloc((size_t)t->nkids * sizeof(SType*));
        for (int i = 0; i < t->nkids; i++) args[i] = build_type(k, t->kids[i]);
        return instantiate_generic_type(k, s->type->decl, args, t->nkids);
    }
    if (s->is_generic) {
        ck_err(k, t, "generic type '%.*s' needs explicit type arguments", (int)t->len, t->str);
        return T_ERR;
    }
    return s->type;
}

/* path: 0 = ordinary/layout-dependent, 2 = aggregate field root,
 * 1 = every edge from that root so far is a pointer. */
static SType* build_type_ctx(Ck* k, Node* t, int path) {
    switch (t->kind) {
    case N_NAMED: return build_named_ctx(k, t, path);
    case N_IDENT: { // typed struct-literal bases and call type arguments
        if (k->nsub) { // expression-position type name: substitute like N_NAMED
            for (int i = 0; i < k->nsub; i++)
                if ((uint32_t)strlen(k->tnames[i]) == t->len &&
                    memcmp(k->tnames[i], t->str, t->len) == 0)
                    return k->ttypes[i];
        }
        SType* p = prim_by_name(t->str, t->len);
        if (p) return p;
        Sym* s = lookup_type(k, t->str, t->len);
        if (!s || !s->type) {
            if (path == 1) {
                s = forward_type(k, t);
                return s->type;
            }
            ck_err(k, t, "unknown type '%.*s'", (int)t->len, t->str);
            return T_ERR;
        }
        if (s->is_forward) {
            if (path == 1) return s->type;
            ck_err(k, t,
                   "type '%.*s' is not declared yet; only a pointer chain or a root-level slice field reference may be fixed up",
                   (int)t->len, t->str);
            return T_ERR;
        }
        if (s->is_generic) {
            ck_err(k, t, "generic type '%.*s' needs explicit type arguments", (int)t->len, t->str);
            return T_ERR;
        }
        return s->type;
    }
    case N_INDEX: { // expression-position generic instantiation: Vec[int]
        if (!t->a || t->a->kind != N_IDENT) {
            ck_err(k, t, "expected a type");
            return T_ERR;
        }
        SType* it = indexed_named_type(k, t);
        if (it) return it;
        Sym* ts = lookup_type(k, t->a->str, t->a->len);
        if (!ts || !ts->type) ck_err(k, t->a, "unknown type '%.*s'", (int)t->a->len, t->a->str);
        else if (ts->is_generic)
            ck_err(k, t, "generic type '%.*s' expects %d type argument%s",
                   (int)t->a->len, t->a->str,
                   ts->type->decl && ts->type->decl->c ? ts->type->decl->c->nkids : 0,
                   ts->type->decl && ts->type->decl->c &&
                   ts->type->decl->c->nkids == 1 ? "" : "s");
        else ck_err(k, t->a, "type '%.*s' is not generic", (int)t->a->len, t->a->str);
        return T_ERR;
    }
    case N_PTR: {
        SType* p = st_new(SK_PTR);
        p->is_mut = (t->flags & F_MUT) != 0;
        int child_path = (path == 1 || path == 2) ? 1 : 0;
        p->base = build_type_ctx(k, t->a, child_path);
        if (p->base && p->base->kind == SK_FORWARD) {
            Sym* s = lookup_type(k, p->base->name, (uint32_t)strlen(p->base->name));
            if (s && s->is_forward) record_type_fix(k, s, &p->base, t->a);
        }
        return p;
    }
    case N_SLICE_T: {
        SType* s = st_new(SK_SLICE);
        s->is_mut = (t->flags & F_MUT) != 0; // mut []T — writable elements
        // A slice's own representation (pointer + count) doesn't depend on
        // its element's size, exactly like a pointer's — so a bare slice at
        // the aggregate field/variant root may also forward-reference a
        // type declared later in the module. Narrower than the pointer
        // case on purpose: only the direct root ([]Later), not a slice
        // reached through a preceding pointer or nested further.
        int child_path = (path == 2) ? 1 : 0;
        s->base = build_type_ctx(k, t->a, child_path);
        if (s->base && s->base->kind == SK_FORWARD) {
            Sym* sym = lookup_type(k, s->base->name, (uint32_t)strlen(s->base->name));
            if (sym && sym->is_forward) record_type_fix(k, sym, &s->base, t->a);
        }
        return s;
    }
    case N_ARRAY_T: {
        coerce(k, t->b, st_prim(P_USIZE));
        int64_t len;
        if (!ut_const(t->b, &len)) {
            ck_err(k, t->b, "array length must be a compile-time usize expression");
            return T_ERR;
        }
        if (len <= 0) {
            ck_err(k, t->b, "array length must be positive");
            return T_ERR;
        }
        SType* a = st_new(SK_ARRAY);
        a->count = (uint64_t)len;
        a->base = build_type_ctx(k, t->a, 0);
        return a;
    }
    case N_STATICMAP_T: {
        SType* m = st_new(SK_STATICMAP);
        m->base = build_type_ctx(k, t->a, 0);
        m->ret = build_type_ctx(k, t->b, 0);
        return m;
    }
    case N_CONST_T: return build_type_ctx(k, t->a, 0);
    case N_TUPLE_T: {
        SType* tu = st_new(SK_TUPLE);
        tu->nfields = t->nkids;
        tu->fields = (SField*)arena_alloc((size_t)(t->nkids ? t->nkids : 1) * sizeof(SField));
        memset(tu->fields, 0, (size_t)(t->nkids ? t->nkids : 1) * sizeof(SField));
        for (int i = 0; i < t->nkids; i++) {
            tu->fields[i].name = NULL;
            tu->fields[i].t = build_type_ctx(k, t->kids[i], 0);
        }
        return tu;
    }
    case N_FUNC_T: {
        SType* f = st_new(SK_FUNC);
        f->is_c_abi = (t->flags & F_CABI) != 0;
        f->nparams = t->nkids;
        f->params = (SType**)arena_alloc((size_t)(t->nkids ? t->nkids : 1) * sizeof(SType*));
        for (int i = 0; i < t->nkids; i++) f->params[i] = build_type_ctx(k, t->kids[i], 0);
        f->ret = t->a ? build_type_ctx(k, t->a, 0) : NULL;
        if (f->is_c_abi) {
            SType* stack[512];
            for (int i = 0; i < f->nparams; i++)
                if (f->params[i]->kind != SK_ERR &&
                    !is_c_abi_type(f->params[i], CABI_DIRECT, stack, 0))
                    ck_err(k, t->kids[i],
                           "C function-pointer parameter type %s is not C-ABI compatible",
                           type_name(f->params[i]));
            if (f->ret && f->ret->kind != SK_ERR &&
                !is_c_abi_type(f->ret, CABI_DIRECT, stack, 0))
                ck_err(k, t->a,
                       "C function-pointer return type %s is not C-ABI compatible",
                       type_name(f->ret));
        }
        return f;
    }
    default:
        ck_err(k, t, "expected a type");
        return T_ERR;
    }
}

static SType* build_type(Ck* k, Node* t) { return build_type_ctx(k, t, 0); }

// Build a function signature. Generic declarations use canonical type-parameter
// placeholders when args is NULL, making alpha-equivalent declarations such as
// f[T](T) and f[U](U) comparable for duplicate detection.
static SType* build_func_type(Ck* k, Node* f, SType** args, int nargs) {
    const char** saved_names = k->tnames;
    SType** saved_types = k->ttypes;
    int saved_nsub = k->nsub;
    if (f->c) {
        int nt = f->c->nkids;
        k->tnames = (const char**)arena_alloc((size_t)(nt ? nt : 1) * sizeof(char*));
        k->ttypes = (SType**)arena_alloc((size_t)(nt ? nt : 1) * sizeof(SType*));
        k->nsub = nt;
        for (int i = 0; i < nt; i++) {
            k->tnames[i] = f->c->kids[i]->str;
            if (args && i < nargs) {
                k->ttypes[i] = args[i];
            } else {
                SType* p = st_new(SK_TYPEPARAM);
                char name[32];
                snprintf(name, sizeof name, "$%d", i);
                size_t len = strlen(name);
                char* stable = (char*)arena_alloc(len + 1);
                memcpy(stable, name, len + 1);
                p->name = stable;
                k->ttypes[i] = p;
            }
        }
    }
    SType* ft = st_new(SK_FUNC);
    ft->nparams = f->nkids;
    ft->params = (SType**)arena_alloc((size_t)(f->nkids ? f->nkids : 1) * sizeof(SType*));
    for (int i = 0; i < f->nkids; i++) {
        ft->params[i] = build_type(k, f->kids[i]->a);
        if (ft->params[i] && ft->params[i]->kind == SK_ERROR_DOMAIN)
            ck_err(k, f->kids[i], "error-domain names are identities, not parameter types; use Error");
    }
    ft->ret = f->a ? build_type(k, f->a) : NULL;
    if (ft->ret && ft->ret->kind == SK_ERROR_DOMAIN)
        ck_err(k, f->a, "error-domain names are identities, not return types; use Error");
    k->tnames = saved_names;
    k->ttypes = saved_types;
    k->nsub = saved_nsub;
    return ft;
}

static const char* method_key(Node* owner, const char* name, uint32_t len, uint32_t* out_len) {
    size_t olen = owner ? owner->len : 0;
    char* key = arena_alloc(olen + len + 2);
    if (olen) memcpy(key, owner->str, olen);
    key[olen] = '$';
    memcpy(key + olen + 1, name, len);
    key[olen + len + 1] = 0;
    *out_len = (uint32_t)(olen + len + 1);
    return key;
}

/*
 * C-ABI whitelist (C Interop #3, chunk 1B decision): positive and
 * position-aware. CABI_DIRECT governs boundary parameters and returns;
 * CABI_FIELD governs struct members; CABI_POINTEE governs pointer targets.
 * Arrays are legal only nested in aggregates or behind pointers. The stack
 * carries the in-progress nominal structs: a repeat hit is a cycle, which can
 * only arise through a pointer, so it is compatible and recursion terminates.
 */
static int has_special_struct_layout(SType* t) {
    if (!t || t->kind != SK_STRUCT) return 0;
    if (t->is_packed || t->requested_align) return 1;
    for (int i = 0; i < t->nfields; i++) if (t->fields[i].requested_align) return 1;
    return 0;
}

static int is_c_abi_type(SType* t, int pos, SType** stack, int nstack) {
    if (!t) return 1; // no-value shape: legal only as a return
    switch (t->kind) {
    case SK_ERR:
        return 1; // already-diagnosed type; do not cascade
    case SK_PRIM:
        switch (t->prim) {
        case P_I8: case P_I16: case P_I32: case P_I64:
        case P_U8: case P_U16: case P_U32: case P_U64:
        case P_F32: case P_F64: case P_USIZE:
            return 1;
        default:
            return 0; // bool, string, typeid
        }
    case SK_PTR:
        return is_c_abi_type(t->base, CABI_POINTEE, stack, nstack);
    case SK_DISTINCT:
        return is_c_abi_type(t->base, pos, stack, nstack);
    case SK_ENUM:
        return t->decl && t->decl->a && is_c_abi_type(t->base, pos, stack, nstack);
    case SK_ARRAY:
        return pos != CABI_DIRECT && is_c_abi_type(t->base, pos, stack, nstack);
    case SK_FUNC:
        if (!t->is_c_abi || pos != CABI_DIRECT) return 0;
        for (int i = 0; i < t->nparams; i++) if (!is_c_abi_type(t->params[i], CABI_DIRECT, stack, nstack)) return 0;
        return is_c_abi_type(t->ret, CABI_DIRECT, stack, nstack);
    case SK_STRUCT: {
        if (has_special_struct_layout(t) &&
            pos != CABI_POINTEE && pos != CABI_POINTEE_FIELD)
            return 0; // special-layout aggregates are pointer-only initially
        if (t->nargs || (t->decl && t->decl->c)) return 0; // generic shapes
        for (int i = 0; i < nstack; i++)
            if (stack[i] == t) return 1; // cycle through a pointer
        if (nstack >= 512) return 0;
        stack[nstack] = t;
        int field_pos = pos == CABI_POINTEE || pos == CABI_POINTEE_FIELD ? CABI_POINTEE_FIELD : CABI_FIELD;
        for (int i = 0; i < t->nfields; i++) if (!is_c_abi_type(t->fields[i].t, field_pos, stack, nstack + 1)) return 0;
        return 1;
    }
    default:
        // slices, tuples (multi-return), function values, default enums, contracts,
        // tagged unions, Error, error domains, typeid, type parameters, void/ut
        return 0;
    }
}

/*
 * "f[int, string]" for diagnostics — bounded ring like type_name.
 */
static const char* spec_tuple_name(SType** args, int nargs) {
    static char bufs[4][256];
    static int bi;
    char* b = bufs[bi = (bi + 1) & 3];
    size_t o = 0;
    b[o++] = '[';
    for (int i = 0; i < nargs; i++) {
        if (o + 2 >= sizeof bufs[0] - 1) break;
        if (i) {
            b[o++] = ',';
            b[o++] = ' ';
        }
        const char* tn = type_name(args[i]);
        size_t n = strlen(tn);
        if (o + n + 2 >= sizeof bufs[0] - 1) {
            b[o++] = '.';
            b[o++] = '.';
            b[o++] = '.';
            break;
        }
        memcpy(b + o, tn, n);
        o += n;
    }
    b[o++] = ']';
    b[o] = 0;
    return b;
}

// Generics #10: a specialization's parameters, 'mut' positions, and return
// must equal the substituted generic signature — checked at declaration
// against the expected signature, never discovered at a call.
static int spec_conforms(SType* sub, Node* gdecl, SType* ft, Node* fdecl) {
    if (!sub || sub->kind != SK_FUNC || !ft || ft->kind != SK_FUNC) return 0;
    if (sub->nparams != ft->nparams) return 0;
    if (!same_param_signature(sub, gdecl, ft, fdecl)) return 0;
    if ((sub->ret == NULL) != (ft->ret == NULL)) return 0;
    return !sub->ret || same_type(sub->ret, ft->ret);
}

/*
 * Specialization registration (Generics #9–12). The name already declares a
 * generic, so these brackets are specialization type arguments: bare named
 * types, supplied positionally. The specialization is an ordinary, indepen-
 * dently checked function binding to exactly one generic overload by substi-
 * tuted-signature conformance; duplicates for one exact tuple on one overload
 * are errors.
 */
static Sym* declare_specialization(Ck* k, Node* f, Sym* head) {
    if (f->flags & F_SPECIALIZED) {
        ck_err(k, f, "a specialization provides its own body — 'specialized' marks only a generic template");
        return NULL;
    }
    int nargs = f->c->nkids;
    SType** targs = (SType**)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(SType*));
    for (int i = 0; i < nargs; i++) targs[i] = build_type(k, f->c->kids[i]);
    SType* ft = build_func_type(k, f, targs, nargs);
    Sym* bind = NULL;
    int nbind = 0;
    for (Sym* old = head; old; old = old->overload) {
        if (!old->is_generic || !old->decl->c || old->decl->c->nkids != nargs)
            continue;
        SType* sub = build_func_type(k, old->decl, targs, nargs);
        if (!spec_conforms(sub, old->decl, ft, f)) continue;
        nbind++;
        bind = old;
    }
    if (!bind) {
        ck_err(k, f,
               "specialization '%.*s%s' does not conform to any generic overload of '%.*s' — parameters, 'mut' positions, and return type must equal the substituted generic signature with every type parameter supplied",
               (int)f->len, f->str, spec_tuple_name(targs, nargs),
               (int)f->len, f->str);
        return NULL;
    }
    if (nbind > 1) {
        ck_err(k, f,
               "specialization '%.*s%s' conforms to more than one generic overload of '%.*s'",
               (int)f->len, f->str, spec_tuple_name(targs, nargs),
               (int)f->len, f->str);
        return NULL;
    }
    for (Sym* sp = bind->specs; sp; sp = sp->spec_next) {
        int same = sp->spec_nargs == nargs;
        for (int i = 0; i < nargs && same; i++)
            same = same_type(sp->spec_args[i], targs[i]);
        if (same) {
            ck_err(k, f,
                   "duplicate specialization '%.*s%s' — one exact type-argument tuple per generic overload (first declared at line %d)",
                   (int)f->len, f->str, spec_tuple_name(targs, nargs),
                   sp->decl->line);
            return NULL;
        }
    }
    Sym* s = sym_new(SYM_FUNC, f->str, f->len);
    s->decl = f;
    s->type = ft;
    s->is_pub = (f->flags & F_PUB) != 0;
    s->spec_args = targs;
    s->spec_nargs = nargs;
    s->spec_next = bind->specs;
    bind->specs = s;
    f->sym = s;
    Sym* tail = head;
    while (tail->overload) tail = tail->overload;
    tail->overload = s;
    return s;
}

static Sym* declare_func(Ck* k, Node* f, int is_extern) {
    uint32_t key_len = f->len;
    const char* key = f->str;
    if (f->flags & (F_METHOD | F_ASSOC)) key = method_key(f->d, f->str, f->len, &key_len);
    else if (is_panic_name(f->str, f->len) || is_assert_name(f->str, f->len) || is_static_assert_name(f->str, f->len)) {
        ck_err(k, f, "'%.*s' is a reserved builtin name — no declaration may take it", (int)f->len, f->str);
        return NULL;
    }
    Sym* head = scope_find(k->scope, 0, key, key_len);
    if (head && head->kind != SYM_FUNC) {
        ck_err(k, f, "duplicate name '%.*s' in this scope", (int)f->len, f->str);
        return NULL;
    }
    /* A method and an associated function can never share owner+name: their
     * overload chains are resolved on separate tracks ('.' vs '::') and mixing
     * them would put a receiverless signature where method resolution expects
     * a receiver in params[0]. Reject at declaration instead of leaving one
     * side silently unreachable through the other's call syntax. */
    if (head && (f->flags & (F_METHOD | F_ASSOC)) &&
        ((head->decl->flags & F_ASSOC) != (f->flags & F_ASSOC))) {
        ck_err(k, f, (f->flags & F_ASSOC)
               ? "'%.*s' is already declared as a method on this type — associated functions and methods cannot share a name"
               : "'%.*s' is already declared as an associated function on this type — associated functions and methods cannot share a name",
               (int)f->len, f->str);
        return NULL;
    }
    int old_c_boundary = 0;
    for (Sym* old = head; old; old = old->overload)
        old_c_boundary |= old->is_extern ||
                          (old->decl && (old->decl->flags & F_EXPO));
    if (head && (is_extern || old_c_boundary || (f->flags & F_EXPO))) {
        ck_err(k, f, "C-boundary function '%.*s' cannot be overloaded", (int)f->len, f->str);
        return NULL;
    }
    /*
     * Ordered bracket interpretation (Generics #12): brackets whose iden-
     * tifiers name existing types are specialization type arguments and must
     * follow a generic declaration of this name; identifiers naming no
     * existing type start a fresh type-parameter list (a further generic
     * overload). Existing-type names with no preceding generic are a compile
     * error — never a silent new generic. Type-owned methods and associated
     * functions keep their owner-parameter shape and never specialize here.
     */
    if (!is_extern && !(f->flags & (F_EXPO | F_METHOD | F_ASSOC)) && f->c) {
        Node* typed_ident = NULL;
        for (int i = 0; i < f->c->nkids && !typed_ident; i++) {
            Node* id = f->c->kids[i];
            if (prim_by_name(id->str, id->len) || lookup_type(k, id->str, id->len)) typed_ident = id;
        }
        if (typed_ident) {
            int has_generic = 0;
            for (Sym* old = head; old; old = old->overload) if (old->is_generic) { has_generic = 1; break; }
            if (has_generic) return declare_specialization(k, f, head);
            ck_err(k, f,
                   "brackets after '%.*s' name the existing type '%.*s' — a specialization must follow a generic declaration of '%.*s'",
                   (int)f->len, f->str, (int)typed_ident->len, typed_ident->str,
                   (int)f->len, f->str);
            return NULL;
        }
    }
    if ((f->flags & F_SPECIALIZED) && !f->c) {
        ck_err(k, f, "'specialized' marks a generic template; '%.*s' declares no type parameters", (int)f->len, f->str);
        return NULL;
    }
    if (f->flags & F_INTRINSIC) {
        if (!f->ival) {
            ck_err(k, f, "'intrinsic' declarations are restricted to compiler-provided standard-library operations");
        }
        if (!(f->flags & F_PUB)) ck_err(k, f, "intrinsic declaration '%.*s' must be public", (int)f->len, f->str);
        if (f->flags & (F_EXPO | F_METHOD | F_ASSOC))
            ck_err(k, f, "intrinsic declaration '%.*s' cannot be a method, associated function, or C export",
                   (int)f->len, f->str);
        if (f->c) ck_err(k, f, "intrinsic declaration '%.*s' cannot be generic or specialized", (int)f->len, f->str);
    }
    SType* ft = build_func_type(k, f, NULL, 0);
    ft->is_c_abi = is_extern || (f->flags & F_EXPO);
    if ((f->flags & F_INTRINSIC) && f->ival) {
        SType* p = ft->nparams == 1 ? ft->params[0] : NULL;
        SType* r = ft->ret;
        int from64 = f->ival == BI_BITS_FROM_FLOAT && p && r &&
            p->kind == SK_PRIM && p->prim == P_F64 &&
            r->kind == SK_PRIM && r->prim == P_U64;
        int from32 = f->ival == BI_BITS_FROM_FLOAT && p && r &&
            p->kind == SK_PRIM && p->prim == P_F32 &&
            r->kind == SK_PRIM && r->prim == P_U32;
        int to64 = f->ival == BI_BITS_TO_FLOAT && p && r &&
            p->kind == SK_PRIM && p->prim == P_U64 &&
            r->kind == SK_PRIM && r->prim == P_F64;
        int to32 = f->ival == BI_BITS_TO_FLOAT && p && r &&
            p->kind == SK_PRIM && p->prim == P_U32 &&
            r->kind == SK_PRIM && r->prim == P_F32;
        int load64 = f->ival == BI_INTERNAL_LOAD_U64 && ft->nparams == 2 &&
            ft->params[0]->kind == SK_PRIM && ft->params[0]->prim == P_STRING &&
            ft->params[1]->kind == SK_PRIM && ft->params[1]->prim == P_USIZE &&
            r->kind == SK_PRIM && r->prim == P_U64;
        SType* ip = ft->nparams >= 1 ? ft->params[0] : NULL;
        int fixed_unsigned = ip && ip->kind == SK_PRIM &&
            (ip->prim == P_U8 || ip->prim == P_U16 ||
             ip->prim == P_U32 || ip->prim == P_U64);
        int bit_count = fixed_unsigned && ft->nparams == 1 && r &&
            r->kind == SK_PRIM && r->prim == P_U64 &&
            (f->ival == BI_BITS_POP_COUNT ||
             f->ival == BI_BITS_LEAD_ZEROS ||
             f->ival == BI_BITS_TRAIL_ZEROS);
        int bit_transform = fixed_unsigned && ft->nparams == 1 && r &&
            r->kind == SK_PRIM && r->prim == ip->prim &&
            f->ival == BI_BITS_BYTE_SWAP;
        int bit_rotate = fixed_unsigned && ft->nparams == 2 && r &&
            ft->params[1]->kind == SK_PRIM && ft->params[1]->prim == P_U64 &&
            r->kind == SK_PRIM && r->prim == ip->prim &&
            (f->ival == BI_BITS_ROTATE_LEFT ||
             f->ival == BI_BITS_ROTATE_RIGHT);
        int bit_wrap = fixed_unsigned && ft->nparams == 2 && r &&
            ft->params[1]->kind == SK_PRIM && ft->params[1]->prim == ip->prim &&
            r->kind == SK_PRIM && r->prim == ip->prim &&
            f->ival >= BI_BITS_WRAP_ADD && f->ival <= BI_BITS_WRAP_MUL;
        int math_exact = f->ival >= BI_MATH_SQRT && f->ival <= BI_MATH_CEIL &&
            ft->nparams == 1 && p && r && p->kind == SK_PRIM && r->kind == SK_PRIM &&
            p->prim == r->prim && (p->prim == P_F32 || p->prim == P_F64);
        int math_fused = f->ival == BI_MATH_FMA && ft->nparams == 3 && ip && r &&
            ip->kind == SK_PRIM && r->kind == SK_PRIM && r->prim == ip->prim &&
            (ip->prim == P_F32 || ip->prim == P_F64) &&
            ft->params[1]->kind == SK_PRIM && ft->params[1]->prim == ip->prim &&
            ft->params[2]->kind == SK_PRIM && ft->params[2]->prim == ip->prim;
        if (!(from64 || from32 || to64 || to32 || load64 || bit_count ||
              bit_transform || bit_rotate || bit_wrap || math_exact || math_fused))
            ck_err(k, f, "intrinsic declaration '%.*s' does not match a compiler-provided signature",
                   (int)f->len, f->str);
    }
    if (is_extern || (f->flags & F_EXPO)) {
        if (f->c) {
            ck_err(k, f, "C-boundary function '%.*s' cannot be generic", (int)f->len, f->str);
        } else {
            SType* stack[512];
            if (ft->ret && ft->ret->kind != SK_ERR &&
                !is_c_abi_type(ft->ret, CABI_DIRECT, stack, 0))
                ck_err(k, f->a ? f->a : f,
                       "C-boundary return type %s is not C-ABI compatible",
                       type_name(ft->ret));
            for (int i = 0; i < ft->nparams; i++) {
                if (f->kids[i]->flags & F_MUT) {
                    ck_err(k, f->kids[i],
                           "C-boundary parameter '%.*s' is 'mut' — mut parameters stay internal; pass a mut *T instead",
                           (int)f->kids[i]->len, f->kids[i]->str);
                    continue;
                }
                if (ft->params[i]->kind != SK_ERR &&
                    !is_c_abi_type(ft->params[i], CABI_DIRECT, stack, 0))
                    ck_err(k, f->kids[i]->a ? f->kids[i]->a : f->kids[i],
                           "C-boundary parameter '%.*s' has type %s, which is not C-ABI compatible",
                           (int)f->kids[i]->len, f->kids[i]->str,
                           type_name(ft->params[i]));
            }
        }
    }
    for (Sym* old = head; old; old = old->overload) {
        int old_nt = old->decl && old->decl->c ? old->decl->c->nkids : 0;
        int new_nt = f->c ? f->c->nkids : 0;
        if (old_nt == new_nt && same_param_signature(old->type, old->decl, ft, f)) {
            ck_err(k, f,
                   "duplicate overload '%.*s': parameter signatures must differ (return type is not considered)",
                   (int)f->len, f->str);
            return NULL;
        }
    }
    Sym* s = sym_new(SYM_FUNC, key, key_len);
    s->decl = f;
    s->type = ft;
    s->is_generic = f->c != NULL;
    s->is_extern = is_extern;
    s->is_pub = (f->flags & F_PUB) != 0;
    f->sym = s;
    if (!head) {
        scope_insert(k->scope, 0, s);
    } else {
        Sym* tail = head;
        while (tail->overload) tail = tail->overload;
        tail->overload = s;
    }
    return s;
}

// --- literal fit -------------------------------------------------------------------

static int ut_fits(Ck* k, Node* e, SType* want) {
    if (is_float(want)) {
        double d = e->kind == N_FLOAT ? e->fval : e->kind == N_INT ? (double)e->ival : 0.0;
        if (want->prim == P_F32 && (d > 3.402823466e38 || d < -3.402823466e38)) {
            ck_err(k, e, "float literal out of range for f32");
            return 0;
        }
        return 1;
    }
    int64_t v;
    if (!ut_const(e, &v)) return 1;
    uint64_t u = (uint64_t)v;
    if (want->kind == SK_ENUM) want = want->base ? want->base : st_prim(P_I32);
    switch (want->prim) {
    case P_I8:
        if ((e->kind == N_INT && e->ival > 127) || v < -128 || v > 127) goto over;
        return 1;
    case P_I16:
        if ((e->kind == N_INT && e->ival > 32767) || v < -32768 || v > 32767) goto over;
        return 1;
    case P_I32:
        if ((e->kind == N_INT && e->ival > INT32_MAX) || v < INT32_MIN || v > INT32_MAX) goto over;
        return 1;
    case P_I64: if (e->kind == N_INT && e->ival > INT64_MAX) goto over; return 1;
    case P_U8: if (u > 255) goto over; return 1;
    case P_U16: if (u > 65535) goto over; return 1;
    case P_U32: if (u > 4294967295u) goto over; return 1;
    case P_U64: if (v < 0 && e->kind != N_INT) goto over; return 1;
    case P_USIZE: if (v < 0 && e->kind != N_INT) goto over; return 1;
    default: return 1;
    }
over:
    ck_err(k, e, "integer literal %lld out of range for %s", (long long)v, type_name(want));
    return 0;
}

static int enum_backing_type(SType* t) {
    return t && t->kind == SK_PRIM &&
           (t->prim == P_I8 || t->prim == P_I16 || t->prim == P_I32 ||
            t->prim == P_I64 || t->prim == P_U8 || t->prim == P_U16 ||
            t->prim == P_U32 || t->prim == P_U64);
}

static int enum_unsigned(SType* t) {
    return t && t->kind == SK_PRIM && (t->prim == P_U8 || t->prim == P_U16 || t->prim == P_U32 || t->prim == P_U64);
}

static uint64_t enum_max(SType* t) {
    switch (t->prim) {
    case P_I8: return INT8_MAX;
    case P_I16: return INT16_MAX;
    case P_I32: return INT32_MAX;
    case P_I64: return INT64_MAX;
    case P_U8: return UINT8_MAX;
    case P_U16: return UINT16_MAX;
    case P_U32: return UINT32_MAX;
    default: return UINT64_MAX;
    }
}

static void check_enum_decl(Ck* k, Node* n, SType* t) {
    t->base = n->a ? build_type(k, n->a) : st_prim(P_I32);
    if (!enum_backing_type(t->base)) {
        ck_err(k, n->a ? n->a : n, "enum backing type must be i8, i16, i32, i64, u8, u16, u32, or u64");
        t->base = st_prim(P_I32);
    }
    int64_t previous = -1;
    for (int i = 0; i < n->nkids; i++) {
        Node* m = n->kids[i];
        int64_t value;
        int valid = 1;
        if (m->a) {
            (void)expr_type(k, m->a);
            if (!ut_const(m->a, &value)) {
                ck_err(k, m->a, "enum discriminant must be a compile-time integer expression");
                value = i == 0 ? 0 : (int64_t)((uint64_t)previous + 1);
                valid = 0;
            } else if (enum_unsigned(t->base) && value < 0 && m->a->kind != N_INT) {
                ck_err(k, m->a, "negative enum discriminant does not fit %s", type_name(t->base));
                valid = 0;
            } else {
                valid = ut_fits(k, m->a, t->base);
            }
        } else {
            if (i > 0 && (uint64_t)previous == enum_max(t->base)) {
                ck_err(k, m, "implicit enum discriminant overflows %s", type_name(t->base));
                value = previous;
                valid = 0;
            } else {
                value = i == 0 ? 0 : (int64_t)((uint64_t)previous + 1);
            }
        }
        m->ival = (uint64_t)value;
        if (valid && i == 0 && value != 0) ck_err(k, m, "first enum member must have discriminant zero");
        for (int j = 0; valid && j < i; j++) if (n->kids[j]->ival == m->ival) {
            ck_err(k, m, "duplicate enum discriminant %lld", (long long)value);
            break;
        }
        previous = value;
    }
    layout(t);
}

static int accepts_null(SType* t) {
    return t && (t->kind == SK_PTR || t->kind == SK_SLICE ||
                 t->kind == SK_FUNC || t->kind == SK_CONTRACT ||
                 t->kind == SK_ERROR ||
                 (t->kind == SK_PRIM &&
                  (t->prim == P_STRING || t->prim == P_TYPEID)));
}

/* Give a standalone expression fixed literal defaults without borrowing type
 * information from its destination. Tuple construction uses this recursively
 * so its member types are determined before compatibility with the declared
 * tuple is checked. */
static SType* materialize_expr_defaults(Ck* k, Node* e) {
    SType* t = expr_type(k, e);
    if (t == T_UT_INT) {
        SType* def = e->kind == N_CHAR ? st_prim(P_U8) : st_prim(P_I64);
        coerce(k, e, def);
        return def;
    }
    if (t == T_UT_FLOAT) {
        SType* def = st_prim(P_F64);
        coerce(k, e, def);
        return def;
    }
    if (e->kind == N_TUPLE_E && t->kind == SK_TUPLE) {
        for (int i = 0; i < e->nkids; i++) t->fields[i].t = materialize_expr_defaults(k, e->kids[i]);
        e->ty = t;
    }
    return t;
}

static int coerce(Ck* k, Node* e, SType* want) {
    if (e->kind == N_ARRAYLIT && want && want->kind == SK_ARRAY) {
        check_arraylit(k, e, want);
        return node_ty(e) != T_ERR;
    }
    if (e->kind == N_STRUCTLIT && !e->a && want && (want->kind == SK_STRUCT || want->kind == SK_BITS)) {
        check_structlit(k, e, want);
        return node_ty(e) != T_ERR;
    }
    if (e->kind == N_TUPLE_E && want && want->kind == SK_TUPLE) {
        SType* t = materialize_expr_defaults(k, e);
        if (t->nfields != want->nfields) {
            ck_err(k, e, "tuple has %d values where %d expected", t->nfields, want->nfields);
            return 0;
        }
        for (int i = 0; i < t->nfields; i++) {
            if (!same_type(t->fields[i].t, want->fields[i].t)) {
                ck_err(k, e->kids[i],
                       "tuple value %d is %s where %s expected (casts are explicit)",
                       i + 1, type_name(t->fields[i].t),
                       type_name(want->fields[i].t));
                return 0;
            }
        }
        e->ty = want; // emitter uses the declared tuple's exact layout
        return 1;
    }
    if (e->kind == N_IDENT && want && want->kind == SK_FUNC) {
        Sym* head = lookup_val(k, e->str, e->len);
        if (head && head->kind == SYM_FUNC && overload_count(head) > 1) {
            Sym* found = NULL;
            for (Sym* s = head; s; s = s->overload) {
                if (s->is_generic || !same_type(s->type, want)) continue;
                if (found) {
                    ck_err(k, e, "ambiguous overloaded function value '%.*s'",
                           (int)e->len, e->str);
                    e->ty = T_ERR;
                    return 0;
                }
                found = s;
            }
            if (!found) {
                ck_err(k, e, "no overload of '%.*s' has the required function type", (int)e->len, e->str);
                e->ty = T_ERR;
                return 0;
            }
            e->sym = found;
            e->ty = found->type;
            return 1;
        }
    }
    SType* t = expr_type(k, e);
    if (same_type(t, want)) return 1;
    if (shrink_ok(t, want)) return 1; // implicit permission shrink, never widening
    if (want->kind == SK_ERR || t->kind == SK_ERR) return 1;
    if (t == T_UT_NULL) {
        if (accepts_null(want)) { e->ty = want; return 1; }
        ck_err(k, e, "null is not a value of %s", type_name(want));
        return 0;
    }
    if (t == T_UT_INT && (is_int(want) || want->kind == SK_ENUM || (want->kind == SK_DISTINCT && is_int(want->base)))) {
        if (ut_fits(k, e, want->kind == SK_DISTINCT ? want->base : want)) {
            if (e->kind == N_BINARY) {
                coerce(k, e->a, want);
                coerce(k, e->b, want);
            } else if (e->kind == N_UNARY) {
                // the whole was range-checked above; a literal operand is a
                // magnitude (i64 min negates 9223372036854775808)
                if (e->a->kind == N_INT || e->a->kind == N_FLOAT) e->a->ty = want;
                else coerce(k, e->a, want);
            }
            e->ty = want; // untyped literal takes its expected type here
            return 1;
        }
        return 0;
    }
    if (t == T_UT_FLOAT && (is_float(want) || (want->kind == SK_DISTINCT && is_float(want->base)))) {
        if (ut_fits(k, e, want->kind == SK_DISTINCT ? want->base : want)) {
            if (e->kind == N_BINARY) {
                coerce(k, e->a, want);
                coerce(k, e->b, want);
            } else if (e->kind == N_UNARY) {
                // the whole was range-checked above; a literal operand is a
                // magnitude (i64 min negates 9223372036854775808)
                if (e->a->kind == N_INT || e->a->kind == N_FLOAT) e->a->ty = want;
                else coerce(k, e->a, want);
            }
            e->ty = want;
            return 1;
        }
        return 0;
    }
    ck_err(k, e, "type mismatch: %s given where %s expected (casts are explicit)", type_name(t), type_name(want));
    return 0;
}

/*
 * An untyped operand beside a typed one materializes at that type. A plain
 * or negated literal takes it directly (an integer literal may meet a float
 * operand); a compound constant expression goes through coerce so every node
 * beneath it is typed, and an integer expression never becomes float math.
 */
static int settle_operand(Ck* k, Node* e, SType* want) {
    if (e->kind == N_UNARY && (e->a->kind == N_INT || e->a->kind == N_FLOAT))
        e->a->ty = want; // a negated literal is still a literal
    else if (e->kind == N_BINARY || e->kind == N_UNARY) return coerce(k, e, want);
    e->ty = want; // literal materializes to the operand type
    return 1;
}

// --- lvalues ------------------------------------------------------------------------

static SType* check_lvalue(Ck* k, Node* e, int write) {
    switch (e->kind) {
    case N_IDENT: {
        Sym* s = lookup_val(k, e->str, e->len);
        if (!s) {
            if (is_panic_name(e->str, e->len) || is_assert_name(e->str, e->len) ||
                is_static_assert_name(e->str, e->len)) {
                ck_err(k, e, "'%.*s' is a builtin call, not a value", (int)e->len, e->str);
                return T_ERR;
            }
            ck_err(k, e, "unknown name '%.*s'", (int)e->len, e->str);
            return T_ERR;
        }
        e->sym = s; // emitter: storage key even in write-only positions
        if (s->kind != SYM_VAR) {
            ck_err(k, e, "'%.*s' is not a variable", (int)e->len, e->str);
            return T_ERR;
        }
        if (write) {
            if (s->is_const) ck_err(k, e, "cannot assign to const '%.*s'", (int)e->len, e->str);
            else if (s->is_param && !s->is_mut)
                ck_err(k, e, "parameter '%.*s' is read-only — mark it 'mut' in the signature",
                       (int)e->len, e->str);
        }
        return s->type;
    }
    case N_FIELD_E: {
        SType* direct = expr_type(k, e->a);
        if (direct && e->len == 3 && memcmp(e->str, "len", 3) == 0 &&
            (direct->kind == SK_ARRAY || direct->kind == SK_SLICE ||
             (direct->kind == SK_PRIM && direct->prim == P_STRING))) {
            ck_err(k, e, "sequence .len is a read-only property, not an assignable place");
            return T_ERR;
        }
        // Reading a pointer value never needs write access to the storage that
        // holds it. For auto-deref writes, only the innermost pointer edge used
        // to reach the final object must be writable: `*mut *T` may write T,
        // while `mut **T` may replace the inner pointer but not write T.
        SType* b = expr_type(k, e->a);
        e->a->ty = b;
        SType* last_ptr = NULL;
        for (SType* q = b; q && q->kind == SK_PTR; q = q->base) last_ptr = q;
        if (write) {
            if (last_ptr) {
                if (!last_ptr->is_mut) ck_err(k, e, "write through read-only pointer — use 'mut *T' and 'mut &value'");
            } else {
                // An ordinary aggregate field is part of its base storage.
                // Re-check that base as a write so const/read-only params fail.
                (void)check_lvalue(k, e->a, 1);
            }
        }
        while (b->kind == SK_PTR) {
            b = b->base;
        }
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind == SK_FORWARD) {
            ck_err(k, e, "type '%s' is not declared yet; member access requires its declaration", type_name(b));
            return T_ERR;
        }
        if (b->kind == SK_ERROR) {
            if (write) ck_err(k, e, "Error fields are read-only");
            if (e->len == 6 && memcmp(e->str, "domain", 6) == 0) return st_prim(P_TYPEID);
            if (e->len == 4 && memcmp(e->str, "code", 4) == 0) return st_prim(P_I32);
            if ((e->len == 4 && memcmp(e->str, "name", 4) == 0) ||
                (e->len == 7 && memcmp(e->str, "message", 7) == 0))
                return st_prim(P_STRING);
            ck_err(k, e, "type Error has no field '%.*s'", (int)e->len, e->str);
            return T_ERR;
        }
        if (b->kind == SK_BITS) {
            for (int i = 0; i < b->nfields; i++)
                if (!b->fields[i].bit_anonymous && strlen(b->fields[i].name) == e->len &&
                    memcmp(b->fields[i].name, e->str, e->len) == 0) {
                    e->ival = (uint64_t)i;
                    return b->fields[i].t;
                }
            ck_err(k, e, "bits type %s has no accessible field '%.*s'", type_name(b), (int)e->len, e->str);
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
        // Slice element writes are content writes: permission comes from the
        // view's type (`mut []T`), not from the descriptor axis holding it —
        // an unmarked parameter of type `mut []T` still has writable bytes.
        // Arrays are direct storage, so their element writes keep the
        // ordinary descriptor check.
        SType* peek = check_lvalue(k, e->a, 0);
        SType* b = peek;
        if (peek->kind == SK_STATICMAP) {
            ck_err(k, e, write ? "StaticMap entries are immutable" : "StaticMap lookup is not an assignable place");
            return T_ERR;
        }
        if (write && peek->kind == SK_SLICE) {
            if (!peek->is_mut)
                ck_err(k, e,
                       "cannot write through the read-only view %s — establish a writable one: 'mut %.*s[lo..hi]' or declare the view 'mut []T'",
                       type_name(peek),
                       e->a->kind == N_IDENT ? (int)e->a->len : 3,
                       e->a->kind == N_IDENT ? e->a->str : "...");
            b = peek;
        } else if (write && peek->kind == SK_PRIM && peek->prim == P_STRING) {
            // Sequences #11: indexed byte writes exist only through a
            // 'mut string'. A plain string is a read-only view regardless of
            // the descriptor axis — permission comes from the view's type,
            // exactly as for slices.
            if (!peek->is_mut)
                ck_err(k, e,
                       "cannot write through the read-only view string — establish a writable one: 'mut %.*s[lo..hi]' or declare the view 'mut string'",
                       e->a->kind == N_IDENT ? (int)e->a->len : 3,
                       e->a->kind == N_IDENT ? e->a->str : "...");
            b = peek;
        } else if (write) {
            b = check_lvalue(k, e->a, 1);
        }
        SType* ix = expr_type(k, e->b);
        if (ix->kind != SK_ERR && !is_int(ix) && ix != T_UT_INT)
            ck_err(k, e->b, "index must be an integer, not %s", type_name(ix));
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind == SK_PRIM && b->prim == P_STRING)
        {
            if (write) check_index_bounds(k, e, e->a, b, 1);
            return st_prim(P_U8);
        }
        if (b->kind != SK_SLICE && b->kind != SK_ARRAY) {
            ck_err(k, e, "cannot index %s", type_name(b));
            return T_ERR;
        }
        if (write) check_index_bounds(k, e, e->a, b, 1);
        return b->base;
    }
    case N_UNARY:
        if (e->op == T_STAR) {
            SType* p = expr_type(k, e->a);
            if (p->kind == SK_ERR) return T_ERR;
            if (p->kind != SK_PTR) {
                ck_err(k, e, "'*' dereferences pointers, not %s", type_name(p));
                return T_ERR;
            }
            if (write && !p->is_mut) ck_err(k, e, "write through read-only pointer — use 'mut *T' and 'mut &value'");
            if (p->base->kind == SK_FORWARD) {
                ck_err(k, e, "type '%s' is not declared yet; dereference requires its declaration", type_name(p->base));
                return T_ERR;
            }
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

static Node* contract_method(SType* contract, const char* name, uint32_t len) {
    if (!contract || contract->kind != SK_CONTRACT || !contract->decl) return NULL;
    for (int i = 0; i < contract->decl->nkids; i++) {
        Node* m = contract->decl->kids[i];
        if (m->len == len && memcmp(m->str, name, len) == 0) return m;
    }
    return NULL;
}

static SType* contract_method_type(Ck* k, SType* contract, Node* m) {
    const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
    Node* tp = contract && contract->decl ? contract->decl->c : NULL;
    if (tp && contract->nargs == tp->nkids) {
        k->nsub = contract->nargs;
        k->ttypes = contract->args;
        k->tnames = (const char**)arena_alloc((size_t)k->nsub * sizeof(char*));
        for (int i = 0; i < k->nsub; i++) k->tnames[i] = tp->kids[i]->str;
    }
    SType* ft = st_new(SK_FUNC);
    ft->nparams = m->nkids;
    ft->params = (SType**)arena_alloc((size_t)(m->nkids ? m->nkids : 1) * sizeof(SType*));
    for (int i = 0; i < m->nkids; i++) ft->params[i] = build_type(k, m->kids[i]->a);
    ft->ret = m->a ? build_type(k, m->a) : NULL;
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
    return ft;
}

static int is_c_variadic_scalar(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    if (!t) return 0;
    if (t->kind == SK_PTR) return 1;
    if (t->kind != SK_PRIM) return 0;
    switch (t->prim) {
    case P_I8: case P_I16: case P_I32: case P_I64:
    case P_U8: case P_U16: case P_U32: case P_U64:
    case P_F32: case P_F64: case P_USIZE:
        return 1;
    default:
        return 0;
    }
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
        if (i < want) {
            coerce(k, raw, ft->params[i]);
        } else {
            // Variadic extras have no declared expected type, but they still
            // need the ordinary expression walk so identifiers resolve and
            // codegen receives complete type/symbol annotations.
            SType* t = expr_type(k, raw);
            if (t == T_UT_FLOAT) coerce(k, raw, st_prim(P_F64));
            else if (t == T_UT_INT) coerce(k, raw, st_prim(P_I64));
            t = node_ty(raw);
            if (!is_c_variadic_scalar(t))
                ck_err(k, raw,
                       "variadic argument has type %s, which is not supported at the C boundary",
                       type_name(t));
            if (am) ck_err(k, arg, "'mut' marks declared parameters, not variadics");
        }
    }
}

static void check_mut_args(Ck* k, Node* call, Node* fndecl) {
    if (!fndecl) return;
    for (int i = 0; i < call->nkids && i < fndecl->nkids; i++) {
        int am = call->kids[i]->kind == N_MUTARG;
        int pm = (fndecl->kids[i]->flags & F_MUT) != 0;
        if (am && !pm) ck_err(k, call->kids[i], "'mut' marks mut parameters; parameter %d is read-only", i + 1);
        else if (pm && !am) ck_err(k, call->kids[i], "mut parameter needs 'mut' at the call site");
        else if (am) { // mut passes the caller's address — the arg must have one
            check_lvalue(k, call->kids[i]->a, 0);
            Node* raw = call->kids[i]->a;
            if (is_bits_field(raw, NULL, NULL)) ck_err(k, raw, "bit fields cannot supply mut arguments");
            if (raw->kind == N_IDENT) {
                Sym* s = lookup_val(k, raw->str, raw->len);
                if (s && s->is_const_backed)
                    ck_err(k, raw, "const-backed contract cannot be passed to a mut parameter");
            }
        }
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
            if (is_panic_name(e->str, e->len) || is_assert_name(e->str, e->len) ||
                is_static_assert_name(e->str, e->len)) {
                ck_err(k, e, "'%.*s' is a builtin call, not a value", (int)e->len, e->str);
                return T_ERR;
            }
            ck_err(k, e, "unknown name '%.*s'", (int)e->len, e->str);
            return T_ERR;
        }
        if (s->kind == SYM_MODULE) {
            ck_err(k, e, "module '%.*s' used as a value", (int)e->len, e->str);
            return T_ERR;
        }
        if (s->kind == SYM_FUNC && s->decl && s->decl->ival == BI_FMT_PRINTF) {
            ck_err(k, e, "fmt.printf is direct-call-only because its checked arguments depend on the static format");
            return T_ERR;
        }
        if (s->kind == SYM_TYPE || (s->kind == SYM_FUNC && s->is_generic)) {
            ck_err(k, e, "'%.*s' is a type or generic — used bare here", (int)e->len, e->str);
            return T_ERR;
        }
        if (s->kind == SYM_FUNC && overload_count(s) > 1) {
            ck_err(k, e,
                   "overloaded function '%.*s' needs a call or an explicit function-value type",
                   (int)e->len, e->str);
            return T_ERR;
        }
        e->sym = s; // emitter: storage (slot / global / function)
        if (s->is_const_backed) e->flags |= F_CONST;
        return s->type ? s->type : T_ERR;
    }
    case N_TUPLE_E: {
        SType* tu = st_new(SK_TUPLE);
        tu->nfields = e->nkids;
        tu->fields = (SField*)arena_alloc((size_t)(e->nkids ? e->nkids : 1) * sizeof(SField));
        memset(tu->fields, 0, (size_t)(e->nkids ? e->nkids : 1) * sizeof(SField));
        for (int i = 0; i < e->nkids; i++) {
            tu->fields[i].name = NULL;
            tu->fields[i].t = expr_type(k, e->kids[i]);
        }
        return tu;
    }
    case N_UNARY: {
        if (e->op == T_AMP) { // &lvalue -> *T; mut &lvalue -> mut *T
            int writable = (e->flags & F_MUT) != 0;
            SType* t = check_lvalue(k, e->a, writable);
            if (is_bits_field(e->a, NULL, NULL)) {
                ck_err(k, e, "bit fields are non-addressable");
                return T_ERR;
            }
            SType* p = st_new(SK_PTR);
            p->is_mut = writable;
            p->base = t;
            return p;
        }
        if (e->op == T_STAR) return check_lvalue(k, e, 0);
        if (e->op == T_VOLATILE) {
            SType* t = check_lvalue(k, e->a, 0);
            if (is_bits_field(e->a, NULL, NULL)) {
                ck_err(k, e, "volatile access is allowed only on a complete bits value");
                return T_ERR;
            }
            if (t->kind != SK_ERR && !is_volatile_scalar(t))
                ck_err(k, e,
                       "volatile access requires an eligible scalar place, got %s",
                       type_name(t));
            return t;
        }
        SType* t = expr_type(k, e->a);
        if (t->kind == SK_ERR) return t;
        if (t == T_UT_INT) return t;
        if (t == T_UT_FLOAT && e->op == T_MINUS) return t;
        switch (e->op) {
        case T_BANG:
            if (is_bool(t)) return t;
            break;
        case T_TILDE:
            if (is_int(t)) return t;
            break;
        case T_MINUS:
            if (is_float(t)) return t;
            if (is_int(t)) {
                if (t->prim == P_U8 || t->prim == P_U16 || t->prim == P_U32 || t->prim == P_U64) {
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
            ck_err(k, e, "'%s' needs bool operands, got %s and %s", tok_disp(op), type_name(a), type_name(b));
            return T_ERR;
        }
        if (a->kind == SK_PTR && (op == T_PLUS || op == T_MINUS)) {
            if (b == T_UT_INT) {
                int64_t offset;
                if (ut_const(e->b, &offset) && offset < 0) {
                    ck_err(k, e->b, "pointer offset must be a nonnegative usize");
                    return T_ERR;
                }
                if (!ut_fits(k, e->b, st_prim(P_USIZE))) return T_ERR;
                b = st_prim(P_USIZE);
                e->b->ty = b;
            }
            if (b->kind != SK_PRIM || b->prim != P_USIZE) {
                ck_err(k, e, "pointer '%s' offset must be usize, not %s", tok_disp(op), type_name(b));
                return T_ERR;
            }
            if (!layout(a->base) || !a->base->size) {
                ck_err(k, e, "pointer arithmetic requires a complete, nonzero-sized pointee, got %s",
                       type_name(a->base));
                return T_ERR;
            }
            uint64_t offset;
            if (usize_const(e->b, &offset) && offset > UINT64_MAX / a->base->size) {
                ck_err(k, e->b, "pointer offset scaled by sizeof(%s) overflows usize", type_name(a->base));
                return T_ERR;
            }
            if (e->a->kind == N_NULL) {
                ck_err(k, e->a, "pointer arithmetic cannot start from null");
                return T_ERR;
            }
            return a;
        }
        if (b->kind == SK_PTR && (op == T_PLUS || op == T_MINUS)) {
            ck_err(k, e, "pointer arithmetic requires the pointer as the left operand");
            return T_ERR;
        }
        if (a == T_UT_INT && b != T_UT_INT && b->kind != SK_UT) {
            if (!ut_fits(k, e->a, is_int(b) ? b : st_prim(P_I64))) return T_ERR;
            a = b;
            if (!settle_operand(k, e->a, a)) return T_ERR;
        } else if (b == T_UT_INT && a != T_UT_INT && a->kind != SK_UT) {
            if (!ut_fits(k, e->b, is_int(a) ? a : st_prim(P_I64))) return T_ERR;
            b = a;
            if (!settle_operand(k, e->b, b)) return T_ERR;
        } else if (a == T_UT_FLOAT && is_float(b)) {
            if (!ut_fits(k, e->a, b)) return T_ERR;
            a = b;
            if (!settle_operand(k, e->a, a)) return T_ERR;
        } else if (b == T_UT_FLOAT && is_float(a)) {
            if (!ut_fits(k, e->b, a)) return T_ERR;
            b = a;
            if (!settle_operand(k, e->b, b)) return T_ERR;
        }
        int cmp = op == T_EQEQ || op == T_BANGEQ || op == T_LT || op == T_LE || op == T_GT || op == T_GE;
        // Two untyped operands compare at their natural type (int or f64);
        // the result is bool, never an untyped constant.
        if (cmp && a == b && (a == T_UT_INT || a == T_UT_FLOAT)) {
            SType* nat = st_prim(a == T_UT_INT ? P_I64 : P_F64);
            if (!coerce(k, e->a, nat) || !coerce(k, e->b, nat)) return T_ERR;
            return st_prim(P_BOOL);
        }
        if (a == T_UT_INT && b == T_UT_INT) return T_UT_INT;
        if (a == T_UT_FLOAT && b == T_UT_FLOAT) return T_UT_FLOAT;
        if (cmp) {
            int equality = op == T_EQEQ || op == T_BANGEQ;
            if (equality && same_type(a, b) &&
                (a->kind == SK_ERROR ||
                 a->kind == SK_BITS ||
                 (a->kind == SK_PRIM && a->prim == P_TYPEID)))
                return st_prim(P_BOOL);
            // union equality: tag + active payload, when every payload supports it
            if (equality && same_type(a, b) && union_supports_equality(a)) {
                SType* ua = a;
                while (ua && ua->kind == SK_DISTINCT) ua = ua->base;
                if (ua && ua->kind == SK_UNION) return st_prim(P_BOOL);
            }
            if (same_modulo_shrink(a, b) &&
                (is_num(a) || is_bool(a) || (a->kind == SK_PTR && equality) ||
                 a->kind == SK_FUNC || a->kind == SK_ENUM ||
                 a->kind == SK_CONTRACT ||
                 a->kind == SK_DISTINCT ||
                 (a->kind == SK_PRIM && a->prim != P_STRING)))
                return st_prim(P_BOOL);
            if (equality && a == T_UT_NULL && accepts_null(b)) {
                e->a->ty = b;
                return st_prim(P_BOOL);
            }
            if (equality && b == T_UT_NULL && accepts_null(a)) {
                e->b->ty = a;
                return st_prim(P_BOOL);
            }
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
        SType* range = NULL;
        if (lo == T_UT_INT && hi == T_UT_INT) {
            range = st_prim(P_I64);
            e->a->ty = range;
            e->b->ty = range;
        } else if (is_int(lo) && same_type(lo, hi)) {
            range = lo;
        } else {
            ck_err(k, e, "range bounds must be one integer type, got %s and %s", type_name(lo), type_name(hi));
            return T_ERR;
        }
        if (e->c) {
            if (!coerce(k, e->c, range)) return T_ERR;
            int64_t step;
            if (ut_const(e->c, &step) && step <= 0) {
                ck_err(k, e->c, "range step must be positive, got %lld", (long long)step);
                return T_ERR;
            }
        }
        return range;
    }
    case N_CMPCHAIN: {
        // One operand type for the whole chain: the first typed operand's, or
        // the natural int/f64 when every operand is an untyped constant.
        SType* want = NULL;
        SType* ut = NULL;
        for (int i = 0; i < e->nkids; i++) {
            SType* u = expr_type(k, e->kids[i]);
            if (u->kind == SK_ERR) return T_ERR;
            if (u == T_UT_INT || u == T_UT_FLOAT) {
                if (!ut) ut = u;
            } else if (!want) {
                want = u;
            }
        }
        if (!want) want = st_prim(ut == T_UT_FLOAT ? P_F64 : P_I64);
        for (int i = 0; i < e->nkids; i++) {
            SType* u = node_ty(e->kids[i]);
            if ((u == T_UT_INT && is_int(want)) || (u == T_UT_FLOAT && is_float(want))) {
                if (!coerce(k, e->kids[i], want)) return T_ERR;
                u = want;
            }
            if (!same_type(u, want) || !is_num(want)) {
                ck_err(k, e, "comparison chain needs one numeric type, got %s and %s", type_name(want), type_name(u));
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
                        e->ival = m->ival;
                        return ts->type;
                    }
                }
                ck_err(k, e, "enum %s has no member '%.*s'", type_name(ts->type), (int)e->len, e->str);
                return T_ERR;
            }
            if (ts && ts->type && ts->type->kind == SK_ERROR_DOMAIN) {
                for (int i = 0; i < ts->type->decl->nkids; i++) {
                    Node* m = ts->type->decl->kids[i];
                    if (m->len == e->len && memcmp(m->str, e->str, e->len) == 0) {
                        e->a->sym = ts;
                        e->a->ty = ts->type;
                        e->ival = ((uint64_t)type_identity(ts->type) << 32) | (uint32_t)m->ival;
                        return T_ERROR;
                    }
                }
                ck_err(k, e, "error domain %s has no entry '%.*s'", type_name(ts->type), (int)e->len, e->str);
                return T_ERR;
            }
            // union variant value (payloadless): Event.quit
            if (ts && ts->type && ts->type->kind == SK_UNION) {
                int vi = union_variant_index(k, e, ts->type, e);
                if (vi < 0) return T_ERR;
                if (ts->type->fields[vi].t) {
                    ck_err(k, e,
                           "variant '%.*s' carries a payload — construct it: %.*s.%.*s(...)",
                           (int)e->len, e->str, (int)e->a->len, e->a->str,
                           (int)e->len, e->str);
                    return T_ERR;
                }
                e->a->sym = ts;        // emitter: union type symbol on the base
                e->a->ty = ts->type;
                e->sym = ts;
                e->ival = (uint64_t)vi; // variant index (the tag value)
                return ts->type;
            }
        }
        // union variant value on a generic base: Pair[int].none
        if (e->a->kind == N_INDEX) {
            SType* ub = union_base_type(k, e->a);
            if (ub == T_ERR) return T_ERR;
            if (ub) {
                int vi = union_variant_index(k, e, ub, e);
                if (vi < 0) return T_ERR;
                if (ub->fields[vi].t) {
                    ck_err(k, e,
                           "variant '%.*s' carries a payload — construct it with a payload argument",
                           (int)e->len, e->str);
                    return T_ERR;
                }
                e->sym = node_sym(e->a->a); // N_IDENT base carries the type sym
                e->ival = (uint64_t)vi;
                return ub;
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
        if (e->len == 3 && memcmp(e->str, "len", 3) == 0 &&
            (b->kind == SK_ARRAY || b->kind == SK_SLICE ||
             (b->kind == SK_PRIM && b->prim == P_STRING))) {
            if (b->kind == SK_ARRAY) e->ival = b->count;
            else if (e->a->kind == N_STR) e->ival = e->a->len;
            return st_prim(P_USIZE);
        }
        while (b->kind == SK_PTR) b = b->base; // p.x auto-derefs
        if (b->kind == SK_FORWARD) {
            ck_err(k, e, "type '%s' is not declared yet; member access requires its declaration", type_name(b));
            return T_ERR;
        }
        if (b->kind == SK_ERROR) {
            if (e->len == 6 && memcmp(e->str, "domain", 6) == 0) return st_prim(P_TYPEID);
            if (e->len == 4 && memcmp(e->str, "code", 4) == 0) return st_prim(P_I32);
            if ((e->len == 4 && memcmp(e->str, "name", 4) == 0) ||
                (e->len == 7 && memcmp(e->str, "message", 7) == 0))
                return st_prim(P_STRING);
            ck_err(k, e, "type Error has no field '%.*s'", (int)e->len, e->str);
            return T_ERR;
        }
        if (b->kind == SK_CONTRACT) {
            Node* m = contract_method(b, e->str, e->len);
            if (!m) {
                ck_err(k, e, "contract %s has no method '%.*s'", type_name(b), (int)e->len, e->str);
                return T_ERR;
            }
            SType* ft = contract_method_type(k, b, m);
            return ft->ret ? ft->ret : T_ERR; // dispatch target
        }
        if (b->kind == SK_BITS) {
            for (int i = 0; i < b->nfields; i++)
                if (!b->fields[i].bit_anonymous && strlen(b->fields[i].name) == e->len &&
                    memcmp(b->fields[i].name, e->str, e->len) == 0) {
                    e->ival = (uint64_t)i;
                    return b->fields[i].t;
                }
            ck_err(k, e, "bits type %s has no accessible field '%.*s'", type_name(b), (int)e->len, e->str);
            return T_ERR;
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
        if (b->kind == SK_STATICMAP) {
            if (!e->a || e->a->kind != N_IDENT || !node_sym(e->a) || !node_sym(e->a)->is_global) {
                ck_err(k, e, "StaticMap lookup requires a module-level table binding");
                return T_ERR;
            }
            SType* ix = expr_type(k, e->b);
            if (ix == T_UT_INT || ix == T_UT_FLOAT || ix == T_UT_NULL) coerce(k, e->b, b->base);
            else if (!same_type(ix, b->base))
                ck_err(k, e->b, "StaticMap key is %s, not the table's exact key type %s",
                       type_name(ix), type_name(b->base));
            return b->ret;
        }
        SType* ix = expr_type(k, e->b);
        if (b->kind == SK_ERR || ix->kind == SK_ERR) return T_ERR;
        if (!is_int(ix) && ix != T_UT_INT) ck_err(k, e->b, "index must be an integer, not %s", type_name(ix));
        if (b->kind == SK_PRIM && b->prim == P_STRING) {
            check_index_bounds(k, e, e->a, b, 0);
            return st_prim(P_U8);
        }
        if (b->kind == SK_SLICE || b->kind == SK_ARRAY) {
            check_index_bounds(k, e, e->a, b, 0);
            return b->base;
        }
        ck_err(k, e, "cannot index %s", type_name(b));
        return T_ERR;
    }
    case N_SLICE_E: {
        int establish = (e->flags & F_MUT) != 0;
        if (establish && e->a->kind == N_STR) {
            // Mutability #11: a literal cannot establish a writable view;
            // its bytes live in static read-only storage.
            ck_err(k, e, "a string literal cannot establish 'mut string' — slice a writable string binding");
            return T_ERR;
        }
        SType* b = establish ? check_lvalue(k, e->a, 0) : expr_type(k, e->a);
        if (establish) e->a->ty = b; // annotate for the definite-init pass
        if (b->kind == SK_ERR) return T_ERR;
        int is_string = b->kind == SK_PRIM && b->prim == P_STRING;
        int is_view = is_string || b->kind == SK_SLICE || b->kind == SK_ARRAY;
        if (!is_view) {
            ck_err(k, e, "can only slice arrays, slices, and strings, not %s", type_name(b));
            return T_ERR;
        }
        if (establish) {
            // Mutability #11: the establishment marker may prove writable
            // array elements, but it may never regrow a plain slice/string.
            // An already-mut view needs no marker; re-slicing preserves it.
            int content_writable = (is_string || b->kind == SK_SLICE) && b->is_mut;
            if ((is_string || b->kind == SK_SLICE) && !content_writable) {
                ck_err(k, e,
                       "cannot establish writable content from plain %s — permission that has shrunk cannot grow again",
                       type_name(b));
                return T_ERR;
            }
            if (!content_writable) { // array: prove its inline element storage
                Node* src = e->a;
                Sym* vs = src->kind == N_IDENT ? node_sym(src) : NULL;
                if (vs && vs->is_const)
                    ck_err(k, e,
                           "const '%.*s' cannot establish a writable view",
                           (int)src->len, src->str);
                else if (vs && vs->is_literal_backed)
                    ck_err(k, e,
                           "'%.*s' is literal-backed and cannot establish a writable view",
                           (int)src->len, src->str);
                else (void)check_lvalue(k, e->a, 1);
            }
        }
        if (e->b) { SType* lo = expr_type(k, e->b);
            if (!is_int(lo) && lo != T_UT_INT) ck_err(k, e->b, "slice bound must be an integer"); }
        if (e->c) { SType* hi = expr_type(k, e->c);
            if (!is_int(hi) && hi != T_UT_INT) ck_err(k, e->c, "slice bound must be an integer"); }
        check_slice_bounds(k, e, e->a, b);
        if (is_string) return b->is_mut || establish ? T_MUT_STRING : st_prim(P_STRING);
        SType* s = st_new(SK_SLICE);
        s->is_mut = b->kind == SK_ARRAY ? establish : (b->is_mut || establish);
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
        if (t->kind == SK_STRUCT || t->kind == SK_BITS) check_structlit(k, e, t);
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

static SType* instantiate_generic_func_args(Ck* k, Sym* gs, SType** args, int nargs, Node* at) {
    Node* f = gs->decl;
    Node* tp = f->c;
    if (!tp || tp->nkids != nargs) {
        ck_err(k, at, "generic function expects %d type arguments, got %d", tp ? tp->nkids : 0, nargs);
        return T_ERR;
    }
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
    SType* ft = build_func_type(k, f, args, nargs);
    STypeInst* inst = (STypeInst*)arena_alloc(sizeof(STypeInst));
    inst->args = args;
    inst->result = ft;
    inst->next = *head;
    *head = inst;
    if (f->flags & F_SPECIALIZED) {
        // no default body: the no-default call-site check has already fired
        k->tnames = sn; k->ttypes = st; k->nsub = ns;
        return ft;
    }
    Scope* saved_scope = k->scope;
    if (gs->decl_scope) k->scope = gs->decl_scope;
    check_func_body(k, f); // stamping: body checked once per instantiation
    k->scope = saved_scope;
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
    return ft;
}

static SType* instantiate_generic_func(Ck* k, Sym* gs, Node* index_node) {
    int nargs = index_node->b ? 1 : index_node->nkids;
    SType** args = (SType**)arena_alloc((size_t)nargs * sizeof(SType*));
    for (int i = 0; i < nargs; i++) args[i] = build_type(k, index_node->b ? index_node->b : index_node->kids[i]);
    return instantiate_generic_func_args(k, gs, args, nargs, index_node);
}

static SType* overload_arg_type(Ck* k, Node* arg) {
    Node* raw = arg->kind == N_MUTARG ? arg->a : arg;
    if (raw->kind == N_ARRAYLIT) return NULL; // candidate parameter supplies its type
    if (raw->kind == N_STRUCTLIT && !raw->a) return NULL; // bare literal: parameter supplies its type
    SType* t = expr_type(k, raw);
    if (t == T_UT_INT) {
        /* A character literal is a one-byte constant, so its overload-call
         * default is the byte type (Stdlib #15), not int. */
        if (raw->kind == N_CHAR) return st_prim(P_U8);
        return st_prim(P_I64); // integer-literal default: int
    }
    if (t == T_UT_FLOAT) return st_prim(P_F64); // float-literal default: f64
    return t;
}

static int exact_arg_type(Node* arg, SType* actual, SType* param) {
    Node* raw = arg->kind == N_MUTARG ? arg->a : arg;
    if (raw->kind == N_ARRAYLIT) {
        if (!param || param->kind != SK_ARRAY) return 0;
        return raw->a ? raw->ival == param->count
                      : (uint64_t)raw->nkids == param->count;
    }
    if (raw->kind == N_STRUCTLIT && !raw->a) return param && (param->kind == SK_STRUCT || param->kind == SK_BITS);
    return same_type(actual, param);
}

static int call_exact_ft(Node* call, Node* decl, SType* ft, SType** actual) {
    if (!ft || ft->kind != SK_FUNC || call->nkids != ft->nparams) return 0;
    for (int i = 0; i < call->nkids; i++) {
        int am = call->kids[i]->kind == N_MUTARG;
        int pm = decl && i < decl->nkids && (decl->kids[i]->flags & F_MUT) != 0;
        if (am != pm || !exact_arg_type(call->kids[i], actual[i], ft->params[i])) return 0;
    }
    return 1;
}

/*
 * Shrink-compatible overload match: identical to the exact match except a
 * mut-qualified argument may bind to the plain parameter form (Mutability #9).
 * Used only when no overload matches exactly, so exact matches keep priority
 * and distinct mut/plain overloads stay unambiguous.
 */
static int call_shrink_ft(Node* call, Node* decl, SType* ft, SType** actual) {
    if (!ft || ft->kind != SK_FUNC || call->nkids != ft->nparams) return 0;
    for (int i = 0; i < call->nkids; i++) {
        int am = call->kids[i]->kind == N_MUTARG;
        int pm = decl && i < decl->nkids && (decl->kids[i]->flags & F_MUT) != 0;
        if (am != pm) return 0;
        if (exact_arg_type(call->kids[i], actual[i], ft->params[i])) continue;
        if (!shrink_ok(actual[i], ft->params[i])) return 0;
    }
    return 1;
}

static int call_exact_match(Node* call, Sym* s, SType** actual) {
    return s && !s->is_generic && !s->is_extern && call_exact_ft(call, s->decl, s->type, actual);
}

// Select a non-generic direct-call overload. During the source walk, no match
// is deferred because a later declaration may add the matching overload.
// At EOF, a sole declaration retains Ace's ordinary expected-literal typing;
// a real overload set uses exact types and int/f64 literal defaults.
static Sym* select_call_overload(Ck* k, Node* call, Sym* head, int final, int* ambiguous) {
    *ambiguous = 0;
    if (!head || head->kind != SYM_FUNC) return NULL;
    int nactual = call->nkids;
    SType** actual = (SType**)arena_alloc((size_t)(nactual ? nactual : 1) * sizeof(SType*));
    for (int i = 0; i < nactual; i++) actual[i] = overload_arg_type(k, call->kids[i]);

    Sym* found = NULL;
    int nongeneric = 0;
    for (Sym* s = head; s; s = s->overload) {
        if (s->is_generic) continue;
        nongeneric++;
        if (!call_exact_match(call, s, actual)) continue;
        if (found) {
            *ambiguous = 1;
            return NULL;
        }
        found = s;
    }
    if (!found) { // no exact match: one shrink-compatible overload may win
        for (Sym* s = head; s; s = s->overload) {
            if (s->is_generic) continue;
            if (!call_shrink_ft(call, s->decl, s->type, actual)) continue;
            if (found) {
                *ambiguous = 1;
                return NULL;
            }
            found = s;
        }
    }
    if (!found && final && nongeneric == 1) {
        for (Sym* s = head; s; s = s->overload) if (!s->is_generic) return s;
    }
    return found;
}

static void finish_direct_call(Ck* k, Node* call, Sym* s) {
    int variadic = s->is_extern && s->decl && (s->decl->flags & F_VARARGS) != 0;
    check_args(k, call, s->type, variadic);
    check_mut_args(k, call, s->decl);
    call->sym = s;
    call->ty = s->type->ret;
    if (s->decl && (s->decl->flags & F_CONST)) call->flags |= F_CONST;
}

static int const_lvalue(Ck* k, Node* e) {
    while (e && (e->kind == N_FIELD_E || e->kind == N_INDEX)) e = e->a;
    if (e && e->kind == N_IDENT) {
        Sym* s = lookup_val(k, e->str, e->len);
        return s && (s->is_const || s->is_const_backed);
    }
    return 0;
}

static Node* find_conformance(Ck* k, SType* conc, SType* contract) {
    if (!conc || conc->kind != SK_STRUCT || !conc->decl || !conc->decl->d) return NULL;
    Node* signing = conc->decl->d;
    const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
    if (signing->c && conc->nargs == signing->c->nkids) {
        k->nsub = conc->nargs;
        k->ttypes = conc->args;
        k->tnames = (const char**)arena_alloc((size_t)k->nsub * sizeof(char*));
        for (int i = 0; i < k->nsub; i++) k->tnames[i] = signing->c->kids[i]->str;
    }
    Node* found = NULL;
    for (int i = 0; i < signing->nkids; i++) {
        SType* it = build_type(k, signing->kids[i]->a);
        if (same_type(it, contract)) { found = signing->kids[i]; break; }
    }
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
    return found;
}

static SType* check_contract_construct(Ck* k, Node* call, SType* contract) {
    if (call->nkids != 1) {
        ck_err(k, call, "contract construction takes exactly one pointer");
        return T_ERR;
    }
    Node* arg = call->kids[0];
    SType* from = expr_type(k, arg);
    if (from->kind != SK_PTR || !from->base || from->base->kind != SK_STRUCT) {
        ck_err(k, arg, "contract construction needs a pointer to a struct");
        return T_ERR;
    }
    Node* conf = find_conformance(k, from->base, contract);
    if (!conf) {
        ck_err(k, call, "type %s has not signed contract %s", type_name(from->base), type_name(contract));
        return T_ERR;
    }
    Node* signing = from->base->decl->d;
    if (signing->path && call->path && strcmp(signing->path, call->path) != 0 && !(signing->flags & F_PUB)) {
        ck_err(k, call, "signing for %s is private", type_name(from->base));
        return T_ERR;
    }
    call->d = conf;
    if (arg->kind == N_UNARY && arg->op == T_AMP && const_lvalue(k, arg->a)) call->flags |= F_CONST;
    return contract;
}

static SType* indexed_named_type(Ck* k, Node* ix) {
    if (!ix || ix->kind != N_INDEX || !ix->a || ix->a->kind != N_IDENT) return NULL;
    Sym* ts = lookup_type(k, ix->a->str, ix->a->len);
    if (!ts || !ts->is_generic || !ts->type || !ts->type->decl) return NULL;
    int n = ix->b ? 1 : ix->nkids;
    SType** args = (SType**)arena_alloc((size_t)(n ? n : 1) * sizeof(SType*));
    for (int i = 0; i < n; i++) args[i] = build_type(k, ix->b ? ix->b : ix->kids[i]);
    return instantiate_generic_type(k, ts->type->decl, args, n);
}

static uint32_t type_identity(SType* t) {
    /* Alias spellings must produce one runtime typeid. Canonicalize string to
       the corresponding byte-slice representative before assigning an id. */
    if (t && t->kind == SK_PRIM && t->prim == P_STRING) {
        ensure_memory_types();
        t = t->is_mut ? T_MUT_BYTE_SLICE : T_BYTE_SLICE;
    }
    if (!t->type_id) t->type_id = next_type_id++;
    return t->type_id;
}

static SType* typeid_argument(Ck* k, Node* arg) {
    if (arg->kind == N_IDENT) return build_type(k, arg);
    if (arg->kind == N_INDEX) return indexed_named_type(k, arg);
    ck_err(k, arg, "typeid takes one exact named type");
    return T_ERR;
}

static SType* check_error_construct(Ck* k, Node* call, Sym* ts) {
    if (call->nkids > 1) {
        ck_err(k, call, "error-domain construction takes zero or one i32 code");
        return T_ERR;
    }
    if (call->nkids == 1 && !coerce(k, call->kids[0], st_prim(P_I32))) return T_ERR;
    call->a->sym = ts;
    call->a->ty = ts->type;
    call->ival = type_identity(ts->type);
    return T_ERROR;
}

static void check_error_domain_decl(Ck* k, Node* n) {
    for (int i = 0; i < n->nkids; i++) {
        Node* entry = n->kids[i];
        int64_t code = 0;
        (void)expr_type(k, entry->a);
        int exact = entry->a->kind == N_INT ||
                    (entry->a->kind == N_UNARY && entry->a->op == T_MINUS &&
                     entry->a->a && entry->a->a->kind == N_INT);
        if (!exact || !ut_const(entry->a, &code)) ck_err(k, entry->a, "error code must be an exact integer literal");
        else if (code < INT32_MIN || code > INT32_MAX)
            ck_err(k, entry->a, "error code %lld is outside i32", (long long)code);
        entry->ival = (uint64_t)code;
        if (!entry->b || entry->b->kind != N_STR)
            ck_err(k, entry->b ? entry->b : entry,
                   "error message must be a string literal");
        for (int j = 0; j < i; j++) {
            Node* prior = n->kids[j];
            if (prior->len == entry->len &&
                memcmp(prior->str, entry->str, entry->len) == 0)
                ck_err(k, entry, "duplicate error entry name '%.*s'",
                       (int)entry->len, entry->str);
            if ((int64_t)prior->ival == code) ck_err(k, entry, "duplicate error code %lld", (long long)code);
        }
    }
}

static SType* check_memory_call(Ck* k, Node* call, Sym* fn, int intrinsic) {
    ensure_memory_types();
    Node* callee = call->a;
    callee->sym = fn;
    callee->ty = fn->type;
    call->sym = fn;
    call->ival = (uint64_t)intrinsic;

    if (intrinsic == BI_MEM_EQUAL) {
        if (call->nkids != 2) {
            ck_err(k, call, "mem.equal takes exactly 2 arguments");
            return T_ERR;
        }
        for (int i = 0; i < 2; i++) {
            Node* arg = call->kids[i];
            if (arg->kind == N_MUTARG) {
                ck_err(k, arg, "'mut' marks mut parameters; mem.equal parameter %d is read-only", i + 1);
                arg = arg->a;
            }
            SType* t = expr_type(k, arg);
            if (t->kind != SK_ERR && !is_byte_sequence(t))
                ck_err(k, arg,
                       "mem.equal requires byte sequences ([N]u8, []u8, or string), got %s",
                       type_name(t));
        }
        return st_prim(P_BOOL);
    }

    if (intrinsic == BI_MEM_ALLOC_SIZE) {
        if (call->nkids != 1) {
            ck_err(k, call, "mem.alloc_size takes exactly one allocation slice");
            return T_ERR;
        }
        Node* data = call->kids[0];
        if (data->kind == N_MUTARG) {
            ck_err(k, data, "'mut' marks mut parameters; mem.alloc_size's argument is read-only");
            data = data->a;
        }
        coerce(k, data, T_BYTE_SLICE);
        return st_prim(P_USIZE);
    }

    if (intrinsic == BI_MEM_ALLOC) {
        if (call->nkids != 2) {
            ck_err(k, call, "mem.alloc takes exactly size and alignment");
            return T_ERR;
        }
        for (int i = 0; i < 2; i++) {
            Node* arg = call->kids[i];
            if (arg->kind == N_MUTARG) {
                ck_err(k, arg, "'mut' marks mut parameters; mem.alloc arguments are read-only");
                arg = arg->a;
            }
            coerce(k, arg, st_prim(P_I64));
        }
        return T_MUT_BYTE_SLICE;
    }
    if (intrinsic == BI_MEM_REALLOC) {
        if (call->nkids != 2) {
            ck_err(k, call, "mem.realloc takes exactly data and new size");
            return T_ERR;
        }
        Node* data = call->kids[0];
        Node* size = call->kids[1];
        if (data->kind == N_MUTARG) {
            ck_err(k, data, "'mut' marks mut parameters; mem.realloc arguments are read-only");
            data = data->a;
        }
        if (size->kind == N_MUTARG) {
            ck_err(k, size, "'mut' marks mut parameters; mem.realloc arguments are read-only");
            size = size->a;
        }
        coerce(k, data, T_BYTE_SLICE);
        coerce(k, size, st_prim(P_I64));
        return T_MUT_BYTE_SLICE;
    }

    if (intrinsic == BI_MEM_RELEASE && call->nkids != 1) {
        ck_err(k, call, "mem.release takes exactly one mut allocation slice");
        return T_ERR;
    }
    if (intrinsic == BI_MEM_RELEASE) {
        Node* arg = call->kids[0];
        if (arg->kind != N_MUTARG) {
            ck_err(k, arg, "mem.release needs 'mut' at the call site: release(mut data)");
            return T_ERR;
        }
        Node* raw = arg->a;
        SType* visible = check_lvalue(k, raw, 1);
        SType* declared = visible;
        Sym* current = raw->kind == N_IDENT ? node_sym(raw) : NULL;
        Sym* original = current && current->decl ? node_sym(current->decl) : NULL;
        if (original && original->type) declared = original->type;
        if (!same_type(declared, T_BYTE_SLICE) && !shrink_ok(declared, T_BYTE_SLICE)) {
            if (declared->kind == SK_SLICE)
                ck_err(k, raw,
                       "mem.release requires a mutable []u8 allocation variable — a typed slice "
                       "states its element type: mem.release[T](mut data)");
            else ck_err(k, raw, "mem.release requires a mutable []u8 allocation variable");
            return T_ERR;
        }
        return T_VOID;
    }

    const char* name = intrinsic == BI_MEM_COPY ? "copy" :
                       intrinsic == BI_MEM_MOVE ? "move" :
                       intrinsic == BI_MEM_FILL ? "fill" :
                       intrinsic == BI_MEM_ZERO ? "zero" : "compare";
    int want = intrinsic == BI_MEM_ZERO ? 1 : 2;
    if (call->nkids != want) {
        ck_err(k, call, "mem.%s takes exactly %d argument%s", name, want, want == 1 ? "" : "s");
        return T_ERR;
    }

    int writes = intrinsic != BI_MEM_COMPARE;
    for (int i = 0; i < call->nkids; i++) {
        Node* arg = call->kids[i];
        int is_dst = writes && i == 0;
        // Content-write destinations carry type-position `mut` (Memory #15):
        // the argument's type must be `mut []u8`; there is no call-site mark.
        if (arg->kind == N_MUTARG) {
            if (is_dst)
                ck_err(k, arg,
                       "mem.%s's destination is a writable view — pass 'mut []u8'; no call-site 'mut'",
                       name);
            else ck_err(k, arg, "'mut' marks mut parameters; mem.%s parameter %d is read-only", name, i + 1);
            arg = arg->a;
        }
        SType* want_type = intrinsic == BI_MEM_FILL && i == 1 ? st_prim(P_U8)
                         : (is_dst ? T_MUT_BYTE_SLICE : T_BYTE_SLICE);
        coerce(k, arg, want_type);
    }
    return intrinsic == BI_MEM_COMPARE ? st_prim(P_I64) : T_VOID;
}

/*
 * Typed allocation intrinsics: mem.alloc[T](count): mut []T,
 * mem.realloc[T](data, new_count): mut []T, and mem.release[T](mut data).
 * They ride the ordinary explicit generic-instantiation path, never stamp a
 * body, and are lowered directly by the emitter. The instantiation must name
 * exactly one element type whose layout the compiler can size.
 */
static SType* check_memory_generic_call(Ck* k, Node* call, Sym* fn, SType* elem, int intrinsic) {
    ensure_memory_types();
    Node* callee = call->a;
    callee->sym = fn;
    callee->a->sym = fn;
    call->sym = fn;
    call->ival = (uint64_t)intrinsic;

    const char* what = intrinsic == BI_MEM_ALLOC_T ? "mem.alloc[T]" :
                       intrinsic == BI_MEM_ZALLOC_T ? "mem.zalloc[T]" :
                       intrinsic == BI_MEM_REALLOC_T ? "mem.realloc[T]" :
                                                       "mem.release[T]";
    if (!elem || elem->kind == SK_ERR) return T_ERR;
    if (elem->kind == SK_TYPEPARAM) {
        ck_err(k, callee, "%s needs a concrete element type at the instantiation", what);
        return T_ERR;
    }
    if (!layout(elem)) return T_ERR;
    SType* slice = st_new(SK_SLICE);
    slice->base = elem;
    layout(slice);
    SType* mut_slice = st_new(SK_SLICE);
    mut_slice->base = elem;
    mut_slice->is_mut = 1;
    layout(mut_slice);
    callee->ty = intrinsic == BI_MEM_RELEASE_T ? slice : mut_slice;
    // The emitter derives sizeof(T)/alignof(T) from the result slice.

    if (intrinsic == BI_MEM_ALLOC_T || intrinsic == BI_MEM_ZALLOC_T) {
        if (call->nkids != 1) {
            ck_err(k, call, "%s takes exactly one element count", what);
            return T_ERR;
        }
        Node* count = call->kids[0];
        if (count->kind == N_MUTARG) {
            ck_err(k, count, "'mut' marks mut parameters; %s arguments are read-only", what);
            count = count->a;
        }
        coerce(k, count, st_prim(P_USIZE));
        return mut_slice;
    }
    if (intrinsic == BI_MEM_REALLOC_T) {
        if (call->nkids != 2) {
            ck_err(k, call, "%s takes exactly data and a new element count", what);
            return T_ERR;
        }
        Node* data = call->kids[0];
        Node* count = call->kids[1];
        if (data->kind == N_MUTARG) {
            ck_err(k, data, "'mut' marks mut parameters; %s data is read-only", what);
            data = data->a;
        }
        if (count->kind == N_MUTARG) {
            ck_err(k, count, "'mut' marks mut parameters; %s arguments are read-only", what);
            count = count->a;
        }
        coerce(k, data, slice);
        coerce(k, count, st_prim(P_USIZE));
        return mut_slice;
    }

    if (call->nkids != 1) {
        ck_err(k, call, "%s takes exactly one mut typed allocation slice", what);
        return T_ERR;
    }
    Node* arg = call->kids[0];
    if (arg->kind != N_MUTARG) {
        ck_err(k, arg, "%s needs 'mut' at the call site: mem.release[T](mut data)", what);
        return T_ERR;
    }
    Node* raw = arg->a;
    SType* visible = check_lvalue(k, raw, 1);
    (void)visible;
    SType* declared = visible;
    Sym* current = raw->kind == N_IDENT ? node_sym(raw) : NULL;
    Sym* original = current && current->decl ? node_sym(current->decl) : NULL;
    if (original && original->type) declared = original->type;
    if (!same_type(declared, slice) && !shrink_ok(declared, slice)) {
        ck_err(k, raw, "%s requires a mutable []%s allocation variable — the "
                       "instantiation must match the slice's element type exactly",
               what, type_name(elem));
        return T_ERR;
    }
    return T_VOID;
}

/*
 * The checked printf lowering resolves each plan part's helper through exact-
 * type specialization lookup (Generics #10): the helper shapes are 'special-
 * ized' generics in std fmt — print_counted[T](value: T): int, print_preci-
 * sion[T](value: T, precision: usize): int, and print_radix[T](value: T,
 * radix: u8, uppercase: bool): int — and the format interpreter's concrete argument type
 * selects the specialization whose type-argument tuple equals it. No name
 * composition remains.
 */
static Sym* printf_helper(Ck* k, Sym* printf_fn, const char* generic, SType** targs, int nargs, Node* at) {
    if (!printf_fn || printf_fn->len < 6) return NULL;
    uint32_t prefix = printf_fn->len - 6; // canonical module prefix before "printf"
    char name[512];
    snprintf(name, sizeof name, "%.*s%s", (int)prefix, printf_fn->name, generic);
    for (Sym* s = lookup_val(k, name, (uint32_t)strlen(name)); s; s = s->overload) {
        if (!s->spec_args || s->spec_nargs != nargs) continue;
        int same = 1;
        for (int i = 0; i < nargs && same; i++)
            same = same_type(s->spec_args[i], targs[i]);
        if (same) return s;
    }
    ck_err(k, at, "internal fmt.printf helper '%s%s' has no specialization", name, spec_tuple_name(targs, nargs));
    return NULL;
}

static void printf_add_part(PrintfPlan* plan, int* cap, PrintfPart part) {
    if (plan->nparts >= *cap) return; // cap is format length + 1
    plan->parts[plan->nparts++] = part;
}

static int printf_expected_type(Ck* k, Node* arg, int kind, SType** out, const char** expected) {
    int errors = k->d->errors;
    SType* t = expr_type(k, arg);
    // A call deferred to end of file is still poison here; picking a helper
    // from it would match any specialization.
    if (t && t->kind == SK_ERR && k->d->errors == errors) {
        ck_err(k, arg, "fmt.printf argument type is not yet known; give the call typed arguments or assign its result first");
        return -1;
    }
    SType* want = NULL;
    switch (kind) {
    case PF_SIGNED: want = st_prim(P_I64); *expected = "int"; break;
    case PF_UNSIGNED: case PF_BINARY: case PF_OCTAL:
    case PF_HEX_LOWER: case PF_HEX_UPPER:
        if (t == T_UT_INT) {
            if (!ut_fits(k, arg, st_prim(P_U64))) return 0;
            arg->ty = t = st_prim(P_U64);
        }
        if (same_type(t, st_prim(P_U64)) || same_type(t, st_prim(P_USIZE))) {
            *out = t;
            *expected = "uint or usize";
            return 1;
        }
        *expected = "uint or usize";
        return 0;
    case PF_FIXED:
        if (t == T_UT_FLOAT) {
            if (!ut_fits(k, arg, st_prim(P_F64))) return 0;
            arg->ty = t = st_prim(P_F64);
        }
        if (same_type(t, st_prim(P_F32)) || same_type(t, st_prim(P_F64))) {
            *out = t;
            *expected = "f32 or f64";
            return 1;
        }
        *expected = "f32 or f64";
        return 0;
    case PF_STRING: want = st_prim(P_STRING); *expected = "string"; break;
    case PF_BYTE: want = st_prim(P_U8); *expected = "u8"; break;
    default: return 0;
    }
    if (t == T_UT_INT && is_int(want)) {
        if (!ut_fits(k, arg, want)) return 0;
        arg->ty = t = want;
    }
    if (same_type(t, want) || shrink_ok(t, want)) { // mut string prints as string
        *out = t;
        return 1;
    }
    return 0;
}

static SType* check_printf_call(Ck* k, Node* call, Sym* fn) {
    Node* callee = call->a;
    callee->sym = fn;
    callee->ty = fn->type;
    call->sym = fn;
    call->ival = BI_FMT_PRINTF;

    if (call->nkids < 1) {
        ck_err(k, call, "fmt.printf requires a compile-time-static format string");
        return T_ERR;
    }
    Node* format_arg = call->kids[0];
    if (format_arg->kind == N_MUTARG) {
        ck_err(k, format_arg, "fmt.printf format is read-only");
        format_arg = format_arg->a;
    }
    if (!coerce(k, format_arg, st_prim(P_STRING))) return T_ERR;
    Node* format = static_string_expr(k, format_arg);
    if (!format) {
        ck_err(k, format_arg, "fmt.printf format must be a string literal or static-literal const global");
        for (int i = 1; i < call->nkids; i++)
            (void)expr_type(k, call->kids[i]->kind == N_MUTARG
                               ? call->kids[i]->a : call->kids[i]);
        return T_ERR;
    }

    PrintfPlan* plan = (PrintfPlan*)arena_alloc(sizeof(PrintfPlan));
    memset(plan, 0, sizeof *plan);
    plan->format = format;
    int cap = (int)format->len + 1;
    plan->parts = (PrintfPart*)arena_alloc((size_t)cap * sizeof(PrintfPart));
    memset(plan->parts, 0, (size_t)cap * sizeof(PrintfPart));

    uint32_t literal = 0;
    int next_arg = 1;
    for (uint32_t i = 0; i < format->len;) {
        if ((unsigned char)format->str[i] != '%') { i++; continue; }
        if (i > literal)
            printf_add_part(plan, &cap, (PrintfPart){ .kind = PF_TEXT,
                .off = literal, .len = i - literal });
        uint32_t conv_off = i++;
        if (i >= format->len) {
            ck_err(k, format_arg, "incomplete fmt.printf conversion at decoded byte %u", conv_off);
            return T_ERR;
        }
        if (format->str[i] == '%') {
            printf_add_part(plan, &cap, (PrintfPart){ .kind = PF_PERCENT, .off = conv_off, .len = 1 });
            i++;
            literal = i;
            continue;
        }

        uint16_t precision = 6;
        int has_precision = 0;
        if (format->str[i] == '.') {
            has_precision = 1;
            i++;
            if (i < format->len && format->str[i] == '*') {
                ck_err(k, format_arg, "fmt.printf dynamic '*' precision is not supported (decoded byte %u)", conv_off);
                return T_ERR;
            }
            if (i >= format->len || format->str[i] < '0' || format->str[i] > '9') {
                ck_err(k, format_arg, "fmt.printf precision needs decimal digits at decoded byte %u", conv_off);
                return T_ERR;
            }
            uint64_t p = 0;
            while (i < format->len && format->str[i] >= '0' && format->str[i] <= '9') {
                if (p <= 1000000) p = p * 10 + (uint64_t)(format->str[i] - '0');
                i++;
            }
            if (p > 324) {
                ck_err(k, format_arg,
                       "fmt.printf precision %llu exceeds the fixed-buffer limit 324",
                       (unsigned long long)p);
                return T_ERR;
            }
            precision = (uint16_t)p;
            if (i >= format->len) {
                ck_err(k, format_arg, "incomplete fmt.printf conversion at decoded byte %u", conv_off);
                return T_ERR;
            }
        } else if (format->str[i] == '*' || format->str[i] == '+' ||
                   format->str[i] == '-' || format->str[i] == '#' ||
                   format->str[i] == ' ' ||
                   (format->str[i] >= '0' && format->str[i] <= '9')) {
            ck_err(k, format_arg,
                   "fmt.printf width, flags, '*', and positional arguments are not supported (decoded byte %u)",
                   conv_off);
            return T_ERR;
        }

        int kind;
        switch (format->str[i]) {
        case 'd': kind = PF_SIGNED; break;
        case 'u': kind = PF_UNSIGNED; break;
        case 'b': kind = PF_BINARY; break;
        case 'o': kind = PF_OCTAL; break;
        case 'x': kind = PF_HEX_LOWER; break;
        case 'X': kind = PF_HEX_UPPER; break;
        case 'f': kind = PF_FIXED; break;
        case 's': kind = PF_STRING; break;
        case 'c': kind = PF_BYTE; break;
        default:
            ck_err(k, format_arg,
                   "unsupported fmt.printf conversion '%%%c' at decoded byte %u",
                   format->str[i], conv_off);
            return T_ERR;
        }
        if (has_precision && kind != PF_FIXED) {
            ck_err(k, format_arg, "fmt.printf precision is supported only for %%f (decoded byte %u)", conv_off);
            return T_ERR;
        }
        printf_add_part(plan, &cap, (PrintfPart){ .kind = (uint8_t)kind,
            .precision = precision, .off = conv_off, .arg = next_arg++ });
        i++;
        literal = i;
    }
    if (literal < format->len)
        printf_add_part(plan, &cap, (PrintfPart){ .kind = PF_TEXT,
            .off = literal, .len = format->len - literal });

    int expected_count = next_arg - 1;
    int provided_count = call->nkids - 1;
    if (provided_count < expected_count) {
        PrintfPart* missing = NULL;
        for (int i = 0; i < plan->nparts; i++)
            if (plan->parts[i].arg == provided_count + 1) {
                missing = &plan->parts[i];
                break;
            }
        ck_err(k, call,
               "fmt.printf format requires %d value argument%s, got %d; missing argument %d for conversion at decoded byte %u",
               expected_count, expected_count == 1 ? "" : "s", provided_count,
               provided_count + 1, missing ? missing->off : 0);
    }
    else if (provided_count > expected_count)
        ck_err(k, call->kids[expected_count + 1],
               "extra fmt.printf argument %d is not consumed by the format",
               expected_count + 1);

    SType* counted_arg[1];
    counted_arg[0] = st_prim(P_STRING);
    Sym* text_helper = printf_helper(k, fn, "print_counted", counted_arg, 1, call);
    counted_arg[0] = st_prim(P_U8);
    Sym* byte_helper = printf_helper(k, fn, "print_counted", counted_arg, 1, call);
    for (int i = 0; i < plan->nparts; i++) {
        PrintfPart* part = &plan->parts[i];
        if (part->kind == PF_TEXT) { part->helper = text_helper; continue; }
        if (part->kind == PF_PERCENT) { part->helper = byte_helper; continue; }
        if (part->arg >= call->nkids) continue;
        Node* arg = call->kids[part->arg];
        if (arg->kind == N_MUTARG) {
            ck_err(k, arg, "fmt.printf value arguments are read-only");
            arg = arg->a;
        }
        SType* actual = NULL;
        const char* expected = "value";
        int typed = printf_expected_type(k, arg, part->kind, &actual, &expected);
        if (typed < 0) continue;
        if (!typed) {
            SType* got = node_ty(arg);
            ck_err(k, arg, "fmt.printf argument %d for conversion at decoded byte %u requires %s, got %s",
                   part->arg, part->off, expected,
                   got ? type_name(got) : "unknown");
            continue;
        }
        const char* generic = "print_counted";
        SType* helper_arg[1] = { actual };
        switch (part->kind) {
        case PF_BINARY: case PF_OCTAL: case PF_HEX_LOWER: case PF_HEX_UPPER:
            generic = "print_radix";
            break;
        case PF_FIXED:
            generic = "print_precision";
            break;
        default: break;
        }
        part->helper = printf_helper(k, fn, generic, helper_arg, 1, call);
    }
    for (int i = expected_count + 1; i < call->nkids; i++) {
        Node* arg = call->kids[i]->kind == N_MUTARG ? call->kids[i]->a : call->kids[i];
        if (!node_ty(arg)) (void)expr_type(k, arg);
    }

    call->aux = plan;
    return st_prim(P_I64);
}

/*
 * Resolve a method call on struct receiver type b. With final == 0 a miss
 * returns NULL so the caller can defer the call to end of file, when every
 * method is declared; with final == 1 a miss is the "no method" error.
 */
static SType* check_method_call(Ck* k, Node* call, SType* b, int qualified, int final) {
    Node* callee = call->a;
    uint32_t key_len;
    const char* key = method_key(b->decl, callee->str, callee->len, &key_len);
    Sym* head = lookup_val(k, key, key_len);
    int supplied = call->nkids + (qualified ? 0 : 1);
    Sym* found = NULL;
    for (Sym* f = head; f; f = f->overload) {
        SType* ft = f->is_generic && b->nargs
            ? build_func_type(k, f->decl, b->args, b->nargs)
            : f->type;
        if (!ft || (f->is_generic && !b->nargs) || ft->nparams != supplied ||
            !same_type(ft->params[0], b)) continue;
        int rmut = (f->decl->flags & F_MUT) != 0;
        int cmut = qualified
            ? (call->nkids > 0 && call->kids[0]->kind == N_MUTARG)
            : ((call->flags & F_MUT) != 0);
        if (rmut != cmut) continue;
        int exact = 1;
        for (int i = 1; i < ft->nparams; i++) {
            Node* arg = call->kids[qualified ? i : i - 1];
            SType* at = overload_arg_type(k, arg);
            int am = arg->kind == N_MUTARG;
            int pm = (f->decl->kids[i]->flags & F_MUT) != 0;
            if (am != pm) exact = 0;
            else if (!exact_arg_type(arg, at, ft->params[i]) &&
                     !shrink_ok(at, ft->params[i])) exact = 0;
        }
        if (!exact && overload_count(head) > 1) continue;
        if (found) {
            ck_err(k, call, "ambiguous method call '%.*s'", (int)callee->len, callee->str);
            return T_ERR;
        }
        found = f;
    }
    /* A directly owned method wins. Otherwise, one explicitly embedded
     * field may promote an exact method by rewriting the receiver to the
     * ordinary field expression that codegen already understands. */
    if (!found && !qualified && b->decl) {
        Sym* promoted = NULL;
        SType* promoted_ft = NULL;
        Node* promoted_field = NULL;
        for (int fi = 0; fi < b->nfields; fi++) {
            Node* fd = b->decl->kids[fi];
            SType* eb = b->fields[fi].t;
            if (!(fd->flags & F_EMBED) || !eb || eb->kind != SK_STRUCT) continue;
            uint32_t ekey_len;
            const char* ekey = method_key(eb->decl, callee->str, callee->len, &ekey_len);
            Sym* ehead = lookup_val(k, ekey, ekey_len);
            for (Sym* f = ehead; f; f = f->overload) {
                SType* ft = f->is_generic && eb->nargs
                    ? build_func_type(k, f->decl, eb->args, eb->nargs)
                    : f->type;
                if (!ft || (f->is_generic && !eb->nargs) ||
                    ft->nparams != call->nkids + 1 ||
                    !same_type(ft->params[0], eb)) continue;
                int rmut = (f->decl->flags & F_MUT) != 0;
                int cmut = (call->flags & F_MUT) != 0;
                if (rmut != cmut) continue;
                int exact = 1;
                for (int i = 1; i < ft->nparams; i++) {
                    Node* arg = call->kids[i - 1];
                    SType* at = overload_arg_type(k, arg);
                    int am = arg->kind == N_MUTARG;
                    int pm = (f->decl->kids[i]->flags & F_MUT) != 0;
                    if (am != pm) exact = 0;
                    else if (!exact_arg_type(arg, at, ft->params[i]) &&
                             !shrink_ok(at, ft->params[i]))
                        exact = 0;
                }
                if (!exact && overload_count(ehead) > 1) continue;
                if (promoted) {
                    ck_err(k, call, "ambiguous promoted method call '%.*s'",
                           (int)callee->len, callee->str);
                    return T_ERR;
                }
                promoted = f;
                promoted_ft = ft;
                promoted_field = fd;
            }
        }
        if (promoted) {
            Node* recv = (Node*)arena_alloc(sizeof(Node));
            memset(recv, 0, sizeof *recv);
            recv->kind = N_FIELD_E;
            recv->path = callee->path; recv->line = callee->line;
            recv->col = callee->col;
            recv->a = callee->a;
            recv->str = promoted_field->str;
            recv->len = promoted_field->len;
            recv->ty = promoted_ft->params[0];
            callee->a = recv;
            found = promoted;
            b = promoted_ft->params[0];
        }
    }
    if (found) {
        SType* method_ft = found->is_generic ? instantiate_generic_func_args(k, found, b->args, b->nargs, call)
            : found->type;
        int rmut = (found->decl->flags & F_MUT) != 0;
        Node* recv = qualified ? (call->kids[0]->kind == N_MUTARG ? call->kids[0]->a : call->kids[0])
            : callee->a;
        if (rmut) check_lvalue(k, recv, 1);
        else coerce(k, recv, b);
        int start = qualified ? 1 : 0;
        for (int i = start; i < call->nkids; i++) {
            Node* arg = call->kids[i];
            int pi = i + (qualified ? 0 : 1);
            coerce(k, arg->kind == N_MUTARG ? arg->a : arg, method_ft->params[pi]);
        }
        if (found->decl->path && call->path &&
            strcmp(found->decl->path, call->path) != 0 && !found->is_pub)
            ck_err(k, call, "method '%.*s' is private", (int)callee->len, callee->str);
        call->sym = found;
        callee->ty = method_ft;
        if (qualified) call->flags |= F_METHOD; // emitter: receiver is argument zero
        return method_ft->ret;
    }
    if (!final) return NULL; // a later declaration may add the method
    ck_err(k, call, "no method '%.*s' for %s", (int)callee->len, callee->str, type_name(b));
    return T_ERR;
}

/*
 * A generic owner (Map[K, V]::new(...)) needs each candidate's signature
 * rebuilt under the owner's actual type arguments before matching, exactly
 * like check_method_call's generic branch — mirrored here without the
 * receiver-shaped logic (rmut/cmut, embedding promotion) that doesn't apply
 * to a receiverless call.
 */
static SType* check_generic_assoc_call(Ck* k, Node* call, SType* owner, Sym* head, int final) {
    Node* callee = call->a;
    Sym* found = NULL;
    for (Sym* f = head; f; f = f->overload) {
        SType* ft = f->is_generic ? build_func_type(k, f->decl, owner->args, owner->nargs) : f->type;
        if (!ft || ft->nparams != call->nkids) continue;
        int exact = 1;
        for (int i = 0; i < ft->nparams; i++) {
            Node* arg = call->kids[i];
            SType* at = overload_arg_type(k, arg);
            int am = arg->kind == N_MUTARG;
            int pm = (f->decl->kids[i]->flags & F_MUT) != 0;
            if (am != pm) exact = 0;
            else if (!exact_arg_type(arg, at, ft->params[i]) && !shrink_ok(at, ft->params[i])) exact = 0;
        }
        if (!exact && overload_count(head) > 1) continue;
        if (found) {
            ck_err(k, call, "ambiguous associated-function call '%.*s'", (int)callee->len, callee->str);
            return T_ERR;
        }
        found = f;
    }
    if (found) {
        // instantiate_generic_func_args (not build_func_type) memoizes the
        // instantiation and stamps its body once — emit later walks that same
        // memo to generate one function per used (owner-args) tuple.
        SType* found_ft = found->is_generic
            ? instantiate_generic_func_args(k, found, owner->args, owner->nargs, call)
            : found->type;
        if (found_ft->kind == SK_ERR) return T_ERR;
        for (int i = 0; i < call->nkids; i++) {
            Node* arg = call->kids[i];
            coerce(k, arg->kind == N_MUTARG ? arg->a : arg, found_ft->params[i]);
        }
        if (found->decl->path && call->path &&
            strcmp(found->decl->path, call->path) != 0 && !found->is_pub)
            ck_err(k, call, "associated function '%.*s' is private", (int)callee->len, callee->str);
        call->sym = found;
        callee->ty = found_ft;
        call->flags |= F_ASSOC;
        return found_ft->ret;
    }
    if (!final) return NULL;
    ck_err(k, call, "no associated function '%.*s' for %s", (int)callee->len, callee->str, type_name(owner));
    return T_ERR;
}

/*
 * Associated-function calls (Type::name(...)) have no receiver. A non-generic
 * owner resolves exactly like an ordinary direct call once found under the
 * owner-namespaced key — select_call_overload/finish_direct_call already
 * handle overload selection, literal defaulting, and the final-pass retry
 * without any receiver-shaped logic to strip out. A generic owner needs
 * per-candidate signature substitution instead (check_generic_assoc_call).
 */
static SType* check_assoc_call(Ck* k, Node* call, SType* owner, int final) {
    Node* callee = call->a;
    uint32_t key_len;
    const char* key = method_key(owner->decl, callee->str, callee->len, &key_len);
    Sym* head = lookup_val(k, key, key_len);
    if (owner->nargs) return check_generic_assoc_call(k, call, owner, head, final);
    int ambiguous = 0;
    Sym* found = select_call_overload(k, call, head, final, &ambiguous);
    if (ambiguous) {
        ck_err(k, call, "ambiguous associated-function call '%.*s'", (int)callee->len, callee->str);
        return T_ERR;
    }
    if (found) {
        if (found->decl->path && call->path &&
            strcmp(found->decl->path, call->path) != 0 && !found->is_pub)
            ck_err(k, call, "associated function '%.*s' is private", (int)callee->len, callee->str);
        finish_direct_call(k, call, found);
        call->flags |= F_ASSOC; // emitter: no receiver to prepend, unlike an unqualified method call
        return found->type->ret;
    }
    if (!final) return NULL; // a later declaration may add it
    ck_err(k, call, "no associated function '%.*s' for %s", (int)callee->len, callee->str, type_name(owner));
    return T_ERR;
}

static SType* check_call(Ck* k, Node* call) {
    Node* callee = call->a;
    /* Pointer types are conversion callees only for the exact usize-address
     * boundary.  Writable construction is an explicit authority assertion. */
    if (callee->kind == N_PTR) {
        SType* want = build_type(k, callee);
        if (call->nkids != 1) {
            ck_err(k, call, "a data-pointer conversion takes exactly one usize address");
            return T_ERR;
        }
        if (!layout(want->base) || !want->base->size) {
            ck_err(k, call,
                   "data-pointer conversion requires a complete, nonzero-sized pointee, got %s",
                   type_name(want->base));
            return T_ERR;
        }
        SType* from = expr_type(k, call->kids[0]);
        if (from->kind == SK_ERR) return T_ERR;
        if (from->kind != SK_PRIM || from->prim != P_USIZE) {
            ck_err(k, call, "data-pointer conversion requires usize, got %s", type_name(from));
            return T_ERR;
        }
        uint64_t address;
        if (usize_const(call->kids[0], &address)) {
            if (address && address % want->base->align) {
                ck_err(k, call->kids[0],
                       "address 0x%llx is not aligned for %s",
                       (unsigned long long)address, type_name(want->base));
                return T_ERR;
            }
            call->flags |= F_BOUNDS_PROVEN;
        }
        callee->ty = want;
        return want;
    }
    // direct name: call, cast, or forward (pending) call
    if (callee->kind == N_IDENT) {
        Sym* s = lookup_val(k, callee->str, callee->len);
        if (!s) {
            if (callee->len == 6 && memcmp(callee->str, "typeid", 6) == 0) {
                if (call->nkids != 1) {
                    ck_err(k, call, "typeid takes one exact named type");
                    return T_ERR;
                }
                SType* t = typeid_argument(k, call->kids[0]);
                if (!t || t->kind == SK_ERR) return T_ERR;
                call->ival = type_identity(t);
                callee->ty = st_prim(P_TYPEID);
                return st_prim(P_TYPEID);
            }
            SType* prim = prim_by_name(callee->str, callee->len);
            Sym* ts = lookup_type(k, callee->str, callee->len);
            if (prim || ts) { // constructor-shaped cast or contract construction
                if (ts && ts->type && ts->type->kind == SK_ERROR_DOMAIN) return check_error_construct(k, call, ts);
                if (call->nkids != 1) {
                    ck_err(k, call, "a cast takes exactly one argument");
                    return T_ERR;
                }
                SType* want = prim ? prim : build_type(k, callee);
                if (want->kind == SK_CONTRACT) {
                    callee->sym = ts;
                    callee->ty = want;
                    return check_contract_construct(k, call, want);
                }
                if (want->kind == SK_UNION) {
                    ck_err(k, call,
                           "unions have no constructor call — construct a variant: %s.<variant>(...)",
                           type_name(want));
                    return T_ERR;
                }
                SType* from = expr_type(k, call->kids[0]);
                if (want->kind == SK_ERR || from->kind == SK_ERR) return T_ERR;
                // Preserve writable permission through an explicit legacy
                // spelling cast between the shared byte-view aliases.
                if (want->kind == SK_PRIM && want->prim == P_STRING &&
                    from->kind == SK_SLICE && from->base == st_prim(P_U8) &&
                    from->is_mut)
                    want = T_MUT_STRING;
                if (want->kind == SK_PRIM && want->prim == P_USIZE && from->kind == SK_PTR) {
                    /* exact address extraction, including null */
                } else if (!castable(from, want)) {
                    ck_err(k, call, "cannot cast %s to %s", type_name(from), type_name(want));
                    return T_ERR;
                }
                // A compound untyped constant under a float cast is settled
                // here: float math at the target type, integer math as int.
                if ((from == T_UT_FLOAT || from == T_UT_INT) &&
                    (is_float(want) || (want->kind == SK_DISTINCT && is_float(want->base))) &&
                    (call->kids[0]->kind == N_UNARY || call->kids[0]->kind == N_BINARY) &&
                    !coerce(k, call->kids[0], from == T_UT_FLOAT ? want : st_prim(P_I64)))
                    return T_ERR;
                callee->sym = ts; // named-type cast marker
                callee->ty = want; // primitive cast marker
                if (from == T_UT_INT &&
                    !ut_fits(k, call->kids[0], want->kind == SK_ENUM ? want : want->kind == SK_DISTINCT ? (is_int(want->base) ? want->base : st_prim(P_I64)) : want))
                    return T_ERR;
                return want;
            }
            if (callee->len == 5 && memcmp(callee->str, "blank", 5) == 0) {
                // blank(T): the canonical zero value of any laid-out type T
                if (call->nkids != 1) {
                    ck_err(k, call, "blank takes one type name");
                    return T_ERR;
                }
                SType* t = build_type(k, call->kids[0]);
                if (!t || t->kind == SK_ERR) return T_ERR;
                if (t->kind == SK_FORWARD) {
                    ck_err(k, call->kids[0], "type '%s' is not declared yet; blank requires its layout", type_name(t));
                    return T_ERR;
                }
                if (t->kind == SK_ERROR_DOMAIN) {
                    ck_err(k, call->kids[0], "error-domain names have no value layout; use Error");
                    return T_ERR;
                }
                if (!layout(t)) return T_ERR;
                return t;
            }
            if (callee->len == 6 && memcmp(callee->str, "sizeof", 6) == 0) {
                if (call->nkids == 1) {
                    Node* arg = call->kids[0];
                    SType* t = NULL;
                    if (arg->kind == N_IDENT) {
                        t = prim_by_name(arg->str, arg->len);
                        if (!t) {
                            Sym* s2 = lookup_type(k, arg->str, arg->len);
                            if (s2) t = s2->type;
                        }
                    } else { // inline type: []T, [N]T, *T, ...
                        t = build_type(k, arg);
                        if (t->kind == SK_ERR) return T_ERR;
                    }
                    if (t && t->kind == SK_FORWARD) {
                        ck_err(k, arg, "type '%s' is not declared yet; sizeof requires its layout", type_name(t));
                        return T_ERR;
                    }
                    if (t && t->kind == SK_ERROR_DOMAIN) {
                        ck_err(k, arg, "error-domain names have no value layout; use Error");
                        return T_ERR;
                    }
                    if (t && t->kind != SK_ERR) {
                        if (!layout(t)) return T_ERR;
                        call->ival = t->size; // constant for the emitter
                        return st_prim(P_USIZE);
                    }
                }
                ck_err(k, call, "sizeof takes one type name");
                return T_ERR;
            }
            if (callee->len == 7 && memcmp(callee->str, "alignof", 7) == 0) {
                if (call->nkids == 1) {
                    Node* arg = call->kids[0];
                    SType* t = NULL;
                    if (arg->kind == N_IDENT) {
                        t = prim_by_name(arg->str, arg->len);
                        if (!t) {
                            Sym* s2 = lookup_type(k, arg->str, arg->len);
                            if (s2) t = s2->type;
                        }
                    } else { // inline type: []T, [N]T, *T, ...
                        t = build_type(k, arg);
                        if (t->kind == SK_ERR) return T_ERR;
                    }
                    if (t && t->kind == SK_FORWARD) {
                        ck_err(k, arg, "type '%s' is not declared yet; alignof requires its layout", type_name(t));
                        return T_ERR;
                    }
                    if (t && t->kind == SK_ERROR_DOMAIN) {
                        ck_err(k, arg, "error-domain names have no value layout; use Error");
                        return T_ERR;
                    }
                    if (t && t->kind != SK_ERR) {
                        if (!layout(t)) return T_ERR;
                        call->ival = t->align; // constant for the emitter
                        return st_prim(P_USIZE);
                    }
                }
                ck_err(k, call, "alignof takes one type name");
                return T_ERR;
            }
            if (callee->len == 8 && memcmp(callee->str, "offsetof", 8) == 0) {
                if (call->nkids != 2) {
                    ck_err(k, call, "offsetof takes one struct type and one field path");
                    return T_ERR;
                }
                SType* t = build_type(k, call->kids[0]);
                if (!t || t->kind == SK_ERR) return T_ERR;
                if (t->kind == SK_FORWARD) {
                    ck_err(k, call->kids[0], "offsetof requires a complete struct type");
                    return T_ERR;
                }
                Node* parts[64]; int np = 0;
                Node* path = call->kids[1];
                while (path && path->kind == N_FIELD_E && np < 64) {
                    parts[np++] = path; path = path->a;
                }
                if (path && path->kind == N_IDENT && np < 64) parts[np++] = path;
                uint64_t total = 0;
                for (int pi = np - 1; pi >= 0; pi--) {
                    if (t->kind != SK_STRUCT) {
                        ck_err(k, parts[pi], "offsetof field paths traverse inline structs only");
                        return T_ERR;
                    }
                    int found = -1;
                    for (int fi = 0; fi < t->nfields; fi++) {
                        if (strlen(t->fields[fi].name) == parts[pi]->len &&
                            memcmp(t->fields[fi].name, parts[pi]->str, parts[pi]->len) == 0) {
                            found = fi; break;
                        }
                    }
                    if (found < 0) {
                        ck_err(k, parts[pi], "type %s has no field '%.*s'", type_name(t),
                               (int)parts[pi]->len, parts[pi]->str);
                        return T_ERR;
                    }
                    total += t->fields[found].offset; t = t->fields[found].t;
                }
                call->ival = total;
                return st_prim(P_USIZE);
            }
            // reserved builtin: assert(condition: bool) — checked-only
            if (is_assert_name(callee->str, callee->len)) {
                if (call->nkids != 1) {
                    ck_err(k, call, "assert takes exactly one bool condition, got %d arguments", call->nkids);
                    return T_ERR;
                }
                if (call->kids[0]->kind == N_MUTARG) {
                    ck_err(k, call->kids[0], "'mut' marks mut parameters; assert's condition is read-only");
                    return T_ERR;
                }
                if (!coerce(k, call->kids[0], st_prim(P_BOOL))) return T_ERR;
                int known = 0;
                if (bool_const(call->kids[0], &known) && !known) {
                    ck_err(k, call->kids[0], "assert condition is always false");
                    return T_ERR;
                }
                call->ival = known ? 3 : 2; // 3: proven true and fully erased
                ensure_memory_types();
                return T_VOID;
            }
            if (is_static_assert_name(callee->str, callee->len)) {
                if (call->nkids != 1) {
                    ck_err(k, call, "static_assert takes exactly one bool condition");
                    return T_ERR;
                }
                if (!coerce(k, call->kids[0], st_prim(P_BOOL))) return T_ERR;
                int known;
                if (!bool_const(call->kids[0], &known)) {
                    ck_err(k, call->kids[0], "static_assert condition is not compile-time constant");
                    return T_ERR;
                }
                if (!known) {
                    ck_err(k, call->kids[0], "static assertion failed");
                    return T_ERR;
                }
                call->ival = 1;
                return T_VOID;
            }
            // reserved builtin: panic(message: string) — never returns
            if (is_panic_name(callee->str, callee->len)) {
                if (call->nkids != 1) {
                    ck_err(k, call, "panic takes exactly one string message, got %d arguments", call->nkids);
                    return T_ERR;
                }
                if (call->kids[0]->kind == N_MUTARG) {
                    ck_err(k, call->kids[0], "'mut' marks mut parameters; panic's message is read-only");
                    return T_ERR;
                }
                if (!coerce(k, call->kids[0], st_prim(P_STRING))) return T_ERR;
                call->ival = 1; // emitter: sema-approved panic call marker
                return T_ERR;   // never returns; poison keeps enclosing uses quiet
            }
            // forward top-level call — deferred like an emitter fixup
            Pending* pd = new_pending(k);
            pd->call = call;
            return T_ERR; // poison; existence + args verified at end of file
        }
        if (s->kind == SYM_FUNC) {
            // raw byte-form intrinsics only; typed forms need [T] and are
            // checked in the generic-instantiation path below
            Sym* mi = NULL;
            for (Sym* it = s; it; it = it->overload)
                if (it->decl &&
                    ((it->decl->ival >= BI_MEM_ALLOC &&
                      it->decl->ival <= BI_MEM_COMPARE) ||
                     it->decl->ival == BI_MEM_EQUAL ||
                     it->decl->ival == BI_MEM_ALLOC_SIZE)) {
                    mi = it;
                    break;
                }
            if (mi) return check_memory_call(k, call, mi, (int)mi->decl->ival);
        }
        if (s->kind == SYM_FUNC && s->decl && s->decl->ival == BI_FMT_PRINTF) return check_printf_call(k, call, s);
        if (s->kind == SYM_VAR && s->type && s->type->kind == SK_FUNC) {
            check_args(k, call, s->type, 0);
            callee->sym = s;
            callee->ty = s->type;
            call->sym = s; // indirect call through a function value
            return s->type->ret;
        }
        if (s->kind != SYM_FUNC) {
            ck_err(k, call, "'%.*s' is not callable", (int)callee->len, callee->str);
            return T_ERR;
        }
        int has_plain = 0;
        for (Sym* it = s; it; it = it->overload) has_plain |= !it->is_generic;
        if (!has_plain) {
            ck_err(k, call, "generic function '%.*s' needs type arguments: %.*s[T](...)",
                   (int)callee->len, callee->str, (int)callee->len, callee->str);
            return T_ERR;
        }
        if (s->is_extern && !s->overload) {
            finish_direct_call(k, call, s);
            return s->type->ret;
        }
        int ambiguous = 0;
        Sym* chosen = select_call_overload(k, call, s, 0, &ambiguous);
        if (ambiguous) {
            ck_err(k, call, "ambiguous call to overloaded function '%.*s'", (int)callee->len, callee->str);
            return T_ERR;
        }
        if (chosen) {
            finish_direct_call(k, call, chosen);
            return chosen->type->ret;
        }
        Pending* pd = new_pending(k);
        pd->call = call;
        return T_ERR; // a matching overload may be declared later
    }
    // Slice-shaped identity cast or explicit pointer-plus-count construction.
    if (callee->kind == N_SLICE_T) {
        SType* want = build_type(k, callee);
        if (want->kind == SK_ERR) return T_ERR;
        if (call->nkids == 2) {
            if (!layout(want->base) || !want->base->size) {
                ck_err(k, call,
                       "slice construction requires a complete, nonzero-sized element type, got %s",
                       type_name(want->base));
                return T_ERR;
            }
            SType* pointer;
            if (call->kids[0]->kind == N_NULL) {
                pointer = st_new(SK_PTR);
                pointer->base = want->base;
                pointer->is_mut = want->is_mut;
                call->kids[0]->ty = pointer;
            } else {
                pointer = expr_type(k, call->kids[0]);
            }
            SType* count = expr_type(k, call->kids[1]);
            if (pointer->kind == SK_ERR || count->kind == SK_ERR) return T_ERR;
            int exact_pointee = pointer->kind == SK_PTR && same_type(pointer->base, want->base);
            int permission = exact_pointee && (!want->is_mut || pointer->is_mut);
            if (!permission) {
                ck_err(k, call->kids[0],
                       "%s construction requires an exact %spointer to %s, got %s",
                       type_name(want), want->is_mut ? "writable " : "",
                       type_name(want->base), type_name(pointer));
                return T_ERR;
            }
            if (count->kind != SK_PRIM || count->prim != P_USIZE) {
                ck_err(k, call->kids[1], "slice element count must be usize, got %s", type_name(count));
                return T_ERR;
            }
            uint64_t n, address;
            int kn = usize_const(call->kids[1], &n);
            int ka = address_const(call->kids[0], &address);
            if (kn && n > UINT64_MAX / want->base->size) {
                ck_err(k, call->kids[1], "slice byte extent count * sizeof(%s) overflows usize", type_name(want->base));
                return T_ERR;
            }
            if (ka && kn && n && !address) {
                ck_err(k, call, "null pointer cannot construct a non-empty slice");
                return T_ERR;
            }
            if (ka && kn && n && address > UINT64_MAX - n * want->base->size) {
                ck_err(k, call, "slice end address overflows usize");
                return T_ERR;
            }
            if ((kn && n == 0) || (ka && kn)) call->flags |= F_BOUNDS_PROVEN;
            callee->ty = want;
            return want;
        }
        if (call->nkids != 1) {
            ck_err(k, call, "a slice cast takes one argument; construction takes pointer and count");
            return T_ERR;
        }
        Node* arg = call->kids[0];
        if (arg->kind == N_MUTARG) {
            ck_err(k, arg, "'mut' marks mut parameters; a cast takes no call-site 'mut'");
            return T_ERR;
        }
        SType* from = expr_type(k, arg);
        if (from->kind == SK_ERR) return T_ERR;
        if (!same_type(from, want) && !shrink_ok(from, want)) {
            ck_err(k, call, "cannot cast %s to %s", type_name(from), type_name(want));
            return T_ERR;
        }
        callee->ty = want; // emitter: slice-type cast marker
        return from->is_mut ? T_MUT_BYTE_SLICE : want;
    }
    // generic instantiation call: max[int](3, 9)
    if (callee->kind == N_INDEX && callee->a->kind == N_IDENT) {
        SType* named = indexed_named_type(k, callee);
        if (named && named->kind == SK_CONTRACT) {
            callee->ty = named;
            return check_contract_construct(k, call, named);
        }
        Sym* head = lookup_val(k, callee->a->str, callee->a->len);
        if (head && head->kind == SYM_FUNC) {
            int nargs = callee->b ? 1 : callee->nkids;
            SType** targs = (SType**)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(SType*));
            for (int i = 0; i < nargs; i++) targs[i] = build_type(k, callee->b ? callee->b : callee->kids[i]);
            SType** actual = (SType**)arena_alloc((size_t)(call->nkids ? call->nkids : 1) * sizeof(SType*));
            for (int i = 0; i < call->nkids; i++) actual[i] = overload_arg_type(k, call->kids[i]);
            // Generics #10: selection is exact — an instantiation resolves to
            // the specialization whose type-argument tuple equals the written
            // arguments, ahead of template stamping.
            Sym* spec = NULL;
            Sym* sole_spec = NULL;
            int nspec = 0;
            for (Sym* s = head; s; s = s->overload) {
                if (!s->spec_args || s->spec_nargs != nargs) continue;
                int same = 1;
                for (int i = 0; i < nargs && same; i++)
                    same = same_type(s->spec_args[i], targs[i]);
                if (!same) continue;
                nspec++;
                sole_spec = s;
                if (!call_exact_ft(call, s->decl, s->type, actual)) continue;
                if (spec) {
                    ck_err(k, call,
                           "ambiguous call to specialized overload '%.*s'",
                           (int)callee->a->len, callee->a->str);
                    return T_ERR;
                }
                spec = s;
            }
            if (!spec && nspec == 1 &&
                sole_spec->type->nparams == call->nkids)
                spec = sole_spec; // sole tuple match: literals coerce below
            if (spec) {
                finish_direct_call(k, call, spec);
                return spec->type->ret;
            }
            Sym* chosen = NULL;
            Sym* sole = NULL;
            int eligible = 0;
            for (Sym* s = head; s; s = s->overload) {
                if (!s->is_generic || !s->decl->c || s->decl->c->nkids != nargs)
                    continue;
                eligible++;
                sole = s;
                SType* candidate = build_func_type(k, s->decl, targs, nargs);
                if (!call_exact_ft(call, s->decl, candidate, actual)) continue;
                if (chosen) {
                    ck_err(k, call, "ambiguous call to generic overload '%.*s'",
                           (int)callee->a->len, callee->a->str);
                    return T_ERR;
                }
                chosen = s;
            }
            if (!chosen && eligible == 1) chosen = sole;
            if (!chosen) {
                ck_err(k, call, "no matching generic overload '%.*s' for these arguments",
                       (int)callee->a->len, callee->a->str);
                return T_ERR;
            }
            // Generics #13: a 'specialized' template has no default body, so
            // an unmatched type tuple errors at the call site naming the
            // missing type tuple and the declaration.
            if (chosen->decl->flags & F_SPECIALIZED) {
                ck_err(k, call,
                       "call '%.*s%s' matches no specialization — '%.*s' is declared 'specialized' (no default body) at line %d",
                       (int)callee->a->len, callee->a->str,
                       spec_tuple_name(targs, nargs),
                       (int)callee->a->len, callee->a->str,
                       chosen->decl->line);
                return T_ERR;
            }
            // typed allocation intrinsics: checked and lowered directly, no stamp
            if (chosen->decl->ival == BI_MEM_ALLOC_T ||
                chosen->decl->ival == BI_MEM_ZALLOC_T ||
                chosen->decl->ival == BI_MEM_REALLOC_T ||
                chosen->decl->ival == BI_MEM_RELEASE_T)
                return check_memory_generic_call(k, call, chosen, targs[0],
                                                 (int)chosen->decl->ival);
            SType* ft = instantiate_generic_func(k, chosen, callee);
            if (ft->kind == SK_ERR) return T_ERR;
            check_args(k, call, ft, 0);
            check_mut_args(k, call, chosen->decl);
            callee->sym = chosen; // the selected generic declaration
            callee->ty = ft;  // the stamped signature — args recovered via the
            return ft->ret;   // instantiation memo on s->decl's tparams node
        }
        ck_err(k, call, "'%.*s' is not a generic function", (int)callee->a->len, callee->a->str);
        return T_ERR;
    }
    // associated-function dispatch (Type::name(...)) — owner is always a
    // type, never a value; '::' does not share '.'s value/type ambiguity.
    // The owner may itself be generic (Map[K, V]::new(...)) via the ordinary
    // indexed-type-name resolution shared with typeid and union-variant use.
    if (callee->kind == N_FIELD_E && (callee->flags & F_ASSOC)) {
        SType* owner = NULL;
        if (callee->a->kind == N_IDENT) {
            Sym* ts = lookup_type(k, callee->a->str, callee->a->len);
            if (ts && ts->is_generic) {
                ck_err(k, call, "generic type '%.*s' needs explicit type arguments",
                       (int)callee->a->len, callee->a->str);
                return T_ERR;
            }
            if (ts && ts->type && ts->type->kind == SK_STRUCT) owner = ts->type;
        } else if (callee->a->kind == N_INDEX) {
            owner = indexed_named_type(k, callee->a);
            if (owner && owner->kind == SK_ERR) return T_ERR; // already diagnosed
        }
        if (!owner || owner->kind != SK_STRUCT) {
            if (callee->a->kind == N_IDENT && lookup_val(k, callee->a->str, callee->a->len))
                ck_err(k, call, "'::' follows a type name, not a value — did you mean '.'?");
            else if (callee->a->kind == N_IDENT)
                ck_err(k, call, "'%.*s' is not a declared struct", (int)callee->a->len, callee->a->str);
            else
                ck_err(k, call, "'::' owner is not a declared generic struct");
            return T_ERR;
        }
        // stashed for the emitter: a generic owner has no receiver parameter
        // to read its type arguments back from, unlike a method call
        callee->a->ty = owner;
        SType* r = check_assoc_call(k, call, owner, 0);
        if (r) return r;
        Pending* pd = new_pending(k);
        pd->call = call;
        pd->recv = owner;
        pd->assoc = 1;
        return T_ERR; // an associated function may be declared later
    }
    // method / contract dispatch
    if (callee->kind == N_FIELD_E) {
        // union variant construction: Event.key(42), Pair[int].ok(v)
        if ((callee->a->kind == N_IDENT &&
             !lookup_val(k, callee->a->str, callee->a->len)) ||
            callee->a->kind == N_INDEX) {
            SType* ub = union_base_type(k, callee->a);
            if (ub == T_ERR) return T_ERR;
            if (ub) {
                int vi = union_variant_index(k, call, ub, callee);
                if (vi < 0) return T_ERR;
                SType* pt = ub->fields[vi].t;
                if (!pt) {
                    ck_err(k, call,
                           "variant '%.*s' carries no payload — use the plain value %.*s.%.*s",
                           (int)callee->len, callee->str, (int)callee->a->len,
                           callee->a->kind == N_IDENT ? callee->a->str : callee->a->a->str,
                           (int)callee->len, callee->str);
                    return T_ERR;
                }
                if (call->nkids != 1) {
                    ck_err(k, call, "variant construction takes exactly one payload value");
                    return T_ERR;
                }
                if (call->kids[0]->kind == N_MUTARG) {
                    ck_err(k, call->kids[0], "'mut' marks mut parameters; variant payloads are read-only");
                    return T_ERR;
                }
                coerce_payload(k, call->kids[0], pt);
                callee->ty = ub;
                callee->ival = (uint64_t)vi; // emitter: variant index (the tag)
                return ub;
            }
        }
        if (callee->a->kind == N_IDENT) {
            Sym* ms = lookup_val(k, callee->a->str, callee->a->len);
            if (ms && ms->kind == SYM_MODULE) {
                ck_err(k, call, "module member '%.*s.%.*s' needs the stdlib (not yet linked)",
                       (int)callee->a->len, callee->a->str, (int)callee->len, callee->str);
                return T_ERR;
            }
        }
        int qualified = 0;
        SType* b = NULL;
        if (callee->a->kind == N_IDENT && !lookup_val(k, callee->a->str, callee->a->len)) {
            Sym* ts = lookup_type(k, callee->a->str, callee->a->len);
            if (ts && ts->type && ts->type->kind == SK_STRUCT) {
                b = ts->type;
                qualified = 1;
            }
        }
        if (!b) b = expr_type(k, callee->a);
        if (b->kind == SK_ERR) return T_ERR;
        if (b->kind == SK_CONTRACT) {
            Node* m = contract_method(b, callee->str, callee->len);
            if (!m) {
                ck_err(k, call, "contract %s has no method '%.*s'", type_name(b), (int)callee->len, callee->str);
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
            if (mmut) {
                check_lvalue(k, callee->a, 1);
                if (callee->a->kind == N_IDENT) {
                    Sym* iv = lookup_val(k, callee->a->str, callee->a->len);
                    if (iv && iv->is_const_backed)
                        ck_err(k, call, "cannot call a mutating contract method on const backing storage");
                }
            }
            SType* ft = contract_method_type(k, b, m);
            check_args(k, call, ft, 0);
            return ft->ret;
        }
        if (b->kind != SK_STRUCT) {
            ck_err(k, call, "no method '%.*s' for %s", (int)callee->len, callee->str, type_name(b));
            return T_ERR;
        }
        SType* r = check_method_call(k, call, b, qualified, 0);
        if (r) return r;
        Pending* pd = new_pending(k);
        pd->call = call;
        pd->recv = b;
        pd->qualified = qualified;
        pd->method = 1;
        return T_ERR; // a method may be declared later
    }
    ck_err(k, call, "unsupported callee");
    return T_ERR;
}

// --- statements ------------------------------------------------------------------------

/*
 * Union switch: arms name variants (Type.variant, optionally with an explicit
 * payload binder), or else. Every variant must be covered once. Binders are
 * fresh locals in the arm scope; payload access is impossible outside them.
 */
static void check_union_switch(Ck* k, Node* s, SType* u) {
    uint8_t* covered = (uint8_t*)arena_alloc((size_t)(u->nfields ? u->nfields : 1));
    memset(covered, 0, (size_t)(u->nfields ? u->nfields : 1));
    int has_default = 0;
    for (int i = 0; i < s->nkids; i++) {
        Node* c = s->kids[i];
        if (c->flags & F_DEFAULT) {
            has_default = 1;
        } else {
            if (c->nkids != 1) ck_err(k, c, "union switch cases name exactly one variant");
            for (int j = 0; j < c->nkids; j++) {
                Node* pat = c->kids[j];
                if (pat->kind == N_CALL) {
                    ck_err(k, pat, "match the union variant, not a payload value: Type.variant(binder: type)");
                    continue;
                }
                Node* ref = pat->kind == N_VARPAT ? pat->a : pat;
                if (ref->kind != N_FIELD_E || (ref->a->kind != N_IDENT && ref->a->kind != N_INDEX)) {
                    ck_err(k, pat, "union switch cases must name %s variants (Type.variant)", type_name(u));
                    continue;
                }
                SType* ub = union_base_type(k, ref->a);
                if (ub == T_ERR) return;
                if (!ub || !same_type(ub, u)) {
                    ck_err(k, pat, "case pattern is not a variant of %s", type_name(u));
                    continue;
                }
                int vi = union_variant_index(k, pat, u, ref);
                if (vi < 0) continue;
                if (covered[vi]) {
                    ck_err(k, pat, "duplicate case for variant '%s'", u->fields[vi].name);
                    continue;
                }
                covered[vi] = 1;
                ref->ival = (uint64_t)vi;      // emitter: tag compare constant
                if (pat->kind != N_VARPAT && u->fields[vi].t) {
                    ck_err(k, pat,
                           "variant '%s' carries %s and needs a payload binder",
                           u->fields[vi].name, type_name(u->fields[vi].t));
                } else if (pat->kind == N_VARPAT) {
                    pat->ival = (uint64_t)vi;  // emitter reads the index here
                    Node* binder = pat->b;
                    SType* want = u->fields[vi].t;
                    if (binder && !binder->a) {
                        ck_err(k, binder,
                               "variant pattern binder '%.*s' needs an explicit type: %s.%.*s(%.*s: type)",
                               (int)binder->len, binder->str, type_name(u),
                               (int)ref->len, ref->str,
                               (int)binder->len, binder->str);
                    } else if (binder) {
                        SType* bt = build_type(k, binder->a);
                        if (!want) {
                            ck_err(k, binder, "variant '%s' carries no payload", u->fields[vi].name);
                        } else if (!same_type(bt, want)) {
                            ck_err(k, binder,
                                   "variant '%s' payload is %s, binder is %s",
                                   u->fields[vi].name, type_name(want), type_name(bt));
                        }
                    }
                }
            }
        }
        scope_push(k);
        if (!(c->flags & F_DEFAULT)) {
            for (int j = 0; j < c->nkids; j++) {
                Node* pat = c->kids[j];
                if (pat->kind != N_VARPAT || !pat->b || !pat->b->a) continue;
                Sym* v = declare_val(k, pat->b, SYM_VAR, pat->b->str, pat->b->len);
                if (v) {
                    v->type = build_type(k, pat->b->a);
                    v->decl = pat->b;
                    pat->b->sym = v; // emitter: binder slot key
                }
            }
        }
        check_block(k, c->b);
        scope_pop(k);
    }
    if (!has_default) {
        for (int i = 0; i < u->nfields; i++)
            if (!covered[i])
                ck_err(k, s, "switch on %s is missing variant '%s' (or add default)",
                       type_name(u), u->fields[i].name);
    }
}

// `default` is optional and, when present, the last case (SPEC Conditions #2);
// a second `default` is likewise a case after the first.
static void check_default_last(Ck* k, Node* s) {
    for (int i = 0; i + 1 < s->nkids; i++) {
        if (s->kids[i]->flags & F_DEFAULT) {
            ck_err(k, s->kids[i + 1], "'default' must be the last case");
            return;
        }
    }
}

static void check_switch_fallthrough(Ck* k, Node* s, SType* t) {
    for (int i = 0; i < s->nkids; i++) {
        Node* b = s->kids[i]->b;
        for (int j = 0; b && j < b->nkids; j++) {
            Node* st = b->kids[j];
            if (st->kind != N_FALLTHROUGH) continue;
            if (j != b->nkids - 1) ck_err(k, st, "fallthrough must be the final statement of a switch case");
            if (i == s->nkids - 1) ck_err(k, st, "fallthrough is not allowed in the final switch case");
            if (t && t->kind == SK_UNION) ck_err(k, st, "fallthrough is not allowed in a tagged-union switch");
        }
    }
}

static void check_stmt(Ck* k, Node* s) {
    switch (s->kind) {
    case N_VAR: {
        if (s->flags & F_MIXED) {
            /* Resolve every name against the scope as it stood before this
             * statement. Earlier binders in the list do not affect later ones. */
            SType* rt = expr_type(k, s->b);
            SType** wants = (SType**)arena_alloc((size_t)(s->nkids ? s->nkids : 1) * sizeof(SType*));
            Sym** old = (Sym**)arena_alloc((size_t)(s->nkids ? s->nkids : 1) * sizeof(Sym*));
            for (int i = 0; i < s->nkids; i++) {
                Node* binder = s->kids[i];
                old[i] = lookup_val(k, binder->str, binder->len);
                if (binder->kind == N_PARAM) {
                    wants[i] = build_type(k, binder->a);
                    if (wants[i] && wants[i]->kind == SK_ERROR_DOMAIN)
                        ck_err(k, binder, "error-domain names are identities, not value types; use Error");
                    if (old[i])
                        ck_err(k, binder,
                               "redeclaration of '%.*s': a typed name under '=' must be new",
                               (int)binder->len, binder->str);
                } else if (!old[i]) {
                    ck_err(k, binder,
                           "undeclared name '%.*s': a bare name under '=' must already exist",
                           (int)binder->len, binder->str);
                    wants[i] = T_ERR;
                } else {
                    binder->sym = old[i];
                    wants[i] = check_lvalue(k, binder, 1);
                }
            }
            for (int i = 0; i < s->nkids; i++) {
                Node* binder = s->kids[i];
                if (binder->kind != N_PARAM || old[i]) continue;
                Sym* v = declare_val(k, binder, SYM_VAR, binder->str, binder->len);
                if (v) {
                    v->type = wants[i];
                    v->is_const = (s->flags & F_CONST) != 0;
                    v->decl = binder;
                    binder->sym = v;
                }
            }
            if (rt->kind == SK_ERR) return;
            if (rt->kind != SK_TUPLE || rt->nfields != s->nkids) {
                ck_err(k, s, "assignment binds %d names but the right side has %d values",
                       s->nkids, rt->kind == SK_TUPLE ? rt->nfields : 1);
                return;
            }
            if (s->b->kind == N_TUPLE_E) {
                for (int i = 0; i < s->nkids; i++) coerce(k, s->b->kids[i], wants[i]);
            } else {
                for (int i = 0; i < s->nkids; i++)
                    if (wants[i]->kind != SK_ERR &&
                        !same_type(rt->fields[i].t, wants[i]))
                        ck_err(k, s->kids[i],
                               "value %d of the call is %s but '%.*s' is %s (casts are explicit)",
                               i + 1, type_name(rt->fields[i].t),
                               (int)s->kids[i]->len, s->kids[i]->str,
                               type_name(wants[i]));
            }
            return;
        }
        SType** wants = (SType**)arena_alloc((size_t)(s->nkids ? s->nkids : 1) * sizeof(SType*));
        for (int i = 0; i < s->nkids; i++) {
            Node* binder = s->kids[i];
            SType* t = build_type(k, binder->a);
            if (t && t->kind == SK_ERROR_DOMAIN)
                ck_err(k, binder, "error-domain names are identities, not value types; use Error");
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
            if (wants[0]->kind == SK_STATICMAP)
                ck_err(k, s, "StaticMap construction requires a module-level brace initializer");
            else if (s->flags & F_CONST) ck_err(k, s, "const declaration requires an initializer");
            else if (s->nkids > 1) ck_err(k, s, "destructuring declaration requires an initializer");
            return;
        }
        if (s->nkids == 1) {
            Node* init = s->b;
            if (wants[0]->kind == SK_STATICMAP) {
                ck_err(k, s, "StaticMap construction is allowed only in a module-level declaration");
                check_staticmap(k, init, wants[0]);
            } else if (init->kind == N_STRUCTLIT && !init->a && (wants[0]->kind == SK_STRUCT || wants[0]->kind == SK_BITS))
                check_structlit(k, init, wants[0]);
            else if (init->kind == N_ARRAYLIT && wants[0]->kind == SK_ARRAY) check_arraylit(k, init, wants[0]);
            else coerce(k, init, wants[0]);
            Sym* v = lookup_val(k, s->kids[0]->str, s->kids[0]->len);
            if (v) {
                if (wants[0]->kind == SK_CONTRACT && (init->flags & F_CONST)) v->is_const_backed = 1;
                // Literal provenance for string bindings: a literal, another
                // literal-backed binding, or a slice of one. The writable-view
                // establishment reads this flag (Mutability #11).
                if (wants[0]->kind == SK_PRIM && wants[0]->prim == P_STRING && !wants[0]->is_mut) {
                    if (init->kind == N_STR) v->is_literal_backed = 1;
                    else if (init->kind == N_IDENT) {
                        Sym* src = node_sym(init);
                        if (src && src->is_literal_backed) v->is_literal_backed = 1;
                    } else if (init->kind == N_SLICE_E && init->a) {
                        if (init->a->kind == N_STR ||
                            (init->a->kind == N_IDENT &&
                             node_sym(init->a) &&
                             node_sym(init->a)->is_literal_backed))
                            v->is_literal_backed = 1;
                    }
                }
            }
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
            for (int i = 0; i < s->nkids; i++) coerce(k, s->b->kids[i], wants[i]);
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
            if (rt->kind == SK_ERR) {
                /* The right side may still be a deferred (pending) call that
                 * resolves at end of file. Key the targets now so the emitter
                 * finds their storage whichever way resolution lands. */
                for (int i = 0; i < s->nkids; i++) (void)check_lvalue(k, s->kids[i], 1);
                return;
            }
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
        if ((s->flags & F_VOLATILE) && !is_volatile_scalar(t))
            ck_err(k, s,
                   "volatile access requires an eligible scalar place, got %s",
                   type_name(t));
        if ((s->flags & F_VOLATILE) && is_bits_field(s->kids[0], NULL, NULL))
            ck_err(k, s, "volatile access is allowed only on a complete bits value");
        Node* rhs = s->a;
        if (t->kind == SK_PTR && (s->op == T_ADDEQ || s->op == T_SUBEQ)) {
            SType* rt = expr_type(k, rhs);
            if (rt == T_UT_INT) {
                int64_t offset;
                if (ut_const(rhs, &offset) && offset < 0) {
                    ck_err(k, rhs, "pointer offset must be a nonnegative usize");
                    return;
                }
                if (!ut_fits(k, rhs, st_prim(P_USIZE))) return;
                rt = st_prim(P_USIZE);
                rhs->ty = rt;
            }
            if (rt->kind != SK_PRIM || rt->prim != P_USIZE) {
                ck_err(k, rhs, "pointer compound-assignment offset must be usize, not %s", type_name(rt));
                return;
            }
            if (!layout(t->base) || !t->base->size) {
                ck_err(k, s, "pointer arithmetic requires a complete, nonzero-sized pointee, got %s",
                       type_name(t->base));
                return;
            }
            uint64_t offset;
            if (usize_const(rhs, &offset) &&
                offset > UINT64_MAX / t->base->size)
                ck_err(k, rhs, "pointer offset scaled by sizeof(%s) overflows usize",
                       type_name(t->base));
            return;
        }
        SType* bits_owner = NULL; int bits_index = -1;
        if (is_bits_field(s->kids[0], &bits_owner, &bits_index) && !compound) {
            SField* f = &bits_owner->fields[bits_index]; int64_t v;
            if (ut_const(rhs, &v)) {
                int ok;
                if (f->bit_signed) {
                    int64_t lo = f->bit_width == 64 ? INT64_MIN : -(INT64_C(1) << (f->bit_width - 1));
                    int64_t hi = f->bit_width == 64 ? INT64_MAX : (INT64_C(1) << (f->bit_width - 1)) - 1;
                    ok = v >= lo && v <= hi;
                } else ok = v >= 0 && (f->bit_width == 64 || (uint64_t)v < (UINT64_C(1) << f->bit_width));
                if (!ok) ck_err(k, rhs, "constant does not fit %s.%s %s %u-bit field",
                                type_name(bits_owner), f->name, f->bit_signed ? "signed" : "unsigned", f->bit_width);
            }
        }
        if (rhs->kind == N_STRUCTLIT && !rhs->a && (t->kind == SK_STRUCT || t->kind == SK_BITS)) {
            check_structlit(k, rhs, t);
            return;
        }
        if (rhs->kind == N_ARRAYLIT && t->kind == SK_ARRAY) {
            check_arraylit(k, rhs, t);
            return;
        }
        coerce(k, rhs, t); // compound ops check as x = x op y with same type
        if (t->kind == SK_CONTRACT && s->kids[0]->kind == N_IDENT) {
            Sym* target = lookup_val(k, s->kids[0]->str, s->kids[0]->len);
            if (target) target->is_const_backed = (rhs->flags & F_CONST) != 0;
        }
        return;
    }
    case N_EXPRSTMT: {
        SType* t = expr_type(k, s->a);
        if (t == T_UT_INT || t == T_UT_FLOAT)
            ck_err(k, s, "untyped literal expression has no effect — bind it to a type");
        return;
    }
    case N_DEFER: {
        // Hard restriction: `defer` is a direct statement of the function's
        // top-level body block only. Every nested block pushes its own scope
        // (if/for/switch case/label/bare block), while check_block pushes
        // none — so the innermost scope is the body scope exactly when the
        // defer sits at the top level.
        if (!k->scope->is_fnbody) ck_err(k, s, "defer statement must appear at the top level of the function body");
        if (s->a->kind != N_CALL) {
            ck_err(k, s->a, "defer operand must be a call");
            return;
        }
        SType* t = expr_type(k, s->a); // checked like an ordinary call statement
        if (t == T_UT_INT || t == T_UT_FLOAT)
            ck_err(k, s, "untyped literal expression has no effect — bind it to a type");
        return;
    }
    case N_IF: {
        SType* c = expr_type(k, s->a);
        if (c->kind != SK_ERR && !is_bool(c)) ck_err(k, s->a, "if condition must be bool, not %s", type_name(c));
        scope_push(k);
        check_block(k, s->b);
        scope_pop(k);
        if (s->c) {
            scope_push(k);
            check_stmt(k, s->c); // else-if chains are N_IF; blocks self-scope
            scope_pop(k);
        }
        return;
    }
    case N_FOR: {
        if (s->flags & F_FORGEN) {
            // General head: one loop scope covers init, cond, post, and body,
            // so a head declaration covers cond/post/body and is invisible
            // outside. The scope carries the label and is_loop so the
            // break/continue walks match labeled loops of every head form.
            scope_push(k);
            k->scope->is_loop = 1;
            if (s->str) k->scope->label = s->str;
            if (s->a) check_stmt(k, s->a); // head init: runs once, before cond
            if (s->b) {
                SType* c = expr_type(k, s->b);
                if (c->kind != SK_ERR && !is_bool(c))
                    ck_err(k, s->b, "for condition must be bool, not %s",
                           type_name(c));
            }
            if (s->c) check_stmt(k, s->c);
            check_block(k, s->d);
            scope_pop(k);
            return;
        }
        SType* binds[2] = {0};
        for (int i = 0; i < s->nkids && i < 2; i++) binds[i] = build_type(k, s->kids[i]->a);
        SType* it = expr_type(k, s->b);
        if (it->kind != SK_ERR) {
            if (s->b->kind == N_RANGE) {
                if (s->nkids != 1) ck_err(k, s, "range iteration accepts one value binder, not %d", s->nkids);
                if (s->nkids > 0 && !is_int(binds[0]))
                    ck_err(k, s, "range loop variable must be an integer type, not %s",
                           type_name(binds[0]));
            } else if (it->kind == SK_SLICE || it->kind == SK_ARRAY || (it->kind == SK_PRIM && it->prim == P_STRING)) {
                SType* elem = it->kind == SK_PRIM ? st_prim(P_U8) : it->base;
                int value = s->nkids == 2 ? 1 : 0;
                if (s->nkids == 2 && !same_type(binds[0], st_prim(P_USIZE)))
                    ck_err(k, s->kids[0], "sequence index binder must be usize, not %s",
                           type_name(binds[0]));
                if (s->nkids > 0 && !same_type(elem, binds[value]))
                    ck_err(k, s->kids[value], "loop value binder is %s but elements are %s",
                           type_name(binds[value]), type_name(elem));
            } else {
                ck_err(k, s->b, "for-in iterates slices, arrays, strings, or ranges — not %s", type_name(it));
            }
        }
        if ((s->flags & F_MUT) && it->kind != SK_ERR) {
            // `in mut iterable` applies the call-site `mut` discipline to
            // iteration: binders alias writable element storage. A mut-qualified
            // slice or string carries content writability in its type, so its
            // storage needs no separate check; strings participate with no
            // special-casing (Iterations #16). Ranges have no elements.
            if (s->b->kind == N_RANGE) {
                ck_err(k, s->b, "mut iteration requires writable array, slice, or string storage, not a range");
            } else {
                int content_writable = (it->kind == SK_SLICE ||
                                        (it->kind == SK_PRIM && it->prim == P_STRING)) &&
                                       it->is_mut;
                SType* wt = content_writable ? it : check_lvalue(k, s->b, 1);
                if (!content_writable && wt->kind != SK_ERR &&
                    wt->kind != SK_SLICE && wt->kind != SK_ARRAY)
                    ck_err(k, s->b,
                           "mut iteration requires writable array, slice, or string storage, not %s",
                           type_name(wt));
            }
        }
        scope_push(k);
        k->scope->is_loop = 1;
        if (s->str) k->scope->label = s->str; // labeled typed loops
        for (int i = 0; i < s->nkids; i++) {
            Node* binder = s->kids[i];
            Sym* v = declare_val(k, binder, SYM_VAR, binder->str, binder->len);
            if (v) {
                v->type = binds[i];
                v->decl = binder;
                binder->sym = v;
            }
        }
        check_block(k, s->c);
        scope_pop(k);
        return;
    }
    case N_SWITCH: {
        SType* t = expr_type(k, s->a);
        check_switch_fallthrough(k, s, t);
        check_default_last(k, s);
        if (t->kind == SK_UNION) {
            check_union_switch(k, s, t);
            return;
        }
        for (int i = 0; i < s->nkids; i++) {
            Node* c = s->kids[i];
            if (!(c->flags & F_DEFAULT) && t->kind != SK_ERR) {
                for (int j = 0; j < c->nkids; j++) coerce(k, c->kids[j], t);
            }
            scope_push(k);
            check_block(k, c->b);
            scope_pop(k);
        }
        return;
    }
    case N_FALLTHROUGH:
        return; // placement was validated by the enclosing switch
    case N_RETURN: {
        SType* rt = k->ret;
        int nret = rt ? (rt->kind == SK_TUPLE ? rt->nfields : 1) : 0;
        if (!s->a) {
            if (nret != 0) ck_err(k, s, "function returns %d values; bare return returns none", nret);
            return;
        }
        if (s->a->kind == N_TUPLE_E) {
            if (!rt || rt->kind != SK_TUPLE) {
                ck_err(k, s, "function returns %d value, return has %d", nret, s->a->nkids);
                return;
            }
            if (s->a->nkids != rt->nfields) {
                ck_err(k, s, "function returns %d values, return has %d", rt->nfields, s->a->nkids);
                return;
            }
            for (int i = 0; i < s->a->nkids; i++) coerce(k, s->a->kids[i], rt->fields[i].t);
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
        if (rt->kind == SK_CONTRACT && (s->a->flags & F_CONST) && k->curfn)
            k->curfn->flags |= F_CONST; // conservative return-capability summary
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
        if (s->str) {
            for (Scope* sc = k->scope; sc; sc = sc->parent)
                if (sc->label && (uint32_t)strlen(sc->label) == s->len &&
                    memcmp(sc->label, s->str, s->len) == 0) return;
            ck_err(k, s, "no enclosing block labeled '%.*s'", (int)s->len, s->str);
        } else {
            int loop = 0;
            for (Scope* sc = k->scope; sc; sc = sc->parent) loop |= sc->is_loop;
            if (!loop) ck_err(k, s, "continue outside a loop");
        }
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
                        ck_err(k, val, "asm binding '%.*s' is not a local or parameter", (int)val->len, val->str);
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
    if (want->kind == SK_BITS) {
        uint8_t* seen = (uint8_t*)arena_alloc((size_t)(want->nfields ? want->nfields : 1));
        memset(seen, 0, (size_t)(want->nfields ? want->nfields : 1));
        for (int i = 0; i < lit->nkids; i++) {
            Node* fi = lit->kids[i];
            if (!fi->str) { ck_err(k, fi, "bits literals require named fields"); continue; }
            int found = -1;
            for (int j = 0; j < want->nfields; j++)
                if (!want->fields[j].bit_anonymous && strlen(want->fields[j].name) == fi->len &&
                    memcmp(want->fields[j].name, fi->str, fi->len) == 0) { found = j; break; }
            if (found < 0) {
                ck_err(k, fi, "bits type %s has no accessible field '%.*s'",
                       type_name(want), (int)fi->len, fi->str); continue;
            }
            if (seen[found]) { ck_err(k, fi, "duplicate field '%.*s' in %s literal",
                                      (int)fi->len, fi->str, type_name(want)); continue; }
            seen[found] = 1;
            int64_t v;
            SField* f = &want->fields[found];
            if (ut_const(fi->a, &v)) {
                int ok;
                if (f->bit_signed) {
                    int64_t lo = f->bit_width == 64 ? INT64_MIN : -(INT64_C(1) << (f->bit_width - 1));
                    int64_t hi = f->bit_width == 64 ? INT64_MAX : (INT64_C(1) << (f->bit_width - 1)) - 1;
                    ok = v >= lo && v <= hi;
                } else ok = v >= 0 && (f->bit_width == 64 || (uint64_t)v < (UINT64_C(1) << f->bit_width));
                if (!ok) ck_err(k, fi->a, "constant does not fit %s.%s %s %u-bit field",
                                type_name(want), f->name, f->bit_signed ? "signed" : "unsigned", f->bit_width);
            }
            coerce(k, fi->a, f->t);
        }
        return; // omitted named fields and anonymous ranges remain canonical zero
    }
    if (want->kind != SK_STRUCT) {
        ck_err(k, lit, "struct literal used where %s expected", type_name(want));
        return;
    }
    int n = lit->nkids;
    int positional = n > 0 && lit->kids[0]->str == NULL;
    uint8_t* seen = (uint8_t*)arena_alloc((size_t)(want->nfields ? want->nfields : 1));
    memset(seen, 0, (size_t)(want->nfields ? want->nfields : 1));
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
                ck_err(k, fi, "type %s has no field '%.*s'", type_name(want), (int)fi->len, fi->str);
                continue;
            }
            if (seen[found]) {
                ck_err(k, fi, "duplicate field '%.*s' in %s literal", (int)fi->len, fi->str, type_name(want));
                continue;
            }
            seen[found] = 1;
            coerce(k, fi->a, want->fields[found].t);
        }
    }
    if (positional && n < want->nfields)
        ck_err(k, lit, "missing fields: %s has %d, literal has %d",
               type_name(want), want->nfields, n);
    if (!positional) {
        int added = 0;
        for (int i = 0; i < want->nfields; i++) if (!seen[i] && want->fields[i].default_value) added++;
        if (added) {
            Node** kids = (Node**)arena_alloc((size_t)(lit->nkids + added) * sizeof(Node*));
            memcpy(kids, lit->kids, (size_t)lit->nkids * sizeof(Node*));
            int at = lit->nkids;
            for (int i = 0; i < want->nfields; i++) if (!seen[i] && want->fields[i].default_value) {
                Node* fi = (Node*)arena_alloc(sizeof(Node));
                memset(fi, 0, sizeof *fi);
                fi->kind = N_FIELDINIT; fi->str = want->fields[i].name;
                fi->len = (uint32_t)strlen(fi->str); fi->path = lit->path;
                fi->line = lit->line; fi->col = lit->col;
                fi->a = clone_expr(want->fields[i].default_value);
                kids[at++] = fi;
                seen[i] = 1;
            }
            lit->kids = kids; lit->nkids = at;
        }
        for (int i = 0; i < want->nfields; i++)
            if (!seen[i])
                ck_err(k, lit, "missing field '%s' in %s literal",
                       want->fields[i].name, type_name(want));
    }
}

static void check_arraylit(Ck* k, Node* lit, SType* want) {
    lit->ty = want; // emitter: context-typed literal carries its type
    if (want->kind != SK_ARRAY) {
        ck_err(k, lit, "array literal used where %s expected", type_name(want));
        return;
    }
    if (lit->a) {
        coerce(k, lit->b, st_prim(P_USIZE));
        int64_t count;
        if (!ut_const(lit->b, &count)) {
            ck_err(k, lit->b, "array repetition count must be a compile-time usize expression");
            return;
        }
        if (count < 0 || (uint64_t)count != want->count) {
            ck_err(k, lit, "array repetition count is %lld, expected %llu for %s",
                   (long long)count,
                   (unsigned long long)want->count, type_name(want));
            return;
        }
        coerce(k, lit->a, want->base);
        return;
    }
    if ((uint64_t)lit->nkids != want->count) {
        ck_err(k, lit, "array length is %llu, literal has %d elements", (unsigned long long)want->count, lit->nkids);
        return;
    }
    for (int i = 0; i < lit->nkids; i++) coerce(k, lit->kids[i], want->base);
}

// --- definite initialization ---------------------------------------------------------

// This is deliberately a separate post-typecheck walk. It tracks direct Ace
// storage paths without changing runtime representation. Pointer aliases,
// address-taking, asm, and extern code remain explicit trust boundaries.
typedef struct InitPart InitPart;
typedef struct InitState InitState;
typedef struct InitVar InitVar;
typedef struct InitExit InitExit;

struct InitState {
    uint8_t full;
    InitPart* parts; // initialized struct fields / constant array elements
};

struct InitPart {
    uint64_t key;
    InitState state;
    InitPart* next;
};

struct InitVar {
    Sym* sym;
    InitState state;
    InitVar* next;
};

typedef struct {
    Diag* d;
    InitVar* vars;
    PtrMap var_ix; // Sym -> InitVar
    InitExit* exits;
} InitCk;

typedef struct {
    InitVar* var;
    InitState state;
} InitSnapEntry;

typedef struct {
    InitSnapEntry* v;
    int n;
} InitSnap;

struct InitExit {
    const char* name;
    uint32_t len;
    InitSnap base;
    InitSnap merged;
    int have;
    InitExit* next;
};

typedef struct {
    Sym* root;
    uint64_t key[64];
    SType* type[65];
    int n;
    int dynamic_index;
} InitPath;

static SType* init_shape(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    return t;
}

static int init_aggregate(SType* t) {
    t = init_shape(t);
    return t && (t->kind == SK_STRUCT || t->kind == SK_ARRAY || t->kind == SK_TUPLE);
}

static InitPart* init_part(InitState* s, uint64_t key, int create) {
    for (InitPart* p = s->parts; p; p = p->next) if (p->key == key) return p;
    if (!create) return NULL;
    InitPart* p = (InitPart*)arena_alloc(sizeof(InitPart));
    memset(p, 0, sizeof *p);
    p->key = key;
    p->next = s->parts;
    s->parts = p;
    return p;
}

static int init_state_full(InitState* s, SType* t);

static uint64_t init_part_count(SType* t) {
    t = init_shape(t);
    if (!t) return 0;
    if (t->kind == SK_STRUCT || t->kind == SK_TUPLE) return (uint64_t)t->nfields;
    if (t->kind == SK_ARRAY) return t->count;
    return 0;
}

static SType* init_part_type(SType* t, uint64_t key) {
    t = init_shape(t);
    if (!t) return NULL;
    if ((t->kind == SK_STRUCT || t->kind == SK_TUPLE) && key < (uint64_t)t->nfields) return t->fields[key].t;
    if (t->kind == SK_ARRAY && key < t->count) return t->base;
    return NULL;
}

static int init_state_full(InitState* s, SType* t) {
    if (s->full) return 1;
    if (!init_aggregate(t)) return 0;
    uint64_t count = init_part_count(t), have = 0;
    for (InitPart* p = s->parts; p; p = p->next) {
        SType* pt = init_part_type(t, p->key);
        if (pt && init_state_full(&p->state, pt)) have++;
    }
    if (have == count) {
        s->full = 1;
        s->parts = NULL;
        return 1;
    }
    return 0;
}

static InitState init_clone(InitState* s) {
    InitState out = { s->full, NULL };
    InitPart** tail = &out.parts;
    for (InitPart* p = s->parts; p; p = p->next) {
        InitPart* q = (InitPart*)arena_alloc(sizeof(InitPart));
        q->key = p->key;
        q->state = init_clone(&p->state);
        q->next = NULL;
        *tail = q;
        tail = &q->next;
    }
    return out;
}

static InitState init_intersect(InitState* a, InitState* b, SType* t) {
    if (a->full) return init_clone(b);
    if (b->full) return init_clone(a);
    InitState out = {0};
    for (InitPart* p = a->parts; p; p = p->next) {
        InitPart* q = init_part(b, p->key, 0);
        if (!q) continue;
        InitPart* r = init_part(&out, p->key, 1);
        r->state = init_intersect(&p->state, &q->state,
                                  init_part_type(t, p->key));
    }
    (void)init_state_full(&out, t);
    return out;
}

static InitVar* init_var(InitCk* k, Sym* sym, int create) {
    if (!sym || sym->kind != SYM_VAR || sym->is_global) return NULL;
    InitVar* found = (InitVar*)ptrmap_get(&k->var_ix, sym);
    if (found || !create) return found;
    InitVar* v = (InitVar*)arena_alloc(sizeof(InitVar));
    memset(v, 0, sizeof *v);
    v->sym = sym;
    v->next = k->vars;
    k->vars = v;
    ptrmap_put(&k->var_ix, sym, (uintptr_t)v);
    return v;
}

static InitSnap init_snapshot(InitCk* k) {
    int n = 0;
    for (InitVar* v = k->vars; v; v = v->next) n++;
    InitSnap s = {0};
    s.n = n;
    s.v = (InitSnapEntry*)arena_alloc((size_t)(n ? n : 1) * sizeof(InitSnapEntry));
    int i = 0;
    for (InitVar* v = k->vars; v; v = v->next) { s.v[i].var = v; s.v[i].state = init_clone(&v->state); i++; }
    return s;
}

static void init_restore(InitSnap* s) {
    for (int i = 0; i < s->n; i++) s->v[i].var->state = init_clone(&s->v[i].state);
}

static InitState* init_snap_state(InitSnap* s, InitVar* v) {
    for (int i = 0; i < s->n; i++) if (s->v[i].var == v) return &s->v[i].state;
    return NULL;
}

static int init_field_index(SType* t, Node* e) {
    t = init_shape(t);
    if (!t || t->kind != SK_STRUCT) return -1;
    for (int i = 0; i < t->nfields; i++)
        if (strlen(t->fields[i].name) == e->len &&
            memcmp(t->fields[i].name, e->str, e->len) == 0) return i;
    return -1;
}

static int init_const_index(Node* e, uint64_t* out) {
    int64_t v;
    if (!ut_const(e, &v) || v < 0) return 0;
    *out = (uint64_t)v;
    return 1;
}

static int init_path(Node* e, InitPath* p) {
    if (!e || p->n >= 64) return 0;
    if (e->kind == N_IDENT) {
        Sym* s = node_sym(e);
        if (!s || s->kind != SYM_VAR || s->is_global) return 0;
        p->root = s;
        p->type[0] = s->type;
        return 1;
    }
    if (e->kind == N_FIELD_E) {
        if (!init_path(e->a, p)) return 0;
        SType* bt = init_shape(p->type[p->n]);
        int fi = init_field_index(bt, e);
        if (fi < 0) return 0; // pointer auto-deref, Error, or method
        p->key[p->n] = (uint64_t)fi;
        p->type[p->n + 1] = bt->fields[fi].t;
        p->n++;
        return 1;
    }
    if (e->kind == N_INDEX) {
        if (!init_path(e->a, p)) return 0;
        SType* bt = init_shape(p->type[p->n]);
        if (!bt || bt->kind != SK_ARRAY) return 0;
        uint64_t ix;
        if (!init_const_index(e->b, &ix) || ix >= bt->count) {
            p->dynamic_index = 1;
            return 1;
        }
        p->key[p->n] = ix;
        p->type[p->n + 1] = bt->base;
        p->n++;
        return 1;
    }
    return 0;
}

static InitState* init_path_state(InitVar* v, InitPath* p, int create) {
    InitState* s = &v->state;
    if (s->full) return s;
    for (int i = 0; i < p->n; i++) {
        InitPart* part = init_part(s, p->key[i], create);
        if (!part) return NULL;
        s = &part->state;
        if (s->full) return s;
    }
    return s;
}

static void init_diag(InitCk* k, Node* at, InitPath* p, SType* want) {
    const char* path = at && at->path ? at->path : k->d->path;
    fprintf(stderr, "%s:%d:%d: error: '%.*s' was not %sinitialized before use\n",
            path, at ? at->line : 0, at ? at->col : 0,
            p->root ? (int)p->root->len : 1,
            p->root ? p->root->name : "?",
            init_aggregate(want) || (p->n == 0 && init_aggregate(p->root->type))
                ? "fully " : "");
    k->d->errors++;
}

static int init_require_path(InitCk* k, Node* e) {
    InitPath p = {0};
    if (!init_path(e, &p)) return 0;
    InitVar* v = init_var(k, p.root, 0);
    if (!v) return 1;
    if (p.dynamic_index) {
        if (!init_state_full(&v->state, p.root->type)) init_diag(k, e, &p, p.root->type);
        return 1;
    }
    InitState* s = init_path_state(v, &p, 0);
    SType* want = p.type[p.n];
    if (!s || !init_state_full(s, want)) init_diag(k, e, &p, want);
    return 1;
}

static void init_mark_path(InitCk* k, Node* e) {
    InitPath p = {0};
    if (!init_path(e, &p) || p.dynamic_index) return;
    InitVar* v = init_var(k, p.root, 1);
    if (!v) return;
    InitState* s = init_path_state(v, &p, 1);
    s->full = 1;
    s->parts = NULL;
    for (int i = p.n - 1; i >= 0; i--) {
        InitPath prefix = p;
        prefix.n = i;
        InitState* parent = init_path_state(v, &prefix, 0);
        (void)init_state_full(parent, p.type[i]);
    }
}

static void init_expr(InitCk* k, Node* e);
static int init_stmt(InitCk* k, Node* s);

static void init_addr_parts(InitCk* k, Node* e) {
    if (!e) return;
    if (e->kind == N_INDEX) {
        init_expr(k, e->b); // computing the address reads the index
        SType* bt = init_shape(node_ty(e->a));
        if (bt && bt->kind != SK_ARRAY) init_expr(k, e->a); // slice descriptor/pointer
        return;
    }
    if (e->kind == N_FIELD_E) {
        SType* bt = init_shape(node_ty(e->a));
        if (bt && bt->kind == SK_PTR) init_expr(k, e->a); // auto-deref reads pointer
        else init_addr_parts(k, e->a);
        return;
    }
    if (e->kind == N_UNARY && e->op == T_STAR) {
        init_expr(k, e->a);
        return;
    }
}

static void init_expr(InitCk* k, Node* e) {
    if (!e) return;
    switch (e->kind) {
    case N_INT: case N_FLOAT: case N_STR: case N_CHAR: case N_BOOL: case N_NULL:
        return;
    case N_IDENT: {
        Sym* s = node_sym(e);
        if (s && s->kind == SYM_VAR && !s->is_global) (void)init_require_path(k, e);
        return;
    }
    case N_FIELD_E: {
        if (e->len == 3 && memcmp(e->str, "len", 3) == 0) {
            SType* t = init_shape(node_ty(e->a));
            if (t && t->kind == SK_ARRAY) return; // compile-time metadata
        }
        InitPath p = {0};
        if (init_path(e, &p)) {
            (void)init_require_path(k, e);
            init_addr_parts(k, e);
        } else {
            init_expr(k, e->a);
        }
        return;
    }
    case N_INDEX: {
        InitPath p = {0};
        if (init_path(e, &p)) {
            (void)init_require_path(k, e);
            init_expr(k, e->b);
        } else {
            init_expr(k, e->a);
            init_expr(k, e->b);
        }
        return;
    }
    case N_SLICE_E: {
        SType* bt = init_shape(node_ty(e->a));
        if (!bt || bt->kind != SK_ARRAY) init_expr(k, e->a);
        init_expr(k, e->b);
        init_expr(k, e->c);
        return;
    }
    case N_UNARY:
        if (e->op == T_AMP) init_addr_parts(k, e->a);
        else init_expr(k, e->a);
        return;
    case N_BINARY:
        init_expr(k, e->a); init_expr(k, e->b); return;
    case N_RANGE:
        init_expr(k, e->a); init_expr(k, e->b); init_expr(k, e->c); return;
    case N_CMPCHAIN: case N_TUPLE_E:
        for (int i = 0; i < e->nkids; i++) init_expr(k, e->kids[i]);
        return;
    case N_ARRAYLIT:
        if (e->a) init_expr(k, e->a);
        else for (int i = 0; i < e->nkids; i++) init_expr(k, e->kids[i]);
        return;
    case N_STRUCTLIT:
        for (int i = 0; i < e->nkids; i++) init_expr(k, e->kids[i]->a);
        return;
    case N_MUTARG:
        init_expr(k, e->a); return;
    case N_CALL:
        if (e->a && e->a->kind == N_IDENT) {
            Sym* s = node_sym(e->a);
            if (s && s->kind == SYM_VAR) init_expr(k, e->a); // function variable
        } else if (e->a && e->a->kind == N_FIELD_E) {
            init_expr(k, e->a->a); // receiver
        } else if (e->a) {
            init_expr(k, e->a);
        }
        for (int i = 0; i < e->nkids; i++) init_expr(k, e->kids[i]);
        return;
    default:
        return;
    }
}

static void init_merge_snapshots(InitSnap* base, InitSnap* a, InitSnap* b) {
    for (int i = 0; i < base->n; i++) {
        InitVar* v = base->v[i].var;
        InitState* as = init_snap_state(a, v);
        InitState* bs = init_snap_state(b, v);
        if (as && bs) v->state = init_intersect(as, bs, v->sym->type);
    }
}

static InitSnap init_intersect_snapshots(InitSnap* base, InitSnap* a, InitSnap* b) {
    InitSnap out = {0};
    out.n = base->n;
    out.v = (InitSnapEntry*)arena_alloc((size_t)(out.n ? out.n : 1) * sizeof(InitSnapEntry));
    for (int i = 0; i < out.n; i++) {
        InitVar* v = base->v[i].var;
        InitState* as = init_snap_state(a, v);
        InitState* bs = init_snap_state(b, v);
        out.v[i].var = v;
        out.v[i].state = as && bs ? init_intersect(as, bs, v->sym->type)
            : (InitState){0};
    }
    return out;
}

static void init_exit_add(InitExit* x) {
    InitSnap now = {0};
    now.n = x->base.n;
    now.v = (InitSnapEntry*)arena_alloc((size_t)(now.n ? now.n : 1) * sizeof(InitSnapEntry));
    for (int i = 0; i < now.n; i++) {
        now.v[i].var = x->base.v[i].var;
        now.v[i].state = init_clone(&now.v[i].var->state);
    }
    if (!x->have) {
        x->merged = now;
        x->have = 1;
    } else {
        x->merged = init_intersect_snapshots(&x->base, &x->merged, &now);
    }
}

static int init_block(InitCk* k, Node* b) {
    int falls = 1;
    for (int i = 0; b && i < b->nkids; i++) {
        if (falls) falls = init_stmt(k, b->kids[i]);
    }
    return falls;
}

static int init_stmt(InitCk* k, Node* s) {
    if (!s) return 1;
    switch (s->kind) {
    case N_VAR:
        for (int i = 0; i < s->nkids; i++) {
            if ((s->flags & F_MIXED) && s->kids[i]->kind != N_PARAM) continue;
            Sym* sym = node_sym(s->kids[i]);
            InitVar* v = init_var(k, sym, 1);
            if (v) memset(&v->state, 0, sizeof v->state);
        }
        if (s->b) {
            init_expr(k, s->b);
            for (int i = 0; i < s->nkids; i++) {
                if ((s->flags & F_MIXED) && s->kids[i]->kind != N_PARAM) {
                    init_mark_path(k, s->kids[i]);
                } else {
                    InitVar* v = init_var(k, node_sym(s->kids[i]), 1);
                    if (v) { v->state.full = 1; v->state.parts = NULL; }
                }
            }
        }
        return 1;
    case N_ASSIGN:
        init_expr(k, s->a);
        for (int i = 0; i < s->nkids; i++) {
            SType* base = s->kids[i]->kind == N_FIELD_E ? init_shape(node_ty(s->kids[i]->a)) : NULL;
            if (base && base->kind == SK_BITS)
                init_expr(k, s->kids[i]->a); // preserving RMW reads the whole unit
            if (s->op != T_ASSIGN) init_expr(k, s->kids[i]);
            init_addr_parts(k, s->kids[i]);
            init_mark_path(k, s->kids[i]);
        }
        return 1;
    case N_EXPRSTMT:
        init_expr(k, s->a); return 1;
    case N_IF: {
        init_expr(k, s->a);
        InitSnap base = init_snapshot(k);
        int tf = init_block(k, s->b);
        InitSnap then_s = init_snapshot(k);
        init_restore(&base);
        int ef = s->c ? init_stmt(k, s->c) : 1;
        InitSnap else_s = init_snapshot(k);
        if (tf && ef) init_merge_snapshots(&base, &then_s, &else_s);
        else if (tf) init_restore(&then_s);
        else if (ef) init_restore(&else_s);
        else init_restore(&base);
        return tf || ef;
    }
    case N_FOR: {
        if (s->flags & F_FORGEN) {
            if (s->a) init_stmt(k, s->a); // head init runs before the first cond
            if (s->b) init_expr(k, s->b);
            InitSnap base = init_snapshot(k);
            (void)init_block(k, s->d);
            if (s->c) init_stmt(k, s->c); // post-clause reads see the body's state
            init_restore(&base); // loop may execute zero times
            return 1;
        }
        init_expr(k, s->b);
        InitSnap base = init_snapshot(k);
        for (int i = 0; i < s->nkids; i++) {
            InitVar* v = init_var(k, node_sym(s->kids[i]), 1);
            if (v) v->state.full = 1;
        }
        (void)init_block(k, s->c);
        init_restore(&base); // loop may execute zero times
        return 1;
    }
    case N_SWITCH: {
        init_expr(k, s->a);
        InitSnap base = init_snapshot(k);
        InitSnap merged = base;
        int have = 0, has_default = 0;
        for (int i = 0; i < s->nkids; i++) {
            Node* c = s->kids[i];
            if (c->flags & F_DEFAULT) has_default = 1;
            init_restore(&base);
            for (int j = 0; j < c->nkids; j++) {
                Node* pat = c->kids[j];
                if (pat->kind == N_VARPAT && pat->b) {
                    // an arm binder is a fresh copy, always initialized
                    InitVar* bv = init_var(k, node_sym(pat->b), 1);
                    if (bv) { bv->state.full = 1; bv->state.parts = NULL; }
                } else {
                    init_expr(k, pat);
                }
            }
            int falls = init_block(k, c->b);
            if (!falls) continue;
            InitSnap cs = init_snapshot(k);
            if (!have) { merged = cs; have = 1; }
            else {
                init_restore(&merged);
                InitSnap cur = init_snapshot(k);
                init_merge_snapshots(&base, &cur, &cs);
                merged = init_snapshot(k);
            }
        }
        if (!has_default) {
            if (!have) merged = base;
            else {
                init_restore(&merged);
                InitSnap cur = init_snapshot(k);
                init_merge_snapshots(&base, &cur, &base);
                merged = init_snapshot(k);
            }
        }
        init_restore(have || !has_default ? &merged : &base);
        return !has_default || have;
    }
    case N_RETURN:
        init_expr(k, s->a); return 0;
    case N_FALLTHROUGH:
        return 0;
    case N_ASM:
        for (int i = 0; i < s->nkids; i++) {
            Node* b = s->kids[i];
            int is_in = b->len == 2 && memcmp(b->str, "in", 2) == 0;
            int is_out = b->len == 3 && memcmp(b->str, "out", 3) == 0;
            if (!is_in && !is_out) continue;
            for (int j = 1; j < b->nkids; j += 2) {
                if (is_in) init_expr(k, b->kids[j]);
                else init_mark_path(k, b->kids[j]);
            }
        }
        return 1;
    case N_BLOCK:
        return init_block(k, s);
    case N_LABEL: {
        InitExit x = {0};
        x.name = s->str;
        x.len = s->len;
        x.base = init_snapshot(k);
        x.next = k->exits;
        k->exits = &x;
        int falls = init_block(k, s->a);
        if (falls) init_exit_add(&x);
        k->exits = x.next;
        if (x.have) init_restore(&x.merged);
        else init_restore(&x.base);
        return x.have;
    }
    case N_BREAK:
        if (s->str) {
            for (InitExit* x = k->exits; x; x = x->next) {
                if (x->len == s->len && memcmp(x->name, s->str, s->len) == 0) {
                    init_exit_add(x);
                    break;
                }
            }
        }
        return 0;
    case N_CONTINUE:
        return 0;
    default:
        return 1;
    }
}

static void check_definite_initialization(Node* file, Diag* d) {
    for (int i = 0; i < file->nkids; i++) {
        Node* f = file->kids[i];
        if (f->kind != N_FUNC || !f->b || (f->c && !sema_insts(f))) continue;
        InitCk k = { .d = d };
        for (int p = 0; p < f->nkids; p++) {
            Sym* sym = node_sym(f->kids[p]);
            InitVar* v = init_var(&k, sym, 1);
            if (v) v->state.full = 1;
        }
        (void)init_block(&k, f->b);
        ptrmap_free(&k.var_ix);
    }
}

static Node* static_string_expr(Ck* k, Node* e) {
    if (e->kind == N_STR) return e;
    if (e->kind == N_IDENT) {
        Sym* s = lookup_val(k, e->str, e->len);
        if (s && s->is_global && s->is_const && s->type == st_prim(P_STRING) && s->init)
            return static_string_expr(k, s->init);
    }
    return NULL;
}

// --- functions & top level ------------------------------------------------------------------

static void check_func_body(Ck* k, Node* f) {
    SType* saved_ret = k->ret; // nested checks (generic stamping) must not clobber
    Node* saved_fn = k->curfn;
    scope_push(k);
    k->scope->is_fnbody = 1; // `defer` is legal only as a direct statement here
    for (int i = 0; i < f->nkids; i++) {
        Node* pa = f->kids[i];
        Sym* s = declare_val(k, pa, SYM_VAR, pa->str, pa->len);
        if (s) {
            s->type = build_type(k, pa->a);
            s->is_mut = (pa->flags & F_MUT) != 0;
            s->is_noalias = (pa->flags & F_NOALIAS) != 0;
            s->is_param = 1;
            if (s->type && s->type->kind == SK_CONTRACT && !s->is_mut)
                s->is_const_backed = 1; // a read-only contract param may wrap const
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

static uint32_t checked_layout_align(Ck* k, Node* expr, const char* subject) {
    if (!expr) return 0;
    coerce(k, expr, st_prim(P_USIZE));
    int64_t value;
    if (!ut_const(expr, &value)) {
        ck_err(k, expr, "%s @align value must be a compile-time usize expression", subject);
        return 0;
    }
    if (value <= 0 || value > 16 || ((uint64_t)value & ((uint64_t)value - 1)) != 0) {
        ck_err(k, expr, "%s @align value must be a positive power of two no greater than 16", subject);
        return 0;
    }
    return (uint32_t)value;
}

static SType* fill_struct_type(Ck* k, Node* decl, SType* t) {
    k->type_build_depth++;
    t->kind = SK_STRUCT;
    t->name = decl->str;
    t->decl = decl;
    t->size = t->align = 0;
    t->calc = 0;
    t->invalid_layout = 0;
    t->is_packed = (decl->flags & F_PACKED) != 0;
    t->requested_align = 0;
    t->building = 1;
    t->nfields = decl->nkids;
    t->fields = (SField*)arena_alloc((size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    memset(t->fields, 0, (size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    for (int i = 0; i < decl->nkids; i++) {
        t->fields[i].name = decl->kids[i]->str;
        t->fields[i].t = build_type_ctx(k, decl->kids[i]->a, 2);
        t->fields[i].default_value = decl->kids[i]->c;
        t->fields[i].requested_align = checked_layout_align(k, decl->kids[i]->b, "field");
        if (t->fields[i].requested_align && !t->is_packed &&
            layout(t->fields[i].t) &&
            t->fields[i].requested_align < t->fields[i].t->align) {
            ck_err(k, decl->kids[i]->b,
                   "field @align(%u) cannot reduce natural alignment %u outside a packed struct",
                   t->fields[i].requested_align, t->fields[i].t->align);
            t->fields[i].requested_align = t->fields[i].t->align;
        }
        if ((decl->kids[i]->flags & F_EMBED) &&
            t->fields[i].t->kind != SK_STRUCT &&
            t->fields[i].t->kind != SK_ERR)
            ck_err(k, decl->kids[i], "embedded field '%s' must have a struct type",
                   t->fields[i].name);
        if (decl->kids[i]->c) {
            Node* value = decl->kids[i]->c;
            if (value->kind == N_STRUCTLIT && !value->a &&
                t->fields[i].t->kind == SK_STRUCT)
                check_structlit(k, value, t->fields[i].t);
            else if (value->kind == N_ARRAYLIT && t->fields[i].t->kind == SK_ARRAY)
                check_arraylit(k, value, t->fields[i].t);
            else coerce(k, value, t->fields[i].t);
            if (!compile_time_expr(value))
                ck_err(k, value, "default for field '%s' must be a compile-time expression",
                       t->fields[i].name);
        }
    }
    for (int i = 0; i < t->nfields; i++)
        for (int j = i + 1; j < t->nfields; j++)
            if (strcmp(t->fields[i].name, t->fields[j].name) == 0)
                ck_err(k, decl, "duplicate field '%s' in struct %s",
                       t->fields[i].name, decl->str);
    uint32_t base_align = 1;
    for (int i = 0; i < t->nfields; i++) {
        uint32_t a = t->is_packed ? 1 : sema_align(t->fields[i].t);
        if (t->fields[i].requested_align) a = t->fields[i].requested_align;
        if (base_align < a) base_align = a;
    }
    t->requested_align = checked_layout_align(k, decl->b, "struct");
    if (t->requested_align && t->requested_align < base_align) {
        ck_err(k, decl->b, "struct @align(%u) cannot reduce computed alignment %u", t->requested_align, base_align);
        t->requested_align = base_align;
    }
    t->building = 0;
    if (!layout(t)) {
        if (k->type_build_depth == 1)
            ck_err(k, decl,
                   "type '%s' has a recursive value layout; use a pointer to break the cycle",
                   decl->str);
        t->invalid_layout = 1;
        t->size = t->align = 1; // poison recovery after the required diagnostic
        t->calc = 0;
    }
    k->type_build_depth--;
    return t;
}

static SType* fill_bits_type(Ck* k, Node* decl, SType* t) {
    t->kind = SK_BITS; t->name = decl->str; t->decl = decl;
    t->base = build_type(k, decl->a);
    int valid_backing = t->base && t->base->kind == SK_PRIM && t->base->prim >= P_U8 && t->base->prim <= P_U64;
    if (!valid_backing) {
        ck_err(k, decl->a, "bits backing type must be exactly u8, u16, u32, or u64");
        t->base = st_prim(P_U8);
    }
    uint32_t total = 0, backing_bits = sema_size(t->base) * 8;
    t->nfields = decl->nkids;
    t->fields = (SField*)arena_alloc((size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    memset(t->fields, 0, (size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    for (int i = 0; i < decl->nkids; i++) {
        Node* fd = decl->kids[i]; SField* f = &t->fields[i];
        f->name = fd->str; f->bit_offset = (uint8_t)total;
        f->bit_width = (uint8_t)fd->ival; f->bit_signed = (fd->flags & F_CONST) != 0;
        f->bit_anonymous = fd->len == 1 && fd->str[0] == '_';
        if (fd->ival == 0 || fd->ival > backing_bits || total + fd->ival > backing_bits)
            ck_err(k, fd, "bit-field width must be positive and fit within the %u-bit backing integer", backing_bits);
        total += (uint32_t)fd->ival;
        int p = t->base->prim;
        if (f->bit_signed) p = P_I8 + (p - P_U8);
        f->t = st_prim(p);
    }
    if (total != backing_bits)
        ck_err(k, decl, "bits fields total %u bits; backing type requires exactly %u", total, backing_bits);
    for (int i = 0; i < t->nfields; i++) if (!t->fields[i].bit_anonymous)
        for (int j = i + 1; j < t->nfields; j++)
            if (!t->fields[j].bit_anonymous && strcmp(t->fields[i].name, t->fields[j].name) == 0)
                ck_err(k, decl->kids[j], "duplicate bit-field '%s' in %s", t->fields[j].name, decl->str);
    layout(t);
    return t;
}

static SType* fill_union_type(Ck* k, Node* decl, SType* t) {
    k->type_build_depth++;
    if (decl->nkids == 0) ck_err(k, decl, "a union needs at least one variant");
    t->kind = SK_UNION;
    t->name = decl->str;
    t->decl = decl;
    t->size = t->align = 0;
    t->calc = 0;
    t->invalid_layout = 0;
    t->building = 1;
    t->nfields = decl->nkids;
    t->fields = (SField*)arena_alloc((size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    memset(t->fields, 0, (size_t)(decl->nkids ? decl->nkids : 1) * sizeof(SField));
    for (int i = 0; i < decl->nkids; i++) {
        t->fields[i].name = decl->kids[i]->str;
        t->fields[i].t = decl->kids[i]->a ? build_type_ctx(k, decl->kids[i]->a, 2) : NULL;
    }
    for (int i = 0; i < t->nfields; i++)
        for (int j = i + 1; j < t->nfields; j++)
            if (strcmp(t->fields[i].name, t->fields[j].name) == 0)
                ck_err(k, decl, "duplicate variant '%s' in union %s",
                       t->fields[i].name, decl->str);
    t->building = 0;
    if (!layout(t)) {
        if (k->type_build_depth == 1)
            ck_err(k, decl,
                   "type '%s' has a recursive value layout; use a pointer to break the cycle",
                   decl->str);
        t->invalid_layout = 1;
        t->size = t->align = 1;
        t->calc = 0;
    }
    k->type_build_depth--;
    return t;
}

static SType* instantiate_generic_type(Ck* k, Node* decl, SType** args, int nargs) {
    if (!decl || !decl->c) return T_ERR;
    Node* tp = decl->c;
    if (tp->nkids != nargs) {
        ck_err(k, decl, "generic type expects %d type arguments, got %d", tp->nkids, nargs);
        return T_ERR;
    }
    STypeInst** head = (STypeInst**)&tp->b; // memo on the tparams node
    for (STypeInst* it = *head; it; it = it->next) {
        int same = 1;
        for (int i = 0; i < nargs && same; i++) same = same_type(it->args[i], args[i]);
        if (same) {
            if (!it->result) {
                ck_err(k, decl,
                       "generic alias '%s' has a recursive expansion",
                       decl->str);
                return T_ERR;
            }
            return it->result;
        }
    }
    if (decl->kind == N_ALIAS) {
        /* An alias has no nominal shell: stamp its right-hand type under the
         * substitution environment and memoize that terminal type directly.
         * A null result marks an expansion in progress so self-reference is
         * diagnosed instead of recursing indefinitely. */
        STypeInst* inst = (STypeInst*)arena_alloc(sizeof(STypeInst));
        inst->args = args;
        inst->result = NULL;
        inst->next = *head;
        *head = inst;

        const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
        k->tnames = (const char**)arena_alloc((size_t)nargs * sizeof(char*));
        k->ttypes = args;
        k->nsub = nargs;
        for (int i = 0; i < nargs; i++) k->tnames[i] = tp->kids[i]->str;
        inst->result = build_type(k, decl->a);
        k->tnames = sn; k->ttypes = st; k->nsub = ns;
        return inst->result;
    }
    SType* t = st_new(decl->kind == N_STRUCT ? SK_STRUCT : decl->kind == N_UNION ? SK_UNION : SK_CONTRACT);
    t->name = decl->str;
    t->decl = decl;
    t->nargs = nargs;
    t->args = (SType**)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(SType*));
    for (int i = 0; i < nargs; i++) t->args[i] = args[i];
    /* Memoize the nominal shell before its fields are built. Self-recursive
     * generic pointers then resolve to this exact instantiation. */
    STypeInst* inst = (STypeInst*)arena_alloc(sizeof(STypeInst));
    inst->args = args;
    inst->result = t;
    inst->next = *head;
    *head = inst;

    const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
    k->tnames = (const char**)arena_alloc((size_t)nargs * sizeof(char*));
    k->ttypes = args;
    k->nsub = nargs;
    for (int i = 0; i < nargs; i++) k->tnames[i] = tp->kids[i]->str;
    if (decl->kind == N_STRUCT) {
        fill_struct_type(k, decl, t);
    } else if (decl->kind == N_CONTRACT) {
        layout(t);
    } else if (decl->kind == N_UNION) {
        fill_union_type(k, decl, t);
    } else {
        t = T_ERR;
    }
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
    return t;
}

// A concrete type owns one canonical signing. Its methods may follow, so the
// signing is registered during the source walk and validated at EOF.
static void register_signing(Ck* k, Node* signing) {
    Sym* cs = lookup_type(k, signing->a->str, signing->a->len);
    if (!cs || !cs->type || cs->type->kind != SK_STRUCT) {
        ck_err(k, signing->a, "signing owner '%.*s' is not a declared struct", (int)signing->a->len, signing->a->str);
        return;
    }
    Node* decl = cs->type->decl;
    if (decl->path && signing->path && strcmp(decl->path, signing->path) != 0) {
        ck_err(k, signing, "a signing must be in the struct's defining module");
        return;
    }
    if (decl->d) {
        ck_err(k, signing, "signing for %s was already declared once", type_name(cs->type));
        return;
    }
    for (Sym* v = k->scope->values; v; v = v->next)
        for (Sym* f = v; f; f = f->overload)
            if (f->kind == SYM_FUNC && f->decl && (f->decl->flags & F_METHOD) &&
                f->decl->d && f->decl->d->len == signing->a->len &&
                memcmp(f->decl->d->str, signing->a->str, signing->a->len) == 0)
                ck_err(k, signing, "signing for %s must appear before its methods",
                       type_name(cs->type));
    int want = decl->c ? decl->c->nkids : 0;
    int got = signing->c ? signing->c->nkids : 0;
    if (want != got) ck_err(k, signing, "signing owner must repeat the struct's %d type parameters", want);
    else for (int i = 0; i < want; i++)
        if (decl->c->kids[i]->len != signing->c->kids[i]->len ||
            memcmp(decl->c->kids[i]->str, signing->c->kids[i]->str,
                   decl->c->kids[i]->len) != 0)
            ck_err(k, signing->c->kids[i], "signing owner must repeat type parameter '%.*s'",
                   (int)decl->c->kids[i]->len, decl->c->kids[i]->str);
    for (int i = 0; i < signing->nkids; i++) {
        signing->kids[i]->d = signing;
        Node* it = signing->kids[i]->a;
        Sym* is = lookup_type(k, it->str, it->len);
        if (!is || !is->type || is->type->kind != SK_CONTRACT)
            ck_err(k, it, "signed contract '%.*s' must be declared first",
                   (int)it->len, it->str);
    }
    decl->d = signing;
    signing->a->sym = cs;
}

static void canonical_substitution(Ck* k, Node* tp) {
    int n = tp ? tp->nkids : 0;
    k->tnames = (const char**)arena_alloc((size_t)(n ? n : 1) * sizeof(char*));
    k->ttypes = (SType**)arena_alloc((size_t)(n ? n : 1) * sizeof(SType*));
    k->nsub = n;
    for (int i = 0; i < n; i++) {
        k->tnames[i] = tp->kids[i]->str;
        SType* p = st_new(SK_TYPEPARAM);
        char buf[32];
        snprintf(buf, sizeof buf, "$%d", i);
        size_t len = strlen(buf);
        char* stable = arena_alloc(len + 1);
        memcpy(stable, buf, len + 1);
        p->name = stable;
        k->ttypes[i] = p;
    }
}

static void check_signing(Ck* k, Node* signing) {
    const char** sn = k->tnames; SType** st = k->ttypes; int ns = k->nsub;
    canonical_substitution(k, signing->c);
    SType* conc = build_type(k, signing->a);
    for (int ci = 0; ci < signing->nkids; ci++) {
        Node* conf = signing->kids[ci];
        SType* contract = build_type(k, conf->a);
        conf->a->ty = contract;
        if (contract->kind != SK_CONTRACT) {
            ck_err(k, conf->a, "%s is not a contract", type_name(contract));
            continue;
        }
        for (int j = 0; j < ci; j++)
            if (same_type(contract, node_ty(signing->kids[j]->a)))
                ck_err(k, conf->a, "contract %s is repeated in this signing",
                       type_name(contract));

        Node** selected = (Node**)arena_alloc(
            (size_t)(contract->decl->nkids ? contract->decl->nkids : 1) * sizeof(Node*));
        conf->kids = selected;
        conf->nkids = contract->decl->nkids;
        for (int mi = 0; mi < contract->decl->nkids; mi++) {
            Node* m = contract->decl->kids[mi];
            SType* mft = contract_method_type(k, contract, m);
            uint32_t klen;
            const char* key = method_key(signing->a, m->str, m->len, &klen);
            Sym* head = lookup_val(k, key, klen);
            Sym* found = NULL;
            Node* embedded_field = NULL;
            int promoted_ambiguous = 0;
            for (Sym* fn = head; fn; fn = fn->overload) {
                SType* ft = fn->is_generic
                    ? build_func_type(k, fn->decl, k->ttypes, k->nsub)
                    : fn->type;
                if (!ft || ft->nparams != m->nkids + 1 ||
                    !same_type(ft->params[0], conc)) continue;
                int mut = fn->decl && (fn->decl->flags & F_MUT);
                if (mut != ((m->flags & F_MUT) != 0)) continue;
                int exact = 1;
                for (int p = 0; p < m->nkids; p++)
                    if (!same_type(ft->params[p + 1], mft->params[p]))
                        exact = 0;
                if (!same_type(ft->ret, mft->ret)) exact = 0;
                if (!exact) continue;
                if (found) {
                    ck_err(k, signing, "ambiguous method '%.*s' for contract %s",
                           (int)m->len, m->str, type_name(contract));
                    found = NULL;
                    break;
                }
                found = fn;
            }
            if (!found && conc->decl) {
                for (int fi = 0; fi < conc->nfields; fi++) {
                    Node* fd = conc->decl->kids[fi];
                    SType* eb = conc->fields[fi].t;
                    if (!(fd->flags & F_EMBED) || !eb || eb->kind != SK_STRUCT || !eb->decl || !eb->decl->d) continue;
                    int base_signs = 0;
                    Node* bs = eb->decl->d;
                    if (bs->path && signing->path &&
                        strcmp(bs->path, signing->path) != 0 &&
                        !(bs->flags & F_PUB)) continue;
                    for (int bi = 0; bi < bs->nkids; bi++) {
                        SType* bc = build_type(k, bs->kids[bi]->a);
                        if (same_type(bc, contract)) base_signs = 1;
                    }
                    if (!base_signs) continue;
                    uint32_t eklen;
                    const char* ekey = method_key(eb->decl, m->str, m->len, &eklen);
                    for (Sym* fn = lookup_val(k, ekey, eklen); fn; fn = fn->overload) {
                        SType* ft = fn->type;
                        if (!ft || ft->nparams != m->nkids + 1 ||
                            !same_type(ft->params[0], eb)) continue;
                        int mut = fn->decl && (fn->decl->flags & F_MUT);
                        if (mut != ((m->flags & F_MUT) != 0)) continue;
                        int exact = same_type(ft->ret, mft->ret);
                        for (int p = 0; p < m->nkids; p++)
                            if (!same_type(ft->params[p + 1], mft->params[p])) exact = 0;
                        if (!exact) continue;
                        if (found) {
                            ck_err(k, signing,
                                   "ambiguous promoted method '%.*s' for contract %s",
                                   (int)m->len, m->str, type_name(contract));
                            found = NULL;
                            embedded_field = NULL;
                            promoted_ambiguous = 1;
                            break;
                        }
                        found = fn;
                        embedded_field = fd;
                    }
                    if (promoted_ambiguous) break;
                }
            }
            Node* ref = (Node*)arena_alloc(sizeof(Node));
            memset(ref, 0, sizeof *ref);
            ref->kind = N_IDENT;
            ref->path = signing->path; ref->line = signing->line; ref->col = signing->col;
            ref->str = m->str; ref->len = m->len;
            ref->sym = found;
            ref->a = embedded_field; // non-null means vtable needs an offset thunk
            selected[mi] = ref;
            if (!found && !promoted_ambiguous) {
                ck_err(k, signing, "type %s has no exact method '%.*s' required by %s",
                       type_name(conc), (int)m->len, m->str, type_name(contract));
            } else if (found && found->is_generic && conc->decl && conc->decl->c) {
                for (STypeInst* it = (STypeInst*)conc->decl->c->b; it; it = it->next) {
                    int concrete = 1;
                    for (int a = 0; a < conc->decl->c->nkids; a++)
                        if (it->args[a]->kind == SK_TYPEPARAM) concrete = 0;
                    if (concrete)
                        instantiate_generic_func_args(k, found, it->args,
                                                      conc->decl->c->nkids, signing);
                }
            }
        }
    }
    k->tnames = sn; k->ttypes = st; k->nsub = ns;
}

// --- emitter support ---------------------------------------------------------------

static Scope* g_file_scope; // survives sema_check for stamping and type building
static Diag g_dummy_diag = {"(stamp)", 0}; // re-stamps are pre-checked: silent

uint32_t sema_size(SType* t) { layout(t); return t->size; }
uint32_t sema_align(SType* t) { layout(t); return t->align; }

uint32_t sema_union_payload_off(SType* t) {
    layout(t);
    if (t->kind != SK_UNION) return 0;
    uint32_t maxa = 1;
    for (int i = 0; i < t->nfields; i++)
        if (t->fields[i].t && maxa < sema_align(t->fields[i].t))
            maxa = sema_align(t->fields[i].t);
    return align_to(4, maxa);
}

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
    if (f->flags & F_SPECIALIZED) return; // no default body: nothing to stamp
    Ck k = {0};
    k.d = &g_dummy_diag;
    Sym* gs = (Sym*)f->sym;
    k.scope = gs && gs->decl_scope ? gs->decl_scope : g_file_scope;
    k.tnames = (const char**)arena_alloc((size_t)nargs * sizeof(char*));
    k.ttypes = args;
    k.nsub = nargs;
    for (int i = 0; i < nargs; i++) k.tnames[i] = tp->kids[i]->str;
    check_func_body(&k, f); // re-annotate the body under substitution
}

// Root import aliases live in their own scope, parented to the shared global
// scope, so a same-named local anywhere else in the merged unit — including
// inside a std module the root package never names directly — cannot collide
// with them. Only nodes originating in the root file consult it.
static int is_root_node(Diag* d, Node* n) {
    return n->path && d->path && strcmp(n->path, d->path) == 0;
}

static void install_predeclared_error_types(Scope* file_scope) {
    if (!T_ERROR) {
        T_ERROR = st_new(SK_ERROR);
        T_ERROR->name = "Error";
        layout(T_ERROR);
    }
    Sym* error = sym_new(SYM_TYPE, "Error", 5);
    error->type = T_ERROR;
    scope_insert(file_scope, 1, error);
}

int sema_check(Node* file, Diag* d) {
    static Scope file_scope; // static: outlives the call for stamp re-checks
    static Scope import_scope; // likewise: root generics stamp under it
    memset(&file_scope, 0, sizeof file_scope);
    memset(&import_scope, 0, sizeof import_scope);
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
        ensure_memory_types(); // mut-view singletons exist from the first build
    }
    install_predeclared_error_types(&file_scope);
    // import_scope: root's own aliased imports; see is_root_node
    import_scope.parent = &file_scope;
    Ck k = {0};
    k.d = d;
    k.scope = &file_scope;
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        switch (n->kind) {
        case N_EXPRSTMT: {
            int root = is_root_node(d, n);
            if (root) k.scope = &import_scope;
            check_stmt(&k, n);
            if (root) k.scope = &file_scope;
            break;
        }
        case N_IMPORT: {
            k.scope = &import_scope;
            Sym* m = declare_val(&k, n, SYM_MODULE, n->str, n->len);
            (void)m;
            k.scope = &file_scope;
            break;
        }
        case N_FUNC: {
            if (n->flags & (F_METHOD | F_ASSOC)) {
                const char* kindname = (n->flags & F_ASSOC) ? "associated-function" : "method";
                Sym* owner = lookup_type(&k, n->d->str, n->d->len);
                if (!owner || !owner->type || owner->type->kind != SK_STRUCT) {
                    ck_err(&k, n->d, "%s owner '%.*s' is not a declared struct", kindname, (int)n->d->len, n->d->str);
                    break;
                }
                if (owner->type->decl->path && n->path && strcmp(owner->type->decl->path, n->path) != 0) {
                    ck_err(&k, n, "%ss must be declared in the struct's defining module", kindname);
                    break;
                }
                int want = owner->type->decl->c ? owner->type->decl->c->nkids : 0;
                int got = n->c ? n->c->nkids : 0;
                if (want != got) {
                    ck_err(&k, n, "%s owner must repeat the struct's %d type parameters", kindname, want);
                    break;
                }
                int mismatch = 0;
                for (int ti = 0; ti < want; ti++)
                    if (owner->type->decl->c->kids[ti]->len != n->c->kids[ti]->len ||
                        memcmp(owner->type->decl->c->kids[ti]->str,
                               n->c->kids[ti]->str, n->c->kids[ti]->len) != 0)
                        mismatch = 1;
                if (mismatch) {
                    ck_err(&k, n, "%s owner must repeat the struct's type parameter names", kindname);
                    break;
                }
            }
            Sym* s = declare_func(&k, n, 0);
            if (!s) break;
            if (s->is_generic) s->decl_scope = is_root_node(d, n) ? &import_scope : &file_scope;
            if (!(n->flags & (F_METHOD | F_ASSOC)) && n->len == 4 && memcmp(n->str, "main", 4) == 0) {
                int spells_int = n->a && n->a->kind == N_NAMED && n->a->len == 3 && memcmp(n->a->str, "int", 3) == 0;
                Sym* head = scope_find(k.scope, 0, n->str, n->len);
                if (s->is_generic || n->nkids != 0 || !spells_int ||
                    overload_count(head) != 1)
                    ck_err(&k, n, "entry function must be main(): int");
            }
            if (!s->is_generic && !(n->flags & F_INTRINSIC)) {
                int root = is_root_node(d, n);
                if (root) k.scope = &import_scope;
                check_func_body(&k, n); // single pass: check now, in order
                if (root) k.scope = &file_scope;
            }
            break;
        }
        case N_EXTERN: {
            for (int j = 0; j < n->nkids; j++) {
                Node* f = n->kids[j];
                Sym* s = declare_func(&k, f, 1);
                if (!s) continue;
            }
            break;
        }
        case N_VAR: {
            Node* binder = n->kids[0];
            SType* t = build_type(&k, binder->a);
            if (t && t->kind == SK_ERROR_DOMAIN)
                ck_err(&k, binder, "error-domain names are identities, not value types; use Error");
            Sym* s = declare_val(&k, n, SYM_VAR, binder->str, binder->len);
            if (s) {
                s->type = t;
                s->is_const = (n->flags & F_CONST) != 0 || t->kind == SK_STATICMAP;
                s->is_pub = (n->flags & F_PUB) != 0;
                s->is_global = 1;
                s->decl = binder;
                s->init = n->b;
                binder->sym = s; // emitter: global storage key
            }
            if (n->b) {
                int root = is_root_node(d, n);
                if (root) k.scope = &import_scope;
                if (t->kind == SK_STATICMAP) {
                    if (n->flags & F_CONST) ck_err(&k, n, "const is not permitted on a StaticMap declaration");
                    check_staticmap(&k, n->b, t);
                } else if (n->b->kind == N_STRUCTLIT && !n->b->a && (t->kind == SK_STRUCT || t->kind == SK_BITS))
                    check_structlit(&k, n->b, t);
                else if (n->b->kind == N_ARRAYLIT && t->kind == SK_ARRAY) check_arraylit(&k, n->b, t);
                else coerce(&k, n->b, t);
                if (root) k.scope = &file_scope;
                if (s && t->kind == SK_CONTRACT && (n->b->flags & F_CONST)) s->is_const_backed = 1;
                if (s && t->kind == SK_PRIM && t->prim == P_STRING &&
                    !t->is_mut && n->b->kind == N_STR)
                    s->is_literal_backed = 1;
            } else if (t->kind == SK_STATICMAP) {
                if (n->flags & F_CONST) ck_err(&k, n, "const is not permitted on a StaticMap declaration");
                ck_err(&k, n, "StaticMap declaration requires a brace initializer");
            } else if (n->flags & F_CONST) {
                ck_err(&k, n, "const declaration requires an initializer");
            }
            break;
        }
        case N_STRUCT: case N_BITS: case N_CONTRACT: case N_ENUM: case N_UNION: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            SType* t = st_new(n->kind == N_STRUCT ? SK_STRUCT :
                              n->kind == N_BITS ? SK_BITS :
                              n->kind == N_ENUM ? SK_ENUM :
                              n->kind == N_UNION ? SK_UNION : SK_CONTRACT);
            t->name = n->str;
            t->decl = n;
            s->is_generic = n->c != NULL;
            resolve_type_fixes(&k, s, t, n);
            if (!s->is_generic && n->kind == N_STRUCT) {
                // register first (self-reference via pointer), then build fields
                fill_struct_type(&k, n, t);
            } else if (n->kind == N_BITS) {
                fill_bits_type(&k, n, t);
            } else if (!s->is_generic && n->kind == N_UNION) {
                fill_union_type(&k, n, t);
            } else if (n->kind == N_ENUM) {
                check_enum_decl(&k, n, t);
            }
            break;
        }
        case N_ALIAS: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            int was_forward = s->type && s->type->kind == SK_FORWARD;
            Node* forward_ref = was_forward ? s->decl : NULL;
            s->is_generic = n->c != NULL;
            if (s->is_generic) {
                if (was_forward)
                    ck_err(&k, forward_ref,
                           "forward generic type '%.*s' must be declared before use with explicit arguments",
                           (int)forward_ref->len, forward_ref->str);
                SType* marker = st_new(SK_FORWARD);
                marker->name = n->str;
                marker->decl = n;
                resolve_type_fixes(&k, s, marker, n);
            } else {
                s->type = NULL; // the alias name is not visible in its own expansion
                SType* actual = build_type(&k, n->a);
                resolve_type_fixes(&k, s, actual, n);
            }
            break;
        }
        case N_DISTINCT: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            SType* t = st_new(SK_DISTINCT);
            t->name = n->str;
            t->decl = n;
            resolve_type_fixes(&k, s, t, n);
            t->base = build_type(&k, n->a);
            if (!layout(t)) {
                ck_err(&k, n, "type '%s' has a recursive value layout; use a pointer to break the cycle", n->str);
                t->size = t->align = 1;
                t->calc = 0;
            }
            break;
        }
        case N_ERROR_DOMAIN: {
            Sym* s = declare_type(&k, n, n->str, n->len);
            if (!s) break;
            n->sym = s;
            SType* t = st_new(SK_ERROR_DOMAIN);
            t->name = n->str;
            t->decl = n;
            t->size = t->align = 8;
            resolve_type_fixes(&k, s, t, n);
            check_error_domain_decl(&k, n);
            break;
        }
        case N_SIGNING:
            register_signing(&k, n);
            break;
        default:
            break;
        }
    }

    // Generics #13: a 'specialized' template with zero specializations in the
    // compiled program is a declaration error.
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (n->kind != N_FUNC || !(n->flags & F_SPECIALIZED)) continue;
        Sym* s = node_sym(n);
        if (s && s->is_generic && !s->specs)
            ck_err(&k, n,
                   "generic '%.*s' is marked 'specialized' but no specialization is declared in this program",
                   (int)n->len, n->str);
    }

    for (Sym* s = file_scope.types; s; s = s->next)
        if (s->is_forward)
            ck_err(&k, s->decl,
                   "forward-referenced type '%.*s' was never declared in this module",
                   (int)s->len, s->name);

    // Contracts precede their methods; validate their complete method sets now.
    k.scope = &file_scope;
    for (int i = 0; i < file->nkids; i++) if (file->kids[i]->kind == N_SIGNING) check_signing(&k, file->kids[i]);

    // forward top-level calls: verify existence and argument types
    k.scope = &file_scope;
    for (Pending* pd = k.pending; pd; pd = pd->next) {
        Node* call = pd->call;
        Node* callee = call->a;
        k.scope = pd->scope;
        if (pd->method) {
            SType* r = check_method_call(&k, call, pd->recv, pd->qualified, 1);
            call->ty = r;
            continue;
        }
        if (pd->assoc) {
            SType* r = check_assoc_call(&k, call, pd->recv, 1);
            call->ty = r;
            continue;
        }
        Sym* head = lookup_val(&k, callee->str, callee->len);
        if (!head || head->kind != SYM_FUNC) {
            ck_err(&k, call, "unknown function '%.*s'", (int)callee->len, callee->str);
        } else {
            int ambiguous = 0;
            Sym* chosen = select_call_overload(&k, call, head, 1, &ambiguous);
            if (ambiguous) {
                ck_err(&k, call, "ambiguous call to overloaded function '%.*s'",
                       (int)callee->len, callee->str);
            } else if (!chosen) {
                ck_err(&k, call, "no matching overload '%.*s' for these argument types",
                       (int)callee->len, callee->str);
            } else {
                finish_direct_call(&k, call, chosen);
            }
        }
    }
    k.scope = &file_scope;
    if (d->errors == 0) check_definite_initialization(file, d);
    return d->errors;
}
