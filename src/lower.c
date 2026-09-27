// lower.c — lowering a checked function to SSA IR (pass 5).
//
// SSA is built during the one walk of the function (Braun et al., "Simple and
// Efficient Construction of Static Single Assignment Form", CC 2013): every
// scalar local is a variable written and read per block. A read in a block
// with one predecessor asks that predecessor; with several, it places a phi.
// A block whose predecessors are not all known yet (a loop header before its
// back edge) is unsealed: reads there make an incomplete phi, filled in when
// the block is sealed. A phi whose operands are all one value (or itself) is
// trivial and is replaced by that value.
//
// A local lives in a stack slot instead when something needs its storage (its
// address, a mut argument or method receiver, a volatile access); a mut
// parameter or method receiver lives at the address it arrives as. Those
// variables are read and written by load and store.
//
// A struct, tuple, or array is its scalar leaves (ir_leaves): a local is one
// variable per leaf, and a call passes and returns leaves, which the target
// maps onto its ABI. A local whose storage something needs holds its leaves in
// a slot, as does one holding an array, whose elements an index addresses, or a
// union, whose payload lives in memory (its leaves are then its words, as a
// union's own). An
// aggregate of over 16 leaves is never split: it is copied in memory, and a
// parameter of it arrives by address, as AAPCS64 passes it.
//
// A function that uses anything outside the covered subset is not lowered;
// the AST emitter compiles it (migration steps 1 to 3h: scalar code, floats,
// pointers, globals, calls, structs, tuples, arrays, Error values, and
// strings, slices, and arrays with their indexing, slicing, and debug bounds
// checks, inline calls, switch on a scalar, defer, tagged unions, the debug noalias checks, `continue` on a labeled block, `typeid`,
// computed global constants, an Error's name and message, `panic`, and `for`
// over a string, slice, or array, and static map lookups).
#include "ir.h"
#include "backend.h"

#include <setjmp.h>

typedef struct { // (block, variable) -> current definition
    uint64_t* key;
    int*      val; // value + 1; 0 = empty slot
    size_t    cap, n;
} DefMap;

typedef struct { int var, phi, next; } Incomplete; // an unsealed block's pending phi

typedef struct { // a break/continue target
    const char* label;
    uint32_t    llen;
    int         brk, cont; // blocks; cont < 0 for a labeled block
    int         restart;   // a labeled block: its first block, where `continue label` returns; else -1
} Target;

typedef struct { // an inline call being expanded
    Node*  f;    // the callee
    SType* rt;   // its result type, or NULL
    int    join; // the block after the expansion, where each return goes
    int    res;  // the variable holding the result, or -1
} Expansion;

typedef struct { // a captured `defer`: its call over snapshot variables
    Node* call;
    Node* at;    // the defer statement, whose line the exit-time call carries
} Deferred;

typedef struct {
    IrFunc*     fn;
    Node*       f;
    Node*       file;      // the program
    EmitMode    mode;
    const char* why;
    jmp_buf     fail;
    int         cur;       // block receiving instructions, or -1 when unreachable
    Node*       at;        // statement being lowered
    PtrMap      var_ix;    // binder Sym -> variable index + 1
    PtrMap      stored;    // Syms whose storage something needs: they live in a slot
    IrType*     vtype;
    int*        home;      // variable -> the address value it lives at, or -1 (SSA)
    int         chome;
    int         nvars, cvars;
    DefMap      defs;
    Incomplete* inc;       // pending phis, chained per block from inc_head
    int         ninc, cinc;
    int*        inc_head;  // block -> first pending phi + 1
    int         cinc_head;
    int*        repl;      // value -> replacement + 1 (removed trivial phis)
    int         crepl;
    Target*     tgt;
    int         ntgt, ctgt;
    int         self_head; // release self tail recursion: the loop head block, or -1
    int         is_main;
    SType*      rtype;     // the function's result type, or NULL
    int         result;    // a result in memory: the RESULT address, else -1
    Expansion*  inl;       // inline calls being expanded, innermost last
    int         ninl, cinl;
    Deferred*   defers;    // captured defers, source order
    int         ndefers, cdefers;
} Lw;

typedef struct { // the leaf values of an aggregate, or a scalar's one
    int n;
    int v[IR_MAX_LEAVES];
} Agg;

static _Noreturn void unsupported(Lw* l, const char* why) {
    l->why = why;
    longjmp(l->fail, 1);
}

static void* grow(void* p, int* cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    p = realloc(p, (size_t)c * elem);
    if (!p) oom();
    *cap = c;
    return p;
}

// grow an int array, zeroing the new entries
static int* grow_zero(int* p, int* cap, int need) {
    int old = *cap;
    p = grow(p, cap, need, sizeof(int));
    if (*cap > old) memset(p + old, 0, (size_t)(*cap - old) * sizeof(int));
    return p;
}

// --- types --------------------------------------------------------------------------

static SType* unwrap(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    return t;
}

static const char* type_why(SType* t) {
    t = unwrap(t);
    if (!t) return "untyped value";
    switch (t->kind) {
    case SK_PRIM:
        if (t->prim == P_STRING) return "string";
        return "unsupported primitive";
    case SK_PTR: return "pointer";
    case SK_SLICE: return "slice";
    case SK_ARRAY: return "array";
    case SK_TUPLE: return "tuple";
    case SK_FUNC: return "function value";
    case SK_STRUCT: return "struct";
    case SK_ENUM: return "enum";
    case SK_CONTRACT: return "contract value";
    case SK_ERROR: case SK_ERROR_DOMAIN: return "Error value";
    case SK_UNION: return "union";
    case SK_BITS: return "bits type";
    case SK_STATICMAP: return "static map";
    default: return "unsupported type";
    }
}

static IrType need_type(Lw* l, SType* t) {
    int it = ir_type_of(t);
    if (it < 0) unsupported(l, type_why(t));
    return (IrType)it;
}

static int is_untyped(SType* t) { return !t || t->kind == SK_UT; }

// A string or slice: a byte or element ptr and a u64 length (Types #19).
static int is_view(SType* t) {
    t = unwrap(t);
    return t && (t->kind == SK_SLICE || (t->kind == SK_PRIM && t->prim == P_STRING));
}

// A tagged union: an i32 tag, then its payload, always in memory. On its own
// it is 1 or 2 word-sized leaves, as the ABI moves it, or when over 16 bytes an
// address (Compiler #5, migration step 3).
static int is_union(SType* t) {
    t = unwrap(t);
    return t && t->kind == SK_UNION;
}

static int is_agg(SType* t) {
    t = unwrap(t);
    return t && (t->kind == SK_STRUCT || t->kind == SK_TUPLE || t->kind == SK_ARRAY || is_union(t) || is_view(t));
}

// Whether t holds an array or a union member: a variable of it lives in
// memory, where an element's index or a union's payload addresses it.
static int has_array(SType* t) {
    t = unwrap(t);
    if (!t) return 0;
    if (t->kind == SK_ARRAY) return 1;
    if (t->kind != SK_STRUCT && t->kind != SK_TUPLE) return 0;
    for (int i = 0; i < t->nfields; i++) if (has_array(t->fields[i].t) || is_union(t->fields[i].t)) return 1;
    return 0;
}

// An aggregate holding a union in a member: never split, its leaves are its
// words (like a union's), and a literal of it is built member by member.
static int has_union(SType* t) { return ir_holds_union(t); }

// An aggregate of over IR_MAX_LEAVES leaves: never split, it stays in memory
// and travels by address (AAPCS64 passes it so, as a copy).
static int is_big(SType* t) {
    if (is_union(t) || has_union(t)) return sema_size(unwrap(t)) > 16;
    return is_agg(t) && ir_leaf_count(t) > IR_MAX_LEAVES;
}

// An aggregate stored member by member, never as leaves: too big, or holding a
// union.
static int by_parts(SType* t) { return is_big(t) || has_union(t); }

static int in_memory(SType* t) { return is_union(t) || (is_agg(t) && (has_array(t) || is_big(t))); }

// A parameter in memory that AAPCS64 passes by address (over 16 bytes and no
// floating aggregate): it lives at that address, the caller's copy.
static int param_by_address(SType* t) {
    if (!in_memory(t)) return 0;
    if (is_big(t)) return 1;
    IrLeaf lv[IR_MAX_LEAVES];
    SType* bad;
    int n = ir_leaves(t, lv, &bad);
    if (n < 0 || sema_size(t) <= 16) return 0;
    if (n <= 4 && ir_type_float((IrType)lv[0].type)) {
        int hfa = 1;
        for (int k = 1; k < n; k++) hfa &= lv[k].type == lv[0].type;
        if (hfa) return 0;
    }
    return 1;
}

// The leaves of t, which the IR must cover.
static int leaves(Lw* l, SType* t, IrLeaf* out) {
    SType* bad = NULL;
    int n = ir_leaves(t, out, &bad);
    if (n < 0) unsupported(l, bad && (ir_type_of(bad) >= 0 || is_view(bad)) ? "aggregate of over 16 leaves" : type_why(bad));
    return n;
}

static int leaf_count(Lw* l, SType* t) {
    IrLeaf lv[IR_MAX_LEAVES];
    return leaves(l, t, lv);
}

// Refuse t unless the IR covers each of its leaves, however many.
static void covered(Lw* l, SType* t) {
    if (ir_leaf_count(t) < 0) leaf_count(l, t);
}

// The index of field fi's first leaf among its struct's or tuple's.
static int leaf_index(Lw* l, SType* t, int fi) {
    t = unwrap(t);
    int k = 0;
    for (int i = 0; i < fi; i++) k += leaf_count(l, t->fields[i].t);
    return k;
}

// --- instructions ---------------------------------------------------------------------

static int emit(Lw* l, IrOp op, IrType type) {
    int id = ir_ins_new(l->fn, l->cur, op, type);
    l->fn->ins[id].at = l->at;
    return id;
}

static int emit2(Lw* l, IrOp op, IrType type, int a, int b) {
    int id = emit(l, op, type);
    ir_arg_add(l->fn, id, a);
    if (b >= 0) ir_arg_add(l->fn, id, b);
    return id;
}

static int konst(Lw* l, IrType type, uint64_t v) {
    int id = emit(l, IR_CONST, type);
    l->fn->ins[id].imm = ir_wrap(type, v);
    return id;
}

// A float constant: d rounded to type's precision, as its bit pattern.
static int fconst(Lw* l, IrType type, double d) {
    uint64_t bits;
    if (type == IT_F32) {
        float f = (float)d;
        uint32_t b;
        memcpy(&b, &f, 4);
        bits = b;
    } else {
        memcpy(&bits, &d, 8);
    }
    return konst(l, type, bits);
}

// A literal (or its negation) at type: an integer literal meeting a float
// converts as the AST emitter does.
static int literal(Lw* l, IrType type, Node* x, int neg) {
    if (ir_type_float(type)) {
        double d = x->kind == N_FLOAT ? x->fval : (double)x->ival;
        return fconst(l, type, neg ? -d : d);
    }
    if (x->kind == N_FLOAT) unsupported(l, "float literal at an integer type");
    return konst(l, type, neg ? (uint64_t)0 - x->ival : x->ival);
}


static int at_entry(Lw* l, IrOp op, IrType type);

// A zero of var's type at the top of the entry block: the value of a read no
// definition reaches, which definite initialization confines to dead paths.
static int zero_at_entry(Lw* l, IrType type) { return at_entry(l, IR_CONST, type); }

static void terminate(Lw* l, IrOp op, int arg, int s0, int s1) {
    int id = emit(l, op, IT_VOID);
    if (arg >= 0) ir_arg_add(l->fn, id, arg);
    int from = l->cur;
    if (s0 >= 0) ir_edge_add(l->fn, from, s0);
    if (s1 >= 0) ir_edge_add(l->fn, from, s1);
    l->cur = -1;
}

static void jump(Lw* l, int to) {
    if (l->cur >= 0) terminate(l, IR_JMP, -1, to, -1);
}

static void enter(Lw* l, int b) { l->cur = l->fn->blocks[b].npreds ? b : -1; }

// --- SSA variables ------------------------------------------------------------------

static uint64_t def_key(int b, int var) { return ((uint64_t)(uint32_t)b << 32) | (uint32_t)var; }

static size_t def_slot(const DefMap* m, uint64_t k) {
    uint64_t h = k * UINT64_C(0x9E3779B97F4A7C15);
    return (size_t)(h >> 20) & (m->cap - 1);
}

static void def_put(DefMap* m, uint64_t k, int v) {
    if ((m->n + 1) * 2 > m->cap) {
        DefMap old = *m;
        m->cap = old.cap ? old.cap * 2 : 256;
        m->key = malloc(m->cap * sizeof(uint64_t));
        m->val = calloc(m->cap, sizeof(int));
        if (!m->key || !m->val) oom();
        m->n = 0;
        for (size_t i = 0; i < old.cap; i++) if (old.val[i]) def_put(m, old.key[i], old.val[i] - 1);
        free(old.key);
        free(old.val);
    }
    size_t i = def_slot(m, k);
    while (m->val[i] && m->key[i] != k) i = (i + 1) & (m->cap - 1);
    if (!m->val[i]) m->n++;
    m->key[i] = k;
    m->val[i] = v + 1;
}

static int def_get(const DefMap* m, uint64_t k) { // value, or -1
    if (!m->cap) return -1;
    for (size_t i = def_slot(m, k); m->val[i]; i = (i + 1) & (m->cap - 1))
        if (m->key[i] == k) return m->val[i] - 1;
    return -1;
}

static int find(Lw* l, int v) {
    int r = v;
    while (r < l->crepl && l->repl[r]) r = l->repl[r] - 1;
    while (v != r) { // path compression
        int next = l->repl[v] - 1;
        l->repl[v] = r + 1;
        v = next;
    }
    return r;
}

static void replace(Lw* l, int v, int by) {
    l->repl = grow_zero(l->repl, &l->crepl, l->fn->nins);
    l->repl[v] = by + 1;
    l->fn->ins[v].block = -1;
}

// A new variable for s (NULL for one of lowering's own): an SSA value of
// type, or one living at address home.
// home: -1 for an SSA value, a slot's address value, or ADDR_VAR - v when
// the address itself is SSA variable v (a by-address parameter, which a
// release self tail call may rebind)
enum { ADDR_VAR = -2 };

static int var_new(Lw* l, Sym* s, IrType type, int home) {
    l->vtype = grow(l->vtype, &l->cvars, l->nvars + 1, sizeof(IrType));
    l->home = grow(l->home, &l->chome, l->nvars + 1, sizeof(int));
    l->vtype[l->nvars] = type;
    l->home[l->nvars] = home;
    if (s) ptrmap_put(&l->var_ix, s, (uintptr_t)l->nvars + 1);
    return l->nvars++;
}

static int read_var(Lw* l, int var, int b);

// The address of memory variable var.
static int var_addr(Lw* l, int var) {
    int h = l->home[var];
    return h >= 0 ? h : read_var(l, ADDR_VAR - h, l->cur);
}

// An instruction at the top of the entry block, which dominates every use.
static int at_entry(Lw* l, IrOp op, IrType type) {
    int id = ir_phi_new(l->fn, 0, type); // placed after the entry's (absent) phis
    l->fn->ins[id].op = (uint8_t)op;
    l->fn->ins[id].at = l->f;
    return id;
}

// The variable of local s, made on first sight: in a slot when something
// needs its storage, else an SSA value.
static int var_of(Lw* l, Sym* s) {
    uintptr_t ix = ptrmap_get(&l->var_ix, s);
    if (ix) return (int)ix - 1;
    if (ptrmap_get(&l->stored, s) || in_memory(s->type)) {
        if (is_agg(s->type)) covered(l, s->type);
        int slot = at_entry(l, IR_SLOT, IT_PTR);
        l->fn->ins[slot].imm = sema_size(s->type);
        l->fn->ins[slot].aux = (int32_t)sema_align(s->type);
        return var_new(l, s, IT_VOID, slot);
    }
    if (!is_agg(s->type)) return var_new(l, s, need_type(l, s->type), -1);
    IrLeaf lv[IR_MAX_LEAVES];
    int n = leaves(l, s->type, lv), first = l->nvars;
    if (!n) var_new(l, NULL, IT_VOID, -1); // an empty aggregate's placeholder, never read
    for (int k = 0; k < n; k++) var_new(l, NULL, (IrType)lv[k].type, -1);
    ptrmap_put(&l->var_ix, s, (uintptr_t)first + 1);
    return first;
}

static void write_var(Lw* l, int var, int b, int v) { def_put(&l->defs, def_key(b, var), v); }

static int read_var(Lw* l, int var, int b);

// A phi whose operands are one value, apart from itself, is that value.
static int try_remove_trivial(Lw* l, int phi) {
    int same = -1;
    IrIns* p = &l->fn->ins[phi];
    for (int i = 0; i < p->nargs; i++) {
        int op = find(l, p->args[i]);
        if (op == same || op == phi) continue;
        if (same >= 0) return phi;
        same = op;
    }
    if (same < 0) same = zero_at_entry(l, (IrType)l->fn->ins[phi].type);
    replace(l, phi, same);
    return same;
}

static int add_phi_operands(Lw* l, int var, int phi) {
    int b = l->fn->ins[phi].block;
    for (int i = 0; i < l->fn->blocks[b].npreds; i++) {
        int v = read_var(l, var, l->fn->blocks[b].preds[i]);
        ir_arg_add(l->fn, phi, v);
    }
    return try_remove_trivial(l, phi);
}

static int read_var(Lw* l, int var, int b) {
    int v = def_get(&l->defs, def_key(b, var));
    if (v >= 0) return find(l, v);
    IrBlock* bb = &l->fn->blocks[b];
    if (!bb->sealed) {
        v = ir_phi_new(l->fn, b, l->vtype[var]);
        l->inc = grow(l->inc, &l->cinc, l->ninc + 1, sizeof(Incomplete));
        l->inc[l->ninc] = (Incomplete){ var, v, l->inc_head[b] - 1 };
        l->inc_head[b] = ++l->ninc;
    } else if (bb->npreds == 0) {
        v = zero_at_entry(l, l->vtype[var]);
    } else if (bb->npreds == 1) {
        v = read_var(l, var, bb->preds[0]);
    } else {
        v = ir_phi_new(l->fn, b, l->vtype[var]);
        write_var(l, var, b, v); // breaks cycles through this block
        v = add_phi_operands(l, var, v);
    }
    write_var(l, var, b, v);
    return v;
}

static void seal(Lw* l, int b) {
    if (b < l->cinc_head) {
        for (int i = l->inc_head[b] - 1; i >= 0; i = l->inc[i].next)
            add_phi_operands(l, l->inc[i].var, l->inc[i].phi);
        l->inc_head[b] = 0;
    }
    l->fn->blocks[b].sealed = 1;
}

static int new_block(Lw* l) {
    int b = ir_block_new(l->fn);
    l->inc_head = grow_zero(l->inc_head, &l->cinc_head, l->fn->nblocks);
    return b;
}

// --- storage ------------------------------------------------------------------------

// The local whose storage a place expression names, or NULL: through fields
// of a struct value and elements of an array value, never through a pointer.
static Sym* root_local(Node* x) {
    while (x && (x->kind == N_FIELD_E || x->kind == N_INDEX)) {
        SType* bt = unwrap(node_ty(x->a));
        if (!bt || bt->kind == SK_PTR || (x->kind == N_INDEX && bt->kind != SK_ARRAY)) return NULL;
        x = x->a;
    }
    Sym* s = x && x->kind == N_IDENT ? node_sym(x) : NULL;
    return s && s->kind == SYM_VAR && !s->is_global ? s : NULL;
}

static void need_storage(Lw* l, Node* x) {
    Sym* s = root_local(x);
    if (s) ptrmap_put(&l->stored, s, 1);
}

// Mark every local whose storage the body needs, before any is read. Follows
// a/b/c/kids, and d only as a general for's body (sema's d links form cycles).
static void scan_storage(Lw* l, Node* n) {
    if (!n || n->kind == N_FUNC) return;
    if ((n->kind == N_UNARY && (n->op == T_AMP || n->op == T_VOLATILE)) || n->kind == N_MUTARG)
        need_storage(l, n->a);
    if (n->kind == N_ASSIGN && (n->flags & F_VOLATILE))
        for (int i = 0; i < n->nkids; i++) need_storage(l, n->kids[i]);
    if (n->kind == N_CALL && n->a && n->a->kind == N_FIELD_E && !(n->flags & (F_METHOD | F_ASSOC)))
        need_storage(l, n->a->a); // a method receiver travels by address
    if (n->kind == N_PARAM && node_sym(n) && in_memory(node_sym(n)->type)) // a binder in memory
        ptrmap_put(&l->stored, node_sym(n), 1);
    scan_storage(l, n->a);
    scan_storage(l, n->b);
    scan_storage(l, n->c);
    if (n->kind == N_FOR && (n->flags & F_FORGEN)) scan_storage(l, n->d);
    for (int i = 0; i < n->nkids; i++) scan_storage(l, n->kids[i]);
}

