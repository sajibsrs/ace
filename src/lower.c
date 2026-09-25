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
// A function that uses anything outside the covered subset is not lowered;
// the AST emitter compiles it (migration step 1: scalar integer code).
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
    IrType*     vtype;
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
} Lw;

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

// The IR type of t, or -1 when the IR does not cover it yet.
static int ir_type_of(SType* t) {
    t = unwrap(t);
    if (!t || t->kind != SK_PRIM) return -1;
    switch (t->prim) {
    case P_BOOL: return IT_BOOL;
    case P_I8: return IT_I8;
    case P_I16: return IT_I16;
    case P_I32: return IT_I32;
    case P_I64: return IT_I64;
    case P_U8: return IT_U8;
    case P_U16: return IT_U16;
    case P_U32: return IT_U32;
    case P_U64: case P_USIZE: return IT_U64;
    default: return -1;
    }
}

static const char* type_why(SType* t) {
    t = unwrap(t);
    if (!t) return "untyped value";
    switch (t->kind) {
    case SK_PRIM:
        if (t->prim == P_F32 || t->prim == P_F64) return "floating-point value";
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

// A zero of var's type at the top of the entry block: the value of a read no
// definition reaches, which definite initialization confines to dead paths.
static int zero_at_entry(Lw* l, IrType type) {
    IrFunc* fn = l->fn;
    int id = ir_phi_new(fn, 0, type); // placed after the entry's (absent) phis
    fn->ins[id].op = IR_CONST;
    fn->ins[id].imm = 0;
    fn->ins[id].at = l->f;
    return id;
}

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

static int var_of(Lw* l, Sym* s, IrType type) {
    uintptr_t ix = ptrmap_get(&l->var_ix, s);
    if (ix) return (int)ix - 1;
    l->vtype = grow(l->vtype, &l->cvars, l->nvars + 1, sizeof(IrType));
    l->vtype[l->nvars] = type;
    ptrmap_put(&l->var_ix, s, (uintptr_t)l->nvars + 1);
    return l->nvars++;
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

static int local_var(Lw* l, Node* ident) {
    Sym* s = node_sym(ident);
    if (!s || s->kind != SYM_VAR) unsupported(l, "non-variable name");
    if (s->is_global) unsupported(l, "global variable");
    return var_of(l, s, need_type(l, s->type));
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
    if (l->mode != EMIT_DEBUG || t == IT_BOOL) return 0;
    return op == IR_ADD || op == IR_SUB || op == IR_MUL || op == IR_DIV || op == IR_REM ||
           op == IR_SHL || op == IR_SHR || op == IR_NEG;
}

static int arith(Lw* l, int op, IrType t, int a, int b) {
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
    if (!x || x->kind != N_CALL || !x->a || x->a->kind != N_IDENT) return 0;
    Sym* s = node_sym(x);
    return s && s->kind == SYM_FUNC && s->decl == l->f;
}

// Arguments of a direct call, each at its parameter's type.
static void call_args(Lw* l, Node* call, Sym* s, int* out) {
    Node* callee = s->decl;
    if (call->nkids != callee->nkids) unsupported(l, "default argument");
    for (int i = 0; i < call->nkids; i++) {
        Node* a = call->kids[i];
        if (a->kind == N_MUTARG || (callee->kids[i]->flags & F_MUT)) unsupported(l, "mut parameter");
        Sym* ps = node_sym(callee->kids[i]);
        IrType pt = need_type(l, ps ? ps->type : NULL);
        out[i] = expr(l, a, pt);
    }
}

static int call(Lw* l, Node* x, int want) {
    Node* callee = x->a;
    Sym* s = node_sym(x);
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
    if (!s || s->kind != SYM_FUNC || !callee || callee->kind != N_IDENT) unsupported(l, "indirect or method call");
    Node* d = s->decl;
    if (!d || d->kind != N_FUNC) unsupported(l, "extern call");
    if (s->is_extern) unsupported(l, "extern call");
    if (s->is_generic || s->spec_args || d->c) unsupported(l, "generic call");
    if (d->ival || (d->flags & F_INTRINSIC)) unsupported(l, "compiler-backed call");
    if (d->flags & F_INLINE) unsupported(l, "inline call");
    if (d->flags & (F_METHOD | F_ASSOC | F_EXPO)) unsupported(l, "method or exported call");
    if (d->nkids > 8) unsupported(l, "stack-passed argument");
    int args[8];
    call_args(l, x, s, args);
    SType* rt = s->type ? s->type->ret : NULL;
    IrType t = rt && unwrap(rt)->kind != SK_VOID ? need_type(l, rt) : IT_VOID;
    int id = emit(l, IR_CALL, t);
    l->fn->ins[id].fn = s;
    for (int i = 0; i < x->nkids; i++) ir_arg_add(l->fn, id, args[i]);
    return id;
}

static int expr(Lw* l, Node* x, int want) {
    switch (x->kind) {
    case N_INT: case N_CHAR: case N_BOOL:
        return konst(l, type_in(l, x, want), x->ival);
    case N_IDENT: {
        Sym* s = node_sym(x);
        if (s && s->kind == SYM_VAR && s->is_global && s->is_const && s->init) {
            IrType t = type_in(l, x, want);
            Node* init = s->init;
            int neg = 0;
            if (init->kind == N_UNARY && init->op == T_MINUS) { neg = 1; init = init->a; }
            if (init->kind != N_INT && init->kind != N_CHAR && init->kind != N_BOOL)
                unsupported(l, "computed global constant");
            return konst(l, t, neg ? (uint64_t)0 - init->ival : init->ival);
        }
        return read_var(l, local_var(l, x), l->cur);
    }
    case N_UNARY: {
        IrType t = type_in(l, x, want);
        if (x->op == T_MINUS) {
            if (x->a->kind == N_INT) return konst(l, t, (uint64_t)0 - x->a->ival);
            return arith(l, IR_NEG, t, expr(l, x->a, t), -1);
        }
        if (x->op == T_BANG || x->op == T_TILDE) return emit2(l, IR_NOT, t, expr(l, x->a, t), -1);
        unsupported(l, x->op == T_AMP ? "address-of" : "pointer dereference");
    }
    case N_BINARY: {
        if (x->op == T_ANDAND || x->op == T_OROR) return bool_value(l, x);
        int op = binop_of((TokKind)x->op);
        if (op < 0) unsupported(l, "operator");
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
        return call(l, x, want);
    default:
        unsupported(l, "expression form");
    }
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

static void ret(Lw* l, int v) {
    leaks_check(l);
    terminate(l, IR_RET, v, -1, -1);
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
    if (s->nkids != 1 || s->kids[0]->kind != N_IDENT) unsupported(l, "assignment target");
    if (s->flags & F_VOLATILE) unsupported(l, "volatile store");
    Node* dst = s->kids[0];
    int var = local_var(l, dst);
    IrType t = l->vtype[var];
    int v;
    if (s->op == T_ASSIGN) {
        v = expr(l, s->a, t);
    } else {
        int op = binop_of((TokKind)s->op);
        if (op < 0) unsupported(l, "compound assignment");
        int old = read_var(l, var, l->cur);
        v = arith(l, op, t, old, expr(l, s->a, t));
    }
    write_var(l, var, l->cur, v);
}

static void var_decl(Lw* l, Node* s) {
    if (s->nkids != 1 || s->kids[0]->kind != N_PARAM || (s->flags & F_MIXED)) unsupported(l, "multiple binders");
    Node* binder = s->kids[0];
    Sym* sym = node_sym(binder);
    if (!sym) unsupported(l, "unresolved binder");
    int var = var_of(l, sym, need_type(l, sym->type));
    if (!s->b) return; // assigned before any read (definite initialization)
    if (s->b->kind == N_TUPLE_E) unsupported(l, "tuple");
    write_var(l, var, l->cur, expr(l, s->b, l->vtype[var]));
}

static void for_loop(Lw* l, Node* s) {
    if (!(s->flags & F_FORGEN)) unsupported(l, "range or sequence loop");
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
        IrType rt = l->fn->ret;
        if (!s->a) {
            ret(l, -1);
            return;
        }
        if (s->a->kind == N_TUPLE_E) unsupported(l, "tuple");
        if (l->self_head >= 0 && is_self_call(l, s->a)) { // release: a branch back to the head
            int args[8];
            call_args(l, s->a, node_sym(s->a), args);
            for (int i = 0; i < l->fn->nparams; i++)
                write_var(l, (int)ptrmap_get(&l->var_ix, node_sym(l->f->kids[i])) - 1, l->cur, args[i]);
            jump(l, l->self_head);
            return;
        }
        ret(l, expr(l, s->a, rt));
        return;
    }
    case N_DEFER: unsupported(l, "defer");
    case N_SWITCH: unsupported(l, "switch");
    case N_ASM: unsupported(l, "asm");
    default: unsupported(l, "statement form");
    }
}

// --- functions --------------------------------------------------------------------------

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
    else if (f->flags & F_METHOD) *why = "method";
    else if (f->c && !(fs && fs->spec_args)) *why = "generic";
    else if (f->flags & F_EXPO) *why = "exported";
    else if (f->flags & F_INLCALLS) *why = "calls an inline function";
    else if (f->nkids > 8) *why = "stack-passed parameter";
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
        free(l.vtype); free(l.defs.key); free(l.defs.val); free(l.inc); free(l.inc_head); free(l.repl);
        return NULL;
    }
    IrFunc* fn = l.fn;
    SType* rt = fs && fs->type ? fs->type->ret : NULL;
    fn->ret = rt && unwrap(rt)->kind != SK_VOID ? need_type(&l, rt) : IT_VOID;
    if (rt && unwrap(rt)->kind == SK_TUPLE) unsupported(&l, "tuple");
    fn->nparams = f->nkids;
    fn->params = malloc((size_t)(f->nkids ? f->nkids : 1) * sizeof(IrType));
    if (!fn->params) oom();

    l.cur = new_block(&l);
    seal(&l, l.cur);
    l.at = f;
    for (int i = 0; i < f->nkids; i++) {
        Node* p = f->kids[i];
        Sym* ps = node_sym(p);
        if (p->flags & F_MUT) unsupported(&l, "mut parameter");
        if (!ps) unsupported(&l, "unresolved parameter");
        IrType t = need_type(&l, ps->type);
        fn->params[i] = t;
        int v = emit(&l, IR_PARAM, t);
        fn->ins[v].imm = (uint64_t)i;
        write_var(&l, var_of(&l, ps, t), l.cur, v);
    }
    if (mode == EMIT_RELEASE && !l.is_main && has_self_tail(&l, f->b)) {
        l.self_head = new_block(&l);
        jump(&l, l.self_head);
        l.cur = l.self_head;
    }
    block(&l, f->b);
    if (l.cur >= 0) ret(&l, fn->ret == IT_VOID ? -1 : konst(&l, fn->ret, 0)); // falling off the end
    if (l.self_head >= 0) seal(&l, l.self_head);
    finish(&l);
    ptrmap_free(&l.var_ix);
    free(l.vtype); free(l.defs.key); free(l.defs.val); free(l.inc); free(l.inc_head); free(l.repl);
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
