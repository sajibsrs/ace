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
// A struct or tuple is its scalar leaves (ir_leaves): a local is one variable
// per leaf, and a call passes and returns leaves, which the target maps onto
// its ABI. A local whose storage something needs holds its leaves in a slot.
//
// A function that uses anything outside the covered subset is not lowered;
// the AST emitter compiles it (migration steps 1 and 2: scalar code, floats,
// pointers, globals, calls, structs, tuples, and Error values).
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
} Target;

typedef struct {
    IrFunc*     fn;
    Node*       f;
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
    Target      tgt[64];
    int         ntgt;
    int         self_head; // release self tail recursion: the loop head block, or -1
    int         is_main;
    SType*      rtype;     // the function's result type, or NULL
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

static int is_agg(SType* t) {
    t = unwrap(t);
    return t && (t->kind == SK_STRUCT || t->kind == SK_TUPLE || is_view(t));
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
    if (ptrmap_get(&l->stored, s)) {
        if (is_agg(s->type)) leaf_count(l, s->type); // covered, although in memory
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
        unsupported(l, type_why(node_ty(x->a)));
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

static int bool_value(Lw* l, Node* x) { // a short-circuit form as a value: a phi of true and false
    int t = new_block(l), f = new_block(l), join = new_block(l);
    cond(l, x, t, f);
    seal(l, t);
    seal(l, f);
    enter(l, t);
    int one = l->cur >= 0 ? konst(l, IT_BOOL, 1) : -1;
    jump(l, join);
    enter(l, f);
    int zero = l->cur >= 0 ? konst(l, IT_BOOL, 0) : -1;
    jump(l, join);
    seal(l, join);
    enter(l, join);
    if (l->cur < 0) return -1;
    IrBlock* jb = &l->fn->blocks[join];
    if (jb->npreds == 1) return jb->preds[0] == t ? one : zero;
    int phi = ir_phi_new(l->fn, join, IT_BOOL);
    for (int i = 0; i < jb->npreds; i++) ir_arg_add(l->fn, phi, jb->preds[i] == t ? one : zero);
    return phi;
}

static int is_self_call(Lw* l, Node* x) {
    if (!x || x->kind != N_CALL || !x->a || (x->a->kind != N_IDENT && x->a->kind != N_FIELD_E)) return 0;
    Sym* s = node_sym(x);
    return s && s->kind == SYM_FUNC && s->decl == l->f;
}

static void value(Lw* l, Node* x, SType* t, Agg* out);

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
    Agg v;
    value(l, a, pt, &v);
    if (g->n + v.n > 40) unsupported(l, "too many arguments");
    g->part[g->nparts++] = (IrPart){ pt, g->n, v.n };
    for (int k = 0; k < v.n; k++) g->v[g->n++] = v.v[k];
    g->agg = 1;
}

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
    for (int i = 0; i < n; i++) {
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
    }
    return first_var;
}

// Emit call id's arguments, and its signature when a part or the result rt
// is an aggregate; an aggregate result's leaves follow as projections into
// res (none without res, whose value is unused).
static int call_finish(Lw* l, int id, Args* g, SType* rt, Agg* res) {
    for (int k = 0; k < g->n; k++) ir_arg_add(l->fn, id, g->v[k]);
    if (!g->agg && !is_agg(rt)) return id;
    IrSig* sig = (IrSig*)arena_alloc(sizeof(IrSig));
    sig->parts = (IrPart*)arena_alloc((size_t)(g->nparts ? g->nparts : 1) * sizeof(IrPart));
    memcpy(sig->parts, g->part, (size_t)g->nparts * sizeof(IrPart));
    sig->nparts = g->nparts;
    sig->ret = (IrPart){ NULL, 0, 0 };
    l->fn->ins[id].sig = sig;
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

// A call: a scalar result's value; an aggregate's leaves into res.
static int call(Lw* l, Node* x, int want, Agg* res) {
    Node* callee = x->a;
    int inst = callee && callee->kind == N_INDEX && callee->sym; // larger[int](a, b)
    Sym* s = inst ? node_sym(callee) : node_sym(x);
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
        return call_finish(l, id, &g, ft->ret, res);
    }
    if (!s || s->kind != SYM_FUNC || !callee) unsupported(l, "indirect or method call");
    Node* d = s->decl;
    if (!d) unsupported(l, "extern call");
    if ((d->kind == N_FUNC && d->ival) || (d->flags & F_INTRINSIC)) unsupported(l, "compiler-backed call");
    if (d->flags & F_INLINE) unsupported(l, "inline call");
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
    return call_finish(l, id, &g, rt, res);
}

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
                unsupported(l, "computed global constant");
            return literal(l, t, init, neg);
        }
        if (s && s->kind == SYM_FUNC) { // a function value
            if (s->decl && (s->decl->flags & (F_INTRINSIC | F_INLINE | F_EXPO))) unsupported(l, "function value");
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
        unsupported(l, type_why(node_ty(x->a)));
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
    }
    switch (x->kind) {
    case N_STRUCTLIT: case N_TUPLE_E:
        literal_agg(l, x, t, out);
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
    case N_IDENT: case N_UNARY:
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

static void ret(Lw* l, const Agg* a) {
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

// Tuple destructuring (Errors #12): the right side once, then each member
// into a new binder (N_PARAM) or an existing place, in order.
static void destructure(Lw* l, Node** kids, int n, Node* rhs) {
    SType* tt = node_ty(rhs);
    SType* u = unwrap(tt);
    if (!u || u->kind != SK_TUPLE || u->nfields != n) unsupported(l, "destructuring");
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

static void push_target(Lw* l, Node* s, int brk, int cont) {
    if (l->ntgt == 64) unsupported(l, "nesting depth");
    l->tgt[l->ntgt++] = (Target){ s->str, s->len, brk, cont };
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
    Agg a;
    value(l, s->b, sym->type, &a);
    set_leaves(l, var, sym->type, &a);
}

// The body of a loop; continue reaches the returned block, current on
// return, which the caller finishes with the step back to the head.
static void loop_body(Lw* l, Node* s, int exit, Node* stmts) {
    int post = new_block(l);
    push_target(l, s, exit, post);
    if (l->cur >= 0) block(l, stmts);
    l->ntgt--;
    jump(l, post);
    seal(l, post);
    enter(l, post);
}

static int widen(Lw* l, int v, IrType from, IrType to) { return from == to ? v : emit2(l, IR_CAST, to, v, -1); }

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

static void for_loop(Lw* l, Node* s) {
    if (!(s->flags & F_FORGEN)) {
        if (!s->b || s->b->kind != N_RANGE) unsupported(l, type_why(node_ty(s->b)));
        range_loop(l, s);
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
    push_target(l, s, exit, post);
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

static void stmt(Lw* l, Node* s) {
    if (l->cur < 0) return;
    l->at = s;
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
    case N_EXPRSTMT:
        expr(l, s->a, -1);
        return;
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
    case N_LABEL: {
        int exit = new_block(l);
        push_target(l, s, exit, -1);
        block(l, s->a);
        l->ntgt--;
        jump(l, exit);
        seal(l, exit);
        enter(l, exit);
        return;
    }
    case N_BREAK:
        jump(l, target(l, s, 0)->brk);
        return;
    case N_CONTINUE: {
        Target* t = target(l, s, 1);
        if (t->cont < 0) unsupported(l, "continue target");
        jump(l, t->cont);
        return;
    }
    case N_RETURN: {
        if (!s->a) {
            ret(l, NULL);
            return;
        }
        if (l->self_head >= 0 && is_self_call(l, s->a)) { // release: a branch back to the head
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
        Agg a;
        value(l, s->a, l->rtype, &a);
        ret(l, &a);
        return;
    }
    case N_DEFER: unsupported(l, "defer");
    case N_SWITCH: unsupported(l, "switch");
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

IrFunc* ir_lower(Node* f, EmitMode mode, const char** why) {
    Sym* fs = node_sym(f);
    *why = NULL;
    if (f->ival || (f->flags & F_INTRINSIC)) *why = "compiler-backed";
    else if (!f->b) *why = "no body";
    else if (f->c && !(fs && fs->spec_args)) *why = "generic";
    else if (f->flags & F_EXPO) *why = "exported";
    else if (f->flags & F_INLCALLS) *why = "calls an inline function";
    if (*why) return NULL;

    Lw l;
    memset(&l, 0, sizeof l);
    l.f = f;
    l.mode = mode;
    l.self_head = -1;
    l.fn = ir_func_new(f, fs);
    l.is_main = !(f->flags & (F_METHOD | F_ASSOC)) && f->len == 4 && memcmp(f->str, "main", 4) == 0;
    if (setjmp(l.fail)) {
        *why = l.why;
        ir_free(l.fn);
        ptrmap_free(&l.var_ix);
        ptrmap_free(&l.stored);
        free(l.vtype); free(l.home); free(l.defs.key); free(l.defs.val); free(l.inc); free(l.inc_head); free(l.repl);
        return NULL;
    }
    IrFunc* fn = l.fn;
    SType* rt = fs && fs->type ? fs->type->ret : NULL;
    if (rt && unwrap(rt)->kind == SK_VOID) rt = NULL;
    l.rtype = rt;
    IrLeaf lv[IR_MAX_LEAVES];
    fn->nrets = rt ? leaves(&l, rt, lv) : 0;
    fn->rets = malloc((size_t)(fn->nrets ? fn->nrets : 1) * sizeof(IrType));
    if (!fn->rets) oom();
    for (int k = 0; k < fn->nrets; k++) fn->rets[k] = (IrType)lv[k].type;
    Args g; // the parameters' leaves, as a call's arguments group
    memset(&g, 0, sizeof g);

    l.cur = new_block(&l);
    seal(&l, l.cur);
    l.at = f;
    scan_storage(&l, f->b);
    for (int i = 0; i < f->nkids; i++) {
        Node* p = f->kids[i];
        Sym* ps = node_sym(p);
        if (!ps) unsupported(&l, "unresolved parameter");
        if (by_address(f, i)) { // lives at the caller's address, held in an SSA variable
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
    if (g.agg || is_agg(rt)) {
        fn->sig = (IrSig*)arena_alloc(sizeof(IrSig));
        fn->sig->parts = (IrPart*)arena_alloc((size_t)(g.nparts ? g.nparts : 1) * sizeof(IrPart));
        memcpy(fn->sig->parts, g.part, (size_t)g.nparts * sizeof(IrPart));
        fn->sig->nparts = g.nparts;
        fn->sig->ret = (IrPart){ is_agg(rt) ? rt : NULL, 0, fn->nrets };
    }
    if (mode == EMIT_RELEASE && !l.is_main && !frame_storage(&l) && has_self_tail(&l, f->b)) {
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
    ptrmap_free(&l.var_ix);
    ptrmap_free(&l.stored);
    free(l.vtype); free(l.home); free(l.defs.key); free(l.defs.val); free(l.inc); free(l.inc_head); free(l.repl);
    return fn;
}

void ir_dump_file(Node* file, EmitMode mode) {
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (n->kind != N_FUNC || !n->path || strcmp(n->path, file->path) != 0) continue; // the root file's own
        const char* why;
        IrFunc* fn = ir_lower(n, mode, &why);
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