static int load(Lw* l, IrType t, int addr, int32_t off, int flags) {
    int id = emit2(l, IR_LOAD, t, addr, -1);
    l->fn->ins[id].aux = off;
    l->fn->ins[id].flags = (uint8_t)flags;
    return id;
}

static void store(Lw* l, int addr, int32_t off, int v, int flags) {
    int id = emit2(l, IR_STORE, IT_VOID, addr, v);
    l->fn->ins[id].aux = off;
    l->fn->ins[id].flags = (uint8_t)flags;
}

// A debug null check on an address that came from a pointer value.
static int null_checked(Lw* l) { return l->mode == EMIT_DEBUG ? IRF_CHECKED : 0; }

// The check for an access through address value addr: none for a slot or a
// global, which is never null.
static int access_check(Lw* l, int addr) {
    IrOp op = (IrOp)l->fn->ins[addr].op;
    return op == IR_SLOT || op == IR_GLOBAL ? 0 : null_checked(l);
}

static const char* arena_name(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = arena_alloc(n);
    memcpy(p, s, n);
    return p;
}

// The link symbol of a function: an extern's C name, else the mangled Ace name.
static const char* link_name(Sym* s, SType* ft) {
    char buf[512];
    if (s->is_extern) {
        if (s->decl && s->decl->cname) snprintf(buf, sizeof buf, "_%s", s->decl->cname);
        else snprintf(buf, sizeof buf, "_%.*s", (int)s->len, s->name);
        return arena_name(buf);
    }
    return arena_name(fn_mangle(s->name, s->len, ft, s->decl, NULL, 0, 0));
}

// The address of global variable or function s.
static int global_addr(Lw* l, Sym* s) {
    int id = emit(l, IR_GLOBAL, IT_PTR);
    IrIns* x = &l->fn->ins[id];
    x->sym = s;
    if (s->kind == SYM_FUNC) {
        x->name = link_name(s, s->type);
    } else {
        char buf[512];
        snprintf(buf, sizeof buf, "__ace$g$%.*s", (int)s->len, s->name);
        x->name = arena_name(buf);
    }
    return id;
}

// A place: an SSA variable, or memory at addr + off.
typedef struct {
    int     var;   // an SSA variable, or -1 for memory
    int     addr;  // memory: the base address value
    int32_t off;   // memory: the byte offset from addr
    int     flags; // memory: IRF_CHECKED when addr came from a pointer value
    SType*  t;     // the place's type
} Place;

static int expr(Lw* l, Node* x, int want);
static Place element(Lw* l, Node* x);
static int is_place(Node* x);
static void value(Lw* l, Node* x, SType* t, Agg* out);

static Place place(Lw* l, Node* x) {
    Place p = { -1, -1, 0, 0, node_ty(x) };
    switch (x->kind) {
    case N_IDENT: {
        Sym* s = node_sym(x);
        if (!s || s->kind != SYM_VAR) unsupported(l, "non-variable name");
        p.t = s->type;
        if (s->is_extern) unsupported(l, "extern global");
        if (s->is_global) {
            p.addr = global_addr(l, s);
            return p;
        }
        int var = var_of(l, s);
        if (l->home[var] == -1) p.var = var;
        else p.addr = var_addr(l, var);
        return p;
    }
    case N_UNARY:
        if (x->op != T_STAR) break;
        p.addr = expr(l, x->a, IT_PTR);
        p.flags = access_check(l, p.addr);
        p.t = unwrap(node_ty(x->a))->base; // an assignment target's node carries no type
        return p;
    case N_FIELD_E: {
        SType* bt = unwrap(node_ty(x->a));
        if (bt && bt->kind == SK_PTR) { // p.x is (*p).x
            p.addr = expr(l, x->a, IT_PTR);
            p.flags = access_check(l, p.addr);
            bt = unwrap(bt->base);
        } else {
            p = place(l, x->a);
        }
        if (!bt || bt->kind != SK_STRUCT) unsupported(l, bt ? type_why(bt) : "field access");
        int fi = find_field(bt, x->str, x->len);
        if (fi < 0) unsupported(l, "field access");
        if (p.var >= 0) p.var += leaf_index(l, bt, fi); // its leaves' variables
        p.off += (int32_t)field_off(bt, fi);
        p.t = bt->fields[fi].t;
        return p;
    }
    case N_INDEX:
        return element(l, x);
    default:
        break;
    }
    unsupported(l, "assignment target");
}

// Assign variable var, in SSA or at its home.
static void set_var(Lw* l, int var, int v) {
    if (l->home[var] == -1) write_var(l, var, l->cur, v);
    else store(l, var_addr(l, var), 0, v, 0);
}

static int place_read(Lw* l, Place p, int flags) {
    if (p.var >= 0) return read_var(l, p.var, l->cur);
    return load(l, need_type(l, p.t), p.addr, p.off, p.flags | flags);
}

static void place_write(Lw* l, Place p, int v, int flags) {
    if (p.var >= 0) write_var(l, p.var, l->cur, v);
    else store(l, p.addr, p.off, v, p.flags | flags);
}

// Every leaf of a place, a scalar's included.
static void place_get(Lw* l, Place p, int flags, Agg* out) {
    IrLeaf lv[IR_MAX_LEAVES];
    out->n = leaves(l, p.t, lv);
    for (int k = 0; k < out->n; k++)
        out->v[k] = p.var >= 0 ? read_var(l, p.var + k, l->cur)
                               : load(l, (IrType)lv[k].type, p.addr, p.off + (int32_t)lv[k].off, p.flags | flags);
}

static void place_put(Lw* l, Place p, const Agg* a, int flags) {
    IrLeaf lv[IR_MAX_LEAVES];
    leaves(l, p.t, lv);
    for (int k = 0; k < a->n; k++) {
        if (p.var >= 0) write_var(l, p.var + k, l->cur, a->v[k]);
        else store(l, p.addr, p.off + (int32_t)lv[k].off, a->v[k], p.flags | flags);
    }
}

// The address of a memory place; a pointer-derived one is null-checked here.
static int place_address(Lw* l, Place p) {
    if (p.var >= 0) unsupported(l, "address of a register value"); // scan_storage missed it
    if (!p.off && !p.flags) return p.addr;
    int k = konst(l, IT_U64, (uint64_t)p.off);
    int id = emit2(l, IR_PADD, IT_PTR, p.addr, k);
    l->fn->ins[id].imm = 1;
    l->fn->ins[id].flags = (uint8_t)p.flags;
    return id;
}

// --- expressions ------------------------------------------------------------------

static int expr(Lw* l, Node* x, int want);
static void cond(Lw* l, Node* x, int t, int f);

// The IR type of x, taking an untyped constant's type from its context.
static IrType type_in(Lw* l, Node* x, int want) {
    SType* t = node_ty(x);
    if (is_untyped(t)) {
        if (want < 0) unsupported(l, "untyped constant");
        return (IrType)want;
    }
    return need_type(l, t);
}

static int binop_of(TokKind op) {
    switch (op) {
    case T_PLUS: case T_ADDEQ: return IR_ADD;
    case T_MINUS: case T_SUBEQ: return IR_SUB;
    case T_STAR: case T_MULEQ: return IR_MUL;
    case T_SLASH: case T_DIVEQ: return IR_DIV;
    case T_PERCENT: case T_MODEQ: return IR_REM;
    case T_AMP: case T_ANDEQ: return IR_AND;
    case T_PIPE: case T_OREQ: return IR_OR;
    case T_CARET: case T_XOREQ: return IR_XOR;
    case T_SHL: case T_SHLEQ: return IR_SHL;
    case T_SHR: case T_SHREQ: return IR_SHR;
    case T_EQEQ: return IR_EQ;
    case T_BANGEQ: return IR_NE;
    case T_LT: return IR_LT;
    case T_LE: return IR_LE;
    case T_GT: return IR_GT;
    case T_GE: return IR_GE;
    default: return -1;
    }
}

static int is_compare(int op) { return op >= IR_EQ && op <= IR_GE; }

// Arithmetic that a debug build checks (Safety & Debug Builds #6).
static int checked_op(Lw* l, int op, IrType t) {
    if (l->mode != EMIT_DEBUG || t == IT_BOOL || ir_type_float(t)) return 0; // IEEE arithmetic never traps
    return op == IR_ADD || op == IR_SUB || op == IR_MUL || op == IR_DIV || op == IR_REM ||
           op == IR_SHL || op == IR_SHR || op == IR_NEG;
}

static int arith(Lw* l, int op, IrType t, int a, int b) {
    if (ir_type_float(t) && op != IR_ADD && op != IR_SUB && op != IR_MUL && op != IR_DIV && op != IR_NEG)
        unsupported(l, "float operator");
    int id = emit2(l, (IrOp)op, t, a, b);
    if (checked_op(l, op, t)) l->fn->ins[id].flags |= IRF_CHECKED;
    return id;
}

// Operands of a binary node: an untyped side takes the other side's type.
static IrType operand_type(Lw* l, Node* x, int want) {
    if (!is_untyped(node_ty(x->a))) return need_type(l, node_ty(x->a));
    if (!is_untyped(node_ty(x->b))) return need_type(l, node_ty(x->b));
    if (want < 0) unsupported(l, "untyped constant");
    return (IrType)want;
}

// a && b or a || b as a value: branch on a, then b's own value where it
// decides, joined with the constant a decided (so b's compare sets the bool)
static int bool_value(Lw* l, Node* x) {
    int and = x->op == T_ANDAND;
    int rhs = new_block(l), done = new_block(l), join = new_block(l);
    cond(l, x->a, and ? rhs : done, and ? done : rhs);
    seal(l, rhs);
    seal(l, done);
    enter(l, rhs);
    int b = l->cur >= 0 ? expr(l, x->b, IT_BOOL) : -1;
    int from_b = l->cur;
    jump(l, join);
    enter(l, done);
    int k = l->cur >= 0 ? konst(l, IT_BOOL, (uint64_t)!and) : -1;
    jump(l, join);
    seal(l, join);
    enter(l, join);
    if (l->cur < 0) return -1;
    IrBlock* jb = &l->fn->blocks[join];
    if (jb->npreds == 1) return jb->preds[0] == from_b ? b : k;
    int phi = ir_phi_new(l->fn, join, IT_BOOL);
    for (int i = 0; i < jb->npreds; i++) ir_arg_add(l->fn, phi, jb->preds[i] == from_b ? b : k);
    return phi;
}

static int is_self_call(Lw* l, Node* x) {
    if (!x || x->kind != N_CALL || !x->a || (x->a->kind != N_IDENT && x->a->kind != N_FIELD_E)) return 0;
    Sym* s = node_sym(x);
    return s && s->kind == SYM_FUNC && s->decl == l->f;
}

static void value(Lw* l, Node* x, SType* t, Agg* out);
static int new_slot(Lw* l, SType* t);
static void store_value(Lw* l, Node* x, SType* t, int dst, int32_t off);
static int union_equal(Lw* l, Node* x, int negate);
static Place array_place(Lw* l, Node* x);

// Arguments, flattened to leaves, and how they group into parts.
typedef struct {
    int    v[40];
    int    n;
    IrPart part[40];
    int    nparts;
    int    agg;    // some part is an aggregate
} Args;

static void arg_scalar(Lw* l, Args* g, int v) {
    if (g->n == 40) unsupported(l, "too many arguments");
    g->part[g->nparts++] = (IrPart){ NULL, g->n, 1 };
    g->v[g->n++] = v;
}

// A call argument: the address of its place for a mut parameter or method
// receiver, else its leaves at the parameter's type.
static void call_arg(Lw* l, Args* g, Node* a, SType* pt, int by_address) {
    if (a->kind == N_MUTARG) a = a->a;
    if (by_address) {
        arg_scalar(l, g, place_address(l, place(l, a)));
        return;
    }
    if (!is_agg(pt)) {
        arg_scalar(l, g, expr(l, a, need_type(l, pt)));
        return;
    }
    if (is_big(pt)) { // the address of a copy
        covered(l, pt);
        int slot = new_slot(l, pt);
        store_value(l, a, pt, slot, 0);
        arg_scalar(l, g, slot);
        return;
    }
    Agg v;
    value(l, a, pt, &v);
    if (g->n + v.n > 40) unsupported(l, "too many arguments");
    g->part[g->nparts++] = (IrPart){ pt, g->n, v.n };
    for (int k = 0; k < v.n; k++) g->v[g->n++] = v.v[k];
    g->agg = 1;
}

typedef struct { int ptr, len; uint64_t esz; } Range; // a slice argument, ptr < 0 for another
static void noalias_check(Lw* l, Node* d, const Range* r, int n);

// Arguments of a direct call to s: an instance method's receiver first.
// Returns the leaf index of the first variadic argument, or -1.
static int call_args(Lw* l, Node* call, Sym* s, Args* g) {
    Node* callee = call->a;
    Node* d = s->decl;
    SType* ft = node_ty(callee);
    if (!ft || ft->kind != SK_FUNC) ft = s->type;
    int recv = callee->kind == N_FIELD_E && !s->is_extern && !(call->flags & (F_METHOD | F_ASSOC));
    int n = recv + call->nkids;
    int named = ft ? ft->nparams : n;
    int varargs = s->is_extern && d && (d->flags & F_VARARGS);
    if (n > 40) unsupported(l, "too many arguments");
    if (varargs ? n < named : n != named) unsupported(l, "default argument");
    int first_var = -1;
    Range rng[40];
    int noalias = 0;
    for (int i = 0; l->mode == EMIT_DEBUG && d && i < n && i < d->nkids; i++) noalias |= (d->kids[i]->flags & F_NOALIAS) != 0;
    for (int i = 0; i < n; i++) {
        rng[i].ptr = -1;
        Node* a = recv && i == 0 ? callee->a : call->kids[i - recv];
        if (i >= named) { // variadic: promoted to 64 bits (an f32 to f64)
            SType* at = node_ty(a);
            if (is_agg(at)) unsupported(l, "variadic aggregate");
            IrType t = is_untyped(at) ? (a->kind == N_FLOAT ? IT_F64 : IT_I64) : need_type(l, at);
            int v = expr(l, a, t);
            if (t == IT_F32) v = emit2(l, IR_CAST, IT_F64, v, -1);
            else if (t != IT_PTR && ir_type_bits(t) < 64) v = emit2(l, IR_CAST, ir_type_signed(t) ? IT_I64 : IT_U64, v, -1);
            if (first_var < 0) first_var = g->n;
            arg_scalar(l, g, v);
            continue;
        }
        int by_address = (d && i < d->nkids && (d->kids[i]->flags & F_MUT)) ||
                         (d && (d->flags & F_METHOD) && i == 0);
        call_arg(l, g, a, ft->params[i], by_address);
        SType* pu = unwrap(ft->params[i]);
        if (!noalias || !pu || pu->kind != SK_SLICE) continue;
        if (by_address) unsupported(l, "noalias contract"); // a descriptor the callee replaces
        IrPart* pp = &g->part[g->nparts - 1];
        rng[i] = (Range){ g->v[pp->first], g->v[pp->first + 1], sema_size(pu->base) };
    }
    if (noalias) noalias_check(l, d, rng, n);
    return first_var;
}

// Emit call id's arguments, and its signature when a part or the result rt
// is an aggregate; an aggregate result's leaves follow as projections into
// res (none without res, whose value is unused). One of over 16 leaves goes
// to memory at dst (a new stack object when dst < 0), passed last.
static int call_finish(Lw* l, int id, Args* g, SType* rt, Agg* res, int dst) {
    for (int k = 0; k < g->n; k++) ir_arg_add(l->fn, id, g->v[k]);
    if (is_big(rt)) {
        covered(l, rt);
        if (dst < 0) dst = new_slot(l, rt);
        ir_arg_add(l->fn, id, dst);
        l->fn->ins[id].flags |= IRF_SRET;
    }
    if (!g->agg && !is_agg(rt)) return id;
    IrSig* sig = (IrSig*)arena_alloc(sizeof(IrSig));
    sig->parts = (IrPart*)arena_alloc((size_t)(g->nparts ? g->nparts : 1) * sizeof(IrPart));
    memcpy(sig->parts, g->part, (size_t)g->nparts * sizeof(IrPart));
    sig->nparts = g->nparts;
    sig->ret = (IrPart){ NULL, 0, 0 };
    l->fn->ins[id].sig = sig;
    if (is_big(rt)) return id;
    if (!is_agg(rt)) {
        if (rt && unwrap(rt)->kind != SK_VOID) sig->ret.n = 1;
        return id;
    }
    IrLeaf lv[IR_MAX_LEAVES];
    int n = leaves(l, rt, lv);
    sig->ret = (IrPart){ rt, 0, n };
    if (res) {
        res->n = n;
        for (int k = 0; k < n; k++) {
            res->v[k] = emit2(l, IR_PROJ, (IrType)lv[k].type, id, -1);
            l->fn->ins[res->v[k]].imm = (uint64_t)k;
        }
    }
    return id;
}

static IrType result_type(Lw* l, SType* rt) {
    if (!rt || unwrap(rt)->kind == SK_VOID || is_agg(rt)) return IT_VOID;
    return need_type(l, rt);
}

static int is_domain(SType* t) { return t && t->kind == SK_ERROR_DOMAIN; }

static int widen(Lw* l, int v, IrType from, IrType to) { return from == to ? v : emit2(l, IR_CAST, to, v, -1); }

// A call to routine name (its link symbol) with scalar arguments args[0..n):
// a C function when c, whose sym marks it extern, bound at link time; else
// one of the object's own runtime routines.
static int call_routine(Lw* l, const char* name, int c, IrType t, const int* args, int n) {
    static Sym c_routine = { .kind = SYM_FUNC, .is_extern = 1 };
    int id = emit(l, IR_CALL, t);
    l->fn->ins[id].name = name;
    if (c) l->fn->ins[id].sym = &c_routine;
    for (int k = 0; k < n; k++) ir_arg_add(l->fn, id, args[k]);
    return id;
}

static int call_extern(Lw* l, const char* name) { return call_routine(l, name, 1, IT_PTR, NULL, 0); }

// Continue in a new block when bool c holds; guard_end joins the path where
// it did not. A variable written between them meets its earlier value there.
static int guard(Lw* l, int c) {
    int yes = new_block(l), join = new_block(l);
    terminate(l, IR_BR, c, yes, join);
    seal(l, yes);
    enter(l, yes);
    return join;
}

static void guard_end(Lw* l, int join) {
    jump(l, join);
    seal(l, join);
    enter(l, join);
}

static int cmp(Lw* l, IrOp op, int a, int b) { return emit2(l, op, IT_BOOL, a, b); }

// A variable of lowering's own, first holding v.
static int temp_var(Lw* l, IrType t, int v) {
    int var = var_new(l, NULL, t, -1);
    write_var(l, var, l->cur, v);
    return var;
}

// The length of the NUL-terminated bytes at p: a u64 counted up.
static int c_length(Lw* l, int p) {
    int n = var_new(l, NULL, IT_U64, -1);
    write_var(l, n, l->cur, konst(l, IT_U64, 0));
    int head = new_block(l), body = new_block(l), exit = new_block(l);
    jump(l, head);
    enter(l, head);
    int i = read_var(l, n, head);
    int at = emit2(l, IR_PADD, IT_PTR, p, i);
    l->fn->ins[at].imm = 1;
    int b = load(l, IT_U8, at, 0, 0);
    terminate(l, IR_BR, emit2(l, IR_NE, IT_BOOL, b, konst(l, IT_U8, 0)), body, exit);
    seal(l, body);
    enter(l, body);
    write_var(l, n, l->cur, emit2(l, IR_ADD, IT_U64, i, konst(l, IT_U64, 1)));
    jump(l, head);
    seal(l, head);
    seal(l, exit);
    enter(l, exit);
    return i;
}

static void view_parts(Lw* l, Node* x, int* ptr, int* len);

// The bytes of [N]u8, []u8, or string x: its address and length.
static void bytes_parts(Lw* l, Node* x, int* ptr, int* len) {
    if (x->kind == N_MUTARG) x = x->a;
    SType* t = unwrap(node_ty(x));
    if (t && t->kind == SK_ARRAY) {
        *ptr = place_address(l, array_place(l, x));
        *len = konst(l, IT_U64, t->count);
        return;
    }
    view_parts(l, x, ptr, len);
}

// A slice result's leaves from variables pv and nv, into res when wanted.
static int slice_out(Lw* l, Agg* res, int pv, int nv) {
    if (res) {
        res->n = 2;
        res->v[0] = read_var(l, pv, l->cur);
        res->v[1] = read_var(l, nv, l->cur);
    }
    return -1;
}

// Debug: trap through the call contract failure unless c holds.
static void contract_check(Lw* l, int c) {
    int id = emit2(l, IR_CHECK, IT_VOID, c, -1);
    l->fn->ins[id].at = l->at;
}

// Debug, at a call boundary (Safety #7): the byte range of each slice argument
// must be representable, and two whose parameters include a noalias one must
// be disjoint unless one is empty. Nothing else is walked.
static void noalias_check(Lw* l, Node* d, const Range* r, int n) {
    int end[40];
    for (int i = 0; i < n && i < d->nkids; i++) {
        if (r[i].ptr < 0) continue;
        int bytes = r[i].len;
        if (r[i].esz != 1) {
            contract_check(l, cmp(l, IR_LE, bytes, konst(l, IT_U64, UINT64_MAX / r[i].esz)));
            bytes = emit2(l, IR_MUL, IT_U64, bytes, konst(l, IT_U64, r[i].esz));
        }
        end[i] = emit2(l, IR_PADD, IT_PTR, r[i].ptr, bytes);
        l->fn->ins[end[i]].imm = 1;
        contract_check(l, cmp(l, IR_GE, end[i], r[i].ptr));
    }
    for (int i = 0; i < n && i < d->nkids; i++) {
        if (r[i].ptr < 0) continue;
        for (int j = i + 1; j < n && j < d->nkids; j++) {
            if (r[j].ptr < 0 || !((d->kids[i]->flags | d->kids[j]->flags) & F_NOALIAS)) continue;
            int both = emit2(l, IR_AND, IT_BOOL, cmp(l, IR_NE, r[i].len, konst(l, IT_U64, 0)),
                             cmp(l, IR_NE, r[j].len, konst(l, IT_U64, 0)));
            int some = guard(l, both);
            contract_check(l, emit2(l, IR_OR, IT_BOOL, cmp(l, IR_GE, r[j].ptr, end[i]), cmp(l, IR_GE, r[i].ptr, end[j])));
            guard_end(l, some);
        }
    }
}

// Debug allocator tracking (Safety & Debug Builds #9): the registry routines.
static void registry_add(Lw* l, int ptr, int size, int align) {
    if (l->mode != EMIT_DEBUG) return;
    int a[3] = { ptr, size, align };
    call_routine(l, "__ace$registry_add", 0, IT_VOID, a, 3);
}

static int registry_find(Lw* l, int ptr, int size) { // the record of a live allocation
    int a[2] = { ptr, size };
    return call_routine(l, "__ace$registry_find", 0, IT_PTR, a, 2);
}

// mem.alloc(size, align): the null slice unless size > 0 and align is a power
// of two up to 16 and the allocator succeeds.
static int mem_alloc(Lw* l, Node* x, Agg* res) {
    int size = expr(l, x->kids[0], IT_I64), align = expr(l, x->kids[1], IT_I64);
    int pv = temp_var(l, IT_PTR, konst(l, IT_PTR, 0)), nv = temp_var(l, IT_U64, konst(l, IT_U64, 0));
    int pow2 = cmp(l, IR_EQ, emit2(l, IR_AND, IT_I64, align, emit2(l, IR_SUB, IT_I64, align, konst(l, IT_I64, 1))),
                   konst(l, IT_I64, 0));
    int ok = emit2(l, IR_AND, IT_BOOL, cmp(l, IR_GT, size, konst(l, IT_I64, 0)),
                   emit2(l, IR_AND, IT_BOOL, cmp(l, IR_GT, align, konst(l, IT_I64, 0)),
                         emit2(l, IR_AND, IT_BOOL, cmp(l, IR_LE, align, konst(l, IT_I64, 16)), pow2)));
    int valid = guard(l, ok);
    int p = call_routine(l, "_malloc", 1, IT_PTR, &size, 1);
    int got = guard(l, cmp(l, IR_NE, p, konst(l, IT_PTR, 0)));
    registry_add(l, p, size, align);
    write_var(l, pv, l->cur, p);
    write_var(l, nv, l->cur, emit2(l, IR_CAST, IT_U64, size, -1));
    guard_end(l, got);
    guard_end(l, valid);
    return slice_out(l, res, pv, nv);
}

// mem.realloc(data, size): data unchanged and the null slice unless size > 0
// and the allocator succeeds; debug moves data's registry record.
static int mem_realloc(Lw* l, Node* x, Agg* res) {
    int old, oldn;
    view_parts(l, x->kids[0], &old, &oldn);
    int size = expr(l, x->kids[1], IT_I64);
    int pv = temp_var(l, IT_PTR, konst(l, IT_PTR, 0)), nv = temp_var(l, IT_U64, konst(l, IT_U64, 0));
    int valid = guard(l, cmp(l, IR_GT, size, konst(l, IT_I64, 0)));
    int rec = l->mode == EMIT_DEBUG ? registry_find(l, old, oldn) : -1;
    int a[2] = { old, size };
    int p = call_routine(l, "_realloc", 1, IT_PTR, a, 2);
    int got = guard(l, cmp(l, IR_NE, p, konst(l, IT_PTR, 0)));
    int n = emit2(l, IR_CAST, IT_U64, size, -1);
    if (rec >= 0) {
        store(l, rec, 0, p, 0);
        store(l, rec, 8, n, 0);
    }
    write_var(l, pv, l->cur, p);
    write_var(l, nv, l->cur, n);
    guard_end(l, got);
    guard_end(l, valid);
    return slice_out(l, res, pv, nv);
}

// mem.alloc[T](count), mem.zalloc[T](count), mem.realloc[T](data, count):
// count elements of T; a zero byte size, an alignment over 16, an overflowing
// byte count, or an allocator failure is the null slice (data unchanged).
static int mem_alloc_typed(Lw* l, Node* x, int bi, Agg* res) {
    SType* elem = unwrap(node_ty(x))->base;
    uint64_t esz = sema_size(elem), eal = sema_align(elem);
    int old = -1, oldn = -1;
    if (bi == BI_MEM_REALLOC_T) view_parts(l, x->kids[0], &old, &oldn);
    Node* cn = x->kids[bi == BI_MEM_REALLOC_T];
    IrType ct = type_in(l, cn, IT_U64);
    int count = widen(l, expr(l, cn, ct), ct, IT_U64);
    int pv = temp_var(l, IT_PTR, konst(l, IT_PTR, 0)), nv = temp_var(l, IT_U64, konst(l, IT_U64, 0));
    if (!esz || eal > 16) return slice_out(l, res, pv, nv);
    int ok = emit2(l, IR_AND, IT_BOOL, cmp(l, IR_NE, count, konst(l, IT_U64, 0)),
                   cmp(l, IR_LE, count, konst(l, IT_U64, UINT64_MAX / esz)));
    int valid = guard(l, ok);
    int bytes = emit2(l, IR_MUL, IT_U64, count, konst(l, IT_U64, esz));
    int p, rec = -1;
    if (bi == BI_MEM_REALLOC_T) {
        int old_bytes = emit2(l, IR_MUL, IT_U64, oldn, konst(l, IT_U64, esz));
        if (l->mode == EMIT_DEBUG) rec = registry_find(l, old, old_bytes);
        int a[2] = { old, bytes };
        p = call_routine(l, "_realloc", 1, IT_PTR, a, 2);
    } else if (bi == BI_MEM_ZALLOC_T) { // the platform zero-fills
        int a[2] = { bytes, konst(l, IT_U64, 1) };
        p = call_routine(l, "_calloc", 1, IT_PTR, a, 2);
    } else {
        p = call_routine(l, "_malloc", 1, IT_PTR, &bytes, 1);
    }
    int got = guard(l, cmp(l, IR_NE, p, konst(l, IT_PTR, 0)));
    if (rec >= 0) {
        store(l, rec, 0, p, 0);
        store(l, rec, 8, bytes, 0);
    } else if (bi != BI_MEM_REALLOC_T) {
        registry_add(l, p, bytes, konst(l, IT_U64, eal));
    }
    write_var(l, pv, l->cur, p);
    write_var(l, nv, l->cur, count);
    guard_end(l, got);
    guard_end(l, valid);
    return slice_out(l, res, pv, nv);
}

// mem.release(mut data), mem.release[T](mut data): free a non-null data
// (debug first drops its registry record), then data becomes null.
static int mem_release(Lw* l, Node* x) {
    Node* raw = x->kids[0]->kind == N_MUTARG ? x->kids[0]->a : x->kids[0];
    Place p = place(l, raw);
    SType* st = unwrap(p.t);
    uint64_t esz = st && st->kind == SK_SLICE ? sema_size(st->base) : 1;
    int ptr = p.var >= 0 ? read_var(l, p.var, l->cur) : load(l, IT_PTR, p.addr, p.off, p.flags);
    int live = guard(l, cmp(l, IR_NE, ptr, konst(l, IT_PTR, 0)));
    int n = p.var >= 0 ? read_var(l, p.var + 1, l->cur) : load(l, IT_U64, p.addr, p.off + 8, p.flags);
    if (l->mode == EMIT_DEBUG) {
        int a[2] = { ptr, esz == 1 ? n : emit2(l, IR_MUL, IT_U64, n, konst(l, IT_U64, esz)) };
        call_routine(l, "__ace$registry_remove", 0, IT_VOID, a, 2);
    }
    call_routine(l, "_free", 1, IT_VOID, &ptr, 1);
    guard_end(l, live);
    Agg z = { 2, { konst(l, IT_PTR, 0), konst(l, IT_U64, 0) } };
    place_put(l, p, &z, 0);
    return -1;
}

// A byte routine call; the result is an i64 when name is mem_compare's.
static int rtcall(Lw* l, const char* name, int a, int b, int n) {
    int id = emit(l, IR_RTCALL, strcmp(name, "__ace$mem_compare") == 0 ? IT_I64 : IT_VOID);
    l->fn->ins[id].name = name;
    ir_arg_add(l->fn, id, a);
    ir_arg_add(l->fn, id, b);
    ir_arg_add(l->fn, id, n);
    return id;
}

// mem.copy, mem.move, mem.fill, mem.zero (Values & Passing: byte operations):
// debug checks equal lengths, and for copy that nonempty ranges neither wrap
// nor overlap (noalias), before the routine.
static int mem_write(Lw* l, Node* x, int bi) {
    int dst, dlen;
    view_parts(l, x->kids[0]->kind == N_MUTARG ? x->kids[0]->a : x->kids[0], &dst, &dlen);
    if (bi == BI_MEM_FILL || bi == BI_MEM_ZERO) {
        int v = bi == BI_MEM_FILL ? expr(l, x->kids[1], IT_U8) : konst(l, IT_U8, 0);
        rtcall(l, "__ace$mem_fill", dst, v, dlen);
        return -1;
    }
    int src, slen;
    view_parts(l, x->kids[1], &src, &slen);
    if (l->mode == EMIT_DEBUG) {
        contract_check(l, cmp(l, IR_EQ, slen, dlen));
        if (bi == BI_MEM_COPY) {
            int some = guard(l, cmp(l, IR_NE, slen, konst(l, IT_U64, 0)));
            int dend = emit2(l, IR_PADD, IT_PTR, dst, slen), send = emit2(l, IR_PADD, IT_PTR, src, slen);
            l->fn->ins[dend].imm = l->fn->ins[send].imm = 1;
            contract_check(l, cmp(l, IR_GE, dend, dst));
            contract_check(l, cmp(l, IR_GE, send, src));
            contract_check(l, emit2(l, IR_OR, IT_BOOL, cmp(l, IR_GE, dst, send), cmp(l, IR_GE, src, dend)));
            guard_end(l, some);
        }
    }
    rtcall(l, bi == BI_MEM_MOVE ? "__ace$mem_move" : "__ace$mem_copy", dst, src, slen);
    return -1;
}

// mem.compare(a, b): the common prefix's order, then the shorter first.
static int mem_compare(Lw* l, Node* x) {
    int ap, an, bp, bn;
    bytes_parts(l, x->kids[0], &ap, &an);
    bytes_parts(l, x->kids[1], &bp, &bn);
    int count = temp_var(l, IT_U64, an);
    int shorter = guard(l, cmp(l, IR_LT, bn, an));
    write_var(l, count, l->cur, bn);
    guard_end(l, shorter);
    int r = rtcall(l, "__ace$mem_compare", ap, bp, read_var(l, count, l->cur));
    int out = temp_var(l, IT_I64, r);
    int same = guard(l, cmp(l, IR_EQ, r, konst(l, IT_I64, 0)));
    int gt = emit2(l, IR_CAST, IT_I64, cmp(l, IR_GT, an, bn), -1), lt = emit2(l, IR_CAST, IT_I64, cmp(l, IR_LT, an, bn), -1);
    write_var(l, out, l->cur, emit2(l, IR_SUB, IT_I64, gt, lt));
    guard_end(l, same);
    return read_var(l, out, l->cur);
}

// mem.equal(a, b): equal lengths, then equal bytes.
static int mem_equal(Lw* l, Node* x) {
    int ap, an, bp, bn;
    bytes_parts(l, x->kids[0], &ap, &an);
    bytes_parts(l, x->kids[1], &bp, &bn);
    int out = temp_var(l, IT_BOOL, konst(l, IT_BOOL, 0));
    int same = guard(l, cmp(l, IR_EQ, an, bn));
    int r = rtcall(l, "__ace$mem_compare", ap, bp, an);
    write_var(l, out, l->cur, cmp(l, IR_EQ, r, konst(l, IT_I64, 0)));
    guard_end(l, same);
    return read_var(l, out, l->cur);
}

// fmt.printf (Standard Library: fmt): every argument once, in order; then
// each part of sema's plan through its helper, stopping at the first that
// writes nothing or fails, whose count is then the result; else the total.
static int printf_call(Lw* l, Node* x) {
    PrintfPlan* plan = (PrintfPlan*)x->aux;
    int nargs = x->nkids - 1;
    Agg* cap = (Agg*)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(Agg));
    SType** capt = (SType**)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(SType*));
    for (int i = 0; i < nargs; i++) {
        Node* arg = x->kids[i + 1]->kind == N_MUTARG ? x->kids[i + 1]->a : x->kids[i + 1];
        SType* t = node_ty(arg);
        for (int k = 0; k < plan->nparts && is_untyped(t); k++) // an untyped constant takes its helper's type
            if (plan->parts[k].arg == i + 1 && plan->parts[k].helper) t = ((Sym*)plan->parts[k].helper)->type->params[0];
        value(l, arg, t, &cap[i]);
        capt[i] = t;
    }
    int total = temp_var(l, IT_I64, konst(l, IT_I64, 0));
    int done = new_block(l);
    for (int i = 0; i < plan->nparts && l->cur >= 0; i++) {
        PrintfPart* part = &plan->parts[i];
        Sym* h = (Sym*)part->helper;
        SType* ft = h->type;
        Args g;
        memset(&g, 0, sizeof g);
        Agg v;
        SType* vt = ft->params[0];
        int empty = -1;
        if (part->kind == PF_TEXT) {
            v.n = 2;
            v.v[0] = emit(l, IR_STR, IT_PTR);
            l->fn->ins[v.v[0]].name = plan->format->str + part->off;
            l->fn->ins[v.v[0]].imm = part->len;
            v.v[1] = konst(l, IT_U64, part->len);
        } else if (part->kind == PF_PERCENT) {
            v.n = 1;
            v.v[0] = konst(l, need_type(l, vt), '%');
        } else {
            v = cap[part->arg - 1];
            if (!is_agg(vt)) v.v[0] = widen(l, v.v[0], need_type(l, capt[part->arg - 1]), need_type(l, vt));
            if (part->kind == PF_STRING) empty = guard(l, cmp(l, IR_NE, v.v[1], konst(l, IT_U64, 0)));
        }
        if (is_agg(vt)) {
            g.part[g.nparts++] = (IrPart){ vt, 0, v.n };
            for (int k = 0; k < v.n; k++) g.v[g.n++] = v.v[k];
            g.agg = 1;
        } else {
            arg_scalar(l, &g, v.v[0]);
        }
        if (part->kind == PF_BINARY || part->kind == PF_OCTAL || part->kind == PF_HEX_LOWER || part->kind == PF_HEX_UPPER) {
            int base = part->kind == PF_BINARY ? 2 : part->kind == PF_OCTAL ? 8 : 16;
            arg_scalar(l, &g, konst(l, need_type(l, ft->params[1]), (uint64_t)base));
            arg_scalar(l, &g, konst(l, need_type(l, ft->params[2]), part->kind == PF_HEX_UPPER));
        } else if (part->kind == PF_FIXED) {
            arg_scalar(l, &g, konst(l, need_type(l, ft->params[1]), part->precision));
        }
        int id = emit(l, IR_CALL, IT_I64);
        l->fn->ins[id].sym = h;
        l->fn->ins[id].name = arena_name(fn_mangle(h->name, h->len, ft, h->decl, h->spec_args,
                                                   h->spec_args ? h->spec_nargs : 0, h->spec_args != NULL));
        call_finish(l, id, &g, ft->ret, NULL, -1);
        int wrote = new_block(l), failed = new_block(l);
        terminate(l, IR_BR, cmp(l, IR_GT, id, konst(l, IT_I64, 0)), wrote, failed);
        seal(l, wrote);
        seal(l, failed);
        enter(l, failed); // its count is the result
        write_var(l, total, l->cur, id);
        jump(l, done);
        enter(l, wrote);
        write_var(l, total, l->cur, emit2(l, IR_ADD, IT_I64, read_var(l, total, l->cur), id));
        if (empty >= 0) guard_end(l, empty);
    }
    jump(l, done);
    seal(l, done);
    enter(l, done);
    return read_var(l, total, l->cur);
}

// A compiler-backed call the IR covers: its value (a slice's or string's
// leaves into res), or -2 for one it does not.
static int builtin(Lw* l, Node* x, int bi, Agg* res) {
    switch (bi) {
    case BI_BITS_FROM_FLOAT: case BI_BITS_TO_FLOAT: { // the same bits (Types: bits.cast)
        IrType to = need_type(l, node_ty(x)), from = need_type(l, node_ty(x->kids[0]));
        return emit2(l, IR_BITCAST, to, expr(l, x->kids[0], from), -1);
    }
    case BI_BITS_POP_COUNT: case BI_BITS_LEAD_ZEROS: case BI_BITS_TRAIL_ZEROS: case BI_BITS_BYTE_SWAP: {
        IrType t = need_type(l, node_ty(x->kids[0]));
        IrOp op = bi == BI_BITS_POP_COUNT ? IR_POPCNT : bi == BI_BITS_LEAD_ZEROS ? IR_CLZ
                : bi == BI_BITS_TRAIL_ZEROS ? IR_CTZ : IR_BSWAP;
        int v = emit2(l, op, t, expr(l, x->kids[0], t), -1);
        return widen(l, v, t, need_type(l, node_ty(x)));
    }
    case BI_BITS_ROTATE_LEFT: case BI_BITS_ROTATE_RIGHT: {
        IrType t = need_type(l, node_ty(x->kids[0]));
        int v = expr(l, x->kids[0], t);
        IrType ct = type_in(l, x->kids[1], IT_U64);
        int c = widen(l, expr(l, x->kids[1], ct), ct, IT_U64);
        return emit2(l, bi == BI_BITS_ROTATE_LEFT ? IR_ROTL : IR_ROTR, t, v, c);
    }
    case BI_BITS_WRAP_ADD: case BI_BITS_WRAP_SUB: case BI_BITS_WRAP_MUL: { // never checked
        IrType t = need_type(l, node_ty(x));
        int a = expr(l, x->kids[0], t);
        return emit2(l, bi == BI_BITS_WRAP_ADD ? IR_ADD : bi == BI_BITS_WRAP_SUB ? IR_SUB : IR_MUL, t, a,
                     expr(l, x->kids[1], t));
    }
    case BI_MATH_SQRT: case BI_MATH_FLOOR: case BI_MATH_CEIL: {
        IrType t = need_type(l, node_ty(x));
        return emit2(l, bi == BI_MATH_SQRT ? IR_SQRT : bi == BI_MATH_FLOOR ? IR_FLOOR : IR_CEIL, t,
                     expr(l, x->kids[0], t), -1);
    }
    case BI_MATH_FMA: {
        IrType t = need_type(l, node_ty(x));
        int a = expr(l, x->kids[0], t), b = expr(l, x->kids[1], t), c = expr(l, x->kids[2], t);
        int id = emit2(l, IR_FMA, t, a, b);
        ir_arg_add(l->fn, id, c);
        return id;
    }
    case BI_INTERNAL_LOAD_U64: { // the native-endian word at text[index], unchecked
        int p;
        view_parts(l, x->kids[0], &p, NULL);
        IrType it = type_in(l, x->kids[1], IT_U64);
        int at = emit2(l, IR_PADD, IT_PTR, p, widen(l, expr(l, x->kids[1], it), it, IT_U64));
        l->fn->ins[at].imm = 1;
        return load(l, IT_U64, at, 0, 0);
    }
    case BI_MEM_ALLOC: case BI_MEM_REALLOC: case BI_MEM_RELEASE: case BI_MEM_ALLOC_T: case BI_MEM_ZALLOC_T:
    case BI_MEM_REALLOC_T: case BI_MEM_RELEASE_T: case BI_MEM_ALLOC_SIZE: case BI_MEM_COPY: case BI_MEM_MOVE:
    case BI_MEM_FILL: case BI_MEM_ZERO: case BI_MEM_COMPARE: case BI_MEM_EQUAL: case BI_FMT_PRINTF:
        if (x->ival != (uint64_t)bi) return -2; // sema checked it as the builtin
        switch (bi) {
        case BI_MEM_ALLOC: return mem_alloc(l, x, res);
        case BI_MEM_REALLOC: return mem_realloc(l, x, res);
        case BI_MEM_RELEASE: case BI_MEM_RELEASE_T: return mem_release(l, x);
        case BI_MEM_ALLOC_SIZE: { // the allocator's usable size of data's allocation
            int p;
            view_parts(l, x->kids[0], &p, NULL);
            return widen(l, call_routine(l, "_malloc_size", 1, IT_U64, &p, 1), IT_U64, need_type(l, node_ty(x)));
        }
        case BI_MEM_COMPARE: return mem_compare(l, x);
        case BI_MEM_EQUAL: return mem_equal(l, x);
        case BI_FMT_PRINTF: return x->aux ? printf_call(l, x) : -2;
        case BI_MEM_COPY: case BI_MEM_MOVE: case BI_MEM_FILL: case BI_MEM_ZERO: return mem_write(l, x, bi);
        default: return mem_alloc_typed(l, x, bi, res);
        }
    case BI_SYS_ARGC: // Darwin's accessors: an int* and a char*** (as emit.c's)
        return widen(l, load(l, IT_I32, call_extern(l, "__NSGetArgc"), 0, 0), IT_I32, need_type(l, node_ty(x)));
    case BI_SYS_ENVC: {
        int env = load(l, IT_PTR, call_extern(l, "__NSGetEnviron"), 0, 0);
        int n = var_new(l, NULL, IT_U64, -1);
        write_var(l, n, l->cur, konst(l, IT_U64, 0));
        int head = new_block(l), body = new_block(l), exit = new_block(l);
        jump(l, head);
        enter(l, head);
        int i = read_var(l, n, head);
        int at = emit2(l, IR_PADD, IT_PTR, env, i);
        l->fn->ins[at].imm = 8;
        terminate(l, IR_BR, emit2(l, IR_NE, IT_BOOL, load(l, IT_PTR, at, 0, 0), konst(l, IT_PTR, 0)), body, exit);
        seal(l, body);
        enter(l, body);
        write_var(l, n, l->cur, emit2(l, IR_ADD, IT_U64, i, konst(l, IT_U64, 1)));
        jump(l, head);
        seal(l, head);
        seal(l, exit);
        enter(l, exit);
        return widen(l, i, IT_U64, need_type(l, node_ty(x)));
    }
    case BI_SYS_ARG: case BI_SYS_ENV: { // entry i's bytes up to its NUL
        IrType it = type_in(l, x->kids[0], IT_U64);
        int i = widen(l, expr(l, x->kids[0], it), it, IT_U64);
        int base = load(l, IT_PTR, call_extern(l, bi == BI_SYS_ARG ? "__NSGetArgv" : "__NSGetEnviron"), 0, 0);
        int at = emit2(l, IR_PADD, IT_PTR, base, i);
        l->fn->ins[at].imm = 8;
        int p = load(l, IT_PTR, at, 0, 0);
        int n = c_length(l, p);
        if (!res) return -1;
        res->n = 2;
        res->v[0] = p;
        res->v[1] = n;
        return -1;
    }
    default:
        return -2;
    }
}

// --- inline expansion (Functions & ABI #8) -------------------------------------------

// The inline function a direct call names, with the type arguments its body
// is stamped under.
Node* inline_target(Node* call, SType*** targs, int* ntargs) {
    Node* callee = call->a;
    *targs = NULL;
    *ntargs = 0;
    if (callee->kind == N_INDEX && callee->sym) {
        Sym* gs = node_sym(callee);
        if (!gs->decl || !(gs->decl->flags & F_INLINE)) return NULL;
        for (STypeInst* inst = sema_insts(gs->decl); inst; inst = inst->next)
            if (inst->result == callee->ty) {
                *targs = inst->args;
                *ntargs = gs->decl->c ? gs->decl->c->nkids : 0;
                return gs->decl;
            }
        return NULL;
    }
    Sym* s = call->sym ? node_sym(call) : NULL;
    if (!s || s->kind != SYM_FUNC || s->is_extern || !s->decl || !(s->decl->flags & F_INLINE)) return NULL;
    if (s->is_generic) { // a method of a generic type: its owner's arguments
        SType* cft = node_ty(callee);
        if (!cft || cft->kind != SK_FUNC) cft = s->type;
        SType* owner = (call->flags & F_ASSOC) ? (callee->kind == N_FIELD_E ? node_ty(callee->a) : NULL)
                     : cft && cft->nparams > 0 ? cft->params[0] : NULL;
        if (!owner) return NULL;
        *targs = owner->args;
        *ntargs = owner->nargs;
    }
    return s->decl;
}

static PtrMap copy_node; // callee node -> its copy, while copying one callee
static PtrMap copy_sym;  // callee local -> its copy

// n and everything under it, each node once; Node.d only as a general for's body
static Node* copy_tree(Node* n) {
    if (!n) return NULL;
    Node* c = (Node*)ptrmap_get(&copy_node, n);
    if (c) return c;
    c = (Node*)arena_alloc(sizeof(Node));
    *c = *n;
    ptrmap_put(&copy_node, n, (uintptr_t)c);
    c->a = copy_tree(n->a);
    c->b = copy_tree(n->b);
    c->c = copy_tree(n->c);
    if (n->kind == N_FOR && (n->flags & F_FORGEN)) c->d = copy_tree(n->d);
    if (n->nkids) {
        c->kids = (Node**)arena_alloc((size_t)n->nkids * sizeof(Node*));
        for (int i = 0; i < n->nkids; i++) c->kids[i] = copy_tree(n->kids[i]);
    }
    return c;
}

// point a copy's names at fresh locals whose binders are the copied ones
static void copy_syms(Node* c, PtrMap* seen) {
    if (!c || ptrmap_get(seen, c)) return;
    ptrmap_put(seen, c, 1);
    Sym* s = node_sym(c);
    if (s && s->kind == SYM_VAR && !s->is_global && s->decl) {
        Node* d = (Node*)ptrmap_get(&copy_node, s->decl);
        if (d) {
            Sym* ns = (Sym*)ptrmap_get(&copy_sym, s);
            if (!ns) {
                ns = (Sym*)arena_alloc(sizeof(Sym));
                *ns = *s;
                ns->decl = d;
                ptrmap_put(&copy_sym, s, (uintptr_t)ns);
            }
            c->sym = ns;
        }
    }
    copy_syms(c->a, seen);
    copy_syms(c->b, seen);
    copy_syms(c->c, seen);
    if (c->kind == N_FOR && (c->flags & F_FORGEN)) copy_syms(c->d, seen);
    for (int i = 0; i < c->nkids; i++) copy_syms(c->kids[i], seen);
}

void inline_copy(Node* f, SType** targs, int ntargs, Node** bind, Node** body) {
    Sym* fs = node_sym(f);
    if (f->c && !(fs && fs->spec_args)) sema_stamp_body(f, targs, ntargs);
    ptrmap_clear(&copy_node);
    ptrmap_clear(&copy_sym);
    for (int i = 0; i < f->nkids; i++) bind[i] = copy_tree(f->kids[i]);
    *body = copy_tree(f->b);
    PtrMap seen = {0};
    for (int i = 0; i < f->nkids; i++) copy_syms(bind[i], &seen);
    copy_syms(*body, &seen);
    ptrmap_free(&seen);
}

static int inline_call(Lw* l, Node* call);

// Whether x is the layout builtin name(...), which sema resolved to no symbol.
static int is_layout_builtin(Node* x, const char* name) {
    Node* c = x->kind == N_CALL ? x->a : NULL;
    size_t n = strlen(name);
    return c && c->kind == N_IDENT && !x->sym && c->len == n && memcmp(c->str, name, n) == 0;
}

// A call: a scalar result's value; an aggregate's leaves into res. A result
// of over 16 leaves goes to memory at address dst, or a new stack object when
// dst < 0.
static int call_to(Lw* l, Node* x, int want, Agg* res, int dst) {
    Node* callee = x->a;
    if (is_layout_builtin(x, "sizeof") || is_layout_builtin(x, "alignof") || is_layout_builtin(x, "offsetof"))
        return konst(l, type_in(l, x, want), x->ival); // folded by sema
    if (is_layout_builtin(x, "static_assert")) return -1;
    if (is_layout_builtin(x, "blank")) { // the zero of its type
        SType* t = node_ty(x);
        if (!is_agg(t)) return konst(l, type_in(l, x, want), 0);
        IrLeaf lv[IR_MAX_LEAVES];
        int n = leaves(l, t, lv);
        if (res) {
            res->n = n;
            for (int k = 0; k < n; k++) res->v[k] = konst(l, (IrType)lv[k].type, 0);
        }
        return -1;
    }
    int inst = callee && callee->kind == N_INDEX && callee->sym; // larger[int](a, b)
    Sym* s = inst ? node_sym(callee) : node_sym(x);
    if (!s && x->ival && callee && callee->kind == N_IDENT && callee->len == 5 && memcmp(callee->str, "panic", 5) == 0 &&
        x->nkids == 1) { // panic(message): the bytes to stderr, best effort, then abort (Errors #15)
        int ptr, len;
        view_parts(l, x->kids[0], &ptr, &len);
        int w[3] = { konst(l, IT_I32, 2), ptr, len };
        call_routine(l, "_write", 1, IT_I64, w, 3);
        call_routine(l, "_abort", 1, IT_VOID, NULL, 0);
        return -1;
    }
    if (!s && callee && callee->kind == N_IDENT && callee->len == 6 && memcmp(callee->str, "typeid", 6) == 0)
        return konst(l, IT_U64, x->ival); // typeid(T): the type's identity word
    if (!s && callee && callee->kind == N_IDENT && is_domain(node_ty(callee))) { // Domain(code) (Errors #5, #7)
        int id = konst(l, IT_U64, x->ival << 32);
        if (x->nkids != 1) return id;
        int code = emit2(l, IR_CAST, IT_U32, expr(l, x->kids[0], IT_I32), -1); // its raw bits
        return emit2(l, IR_OR, IT_U64, id, emit2(l, IR_CAST, IT_U64, code, -1));
    }
    if (!s && callee && callee->kind == N_IDENT && node_ty(callee) && x->nkids == 1) { // conversion
        IrType to = type_in(l, x, want);
        Node* a = x->kids[0];
        if (is_untyped(node_ty(a))) return expr(l, a, to);
        IrType from = need_type(l, node_ty(a));
        if (to == IT_BOOL && from != IT_BOOL) unsupported(l, "conversion to bool");
        int v = expr(l, a, from);
        if (from == to) return v;
        return emit2(l, IR_CAST, to, v, -1);
    }
    Args g;
    memset(&g, 0, sizeof g);
    if (s && s->kind == SYM_VAR) { // through a function value
        SType* ft = unwrap(s->type);
        if (!ft || ft->kind != SK_FUNC || x->nkids != ft->nparams) unsupported(l, "indirect call");
        int f = expr(l, callee, IT_PTR);
        for (int i = 0; i < x->nkids; i++) {
            if (x->kids[i]->kind == N_MUTARG) unsupported(l, "mut argument through a function value");
            call_arg(l, &g, x->kids[i], ft->params[i], 0);
        }
        int id = emit(l, IR_CALLI, result_type(l, ft->ret));
        l->fn->ins[id].flags = (uint8_t)null_checked(l);
        ir_arg_add(l->fn, id, f); // the target, then the arguments
        return call_finish(l, id, &g, ft->ret, res, dst);
    }
    if (!s || s->kind != SYM_FUNC || !callee) unsupported(l, "indirect or method call");
    Node* d = s->decl;
    if (!d) unsupported(l, "extern call");
    if (d->kind == N_FUNC && d->ival) {
        int v = builtin(l, x, (int)d->ival, res);
        if (v != -2) return v;
    }
    if ((d->kind == N_FUNC && d->ival) || (d->flags & F_INTRINSIC)) unsupported(l, "compiler-backed call");
    if (d->flags & F_INLINE) return inline_call(l, x);
    if (d->flags & F_EXPO) unsupported(l, "exported call");
    int first_var = call_args(l, x, s, &g);
    SType* ft = node_ty(callee);
    if (!ft || ft->kind != SK_FUNC) ft = s->type;
    SType* rt = ft ? ft->ret : NULL;
    int id = emit(l, IR_CALL, result_type(l, rt));
    IrIns* c = &l->fn->ins[id];
    c->sym = s;
    const char* mn = inst ? fn_value_mangle(s, callee) : s->is_extern ? NULL : fn_call_mangle(s, x);
    if (inst && !mn) unsupported(l, "generic call");
    c->name = s->is_extern ? link_name(s, ft) : arena_name(mn);
    if (s->is_extern && (d->flags & F_VARARGS) && first_var >= 0) {
        c->flags |= IRF_VARARGS;
        c->aux = first_var;
    } else if (s->is_extern && (d->flags & F_VARARGS)) {
        c->flags |= IRF_VARARGS;
        c->aux = g.n;
    }
    return call_finish(l, id, &g, rt, res, dst);
}

static int call(Lw* l, Node* x, int want, Agg* res) { return call_to(l, x, want, res, -1); }

// ptr pv (of pointer type pt) +/- u64 n elements of its pointee
static int pointer_offset(Lw* l, IrOp op, SType* pt, int pv, int n) {
    pt = unwrap(pt);
    int id = emit2(l, op, IT_PTR, pv, n);
    l->fn->ins[id].imm = sema_size(pt->base);
    l->fn->ins[id].flags = (uint8_t)null_checked(l);
    return id;
}

static int is_pointer(Node* x) {
    SType* t = unwrap(node_ty(x));
    return t && t->kind == SK_PTR;
}

static int is_place(Node* x);
static void field_leaves(Lw* l, Node* x, Agg* out);
static int view_leaf(Lw* l, Node* x, int k);

static int expr(Lw* l, Node* x, int want) {
    switch (x->kind) {
    case N_INT: case N_CHAR: case N_FLOAT:
        return literal(l, type_in(l, x, want), x, 0);
    case N_BOOL:
        return konst(l, type_in(l, x, want), x->ival);
    case N_IDENT: {
        Sym* s = node_sym(x);
        if (s && s->kind == SYM_VAR && s->is_global && s->is_const && s->init) {
            IrType t = type_in(l, x, want);
            Node* init = s->init;
            int neg = 0;
            if (init->kind == N_UNARY && init->op == T_MINUS) { neg = 1; init = init->a; }
            if (init->kind != N_INT && init->kind != N_CHAR && init->kind != N_BOOL && init->kind != N_FLOAT)
                return place_read(l, place(l, x), 0); // a computed one is read from its storage
            return literal(l, t, init, neg);
        }
        if (s && s->kind == SYM_FUNC) { // a function value
            if (s->decl && (s->decl->flags & (F_INTRINSIC | F_EXPO))) unsupported(l, "function value");
            if (s->is_generic || s->spec_args || (s->decl && s->decl->kind == N_FUNC && s->decl->c)) {
                const char* mn = fn_value_mangle(s, x); // larger[int]
                if (!mn) unsupported(l, "generic function value");
                mn = arena_name(mn); // before global_addr reuses fn_mangle's buffer
                int id = global_addr(l, s);
                l->fn->ins[id].name = mn;
                return id;
            }
            return global_addr(l, s);
        }
        return place_read(l, place(l, x), 0);
    }
    case N_NULL: // a null pointer or Error
        if (want != IT_PTR && want != IT_U64) unsupported(l, type_why(node_ty(x)));
        return konst(l, (IrType)want, 0);
    case N_FIELD_E: {
        Sym* ts = node_sym(x->a);
        if (ts && ts->kind == SYM_TYPE && ts->type && unwrap(ts->type)->kind == SK_ENUM) // Color.red
            return konst(l, type_in(l, x, want), x->ival);
        if (ts && ts->kind == SYM_TYPE && is_domain(ts->type)) return konst(l, IT_U64, x->ival); // Domain.entry
        if (ts && ts->kind == SYM_TYPE) unsupported(l, type_why(ts->type));
        SType* bt = unwrap(node_ty(x->a));
        if (bt && bt->kind == SK_ERROR) { // Errors #9: the domain above the raw i32 code
            int v = expr(l, x->a, IT_U64);
            if (x->len == 6 && memcmp(x->str, "domain", 6) == 0) return emit2(l, IR_SHR, IT_U64, v, konst(l, IT_U64, 32));
            if (x->len == 4 && memcmp(x->str, "code", 4) == 0) return emit2(l, IR_CAST, IT_I32, v, -1);
            unsupported(l, "Error description");
        }
        if (is_view(bt) && x->len == 3 && memcmp(x->str, "len", 3) == 0) return view_leaf(l, x->a, 1);
        if (bt && bt->kind == SK_ARRAY && x->len == 3 && memcmp(x->str, "len", 3) == 0) {
            if (!is_place(x->a)) { Agg discard; value(l, x->a, node_ty(x->a), &discard); } // side effects only: the length is static
            return konst(l, type_in(l, x, want), bt->count);
        }
        if (!bt || (bt->kind != SK_PTR && bt->kind != SK_STRUCT)) unsupported(l, bt ? type_why(bt) : "field access");
        if (!is_place(x)) {
            Agg a;
            field_leaves(l, x, &a);
            return a.v[0];
        }
        return place_read(l, place(l, x), 0);
    }
    case N_UNARY: {
        IrType t = type_in(l, x, want);
        if (x->op == T_MINUS) {
            if (x->a->kind == N_INT || x->a->kind == N_FLOAT) return literal(l, t, x->a, 1);
            return arith(l, IR_NEG, t, expr(l, x->a, t), -1);
        }
        if (x->op == T_BANG || x->op == T_TILDE) return emit2(l, IR_NOT, t, expr(l, x->a, t), -1);
        if (x->op == T_AMP) return place_address(l, place(l, x->a));
        if (x->op == T_STAR) return place_read(l, place(l, x), 0);
        if (x->op == T_VOLATILE) {
            Place p = place(l, x->a);
            if (p.var >= 0) unsupported(l, "volatile access"); // scan_storage missed it
            return place_read(l, p, IRF_VOLATILE);
        }
        unsupported(l, "operator");
    }
    case N_BINARY: {
        if (x->op == T_ANDAND || x->op == T_OROR) return bool_value(l, x);
        int op = binop_of((TokKind)x->op);
        if (op < 0) unsupported(l, "operator");
        if ((op == IR_ADD || op == IR_SUB) && is_pointer(x->a)) { // p + n, p - n
            int pv = expr(l, x->a, IT_PTR);
            return pointer_offset(l, op == IR_ADD ? IR_PADD : IR_PSUB, node_ty(x->a), pv, expr(l, x->b, IT_U64));
        }
        if ((op == IR_EQ || op == IR_NE) && (x->a->kind == N_NULL) != (x->b->kind == N_NULL)) {
            Node* o = x->a->kind == N_NULL ? x->b : x->a;
            if (is_view(node_ty(o))) // Sequences #8: a null view has a null ptr
                return emit2(l, (IrOp)op, IT_BOOL, view_leaf(l, o, 0), konst(l, IT_PTR, 0));
        }
        if ((op == IR_EQ || op == IR_NE) && (is_union(node_ty(x->a)) || is_union(node_ty(x->b))))
            return union_equal(l, x, op == IR_NE);
        if (is_compare(op)) {
            IrType ot = operand_type(l, x, -1);
            int a = expr(l, x->a, ot);
            return emit2(l, (IrOp)op, IT_BOOL, a, expr(l, x->b, ot));
        }
        IrType t = type_in(l, x, want);
        int a = expr(l, x->a, t);
        return arith(l, op, t, a, expr(l, x->b, t));
    }
    case N_CMPCHAIN: { // every operand once, left to right; the comparisons ANDed
        int n = x->nkids;
        IrType ot = (IrType)-1;
        for (int i = 0; i < n && (int)ot < 0; i++)
            if (!is_untyped(node_ty(x->kids[i]))) ot = need_type(l, node_ty(x->kids[i]));
        if ((int)ot < 0) unsupported(l, "untyped constant");
        int prev = expr(l, x->kids[0], ot), acc = -1;
        for (int i = 1; i < n; i++) {
            int v = expr(l, x->kids[i], ot);
            int c = emit2(l, (IrOp)binop_of((TokKind)x->a->kids[i - 1]->op), IT_BOOL, prev, v);
            acc = acc < 0 ? c : emit2(l, IR_AND, IT_BOOL, acc, c);
            prev = v;
        }
        return acc;
    }
    case N_CALL:
        return call(l, x, want, NULL);
    case N_INDEX: case N_SLICE_E:
        if (x->kind == N_INDEX && x->sym) { // larger[int] as a value
            Sym* s = node_sym(x);
            const char* mn = fn_value_mangle(s, x);
            if (!mn) unsupported(l, "generic function value");
            mn = arena_name(mn); // before global_addr reuses fn_mangle's buffer
            int id = global_addr(l, s);
            l->fn->ins[id].name = mn;
            return id;
        }
        if (x->kind == N_SLICE_E) unsupported(l, type_why(node_ty(x)));
        return place_read(l, place(l, x), 0);
    default:
        unsupported(l, is_untyped(node_ty(x)) ? "expression form" : type_why(node_ty(x)));
    }
}

// Whether x names storage: a variable, a dereference, or a field of one.
static int is_place(Node* x) {
    switch (x->kind) {
    case N_IDENT: case N_INDEX: return 1;
    case N_UNARY: return x->op == T_STAR;
    case N_FIELD_E: {
        SType* bt = unwrap(node_ty(x->a));
        if (bt && bt->kind == SK_ERROR) return 0; // an Error's description is looked up
        return (bt && bt->kind == SK_PTR) || is_place(x->a);
    }
    default: return 0;
    }
}

// The leaves of field x of a value that is no place: a call's result or a
// literal.
static void field_leaves(Lw* l, Node* x, Agg* out) {
    SType* bt = unwrap(node_ty(x->a));
    if (!bt || bt->kind != SK_STRUCT) unsupported(l, bt ? type_why(bt) : "field access");
    int fi = find_field(bt, x->str, x->len);
    if (fi < 0) unsupported(l, "field access");
    if (has_union(bt)) { // its words, not its members': stored, then the field read
        covered(l, bt);
        int slot = new_slot(l, bt);
        store_value(l, x->a, bt, slot, 0);
        place_get(l, (Place){ -1, slot, (int32_t)field_off(bt, fi), 0, bt->fields[fi].t }, 0, out);
        return;
    }
    Agg b;
    value(l, x->a, node_ty(x->a), &b);
    int at = leaf_index(l, bt, fi);
    out->n = leaf_count(l, bt->fields[fi].t);
    for (int k = 0; k < out->n; k++) out->v[k] = b.v[at + k];
}

// The leaves of a struct or tuple literal of type t, in field order; its
// fields are evaluated in source order.
static void literal_agg(Lw* l, Node* x, SType* t, Agg* out) {
    SType* u = unwrap(t);
    IrLeaf lv[IR_MAX_LEAVES];
    out->n = leaves(l, t, lv);
    for (int k = 0; k < out->n; k++) out->v[k] = -1;
    int positional = x->kind == N_TUPLE_E || (x->nkids > 0 && x->kids[0]->str == NULL);
    if (x->kind == N_TUPLE_E ? u->kind != SK_TUPLE || x->nkids != u->nfields : u->kind != SK_STRUCT)
        unsupported(l, type_why(t));
    for (int i = 0; i < x->nkids; i++) {
        Node* fi = x->kids[i];
        int fidx = positional ? i : find_field(u, fi->str, fi->len);
        if (fidx < 0 || fidx >= u->nfields) unsupported(l, "field initializer");
        Agg f;
        value(l, x->kind == N_TUPLE_E ? fi : fi->a, u->fields[fidx].t, &f);
        int at = leaf_index(l, u, fidx);
        for (int k = 0; k < f.n; k++) out->v[at + k] = f.v[k];
    }
    for (int k = 0; k < out->n; k++) // sema supplies every field, defaults included
        if (out->v[k] < 0) out->v[k] = konst(l, (IrType)lv[k].type, 0);
}

// The leaves of array literal x of type t, element by element; [v : n]
// evaluates v once.
static void literal_array(Lw* l, Node* x, SType* t, Agg* out) {
    SType* u = unwrap(t);
    if (!u || u->kind != SK_ARRAY) unsupported(l, type_why(t));
    int nb = leaf_count(l, u->base);
    out->n = leaf_count(l, t);
    if (x->a) {
        Agg e;
        if (u->count) value(l, x->a, u->base, &e);
        for (uint64_t i = 0; i < u->count; i++)
            for (int k = 0; k < nb; k++) out->v[i * nb + k] = e.v[k];
        return;
    }
    if ((uint64_t)x->nkids != u->count) unsupported(l, "array literal");
    for (int i = 0; i < x->nkids; i++) {
        Agg e;
        value(l, x->kids[i], u->base, &e);
        for (int k = 0; k < nb; k++) out->v[i * nb + k] = e.v[k];
    }
}

// The literal a module-level string constant names, or NULL.
static Node* const_string(Node* x) {
    Sym* s = x->kind == N_IDENT ? node_sym(x) : NULL;
    if (!s || s->kind != SYM_VAR || !s->is_global || !s->is_const || !s->init) return NULL;
    return s->init->kind == N_STR ? s->init : NULL;
}

// The leaves of string literal x: its bytes' address and length.
static void string_literal(Lw* l, Node* x, Agg* out) {
    int p = emit(l, IR_STR, IT_PTR);
    l->fn->ins[p].name = x->str;
    l->fn->ins[p].imm = (uint64_t)x->len;
    out->n = 2;
    out->v[0] = p;
    out->v[1] = konst(l, IT_U64, (uint64_t)x->len);
}

// Leaf k of string or slice x, a place's read alone: 0 its ptr, 1 its length.
static int view_leaf(Lw* l, Node* x, int k) {
    Node* lit = x->kind == N_STR ? x : const_string(x);
    if (lit && k) return konst(l, IT_U64, (uint64_t)lit->len);
    if (lit) {
        Agg v;
        string_literal(l, lit, &v);
        return v.v[0];
    }
    if (is_place(x)) {
        Place p = place(l, x);
        if (p.var >= 0) return read_var(l, p.var + k, l->cur);
        return load(l, k ? IT_U64 : IT_PTR, p.addr, p.off + 8 * k, p.flags);
    }
    Agg v;
    value(l, x, node_ty(x), &v);
    return v.v[k];
}

// --- memory ------------------------------------------------------------------------

enum { UNROLL = 16 }; // a copy or fill of up to this many words is straight-line

// A counted loop over u64 i in [0, n): loop_begin enters the body and returns
// i; loop_end steps i and leaves through the exit.
typedef struct { int var, head, exit; } Loop;

static int loop_begin(Lw* l, Loop* lp, uint64_t n) {
    lp->var = var_new(l, NULL, IT_U64, -1);
    write_var(l, lp->var, l->cur, konst(l, IT_U64, 0));
    lp->head = new_block(l);
    lp->exit = new_block(l);
    int body = new_block(l);
    jump(l, lp->head);
    enter(l, lp->head);
    int i = read_var(l, lp->var, lp->head);
    terminate(l, IR_BR, emit2(l, IR_LT, IT_BOOL, i, konst(l, IT_U64, n)), body, lp->exit);
    seal(l, body);
    enter(l, body);
    return i;
}

static void loop_end(Lw* l, Loop* lp, int i) {
    write_var(l, lp->var, l->cur, emit2(l, IR_ADD, IT_U64, i, konst(l, IT_U64, 1)));
    jump(l, lp->head);
    seal(l, lp->head);
    seal(l, lp->exit);
    enter(l, lp->exit);
}

// The address addr + off.
static int offset_addr(Lw* l, int addr, int32_t off) {
    if (!off) return addr;
    int id = emit2(l, IR_PADD, IT_PTR, addr, konst(l, IT_U64, (uint64_t)off));
    l->fn->ins[id].imm = 1;
    return id;
}

// Copy size bytes from src + soff (loads flagged sflags) to dst + doff:
// words, then a narrower tail; a long run of words loops.
static void copy_mem(Lw* l, int dst, int32_t doff, int src, int32_t soff, int sflags, uint32_t size) {
    uint32_t words = size / 8, done = 0;
    if (words > UNROLL) {
        int d = offset_addr(l, dst, doff), s = offset_addr(l, src, soff);
        Loop lp;
        int i = loop_begin(l, &lp, words);
        int sa = emit2(l, IR_PADD, IT_PTR, s, i), da = emit2(l, IR_PADD, IT_PTR, d, i);
        l->fn->ins[sa].imm = l->fn->ins[da].imm = 8;
        store(l, da, 0, load(l, IT_U64, sa, 0, sflags), 0);
        loop_end(l, &lp, i);
        done = words * 8;
    }
    for (uint32_t w = 8; w; w /= 2) {
        IrType t = w == 8 ? IT_U64 : w == 4 ? IT_U32 : w == 2 ? IT_U16 : IT_U8;
        for (; size - done >= w; done += w)
            store(l, dst, doff + (int32_t)done, load(l, t, src, soff + (int32_t)done, sflags), 0);
    }
}

// Fill size bytes at dst + off with u64 pattern pat (one repeating unit).
static void fill_mem(Lw* l, int dst, int32_t off, int pat, uint32_t size) {
    uint32_t words = size / 8, done = 0;
    if (words > UNROLL) {
        int d = offset_addr(l, dst, off);
        Loop lp;
        int i = loop_begin(l, &lp, words);
        int da = emit2(l, IR_PADD, IT_PTR, d, i);
        l->fn->ins[da].imm = 8;
        store(l, da, 0, pat, 0);
        loop_end(l, &lp, i);
        done = words * 8;
    }
    for (uint32_t w = 8; w; w /= 2) {
        IrType t = w == 8 ? IT_U64 : w == 4 ? IT_U32 : w == 2 ? IT_U16 : IT_U8;
        if (size - done < w) continue;
        int v = w == 8 ? pat : emit2(l, IR_CAST, t, pat, -1);
        for (; size - done >= w; done += w) store(l, dst, off + (int32_t)done, v, 0);
    }
}

// The u64 repeating element value v of type t, or -1 when its bytes do not
// repeat in a word (a float's, or a width that does not divide 8).
static int fill_pattern(Lw* l, int v, SType* t) {
    IrType it = (IrType)ir_type_of(t);
    uint32_t w = sema_size(t);
    if ((int)it < 0 || ir_type_float(it) || it == IT_PTR || (w != 1 && w != 2 && w != 4 && w != 8)) return -1;
    uint64_t rep = w == 1 ? UINT64_C(0x0101010101010101) : w == 2 ? UINT64_C(0x0001000100010001)
                 : w == 4 ? UINT64_C(0x0000000100000001) : 1;
    IrIns* k = &l->fn->ins[v];
    if (k->op == IR_CONST) {
        uint64_t bits = w == 8 ? k->imm : k->imm & ((UINT64_C(1) << (w * 8)) - 1);
        return konst(l, IT_U64, bits * rep);
    }
    IrType ut = w == 1 ? IT_U8 : w == 2 ? IT_U16 : w == 4 ? IT_U32 : IT_U64;
    if (it == IT_BOOL) ut = IT_BOOL;
    if (ut != it) v = emit2(l, IR_CAST, ut, v, -1); // its bits, zero-extended
    if (ut != IT_U64) v = emit2(l, IR_CAST, IT_U64, v, -1);
    return w == 8 ? v : emit2(l, IR_MUL, IT_U64, v, konst(l, IT_U64, rep));
}

// A new stack object of type t.
static int new_slot(Lw* l, SType* t) {
    int slot = at_entry(l, IR_SLOT, IT_PTR);
    l->fn->ins[slot].imm = sema_size(t);
    l->fn->ins[slot].aux = (int32_t)sema_align(t);
    return slot;
}

// --- tagged unions --------------------------------------------------------------------

// The union a construction's or pattern's base names: a type, or a generic
// union's stamp.
static SType* union_of_base(Node* base) {
    if (!base) return NULL;
    if (base->kind == N_IDENT) {
        Sym* ts = node_sym(base);
        return ts && ts->kind == SYM_TYPE && is_union(ts->type) ? unwrap(ts->type) : NULL;
    }
    if (base->kind == N_INDEX) return is_union(node_ty(base)) ? unwrap(node_ty(base)) : NULL;
    return NULL;
}

// Whether x builds a variant: `Event.quit`, or `Event.key(42)` with its
// payload expression. Sets the variant and payload (NULL for none).
static int union_ctor(Node* x, int* variant, Node** payload) {
    *payload = NULL;
    if (x->kind == N_FIELD_E && union_of_base(x->a)) {
        *variant = (int)x->ival;
        return 1;
    }
    if (x->kind != N_CALL || x->sym || !x->a || x->a->kind != N_FIELD_E || !union_of_base(x->a->a)) return 0;
    *variant = (int)x->a->ival;
    if (x->nkids == 1) *payload = x->kids[0]->kind == N_MUTARG ? x->kids[0]->a : x->kids[0];
    return 1;
}

// Variant construction into memory at dst + off: the payload where it lives,
// then the tag. The inactive bytes are left as they are.
static void union_store(Lw* l, Node* x, SType* t, int dst, int32_t off) {
    SType* ut = unwrap(t);
    int vi;
    Node* pay;
    union_ctor(x, &vi, &pay);
    if (vi < 0 || vi >= ut->nfields || (pay != NULL) != (ut->fields[vi].t != NULL)) unsupported(l, "union variant");
    if (pay) store_value(l, pay, ut->fields[vi].t, dst, off + (int32_t)sema_union_payload_off(ut));
    store(l, dst, off, konst(l, IT_I32, (uint64_t)vi), 0);
}

// The address of the union x names or builds.
static int union_addr(Lw* l, Node* x, SType* t) {
    int vi;
    Node* pay;
    if (!union_ctor(x, &vi, &pay) && is_place(x)) {
        Place p = place(l, x);
        if (p.var < 0) return place_address(l, p);
    }
    covered(l, t);
    int slot = new_slot(l, t);
    store_value(l, x, t, slot, 0);
    return slot;
}

static int union_equal_at(Lw* l, SType* ut, int a, int b);

// Whether the size bytes at a + off and b + off are equal, as the tag's payload
// is compared: a nested union by its own rule, a float by value, the rest word
// by word.
static int payload_equal(Lw* l, SType* pt, int a, int b, int32_t off) {
    SType* u = unwrap(pt);
    if (u->kind == SK_UNION) return union_equal_at(l, u, offset_addr(l, a, off), offset_addr(l, b, off));
    if (u->kind == SK_PRIM && (u->prim == P_F32 || u->prim == P_F64)) {
        IrType ft = u->prim == P_F32 ? IT_F32 : IT_F64;
        return cmp(l, IR_EQ, load(l, ft, a, off, 0), load(l, ft, b, off, 0));
    }
    uint32_t size = sema_size(pt);
    int acc = konst(l, IT_BOOL, 1);
    for (uint32_t at = 0; at < size;) {
        uint32_t rem = size - at, w = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
        IrType wt = w == 8 ? IT_U64 : w == 4 ? IT_U32 : w == 2 ? IT_U16 : IT_U8;
        int c = cmp(l, IR_EQ, load(l, wt, a, off + (int32_t)at, 0), load(l, wt, b, off + (int32_t)at, 0));
        acc = emit2(l, IR_AND, IT_BOOL, acc, c);
        at += w;
    }
    return acc;
}

// Whether the unions at a and b are equal: the tags, and when they match the
// active payload (inactive bytes and padding are never compared).
static int union_equal_at(Lw* l, SType* ut, int a, int b) {
    int res = temp_var(l, IT_BOOL, konst(l, IT_BOOL, 0));
    int join = new_block(l), same = new_block(l);
    int ta = load(l, IT_I32, a, 0, 0), tb = load(l, IT_I32, b, 0, 0);
    terminate(l, IR_BR, cmp(l, IR_EQ, ta, tb), same, join);
    seal(l, same);
    enter(l, same);
    int32_t poff = (int32_t)sema_union_payload_off(ut);
    for (int k = 0; k < ut->nfields; k++) {
        if (!ut->fields[k].t) continue;
        int yes = new_block(l), next = new_block(l);
        terminate(l, IR_BR, cmp(l, IR_EQ, ta, konst(l, IT_I32, (uint64_t)k)), yes, next);
        seal(l, yes);
        enter(l, yes);
        write_var(l, res, l->cur, payload_equal(l, ut->fields[k].t, a, b, poff));
        jump(l, join);
        seal(l, next);
        enter(l, next);
    }
    write_var(l, res, l->cur, konst(l, IT_BOOL, 1)); // a payloadless variant
    jump(l, join);
    seal(l, join);
    enter(l, join);
    return read_var(l, res, join);
}

// x.a == x.b or x.a != x.b on unions.
static int union_equal(Lw* l, Node* x, int negate) {
    SType* t = node_ty(x->a);
    if (!is_union(t)) t = node_ty(x->b);
    int a = union_addr(l, x->a, t), b = union_addr(l, x->b, t);
    int r = union_equal_at(l, unwrap(t), a, b);
    return negate ? emit2(l, IR_NOT, IT_BOOL, r, -1) : r;
}

// x of type t into memory at dst + off: an array or struct literal member by
// member, a place in memory by copying its bytes, anything else by its leaves.
static void store_value(Lw* l, Node* x, SType* t, int dst, int32_t off) {
    SType* u = unwrap(t);
    if (x->kind == N_ARRAYLIT && u && u->kind == SK_ARRAY) {
        uint32_t es = sema_size(u->base);
        if (!x->a) {
            for (int i = 0; i < x->nkids; i++) store_value(l, x->kids[i], u->base, dst, off + (int32_t)(i * es));
            return;
        }
        if (!u->count) return;
        if (!is_agg(u->base)) { // [v : n]: v once, as a repeating word when its bytes allow
            int v = expr(l, x->a, need_type(l, u->base));
            int pat = fill_pattern(l, v, u->base);
            if (pat >= 0) {
                fill_mem(l, dst, off, pat, (uint32_t)u->count * es);
                return;
            }
        }
        store_value(l, x->a, u->base, dst, off); // the first element, then copies of it
        if (u->count - 1 <= UNROLL) {
            for (uint64_t i = 1; i < u->count; i++) copy_mem(l, dst, off + (int32_t)(i * es), dst, off, 0, es);
            return;
        }
        int d = offset_addr(l, dst, off);
        Loop lp;
        int i = loop_begin(l, &lp, u->count - 1);
        int one = konst(l, IT_U64, 1);
        int at = emit2(l, IR_PADD, IT_PTR, d, emit2(l, IR_ADD, IT_U64, i, one));
        l->fn->ins[at].imm = es;
        copy_mem(l, at, 0, d, 0, 0, es);
        loop_end(l, &lp, i);
        return;
    }
    if (u && u->kind == SK_UNION) {
        int vi;
        Node* pay;
        if (union_ctor(x, &vi, &pay)) {
            union_store(l, x, t, dst, off);
            return;
        }
    }
    if (((x->kind == N_STRUCTLIT && u && u->kind == SK_STRUCT) || (x->kind == N_TUPLE_E && u && u->kind == SK_TUPLE)) &&
        by_parts(t)) {
        int positional = x->kind == N_TUPLE_E || (x->nkids > 0 && x->kids[0]->str == NULL);
        if (x->nkids != u->nfields) unsupported(l, "field initializer"); // sema supplies every field
        for (int i = 0; i < x->nkids; i++) {
            Node* fi = x->kids[i];
            int fidx = positional ? i : find_field(u, fi->str, fi->len);
            if (fidx < 0) unsupported(l, "field initializer");
            store_value(l, x->kind == N_TUPLE_E ? fi : fi->a, u->fields[fidx].t, dst, off + (int32_t)field_off(u, fidx));
        }
        return;
    }
    if (is_agg(t) && !is_view(t) && is_place(x)) {
        Place p = place(l, x);
        if (p.var < 0) {
            copy_mem(l, dst, off, p.addr, p.off, p.flags, sema_size(t));
            return;
        }
    }
    if (is_big(t) && is_layout_builtin(x, "blank")) {
        fill_mem(l, dst, off, konst(l, IT_U64, 0), sema_size(t));
        return;
    }
    if (is_big(t) && x->kind == N_CALL) { // its result goes straight to dst + off
        call_to(l, x, -1, NULL, offset_addr(l, dst, off));
        return;
    }
    if (is_big(t)) unsupported(l, "aggregate of over 16 leaves");
    Agg a;
    value(l, x, t, &a);
    IrLeaf lv[IR_MAX_LEAVES];
    leaves(l, t, lv);
    for (int k = 0; k < a.n; k++) store(l, dst, off + (int32_t)lv[k].off, a.v[k], 0);
}

// Array x as a place in memory: its own, or a new stack object holding it.
static Place array_place(Lw* l, Node* x) {
    if (is_place(x)) {
        Place p = place(l, x);
        if (p.var < 0) return p;
    }
    SType* t = node_ty(x);
    covered(l, t);
    int slot = new_slot(l, t);
    store_value(l, x, t, slot, 0);
    Place p = { -1, slot, 0, 0, t };
    return p;
}

// --- sequences ------------------------------------------------------------------------

// The ptr of string or slice x, and its length when len is not NULL: the
// descriptor read once, a place's through one address.
static void view_parts(Lw* l, Node* x, int* ptr, int* len) {
    Node* lit = x->kind == N_STR ? x : const_string(x);
    if (!lit && is_place(x)) {
        Place p = place(l, x);
        if (p.var >= 0) {
            *ptr = read_var(l, p.var, l->cur);
            if (len) *len = read_var(l, p.var + 1, l->cur);
        } else {
            *ptr = load(l, IT_PTR, p.addr, p.off, p.flags);
            if (len) *len = load(l, IT_U64, p.addr, p.off + 8, p.flags);
        }
        return;
    }
    Agg v;
    if (lit) string_literal(l, lit, &v);
    else value(l, x, node_ty(x), &v);
    *ptr = v.v[0];
    if (len) *len = v.v[1];
}

// Debug: trap through the bounds failure at site unless c holds.
static void bounds_check(Lw* l, int c, Node* site) {
    int id = emit2(l, IR_CHECK, IT_VOID, c, -1);
    l->fn->ins[id].name = "bounds violation";
    l->fn->ins[id].at = site;
}

// Index or bound x of site as a u64; checked (a signed one is first checked
// nonnegative, Sequences #12) when nonneg is set.
static int bound(Lw* l, Node* x, Node* site, int nonneg) {
    IrType t = type_in(l, x, IT_U64);
    int v = expr(l, x, t);
    if (nonneg && ir_type_signed(t)) bounds_check(l, emit2(l, IR_GE, IT_BOOL, v, konst(l, t, 0)), site);
    return t == IT_U64 ? v : emit2(l, IR_CAST, IT_U64, v, -1);
}

enum { BP_LO_NONNEG = 1, BP_HI_NONNEG = 2, BP_ORDER = 4, BP_SOURCE = 8 }; // sema's slice proofs

// The link symbol of one table (keys, values, or slots) of static map ms.
static int map_table(Lw* l, Sym* ms, const char* table) {
    int id = emit(l, IR_GLOBAL, IT_PTR);
    IrIns* g = &l->fn->ins[id];
    char buf[640];
    g->sym = ms;
    snprintf(buf, sizeof buf, "__ace$map$%.*s$%s", (int)ms->len, ms->name, table);
    g->name = arena_name(buf);
    return id;
}

// Static map lookup x = table[key] (Carved: compile-time static lookup maps):
// hash the key as emit.c laid the table out (integers by splitmix64, strings
// by FNV-1a, both seeded), load the slot, and confirm the key exactly. The
// entry index is count, the table's zero entry, unless the key is found; the
// place is that entry's value.
static Place staticmap_place(Lw* l, Node* x, SType* map) {
    Sym* ms = node_sym(x->a);
    uint64_t seed;
    uint32_t mask;
    if (!ms || ms->kind != SYM_VAR || !ms->is_global || !ms->init || !staticmap_hash(ms, &seed, &mask))
        unsupported(l, "static map");
    SType* rawkey = unwrap(map->base);
    int strings = rawkey && rawkey->kind == SK_PRIM && rawkey->prim == P_STRING;
    uint64_t count = ms->init->nkids;
    Agg kv;
    IrType kt = IT_U64;
    int k = -1, h;
    if (strings) {
        value(l, x->b, map->base, &kv);
    } else {
        kt = need_type(l, map->base);
        if (kt == IT_PTR || ir_type_float(kt)) unsupported(l, "static map"); // bool's 0/1 casts to u64 like any other integer key below
        k = expr(l, x->b, kt);
    }
    int idx = temp_var(l, IT_U64, konst(l, IT_U64, count));
    if (!strings) {
        h = kt == IT_U64 || kt == IT_I64 ? k : emit2(l, IR_CAST, IT_U64, k, -1);
        h = emit2(l, IR_ADD, IT_U64, h, konst(l, IT_U64, seed + UINT64_C(0x9e3779b97f4a7c15)));
        h = emit2(l, IR_XOR, IT_U64, h, emit2(l, IR_SHR, IT_U64, h, konst(l, IT_U64, 30)));
        h = emit2(l, IR_MUL, IT_U64, h, konst(l, IT_U64, UINT64_C(0xbf58476d1ce4e5b9)));
        h = emit2(l, IR_XOR, IT_U64, h, emit2(l, IR_SHR, IT_U64, h, konst(l, IT_U64, 27)));
        h = emit2(l, IR_MUL, IT_U64, h, konst(l, IT_U64, UINT64_C(0x94d049bb133111eb)));
        h = emit2(l, IR_XOR, IT_U64, h, emit2(l, IR_SHR, IT_U64, h, konst(l, IT_U64, 31)));
    } else {
        int hv = temp_var(l, IT_U64, konst(l, IT_U64, UINT64_C(1469598103934665603) ^ seed));
        int iv = temp_var(l, IT_U64, konst(l, IT_U64, 0));
        int head = new_block(l), body = new_block(l), exit = new_block(l);
        jump(l, head);
        enter(l, head);
        int i = read_var(l, iv, head);
        h = read_var(l, hv, head);
        terminate(l, IR_BR, cmp(l, IR_LT, i, kv.v[1]), body, exit);
        seal(l, body);
        enter(l, body);
        int at = emit2(l, IR_PADD, IT_PTR, kv.v[0], i);
        l->fn->ins[at].imm = 1;
        int b = emit2(l, IR_CAST, IT_U64, load(l, IT_U8, at, 0, 0), -1);
        write_var(l, hv, l->cur, emit2(l, IR_MUL, IT_U64, emit2(l, IR_XOR, IT_U64, h, b),
                                       konst(l, IT_U64, UINT64_C(1099511628211))));
        write_var(l, iv, l->cur, emit2(l, IR_ADD, IT_U64, i, konst(l, IT_U64, 1)));
        jump(l, head);
        seal(l, head);
        seal(l, exit);
        enter(l, exit);
    }
    h = emit2(l, IR_AND, IT_U64, h, konst(l, IT_U64, mask));
    int sp = emit2(l, IR_PADD, IT_PTR, map_table(l, ms, "slots"), h);
    l->fn->ins[sp].imm = 4;
    int slot = load(l, IT_U32, sp, 0, 0);
    int found = guard(l, cmp(l, IR_NE, slot, konst(l, IT_U32, 0)));
    int ei = emit2(l, IR_SUB, IT_U64, emit2(l, IR_CAST, IT_U64, slot, -1), konst(l, IT_U64, 1));
    uint32_t ksz = strings ? 16 : sema_size(map->base);
    int ep = emit2(l, IR_PADD, IT_PTR, map_table(l, ms, "keys"), ei);
    l->fn->ins[ep].imm = ksz;
    int same;
    if (strings) {
        int lenok = guard(l, cmp(l, IR_EQ, load(l, IT_U64, ep, 8, 0), kv.v[1]));
        int r = rtcall(l, "__ace$mem_compare", load(l, IT_PTR, ep, 0, 0), kv.v[0], kv.v[1]);
        same = guard(l, cmp(l, IR_EQ, r, konst(l, IT_I64, 0)));
        write_var(l, idx, l->cur, ei);
        guard_end(l, same);
        guard_end(l, lenok);
    } else {
        same = guard(l, cmp(l, IR_EQ, load(l, kt, ep, 0, 0), k));
        write_var(l, idx, l->cur, ei);
        guard_end(l, same);
    }
    guard_end(l, found);
    int vp = emit2(l, IR_PADD, IT_PTR, map_table(l, ms, "values"), read_var(l, idx, l->cur));
    l->fn->ins[vp].imm = sema_size(map->ret);
    Place p = { -1, vp, 0, 0, map->ret };
    return p;
}

// Element x = s[i] of a string or slice: its descriptor, then i, then (debug)
// 0 <= i < s.len unless sema proved it.
static Place element(Lw* l, Node* x) {
    static SType byte = { .kind = SK_PRIM, .prim = P_U8, .size = 1, .align = 1 };
    SType* st = unwrap(node_ty(x->a));
    int chk = l->mode == EMIT_DEBUG && !(x->flags & F_BOUNDS_PROVEN);
    if (st && st->kind == SK_ARRAY) { // at a fixed offset from the array's place
        Place p = array_place(l, x->a);
        uint32_t es = sema_size(st->base);
        p.t = st->base;
        if (x->b->kind == N_INT && x->b->ival < st->count) { // a constant index (sema rejects one out of range)
            p.off += (int32_t)(x->b->ival * es);
            return p;
        }
        int i = bound(l, x->b, x, chk);
        if (chk) bounds_check(l, emit2(l, IR_LT, IT_BOOL, i, konst(l, IT_U64, st->count)), x);
        int id = emit2(l, IR_PADD, IT_PTR, p.addr, i); // the place's offset stays on the access
        l->fn->ins[id].imm = es;
        l->fn->ins[id].flags = (uint8_t)p.flags; // a null base traps here
        Place e = { -1, id, p.off, 0, st->base };
        return e;
    }
    if (st && st->kind == SK_STATICMAP) return staticmap_place(l, x, st);
    if (!is_view(st)) unsupported(l, type_why(st));
    int ptr, len = -1;
    view_parts(l, x->a, &ptr, chk ? &len : NULL);
    int i = bound(l, x->b, x, chk);
    if (chk) bounds_check(l, emit2(l, IR_LT, IT_BOOL, i, len), x);
    SType* et = st->kind == SK_SLICE ? st->base : &byte;
    int id = emit2(l, IR_PADD, IT_PTR, ptr, i);
    l->fn->ins[id].imm = sema_size(et);
    Place p = { -1, id, 0, 0, et };
    return p;
}

// The leaves of x = s[lo..hi] (Sequences #1-2): s's descriptor, then lo,
// then hi; debug checks each signed bound nonnegative as it is evaluated,
// then lo <= hi, then hi <= s.len, skipping what sema proved (and an
// omitted hi's, which is s.len).
static void slice_leaves(Lw* l, Node* x, Agg* out) {
    SType* st = unwrap(node_ty(x->a));
    uint8_t proof = x->aux ? *(uint8_t*)x->aux : 0;
    int chk = l->mode == EMIT_DEBUG && !(x->flags & F_BOUNDS_PROVEN);
    int ptr, len = -1;
    if (st && st->kind == SK_ARRAY) { // its place's address and its fixed length
        ptr = place_address(l, array_place(l, x->a));
        len = konst(l, IT_U64, st->count);
    } else {
        if (!is_view(st)) unsupported(l, type_why(st));
        view_parts(l, x->a, &ptr, !x->c || (chk && !(proof & BP_SOURCE)) ? &len : NULL);
    }
    int lo = x->b ? bound(l, x->b, x, chk && !(proof & BP_LO_NONNEG)) : -1;
    int hi = x->c ? bound(l, x->c, x, chk && !(proof & BP_HI_NONNEG)) : len;
    if (chk && !(proof & BP_ORDER) && lo >= 0) bounds_check(l, emit2(l, IR_LE, IT_BOOL, lo, hi), x);
    if (chk && !(proof & BP_SOURCE) && x->c) bounds_check(l, emit2(l, IR_LE, IT_BOOL, hi, len), x);
    out->n = 2;
    out->v[0] = ptr;
    out->v[1] = hi;
    if (lo < 0) return;
    out->v[0] = emit2(l, IR_PADD, IT_PTR, ptr, lo);
    l->fn->ins[out->v[0]].imm = st->kind == SK_SLICE || st->kind == SK_ARRAY ? sema_size(st->base) : 1;
    out->v[1] = emit2(l, IR_SUB, IT_U64, hi, lo);
}

// The name or message of an Error (Errors #9), when x reads one: the string
// of the first domain (name) or entry (message) the Error's packed value
// matches, else the empty string. The domains are the program's, searched in
// declaration order, as the AST emitter does.
static int error_description(Lw* l, Node* x, Agg* out) {
    SType* bt = unwrap(node_ty(x->a));
    int message = x->len == 7 && memcmp(x->str, "message", 7) == 0;
    if (!bt || bt->kind != SK_ERROR || (!message && !(x->len == 4 && memcmp(x->str, "name", 4) == 0))) return 0;
    int v = expr(l, x->a, IT_U64);
    int pv = temp_var(l, IT_PTR, konst(l, IT_PTR, 0)), nv = temp_var(l, IT_U64, konst(l, IT_U64, 0));
    int done = new_block(l);
    int domain = message ? -1 : emit2(l, IR_SHR, IT_U64, v, konst(l, IT_U64, 32));
    for (int i = 0; i < l->file->nkids; i++) {
        Node* d = l->file->kids[i];
        if (d->kind != N_ERROR_DOMAIN) continue;
        SType* dt = node_ty(d);
        if (!dt) {
            Sym* ds = node_sym(d);
            dt = ds ? ds->type : NULL;
        }
        if (!dt || !dt->type_id) continue;
        int nk = message ? d->nkids : 1;
        for (int j = 0; j < nk; j++) {
            Node* text;
            int match;
            if (message) {
                Node* entry = d->kids[j];
                uint64_t packed = ((uint64_t)dt->type_id << 32) | (uint32_t)entry->ival;
                match = cmp(l, IR_EQ, v, konst(l, IT_U64, packed));
                text = entry->b;
            } else {
                Node* name = (Node*)arena_alloc(sizeof(Node));
                memset(name, 0, sizeof *name);
                name->kind = N_STR;
                name->str = d->cname ? d->cname : d->str;
                name->len = d->cname ? (uint32_t)strlen(d->cname) : d->len;
                match = cmp(l, IR_EQ, domain, konst(l, IT_U64, dt->type_id));
                text = name;
            }
            int yes = new_block(l), next = new_block(l);
            terminate(l, IR_BR, match, yes, next);
            seal(l, yes);
            enter(l, yes);
            Agg s;
            string_literal(l, text, &s);
            write_var(l, pv, l->cur, s.v[0]);
            write_var(l, nv, l->cur, s.v[1]);
            jump(l, done);
            seal(l, next);
            enter(l, next);
        }
    }
    jump(l, done);
    seal(l, done);
    enter(l, done);
    out->n = 2;
    out->v[0] = read_var(l, pv, l->cur);
    out->v[1] = read_var(l, nv, l->cur);
    return 1;
}

// The leaves of x, converted to type t: a scalar's one value, or those of an
// aggregate.
static void value(Lw* l, Node* x, SType* t, Agg* out) {
    if (!is_agg(t)) {
        out->n = 1;
        out->v[0] = expr(l, x, need_type(l, t));
        return;
    }
    if (is_view(t)) {
        Node* lit = x->kind == N_STR ? x : const_string(x);
        if (lit) {
            string_literal(l, lit, out);
            return;
        }
        if (x->kind == N_NULL) { // Sequences #8: the null view
            out->n = 2;
            out->v[0] = konst(l, IT_PTR, 0);
            out->v[1] = konst(l, IT_U64, 0);
            return;
        }
        if (x->kind == N_CALL && !node_sym(x) && x->a && x->a->kind == N_IDENT && x->nkids == 1 &&
            is_view(node_ty(x->kids[0]))) { // string(b), []u8(s): one type (Types #19)
            value(l, x->kids[0], t, out);
            return;
        }
        SType* at = unwrap(node_ty(x));
        if (at && at->kind == SK_ARRAY) { // an array viewed whole
            out->n = 2;
            out->v[0] = place_address(l, array_place(l, x));
            out->v[1] = konst(l, IT_U64, at->count);
            return;
        }
    }
    if (x->kind == N_FIELD_E && is_view(t) && error_description(l, x, out)) return;
    if (is_union(t)) {
        int vi;
        Node* pay;
        if (union_ctor(x, &vi, &pay)) { // built in a stack object, then read as leaves
            covered(l, t);
            int slot = new_slot(l, t);
            union_store(l, x, t, slot, 0);
            place_get(l, (Place){ -1, slot, 0, 0, t }, 0, out);
            return;
        }
    }
    if (has_union(t) && (x->kind == N_STRUCTLIT || x->kind == N_TUPLE_E || x->kind == N_ARRAYLIT)) {
        covered(l, t); // built in a stack object, then read as leaves
        int slot = new_slot(l, t);
        store_value(l, x, t, slot, 0);
        place_get(l, (Place){ -1, slot, 0, 0, t }, 0, out);
        return;
    }
    switch (x->kind) {
    case N_STRUCTLIT: case N_TUPLE_E:
        literal_agg(l, x, t, out);
        return;
    case N_ARRAYLIT:
        literal_array(l, x, t, out);
        return;
    case N_SLICE_E:
        slice_leaves(l, x, out);
        return;
    case N_CALL:
        call(l, x, -1, out);
        if (out->n != leaf_count(l, t)) unsupported(l, "conversion");
        return;
    case N_FIELD_E:
        if (!is_place(x)) {
            field_leaves(l, x, out);
            return;
        }
        break;
    case N_IDENT: case N_UNARY: case N_INDEX:
        break;
    default:
        unsupported(l, type_why(t));
    }
    if (!is_place(x)) unsupported(l, type_why(t));
    place_get(l, place(l, x), 0, out);
}

// Branch on x to block t when true, f when false.
static void cond(Lw* l, Node* x, int t, int f) {
    if (l->cur < 0) return;
    if (x->kind == N_UNARY && x->op == T_BANG) {
        cond(l, x->a, f, t);
        return;
    }
    if (x->kind == N_BINARY && (x->op == T_ANDAND || x->op == T_OROR)) {
        int mid = new_block(l);
        if (x->op == T_ANDAND) cond(l, x->a, mid, f);
        else cond(l, x->a, t, mid);
        seal(l, mid);
        enter(l, mid);
        cond(l, x->b, t, f);
        return;
    }
    if (x->kind == N_BOOL) {
        jump(l, x->ival ? t : f);
        return;
    }
    int v = expr(l, x, IT_BOOL);
    terminate(l, IR_BR, v, t, f);
}

// --- statements ------------------------------------------------------------------------

static void stmt(Lw* l, Node* s);

static void block(Lw* l, Node* b) {
    for (int i = 0; i < b->nkids && l->cur >= 0; i++) stmt(l, b->kids[i]);
}

static void leaks_check(Lw* l) { // main's allocator leak report (Safety & Debug Builds #9)
    if (!l->is_main || l->mode != EMIT_DEBUG) return;
    int id = emit(l, IR_CALL, IT_VOID);
    l->fn->ins[id].name = "__ace$registry_leaks";
}

static void run_defers(Lw* l);

static void ret(Lw* l, const Agg* a) {
    run_defers(l);
    leaks_check(l);
    int id = emit(l, IR_RET, IT_VOID);
    for (int k = 0; a && k < a->n; k++) ir_arg_add(l->fn, id, a->v[k]);
    l->cur = -1;
}

// A variable's leaves: its SSA variables, or stored at its home.
static void set_leaves(Lw* l, int var, SType* t, const Agg* a) {
    if (l->home[var] == -1) {
        for (int k = 0; k < a->n; k++) write_var(l, var + k, l->cur, a->v[k]);
        return;
    }
    IrLeaf lv[IR_MAX_LEAVES];
    leaves(l, t, lv);
    int addr = var_addr(l, var);
    for (int k = 0; k < a->n; k++) store(l, addr, (int32_t)lv[k].off, a->v[k], 0);
}

// Copy memory place src to place dst of its type: bytes between memory, else
// leaves.
static void move_place(Lw* l, Place dst, Place src) {
    if (dst.var < 0 && (is_big(src.t) || has_array(src.t))) {
        int d = dst.flags ? place_address(l, dst) : dst.addr; // a pointer-derived one is checked once
        copy_mem(l, d, dst.flags ? 0 : dst.off, src.addr, src.off, src.flags, sema_size(src.t));
        return;
    }
    Agg a;
    place_get(l, src, 0, &a);
    place_put(l, dst, &a, 0);
}

// The place of new binder (N_PARAM) b: its SSA variable, or its memory.
static Place binder_place(Lw* l, Node* b) {
    Sym* bs = node_sym(b);
    if (!bs) unsupported(l, "unresolved binder");
    int var = var_of(l, bs);
    return l->home[var] == -1 ? (Place){ var, -1, 0, 0, bs->type } : (Place){ -1, var_addr(l, var), 0, 0, bs->type };
}

// Tuple destructuring (Errors #12): the right side once, then each member
// into a new binder (N_PARAM) or an existing place, in order. A tuple of over
// 16 leaves is stored whole first, then its members copied out.
static void destructure(Lw* l, Node** kids, int n, Node* rhs) {
    SType* tt = node_ty(rhs);
    SType* u = unwrap(tt);
    if (!u || u->kind != SK_TUPLE || u->nfields != n) unsupported(l, "destructuring");
    if (by_parts(tt)) {
        covered(l, tt);
        int slot = new_slot(l, tt);
        store_value(l, rhs, tt, slot, 0);
        for (int i = 0; i < n; i++) {
            Place src = { -1, slot, (int32_t)field_off(u, i), 0, u->fields[i].t };
            Place dst;
            if (kids[i]->kind == N_PARAM) {
                dst = binder_place(l, kids[i]);
            } else {
                dst = place(l, kids[i]);
            }
            move_place(l, dst, src);
        }
        return;
    }
    Agg all;
    value(l, rhs, tt, &all);
    for (int i = 0; i < n; i++) {
        Agg m;
        int at = leaf_index(l, u, i);
        m.n = leaf_count(l, u->fields[i].t);
        for (int k = 0; k < m.n; k++) m.v[k] = all.v[at + k];
        Node* kid = kids[i];
        if (kid->kind == N_PARAM) {
            Sym* bs = node_sym(kid);
            if (!bs) unsupported(l, "unresolved binder");
            if (leaf_count(l, bs->type) != m.n) unsupported(l, "destructuring");
            set_leaves(l, var_of(l, bs), bs->type, &m);
            continue;
        }
        Place p = place(l, kid);
        if (leaf_count(l, p.t) != m.n) unsupported(l, "destructuring");
        place_put(l, p, &m, 0);
    }
}

static Target* target(Lw* l, Node* s, int is_continue) {
    for (int i = l->ntgt - 1; i >= 0; i--) {
        Target* t = &l->tgt[i];
        if (s->str) {
            if (t->label && t->llen == s->len && memcmp(t->label, s->str, s->len) == 0) return t;
        } else if (t->cont >= 0) {
            return t;
        }
    }
    unsupported(l, is_continue ? "continue target" : "break target");
}

static void push_target(Lw* l, Node* s, int brk, int cont, int restart) {
    l->tgt = (Target*)grow(l->tgt, &l->ctgt, l->ntgt + 1, sizeof(Target));
    l->tgt[l->ntgt++] = (Target){ s->str, s->len, brk, cont, restart };
}

static void assign(Lw* l, Node* s) {
    if (s->nkids > 1) {
        if (s->op != T_ASSIGN || (s->flags & F_VOLATILE)) unsupported(l, "assignment target");
        destructure(l, s->kids, s->nkids, s->a);
        return;
    }
    int vol = s->flags & F_VOLATILE ? IRF_VOLATILE : 0;
    Place p = place(l, s->kids[0]);
    if (vol && p.var >= 0) unsupported(l, "volatile access"); // scan_storage missed it
    if (is_agg(p.t)) {
        if (s->op != T_ASSIGN) unsupported(l, "compound assignment");
        if (p.var < 0 && !vol && (is_big(p.t) || (has_array(p.t) && is_place(s->a)))) {
            int src = -1;
            if (!is_place(s->a)) { // whole, before any byte is written
                covered(l, p.t);
                src = new_slot(l, p.t);
                store_value(l, s->a, p.t, src, 0);
            }
            if (src < 0) store_value(l, s->a, p.t, p.addr, p.off); // a place's bytes: equal or disjoint
            else copy_mem(l, p.addr, p.off, src, 0, 0, sema_size(p.t));
            return;
        }
        Agg a;
        value(l, s->a, p.t, &a); // whole, before any leaf is written
        place_put(l, p, &a, vol);
        return;
    }
    IrType t = need_type(l, p.t);
    int v;
    if (s->op == T_ASSIGN) {
        v = expr(l, s->a, t);
    } else {
        int op = binop_of((TokKind)s->op);
        if (op < 0) unsupported(l, "compound assignment");
        int old = place_read(l, p, vol);
        if (t == IT_PTR && (op == IR_ADD || op == IR_SUB)) // p += n, p -= n
            v = pointer_offset(l, op == IR_ADD ? IR_PADD : IR_PSUB, p.t, old, expr(l, s->a, IT_U64));
        else
            v = arith(l, op, t, old, expr(l, s->a, t));
    }
    place_write(l, p, v, vol);
}

// Local sym's variable var, initialized to x.
static void init_var(Lw* l, int var, Sym* sym, Node* x) {
    if (l->home[var] != -1 && in_memory(sym->type)) { // into its memory, which the value cannot name
        store_value(l, x, sym->type, var_addr(l, var), 0);
        return;
    }
    Agg a;
    value(l, x, sym->type, &a);
    set_leaves(l, var, sym->type, &a);
}

static void var_decl(Lw* l, Node* s) {
    if (s->nkids != 1 || s->kids[0]->kind != N_PARAM || (s->flags & F_MIXED)) {
        if (!s->b) unsupported(l, "multiple binders");
        for (int i = 0; i < s->nkids; i++) // make each new binder's variable
            if (s->kids[i]->kind == N_PARAM && node_sym(s->kids[i])) var_of(l, node_sym(s->kids[i]));
        destructure(l, s->kids, s->nkids, s->b);
        return;
    }
    Node* binder = s->kids[0];
    Sym* sym = node_sym(binder);
    if (!sym) unsupported(l, "unresolved binder");
    int var = var_of(l, sym);
    if (!s->b) return; // assigned before any read (definite initialization)
    init_var(l, var, sym, s->b);
}

static int by_address(Node* f, int i);

// A return inside an expansion: the result to the expansion's variable, then
// the join.
static void inline_return(Lw* l, Node* s) {
    // indexes l->inl fresh at each step: expr() may recurse into another
    // inline_call and grow (realloc) l->inl out from under a cached pointer
    int i = l->ninl - 1;
    if (s->a && l->inl[i].res >= 0) {
        int v = expr(l, s->a, need_type(l, l->inl[i].rt));
        write_var(l, l->inl[i].res, l->cur, v);
    }
    jump(l, l->inl[i].join);
}

// A call to an inline function: its body copied in place. Each argument is
// evaluated in order and bound to its parameter, a mut parameter or receiver
// to the argument's own storage and any other to a copy. Each return jumps to
// a join block, where the result is a variable read.
static int inline_call(Lw* l, Node* call) {
    SType** targs;
    int ntargs;
    Node* f = inline_target(call, &targs, &ntargs);
    if (!f || !f->b) unsupported(l, "inline call");
    if (f == l->f) unsupported(l, "recursive inline call");
    for (int i = 0; i < l->ninl; i++)
        if (l->inl[i].f == f) unsupported(l, "recursive inline call");
    Node* callee = call->a;
    Node* args[41];
    int n = 0;
    if (callee->kind == N_FIELD_E && !(call->flags & (F_METHOD | F_ASSOC))) args[n++] = callee->a; // receiver
    if (n + call->nkids != f->nkids || f->nkids > 40) unsupported(l, "inline call");
    for (int i = 0; i < call->nkids; i++) args[n++] = call->kids[i];
    SType* rt = f->a ? node_ty(call) : NULL; // the call of a generic method with no result is not typed void
    if (rt && unwrap(rt)->kind == SK_VOID) rt = NULL;
    if (is_agg(rt)) unsupported(l, "inline aggregate result");

    Node* bind[40];
    Node* body;
    inline_copy(f, targs, ntargs, bind, &body);
    scan_storage(l, body);
    for (int i = 0; i < n; i++) {
        Sym* ps = node_sym(bind[i]);
        if (!ps) unsupported(l, "unresolved parameter");
        Node* a = args[i];
        if (a->kind == N_MUTARG) a = a->a;
        if (by_address(f, i)) {
            var_new(l, ps, IT_VOID, place_address(l, place(l, a)));
            continue;
        }
        init_var(l, var_of(l, ps), ps, a);
    }
    int join = new_block(l);
    int res = rt ? var_new(l, NULL, need_type(l, rt), -1) : -1;
    l->inl = (Expansion*)grow(l->inl, &l->cinl, l->ninl + 1, sizeof(Expansion));
    l->inl[l->ninl++] = (Expansion){ f, rt, join, res };
    block(l, body);
    if (l->cur >= 0) { // falling off the end: zero
        if (res >= 0) write_var(l, res, l->cur, konst(l, l->vtype[res], 0));
        jump(l, join);
    }
    l->ninl--;
    seal(l, join);
    enter(l, join);
    return l->cur >= 0 && res >= 0 ? read_var(l, res, join) : -1;
}

// The body of a loop; continue reaches the returned block, current on
// return, which the caller finishes with the step back to the head.
static void loop_body(Lw* l, Node* s, int exit, Node* stmts) {
    int post = new_block(l);
    push_target(l, s, exit, post, -1);
    if (l->cur >= 0) block(l, stmts);
    l->ntgt--;
    jump(l, post);
    seal(l, post);
    enter(l, post);
}


// for i: T in a .. b [step c] (Iterations #4-5): the endpoints once, in order,
// then the step; every selected value in order from a toward b. Ascending by
// one from a constant start to a constant not below it, or from zero to an
// unsigned end, runs the cursor up to the end; any other range counts
// (|b - a| - 1) / step + 1 values from its first, stepping down when a > b
// from the last value the lower endpoint's stepping selects.
static void range_loop(Lw* l, Node* s) {
    Node* r = s->b;
    Sym* bs = node_sym(s->kids[s->nkids - 1]);
    if (s->nkids != 1 || !bs) unsupported(l, "range binders");
    int var = var_of(l, bs);
    IrType bt = need_type(l, bs->type);
    IrType rt = is_untyped(node_ty(r)) ? bt : need_type(l, node_ty(r)); // endpoints at the binder's type
    int sig = ir_type_signed(rt);
    int lo = expr(l, r->a, rt), hi = expr(l, r->b, rt);
    IrIns* lk = &l->fn->ins[lo];
    IrIns* hk = &l->fn->ins[hi];
    int step_one = !r->c || (r->c->kind == N_INT && r->c->ival == 1);
    int up = step_one && lk->op == IR_CONST &&
             (hk->op == IR_CONST ? (sig ? (int64_t)lk->imm <= (int64_t)hk->imm : lk->imm <= hk->imm)
                                 : !sig && lk->imm == 0);
    if (!up && r->c && r->c->kind != N_INT && l->mode == EMIT_DEBUG) unsupported(l, "range step check");
    int cur = var_new(l, NULL, rt, -1);
    int cnt = up ? -1 : var_new(l, NULL, IT_U64, -1);
    int stp = up ? -1 : var_new(l, NULL, IT_U64, -1);
    int head = new_block(l), body = new_block(l), exit = new_block(l);
    if (up) {
        write_var(l, cur, l->cur, lo);
    } else {
        IrType wt = sig ? IT_I64 : IT_U64; // 64-bit endpoints, then u64 arithmetic
        int step = r->c ? expr(l, r->c, rt) : konst(l, rt, 1);
        int a = widen(l, lo, rt, wt), b = widen(l, hi, rt, wt);
        int ua = widen(l, a, wt, IT_U64), ub = widen(l, b, wt, IT_U64);
        int st = widen(l, widen(l, step, rt, wt), wt, IT_U64);
        int nonempty = new_block(l), empty = new_block(l), asc = new_block(l), desc = new_block(l);
        int setup = new_block(l);
        terminate(l, IR_BR, emit2(l, IR_EQ, IT_BOOL, a, b), empty, nonempty);
        seal(l, empty);
        seal(l, nonempty);
        enter(l, empty);
        write_var(l, cnt, l->cur, konst(l, IT_U64, 0));
        write_var(l, cur, l->cur, konst(l, rt, 0));
        write_var(l, stp, l->cur, konst(l, IT_U64, 0));
        jump(l, setup);
        enter(l, nonempty);
        terminate(l, IR_BR, emit2(l, IR_GT, IT_BOOL, a, b), desc, asc);
        seal(l, asc);
        seal(l, desc);
        enter(l, asc);
        int one = konst(l, IT_U64, 1);
        int q = emit2(l, IR_DIV, IT_U64, emit2(l, IR_SUB, IT_U64, emit2(l, IR_SUB, IT_U64, ub, ua), one), st);
        write_var(l, cnt, l->cur, emit2(l, IR_ADD, IT_U64, q, one));
        write_var(l, cur, l->cur, lo);
        write_var(l, stp, l->cur, st);
        jump(l, setup);
        enter(l, desc);
        one = konst(l, IT_U64, 1);
        q = emit2(l, IR_DIV, IT_U64, emit2(l, IR_SUB, IT_U64, emit2(l, IR_SUB, IT_U64, ua, ub), one), st);
        int first = emit2(l, IR_ADD, IT_U64, ub, emit2(l, IR_MUL, IT_U64, q, st));
        write_var(l, cnt, l->cur, emit2(l, IR_ADD, IT_U64, q, one));
        write_var(l, cur, l->cur, widen(l, first, IT_U64, rt));
        write_var(l, stp, l->cur, emit2(l, IR_NEG, IT_U64, st, -1));
        jump(l, setup);
        seal(l, setup);
        enter(l, setup);
    }
    jump(l, head);
    enter(l, head);
    int c = read_var(l, up ? cur : cnt, head);
    int more = up ? emit2(l, IR_LT, IT_BOOL, c, hi) : emit2(l, IR_NE, IT_BOOL, c, konst(l, IT_U64, 0));
    terminate(l, IR_BR, more, body, exit);
    seal(l, body);
    enter(l, body);
    set_var(l, var, widen(l, read_var(l, cur, body), rt, bt));
    loop_body(l, s, exit, s->c);
    if (l->cur >= 0) { // the cursor wraps past the last value, which no iteration reads
        int v = read_var(l, cur, l->cur);
        if (up) {
            v = emit2(l, IR_ADD, rt, v, konst(l, rt, 1));
        } else {
            v = emit2(l, IR_ADD, IT_U64, widen(l, v, rt, IT_U64), read_var(l, stp, l->cur));
            v = widen(l, v, IT_U64, rt);
            write_var(l, cnt, l->cur, emit2(l, IR_SUB, IT_U64, read_var(l, cnt, l->cur), konst(l, IT_U64, 1)));
        }
        write_var(l, cur, l->cur, v);
        jump(l, head);
    }
    seal(l, head);
    seal(l, exit);
    enter(l, exit);
}

// for v: T in seq, for i: usize, v: T in seq, and either under `in mut`
// (Iterations #7-10): the sequence's pointer and length are fixed at entry, and
// a pointer runs from the start to the end, bumped by the element size. The
// value binder is a copy of the element at the pointer, or under `in mut` an
// alias of it; the index binder counts from zero. The body has no bounds checks.
static void seq_loop(Lw* l, Node* s) {
    static SType byte = { .kind = SK_PRIM, .prim = P_U8, .size = 1, .align = 1 };
    if (s->nkids < 1 || s->nkids > 2) unsupported(l, "sequence binders");
    Node* vb = s->kids[s->nkids - 1];
    Node* ib = s->nkids == 2 ? s->kids[0] : NULL;
    Sym* vs = node_sym(vb);
    Sym* is = ib ? node_sym(ib) : NULL;
    if (!vs || (ib && !is)) unsupported(l, "unresolved binder");
    int mut = (s->flags & F_MUT) != 0;
    SType* st = unwrap(node_ty(s->b));
    SType* et;
    int ptr, len = -1;
    if (st && st->kind == SK_ARRAY) { // its place's address and its fixed length
        et = st->base;
        ptr = place_address(l, array_place(l, s->b));
        len = konst(l, IT_U64, st->count);
    } else {
        if (!is_view(st)) unsupported(l, type_why(st));
        et = st->kind == SK_SLICE ? st->base : &byte;
        view_parts(l, s->b, &ptr, &len);
    }
    uint32_t esz = sema_size(et);
    int end = emit2(l, IR_PADD, IT_PTR, ptr, len);
    l->fn->ins[end].imm = esz;
    int cur = var_new(l, NULL, IT_PTR, -1);
    int ix = is ? var_new(l, NULL, IT_U64, -1) : -1;
    int alias = -1;
    if (mut) { // the binder lives at the element's address, which changes each iteration
        alias = var_new(l, NULL, IT_PTR, -1);
        var_new(l, vs, IT_VOID, ADDR_VAR - alias);
    }
    write_var(l, cur, l->cur, ptr);
    if (is) write_var(l, ix, l->cur, konst(l, IT_U64, 0));
    int head = new_block(l), body = new_block(l), exit = new_block(l);
    jump(l, head);
    enter(l, head);
    terminate(l, IR_BR, emit2(l, IR_LT, IT_BOOL, read_var(l, cur, head), end), body, exit);
    seal(l, body);
    enter(l, body);
    int p = read_var(l, cur, body);
    if (mut) {
        write_var(l, alias, l->cur, p);
    } else {
        Place src = { -1, p, 0, 0, et };
        move_place(l, binder_place(l, vb), src);
    }
    if (is) {
        Agg g;
        g.n = 1;
        g.v[0] = read_var(l, ix, l->cur);
        set_leaves(l, var_of(l, is), is->type, &g);
    }
    loop_body(l, s, exit, s->c);
    if (l->cur >= 0) {
        int next = emit2(l, IR_PADD, IT_PTR, read_var(l, cur, l->cur), konst(l, IT_U64, 1));
        l->fn->ins[next].imm = esz;
        write_var(l, cur, l->cur, next);
        if (is) write_var(l, ix, l->cur, emit2(l, IR_ADD, IT_U64, read_var(l, ix, l->cur), konst(l, IT_U64, 1)));
        jump(l, head);
    }
    seal(l, head);
    seal(l, exit);
    enter(l, exit);
}

static void for_loop(Lw* l, Node* s) {
    if (!(s->flags & F_FORGEN)) {
        if (!s->b) unsupported(l, "for head");
        if (s->b->kind == N_RANGE) range_loop(l, s);
        else seq_loop(l, s);
        return;
    }
    if (s->a) stmt(l, s->a);
    if (l->cur < 0) return;
    int head = new_block(l), body = new_block(l), exit = new_block(l);
    int post = s->c ? new_block(l) : head;
    jump(l, head);
    enter(l, head);
    if (s->b) cond(l, s->b, body, exit);
    else jump(l, body);
    seal(l, body);
    enter(l, body);
    push_target(l, s, exit, post, -1);
    if (l->cur >= 0) block(l, s->d);
    l->ntgt--;
    jump(l, post);
    if (post != head) {
        seal(l, post);
        enter(l, post);
        if (l->cur >= 0) stmt(l, s->c);
        jump(l, head);
    }
    seal(l, head);
    seal(l, exit);
    enter(l, exit);
}

// The payload binders of a union switch's case c, each a copy of its variant's
// payload at addr.
static void union_binders(Lw* l, Node* c, SType* ut, int addr) {
    for (int j = 0; j < c->nkids; j++) {
        Node* pat = c->kids[j];
        if (pat->kind != N_VARPAT || !pat->b) continue;
        int vi = (int)pat->ival;
        SType* pt = vi >= 0 && vi < ut->nfields ? ut->fields[vi].t : NULL;
        if (!pt) continue;
        Place src = { -1, addr, (int32_t)sema_union_payload_off(ut), 0, pt };
        move_place(l, binder_place(l, pat->b), src);
    }
}

// switch on a scalar (Conditions #2): the scrutinee once, then each pattern in
// source order against it, a chain of branches to the first matching case, else
// to the default or past the switch. A case ends past the switch, or, when its
// last statement is `fallthrough`, in the next case. Each branch tests "not
// equal" toward the next test, so the chain lays out ahead of the bodies. A
// plain `break` still names the nearest loop, so a switch pushes no target.
//
// On a union the scrutinee is its tag, loaded once from the union's address, a
// pattern is a variant's index, and an arm's binder is a copy of the payload
// made where the arm is entered.
static Place binder_place(Lw* l, Node* binder);
static void move_place(Lw* l, Place dst, Place src);

static void switch_stmt(Lw* l, Node* s) {
    SType* st = node_ty(s->a);
    int un = is_union(st), addr = -1;
    IrType t = un ? IT_I32 : need_type(l, st);
    int v;
    if (un) {
        addr = union_addr(l, s->a, st);
        v = load(l, IT_I32, addr, 0, 0);
    } else {
        v = expr(l, s->a, t);
    }
    int n = s->nkids, ntests = 0, dflt = -1;
    for (int i = 0; i < n; i++) {
        if (s->kids[i]->flags & F_DEFAULT) dflt = i;
        else ntests += s->kids[i]->nkids;
    }
    int end = new_block(l);
    int* body = (int*)arena_alloc((size_t)(n ? n : 1) * sizeof(int));
    for (int i = 0; i < n; i++) body[i] = new_block(l);
    int miss = dflt >= 0 ? body[dflt] : end;
    if (!ntests) jump(l, miss);
    for (int i = 0, k = 0; i < n; i++) {
        Node* c = s->kids[i];
        if (c->flags & F_DEFAULT) continue;
        for (int j = 0; j < c->nkids; j++) {
            int last = ++k == ntests;
            int next = last ? miss : new_block(l);
            int p = un ? konst(l, IT_I32, c->kids[j]->ival) : expr(l, c->kids[j], t);
            terminate(l, IR_BR, emit2(l, IR_NE, IT_BOOL, v, p), next, body[i]);
            if (!last) {
                seal(l, next);
                enter(l, next);
            }
        }
    }
    for (int i = 0; i < n; i++) {
        Node* b = s->kids[i]->b;
        seal(l, body[i]); // its tests, and the case before it if that falls through, have branched here
        enter(l, body[i]);
        if (un && l->cur >= 0 && !(s->kids[i]->flags & F_DEFAULT)) union_binders(l, s->kids[i], unwrap(st), addr);
        if (l->cur >= 0) block(l, b);
        int falls = i + 1 < n && b->nkids && b->kids[b->nkids - 1]->kind == N_FALLTHROUGH;
        jump(l, falls ? body[i + 1] : end);
    }
    seal(l, end);
    enter(l, end);
}

// --- defer ------------------------------------------------------------------------------
// A `defer` at the top of the body captures its call: each read-only argument
// is evaluated there into a fresh variable, and a `mut` argument or receiver
// keeps its storage's address, so the call re-lowered before every return and
// at the fall-off end, last defer first, reads the snapshots and the final
// storage. The result being returned is already SSA values (or memory the
// caller owns), so it survives the calls.

// a fresh variable's name, standing for the argument captured into it
static Node* defer_name(Node* src, SType* t) {
    Node* id = (Node*)arena_alloc(sizeof(Node));
    memset(id, 0, sizeof *id);
    id->kind = N_IDENT;
    id->path = src->path;
    id->line = src->line;
    id->col = src->col;
    Sym* s = (Sym*)arena_alloc(sizeof(Sym));
    memset(s, 0, sizeof *s);
    s->name = "defer";
    s->len = 5;
    s->kind = SYM_VAR;
    s->type = t;
    s->decl = id;
    id->sym = s;
    id->ty = t;
    return id;
}

static int is_literal(Node* x) {
    if (x->kind == N_INT || x->kind == N_FLOAT || x->kind == N_STR || x->kind == N_CHAR || x->kind == N_BOOL ||
        x->kind == N_NULL)
        return 1;
    Sym* s = x->kind == N_IDENT ? node_sym(x) : NULL;
    return s && s->kind == SYM_VAR && s->is_global && s->is_const; // never changes
}

// Argument a of the deferred call, captured: its storage's address when byaddr,
// else its value, in memory when a receiver reads it by address; the name the
// exit-time call reads in its place.
static Node* defer_arg(Lw* l, Node* a, int byaddr, int in_slot) {
    Node* inner = a->kind == N_MUTARG ? a->a : a;
    SType* t = node_ty(inner);
    Node* id;
    if (byaddr) {
        Place p = place(l, inner);
        id = defer_name(inner, p.t);
        var_new(l, node_sym(id), IT_VOID, place_address(l, p));
    } else {
        if (is_untyped(t) || is_literal(inner)) return a; // nothing to snapshot
        id = defer_name(inner, t);
        if (in_slot) ptrmap_put(&l->stored, node_sym(id), 1);
        init_var(l, var_of(l, node_sym(id)), node_sym(id), inner);
    }
    if (a->kind != N_MUTARG) return id;
    Node* w = (Node*)arena_alloc(sizeof(Node));
    *w = *a;
    w->a = id;
    w->ty = id->ty;
    return w;
}

static void defer_capture(Lw* l, Node* s) {
    int top = 0;
    for (int i = 0; !l->ninl && i < l->f->b->nkids; i++) top |= l->f->b->kids[i] == s;
    if (!top) unsupported(l, "defer placement");
    Node* call = s->a;
    if (call->kind != N_CALL) unsupported(l, "defer operand");
    Node* callee = call->a;
    Sym* tgt = call->sym ? node_sym(call) : NULL;
    int instance = callee->kind == N_FIELD_E && tgt && tgt->kind == SYM_FUNC && !tgt->is_extern &&
                   !(call->flags & (F_METHOD | F_ASSOC));
    if (callee->kind == N_FIELD_E && !tgt) unsupported(l, "contract method call");
    Node* sc = (Node*)arena_alloc(sizeof(Node));
    *sc = *call;
    if (instance) {
        Node* sf = (Node*)arena_alloc(sizeof(Node));
        *sf = *callee;
        sf->a = defer_arg(l, callee->a, (call->flags & F_MUT) != 0, 1);
        sc->a = sf;
    } else if (tgt && tgt->kind == SYM_VAR) { // the function value is snapshotted too
        sc->a = defer_arg(l, callee, 0, 0);
    }
    if (call->nkids) {
        sc->kids = (Node**)arena_alloc((size_t)call->nkids * sizeof(Node*));
        for (int i = 0; i < call->nkids; i++)
            sc->kids[i] = defer_arg(l, call->kids[i], call->kids[i]->kind == N_MUTARG, 0);
    }
    l->defers = grow(l->defers, &l->cdefers, l->ndefers + 1, sizeof(Deferred));
    l->defers[l->ndefers++] = (Deferred){ sc, s };
}

static void run_defers(Lw* l) {
    Node* at = l->at;
    for (int i = l->ndefers - 1; i >= 0; i--) {
        l->at = l->defers[i].at; // exit-time code cites the defer's line
        expr(l, l->defers[i].call, -1);
    }
    l->at = at;
}

static void stmt(Lw* l, Node* s) {
    if (l->cur < 0) return;
    if (!l->ninl) l->at = s; // inlined code keeps its call site's line
    switch (s->kind) {
    case N_BLOCK:
        block(l, s);
        return;
    case N_VAR:
        var_decl(l, s);
        return;
    case N_ASSIGN:
        assign(l, s);
        return;
    case N_EXPRSTMT: {
        SType* t = node_ty(s->a);
        if (is_agg(t)) { Agg discard; value(l, s->a, t, &discard); } // expr() returns one scalar id; an aggregate result needs value()'s leaves, here discarded
        else expr(l, s->a, -1);
        return;
    }
    case N_IF: {
        int t = new_block(l), join = new_block(l);
        int f = s->c ? new_block(l) : join;
        cond(l, s->a, t, f);
        seal(l, t);
        enter(l, t);
        if (l->cur >= 0) block(l, s->b);
        jump(l, join);
        if (s->c) {
            seal(l, f);
            enter(l, f);
            if (l->cur >= 0) stmt(l, s->c);
            jump(l, join);
        }
        seal(l, join);
        enter(l, join);
        return;
    }
    case N_FOR:
        for_loop(l, s);
        return;
    case N_LABEL: { // `continue label` restarts the block, a loop head whose back edges come from inside
        int exit = new_block(l), head = new_block(l);
        jump(l, head);
        enter(l, head);
        push_target(l, s, exit, -1, head);
        block(l, s->a);
        l->ntgt--;
        jump(l, exit);
        seal(l, head);
        seal(l, exit);
        enter(l, exit);
        return;
    }
    case N_BREAK:
        jump(l, target(l, s, 0)->brk);
        return;
    case N_CONTINUE: {
        Target* t = target(l, s, 1);
        jump(l, t->cont >= 0 ? t->cont : t->restart);
        return;
    }
    case N_RETURN: {
        if (l->ninl) {
            inline_return(l, s);
            return;
        }
        if (!s->a) {
            ret(l, NULL);
            return;
        }
        if (l->self_head >= 0 && !l->ndefers && is_self_call(l, s->a)) { // release: a branch back to the head
            Args g;
            memset(&g, 0, sizeof g);
            if (call_args(l, s->a, node_sym(s->a), &g) >= 0 || g.n != l->fn->nparams) unsupported(l, "self call");
            for (int i = 0, k = 0; i < l->f->nkids; i++) { // each parameter's leaves, in order
                Sym* ps = node_sym(l->f->kids[i]);
                int var = (int)ptrmap_get(&l->var_ix, ps) - 1;
                if (l->home[var] <= ADDR_VAR) {
                    write_var(l, ADDR_VAR - l->home[var], l->cur, g.v[k++]);
                    continue;
                }
                int n = is_agg(ps->type) ? leaf_count(l, ps->type) : 1;
                for (int j = 0; j < n; j++) write_var(l, var + j, l->cur, g.v[k++]);
            }
            jump(l, l->self_head);
            return;
        }
        if (l->result >= 0) { // into the caller's memory
            store_value(l, s->a, l->rtype, l->result, 0);
            ret(l, NULL);
            return;
        }
        Agg a;
        value(l, s->a, l->rtype, &a);
        ret(l, &a);
        return;
    }
    case N_SWITCH:
        switch_stmt(l, s);
        return;
    case N_FALLTHROUGH: // the enclosing switch branches to the next case
        return;
    case N_DEFER:
        defer_capture(l, s);
        return;
    case N_ASM: unsupported(l, "asm");
    default: unsupported(l, "statement form");
    }
}

// --- functions --------------------------------------------------------------------------

static int by_address(Node* f, int i) {
    return (f->kids[i]->flags & F_MUT) || ((f->flags & F_METHOD) && i == 0);
}

// Whether a local or parameter of f lives in this frame (a slot): the levels
// of a self tail recursion could then observe each other's.
static int frame_storage(Lw* l) {
    for (size_t k = 0; k < l->stored.cap; k++) {
        if (!l->stored.key || !l->stored.key[k]) continue;
        int param = 0;
        for (int i = 0; i < l->f->nkids && !param; i++)
            param = by_address(l->f, i) && node_sym(l->f->kids[i]) == l->stored.key[k];
        if (!param) return 1;
    }
    return 0;
}

static int has_self_tail(Lw* l, Node* n) {
    if (!n) return 0;
    if (n->kind == N_RETURN) return is_self_call(l, n->a);
    if (n->kind == N_FUNC) return 0;
    if (has_self_tail(l, n->a) || has_self_tail(l, n->b) || has_self_tail(l, n->c)) return 1;
    if (n->kind == N_FOR && has_self_tail(l, n->d)) return 1;
    for (int i = 0; i < n->nkids; i++) if (has_self_tail(l, n->kids[i])) return 1;
    return 0;
}

// Drop removed phis and pred-less blocks, resolve replaced values, and number
// the remaining blocks densely.
static void finish(Lw* l) {
    IrFunc* fn = l->fn;
    for (int changed = 1; changed;) { // phis made trivial by later replacements
        changed = 0;
        for (int v = 0; v < fn->nins; v++) {
            IrIns* p = &fn->ins[v];
            if (p->op != IR_PHI || p->block < 0) continue;
            for (int i = 0; i < p->nargs; i++) p->args[i] = find(l, p->args[i]);
            int before = fn->ins[v].block;
            try_remove_trivial(l, v);
            if (fn->ins[v].block != before) changed = 1;
        }
    }
    for (int v = 0; v < fn->nins; v++)
        for (int i = 0; i < fn->ins[v].nargs; i++) fn->ins[v].args[i] = find(l, fn->ins[v].args[i]);
    int* map = malloc((size_t)fn->nblocks * sizeof(int));
    if (!map) oom();
    int n = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        map[b] = b == 0 || bb->npreds || bb->nins ? n++ : -1;
    }
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        if (map[b] < 0) {
            free(bb->ins);
            free(bb->preds);
            continue;
        }
        int k = 0;
        for (int i = 0; i < bb->nins; i++) if (fn->ins[bb->ins[i]].block >= 0) bb->ins[k++] = bb->ins[i];
        bb->nins = k;
        for (int i = 0; i < bb->npreds; i++) bb->preds[i] = map[bb->preds[i]];
        for (int s = 0; s < 2; s++) if (bb->succ[s] >= 0) bb->succ[s] = map[bb->succ[s]];
        for (int i = 0; i < bb->nins; i++) fn->ins[bb->ins[i]].block = map[b];
        fn->blocks[map[b]] = *bb;
    }
    fn->nblocks = n;
    free(map);
}

IrFunc* ir_lower(Node* file, Node* f, EmitMode mode, SType** targs, int ntargs, const char** why) {
    Sym* fs = node_sym(f);
    *why = NULL;
    if (f->ival || (f->flags & F_INTRINSIC)) *why = "compiler-backed";
    else if (!f->b) *why = "no body";
    else if (f->c && !(fs && fs->spec_args)) {
        if (!targs || f->c->nkids != ntargs) *why = "generic";
        else sema_stamp_body(f, targs, ntargs); // as inline_copy does for an inlined generic call
    }
    else if (f->flags & F_EXPO) *why = "exported";
    if (*why) return NULL;

    Lw l;
    memset(&l, 0, sizeof l);
    l.f = f;
    l.file = file;
    l.mode = mode;
    l.self_head = -1;
    l.fn = ir_func_new(f, fs);
    l.is_main = !(f->flags & (F_METHOD | F_ASSOC)) && f->len == 4 && memcmp(f->str, "main", 4) == 0;
    if (setjmp(l.fail)) {
        *why = l.why;
        ir_free(l.fn);
        ptrmap_free(&l.var_ix);
        ptrmap_free(&l.stored);
        free(l.defers); free(l.vtype); free(l.home); free(l.defs.key); free(l.defs.val); free(l.inc); free(l.inc_head); free(l.repl); free(l.tgt); free(l.inl);
        return NULL;
    }
    IrFunc* fn = l.fn;
    SType* rt = fs && fs->type ? fs->type->ret : NULL;
    if (rt && unwrap(rt)->kind == SK_VOID) rt = NULL;
    l.rtype = rt;
    l.result = -1;
    IrLeaf lv[IR_MAX_LEAVES];
    l.cur = new_block(&l);
    seal(&l, l.cur);
    l.at = f;
    if (is_big(rt)) { // stored through the address the caller passes
        covered(&l, rt);
        fn->ret_mem = rt;
        l.result = emit(&l, IR_RESULT, IT_PTR);
    }
    fn->nrets = rt && !fn->ret_mem ? leaves(&l, rt, lv) : 0;
    fn->rets = malloc((size_t)(fn->nrets ? fn->nrets : 1) * sizeof(IrType));
    if (!fn->rets) oom();
    for (int k = 0; k < fn->nrets; k++) fn->rets[k] = (IrType)lv[k].type;
    Args g; // the parameters' leaves, as a call's arguments group
    memset(&g, 0, sizeof g);
    for (int i = 0; i < f->nkids; i++) // in this frame's memory: no self tail recursion
        if (node_sym(f->kids[i]) && !by_address(f, i) && in_memory(node_sym(f->kids[i])->type))
            ptrmap_put(&l.stored, node_sym(f->kids[i]), 1);
    scan_storage(&l, f->b);
    for (int i = 0; i < f->nkids; i++) {
        Node* p = f->kids[i];
        Sym* ps = node_sym(p);
        if (!ps) unsupported(&l, "unresolved parameter");
        if (by_address(f, i) || param_by_address(ps->type)) { // at the caller's address (or copy), held in an SSA variable
            if (!by_address(f, i)) covered(&l, ps->type);
            int v = emit(&l, IR_PARAM, IT_PTR);
            fn->ins[v].imm = (uint64_t)g.n;
            arg_scalar(&l, &g, v);
            int a = var_new(&l, NULL, IT_PTR, -1);
            set_var(&l, a, v);
            var_new(&l, ps, IT_VOID, ADDR_VAR - a);
            continue;
        }
        Agg a;
        a.n = leaves(&l, ps->type, lv);
        if (g.n + a.n > 40) unsupported(&l, "too many parameters");
        if (is_agg(ps->type)) {
            g.part[g.nparts++] = (IrPart){ ps->type, g.n, a.n };
            g.agg = 1;
        }
        for (int k = 0; k < a.n; k++) {
            a.v[k] = emit(&l, IR_PARAM, (IrType)lv[k].type);
            fn->ins[a.v[k]].imm = (uint64_t)g.n;
            if (is_agg(ps->type)) g.v[g.n++] = a.v[k];
            else arg_scalar(&l, &g, a.v[k]);
        }
        set_leaves(&l, var_of(&l, ps), ps->type, &a);
    }
    fn->nparams = g.n;
    fn->params = malloc((size_t)(g.n ? g.n : 1) * sizeof(IrType));
    if (!fn->params) oom();
    for (int k = 0; k < g.n; k++) fn->params[k] = (IrType)fn->ins[g.v[k]].type;
    if (g.agg || (is_agg(rt) && !fn->ret_mem)) {
        fn->sig = (IrSig*)arena_alloc(sizeof(IrSig));
        fn->sig->parts = (IrPart*)arena_alloc((size_t)(g.nparts ? g.nparts : 1) * sizeof(IrPart));
        memcpy(fn->sig->parts, g.part, (size_t)g.nparts * sizeof(IrPart));
        fn->sig->nparts = g.nparts;
        fn->sig->ret = (IrPart){ is_agg(rt) && !fn->ret_mem ? rt : NULL, 0, fn->nrets };
    }
    if (mode == EMIT_RELEASE && !l.is_main && !(f->flags & F_INLCALLS) && !frame_storage(&l) && has_self_tail(&l, f->b)) {
        l.self_head = new_block(&l);
        jump(&l, l.self_head);
        l.cur = l.self_head;
    }
    block(&l, f->b);
    if (l.cur >= 0) { // falling off the end: zero
        Agg z;
        z.n = fn->nrets;
        for (int k = 0; k < z.n; k++) z.v[k] = konst(&l, fn->rets[k], 0);
        ret(&l, &z);
    }
    if (l.self_head >= 0) seal(&l, l.self_head);
    finish(&l);
    if (mode == EMIT_RELEASE) ir_optimize(fn);
    ptrmap_free(&l.var_ix);
    ptrmap_free(&l.stored);
    free(l.defers); free(l.vtype); free(l.home); free(l.defs.key); free(l.defs.val); free(l.inc); free(l.inc_head); free(l.repl); free(l.tgt); free(l.inl);
    return fn;
}

void ir_dump_file(Node* file, EmitMode mode) {
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (n->kind != N_FUNC || !n->path || strcmp(n->path, file->path) != 0) continue; // the root file's own
        const char* why;
        IrFunc* fn = ir_lower(file, n, mode, NULL, 0, &why);
        if (!fn) {
            printf("; ");
            if (n->d) printf("%.*s::", (int)n->d->len, n->d->str);
            printf("%.*s: not lowered (%s)\n\n", (int)n->len, n->str, why);
            continue;
        }
        if (!ir_verify(fn)) exit(1);
        ir_print(fn, stdout);
        putchar('\n');
        ir_free(fn);
    }
}
