// optimizer.c — pass 6, the release-only optimizer, over one function's IR
// (spec/compiler.md Compiler #2). Each pass runs
// once, in the order spec/compiler.md gives: sparse conditional constant propagation,
// global value numbering, loop strength reduction, dead-code and control-flow
// cleanup, then auto-vectorization over the cleaned loops (and cleanup again
// after it rewrites any).
//
// The passes rely only on facts a debug build checks: an integer operation
// that would trap in debug (a zero divisor, MIN / -1, a shift count outside
// the width) is never folded, and an unused load or division is removed.
#include "ir.h"
#include "backend.h"

// --- shared helpers -----------------------------------------------------------------

static int is_int(IrType t) { return t >= IT_BOOL && t <= IT_PTR; }

static void* xgrow(void* p, int* cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 4;
    while (c < need) c *= 2;
    p = realloc(p, (size_t)c * elem);
    if (!p) oom();
    *cap = c;
    return p;
}

static void* xalloc(size_t n, size_t elem) {
    void* p = calloc(n ? n : 1, elem);
    if (!p) oom();
    return p;
}

static void kill_ins(IrFunc* fn, int v) {
    IrIns* x = &fn->ins[v];
    free(x->args);
    x->args = NULL;
    x->nargs = x->cargs = 0;
    x->block = -1;
}

// --- sparse conditional constant propagation --------------------------------------
//
// Wegman and Zadeck, "Constant Propagation with Conditional Branches" (1991):
// each integer value is unknown (top), one constant, or varying (bottom), and
// only blocks reached through an edge a branch can take are evaluated, so a
// constant condition prunes its other arm before that arm's values are seen.

enum { L_TOP, L_CONST, L_BOT };

typedef struct {
    IrFunc*   fn;
    uint8_t*  lat;
    uint64_t* val;
    uint8_t*  exec;  // per block
    uint8_t*  edge;  // per block and successor slot: 2 * block + slot
    uint8_t*  inq;   // per value: on the worklist
    int*      ustart, *users; // per value, the instructions using it
    int*      work;  // value worklist
    int       nwork;
} Sccp;

static void push(Sccp* s, int v) {
    if (s->inq[v]) return;
    s->inq[v] = 1;
    s->work[s->nwork++] = v;
}

static void lower_to(Sccp* s, int v, int st, uint64_t c) {
    if (st == L_TOP || s->lat[v] == L_BOT) return;
    if (s->lat[v] == L_CONST) {
        if (st == L_CONST && s->val[v] == c) return;
        st = L_BOT;
    }
    s->lat[v] = (uint8_t)st;
    s->val[v] = c;
    for (int k = s->ustart[v]; k < s->ustart[v + 1]; k++) push(s, s->users[k]);
}

static void reach(Sccp* s, int b) {
    if (s->exec[b]) return;
    s->exec[b] = 1;
    IrBlock* bb = &s->fn->blocks[b];
    for (int i = 0; i < bb->nins; i++) push(s, bb->ins[i]);
}

static void take(Sccp* s, int b, int slot) {
    if (s->edge[2 * b + slot]) return;
    s->edge[2 * b + slot] = 1;
    int t = s->fn->blocks[b].succ[slot];
    if (!s->exec[t]) {
        reach(s, t);
        return;
    }
    IrBlock* tb = &s->fn->blocks[t]; // a new incoming edge: re-meet the phis
    for (int i = 0; i < tb->nins && s->fn->ins[tb->ins[i]].op == IR_PHI; i++) push(s, tb->ins[i]);
}

// The operation over constants, or 0 when it is not defined (release makes
// those cases undefined; they stay for run time).
static int fold(IrOp op, IrType t, IrType ot, uint64_t a, uint64_t b, uint64_t* out) {
    int bits = ir_type_bits(t), sg = ir_type_signed(t);
    switch (op) {
    case IR_ADD: *out = ir_wrap(t, a + b); return 1;
    case IR_SUB: *out = ir_wrap(t, a - b); return 1;
    case IR_MUL: *out = ir_wrap(t, a * b); return 1;
    case IR_MULH: *out = (uint64_t)(((unsigned __int128)a * b) >> 64); return 1;
    case IR_AND: *out = ir_wrap(t, a & b); return 1;
    case IR_OR:  *out = ir_wrap(t, a | b); return 1;
    case IR_XOR: *out = ir_wrap(t, a ^ b); return 1;
    case IR_SHL:
        if (b >= (uint64_t)bits) return 0;
        *out = ir_wrap(t, a << b);
        return 1;
    case IR_SHR:
        if (b >= (uint64_t)bits) return 0;
        *out = ir_wrap(t, sg ? (uint64_t)((int64_t)a >> b) : a >> b);
        return 1;
    case IR_DIV: case IR_REM:
        if (b == 0 || (sg && (int64_t)b == -1)) return 0;
        if (sg) *out = ir_wrap(t, op == IR_DIV ? (uint64_t)((int64_t)a / (int64_t)b) : (uint64_t)((int64_t)a % (int64_t)b));
        else *out = ir_wrap(t, op == IR_DIV ? a / b : a % b);
        return 1;
    case IR_NEG: *out = ir_wrap(t, 0 - a); return 1;
    case IR_NOT: *out = t == IT_BOOL ? a ^ 1 : ir_wrap(t, ~a); return 1;
    case IR_CAST:
        if (t == IT_BOOL) return 0;
        *out = ir_wrap(t, a);
        return 1;
    case IR_EQ: *out = a == b; return 1;
    case IR_NE: *out = a != b; return 1;
    case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
        int lt, eq = a == b;
        if (ir_type_signed(ot)) lt = (int64_t)a < (int64_t)b;
        else lt = a < b;
        *out = op == IR_LT ? lt : op == IR_LE ? lt || eq : op == IR_GT ? !lt && !eq : !lt;
        return 1;
    }
    default: return 0;
    }
}

static int foldable(IrFunc* fn, IrIns* x) {
    if (x->flags & IRF_CHECKED) return 0;
    switch (x->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_REM:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_NEG: case IR_NOT: case IR_MULH:
        return is_int(x->type) && x->type != IT_PTR;
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        return is_int(fn->ins[x->args[0]].type);
    case IR_CAST:
        return is_int(x->type) && is_int(fn->ins[x->args[0]].type);
    default: return 0;
    }
}

static void meet_phi(Sccp* s, int v) {
    IrFunc* fn = s->fn;
    IrIns* x = &fn->ins[v];
    IrBlock* bb = &fn->blocks[x->block];
    int have = 0;
    uint64_t c = 0;
    for (int i = 0; i < x->nargs; i++) {
        int p = bb->preds[i], live = 0;
        for (int k = 0; k < 2; k++) live |= fn->blocks[p].succ[k] == x->block && s->edge[2 * p + k];
        int a = x->args[i];
        if (!live || s->lat[a] == L_TOP) continue;
        if (s->lat[a] == L_BOT || (have && c != s->val[a])) {
            lower_to(s, v, L_BOT, 0);
            return;
        }
        have = 1;
        c = s->val[a];
    }
    if (have) lower_to(s, v, L_CONST, c);
}

static void visit(Sccp* s, int v) {
    IrFunc* fn = s->fn;
    IrIns* x = &fn->ins[v];
    if (x->block < 0 || !s->exec[x->block]) return;
    int b = x->block;
    switch (x->op) {
    case IR_JMP: take(s, b, 0); return;
    case IR_BR: {
        int c = x->args[0];
        if (s->lat[c] == L_CONST) take(s, b, s->val[c] ? 0 : 1);
        else if (s->lat[c] == L_BOT) { take(s, b, 0); take(s, b, 1); }
        return;
    }
    case IR_RET: return;
    case IR_CONST:
        if (is_int(x->type)) lower_to(s, v, L_CONST, x->imm);
        else lower_to(s, v, L_BOT, 0);
        return;
    case IR_PHI:
        if (is_int(x->type)) meet_phi(s, v);
        else lower_to(s, v, L_BOT, 0);
        return;
    default: break;
    }
    if (!foldable(fn, x)) {
        lower_to(s, v, L_BOT, 0);
        return;
    }
    int top = 0, bot = 0;
    uint64_t c[2] = { 0, 0 };
    for (int i = 0; i < x->nargs; i++) {
        int a = x->args[i];
        if (s->lat[a] == L_TOP) top = 1;
        else if (s->lat[a] == L_BOT) bot = 1;
        else c[i] = s->val[a];
    }
    if ((x->op == IR_AND || x->op == IR_MUL) && x->nargs == 2) { // a zero operand decides the result
        for (int i = 0; i < 2; i++)
            if (s->lat[x->args[i]] == L_CONST && s->val[x->args[i]] == 0) {
                lower_to(s, v, L_CONST, 0);
                return;
            }
    }
    if (bot) { lower_to(s, v, L_BOT, 0); return; }
    if (top) return;
    uint64_t out;
    IrType ot = (IrType)fn->ins[x->args[0]].type;
    if (fold((IrOp)x->op, (IrType)x->type, ot, c[0], c[1], &out)) lower_to(s, v, L_CONST, out);
    else lower_to(s, v, L_BOT, 0);
}

// Rewrite every value found constant into a CONST; a phi that becomes one
// moves after its block's remaining phis.
static void sccp(IrFunc* fn) {
    Sccp s;
    memset(&s, 0, sizeof s);
    s.fn = fn;
    s.lat = xalloc((size_t)fn->nins, 1);
    s.val = xalloc((size_t)fn->nins, sizeof(uint64_t));
    s.exec = xalloc((size_t)fn->nblocks, 1);
    s.edge = xalloc((size_t)fn->nblocks * 2, 1);
    s.inq = xalloc((size_t)fn->nins, 1);
    s.work = xalloc((size_t)fn->nins, sizeof(int));
    s.ustart = xalloc((size_t)fn->nins + 1, sizeof(int));
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            for (int a = 0; a < x->nargs; a++) s.ustart[x->args[a] + 1]++;
        }
    for (int v = 0; v < fn->nins; v++) s.ustart[v + 1] += s.ustart[v];
    s.users = xalloc((size_t)s.ustart[fn->nins], sizeof(int));
    int* fill = xalloc((size_t)fn->nins, sizeof(int));
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int u = fn->blocks[b].ins[i];
            IrIns* x = &fn->ins[u];
            for (int a = 0; a < x->nargs; a++) {
                int d = x->args[a];
                s.users[s.ustart[d] + fill[d]++] = u;
            }
        }
    free(fill);

    reach(&s, 0);
    while (s.nwork) {
        int v = s.work[--s.nwork];
        s.inq[v] = 0;
        visit(&s, v);
    }

    int ok = 1; // a reachable branch on a value never given one: leave the function alone
    for (int b = 0; b < fn->nblocks && ok; b++) {
        if (!s.exec[b]) continue;
        IrIns* t = &fn->ins[fn->blocks[b].ins[fn->blocks[b].nins - 1]];
        if (t->op == IR_BR && s.lat[t->args[0]] == L_TOP) ok = 0;
    }
    for (int b = 0; b < fn->nblocks && ok; b++) {
        IrBlock* bb = &fn->blocks[b];
        if (!s.exec[b]) continue;
        int phis = 0, nconst = 0;
        while (phis < bb->nins && fn->ins[bb->ins[phis]].op == IR_PHI) phis++;
        for (int i = 0; i < bb->nins; i++) {
            int v = bb->ins[i];
            IrIns* x = &fn->ins[v];
            if (s.lat[v] != L_CONST || x->op == IR_CONST) continue;
            free(x->args);
            x->args = NULL;
            x->nargs = x->cargs = 0;
            x->imm = s.val[v];
            x->flags = 0;
            x->op = IR_CONST;
            nconst += i < phis;
        }
        if (!nconst) continue;
        int* fresh = xalloc((size_t)bb->nins, sizeof(int)); // remaining phis, the phis now constants, the rest
        int k = 0;
        for (int i = 0; i < phis; i++) if (fn->ins[bb->ins[i]].op == IR_PHI) fresh[k++] = bb->ins[i];
        for (int i = 0; i < phis; i++) if (fn->ins[bb->ins[i]].op == IR_CONST) fresh[k++] = bb->ins[i];
        for (int i = phis; i < bb->nins; i++) fresh[k++] = bb->ins[i];
        memcpy(bb->ins, fresh, (size_t)bb->nins * sizeof(int));
        free(fresh);
    }
    free(s.lat); free(s.val); free(s.exec); free(s.edge); free(s.inq); free(s.work); free(s.ustart); free(s.users);
}

// --- global value numbering -------------------------------------------------------------
//
// A walk of the dominator tree with a scoped table of the pure operations seen:
// an operation equal to one in a dominating block is replaced by it, and an
// identity (`x + 0`, `x * 1`, `x | x`, a cast to the same type) by its operand.
// A constant operand counts by its value, not its id, because lowering makes a
// fresh `const` per use. Loads are never numbered (no memory model).

static int is_pure(const IrIns* x) {
    if (x->flags) return 0;
    switch (x->op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_REM:
    case IR_AND: case IR_OR: case IR_XOR: case IR_SHL: case IR_SHR: case IR_NEG: case IR_NOT:
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
    case IR_CAST: case IR_BITCAST:
    case IR_POPCNT: case IR_CLZ: case IR_CTZ: case IR_BSWAP: case IR_ROTL: case IR_ROTR: case IR_MULH:
    case IR_SQRT: case IR_FLOOR: case IR_CEIL: case IR_FMA:
    case IR_PADD: case IR_PSUB: case IR_GLOBAL:
        return 1;
    default: return 0;
    }
}

typedef struct { int v, next; } VnEnt;

typedef struct {
    IrFunc* fn;
    int*    repl;
    int*    head; // hash bucket -> newest entry, or -1
    VnEnt*  ent;
    int*    log;  // bucket of each live entry, newest last
    int     nent, mask;
} Vn;

static int vn_chase(const Vn* g, int v) {
    while (g->repl[v] >= 0) v = g->repl[v];
    return v;
}

static int commutes(const IrFunc* fn, const IrIns* x) {
    switch (x->op) {
    case IR_ADD: case IR_MUL: case IR_MULH: case IR_AND: case IR_OR: case IR_XOR: case IR_EQ: case IR_NE:
        return !ir_type_float((IrType)fn->ins[x->args[0]].type);
    default: return 0;
    }
}

static int same_arg(const IrFunc* fn, int a, int b) {
    if (a == b) return 1;
    const IrIns* x = &fn->ins[a];
    const IrIns* y = &fn->ins[b];
    return x->op == IR_CONST && y->op == IR_CONST && x->type == y->type && x->imm == y->imm;
}

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + UINT64_C(0x9E3779B97F4A7C15) + (h << 6) + (h >> 2);
    return h;
}

static uint64_t arg_hash(const IrFunc* fn, int a) {
    const IrIns* x = &fn->ins[a];
    return x->op == IR_CONST ? mix(mix(1, x->type), x->imm) : mix(2, (uint64_t)a);
}

static uint64_t vn_hash(const IrFunc* fn, const IrIns* x) {
    uint64_t h = mix(mix(mix(x->op, x->type), x->imm), (uint64_t)(uint32_t)x->aux);
    if (x->op == IR_GLOBAL) return mix(h, (uint64_t)(uintptr_t)x->sym);
    if (commutes(fn, x)) return mix(h, arg_hash(fn, x->args[0]) + arg_hash(fn, x->args[1]));
    for (int i = 0; i < x->nargs; i++) h = mix(h, arg_hash(fn, x->args[i]));
    return h;
}

// Whether globals x and y address the same object: one symbol can own several
// named objects (a static map's tables, a generic's instantiations).
static int same_global(const IrIns* x, const IrIns* y) {
    return x->sym == y->sym && (x->name == y->name || (x->name && y->name && !strcmp(x->name, y->name)));
}

// A global counts as equal only by a shared, non-null symbol and name: the
// name-only (vtable) form has no symbol to key on and is never merged.
static int vn_equal(const IrFunc* fn, const IrIns* x, const IrIns* y) {
    if (x->op != y->op || x->type != y->type || x->imm != y->imm || x->aux != y->aux || x->nargs != y->nargs) return 0;
    if (x->op == IR_GLOBAL) return x->sym && same_global(x, y);
    int direct = 1, swapped = x->nargs == 2 && commutes(fn, x);
    for (int i = 0; i < x->nargs; i++) {
        direct &= same_arg(fn, x->args[i], y->args[i]);
        if (swapped) swapped &= same_arg(fn, x->args[i], y->args[1 - i]);
    }
    return direct || swapped;
}

static int is_konst(const IrFunc* fn, int a, uint64_t k) {
    const IrIns* x = &fn->ins[a];
    return x->op == IR_CONST && !ir_type_float((IrType)x->type) && x->imm == k;
}

// The operand x equals, or -1.
static int simplify(const IrFunc* fn, const IrIns* x) {
    if (!x->nargs) return -1;
    int a = x->args[0], b = x->nargs > 1 ? x->args[1] : -1;
    if (ir_type_float((IrType)x->type)) return -1;
    switch (x->op) {
    case IR_ADD: case IR_OR: case IR_XOR:
        if (is_konst(fn, b, 0)) return a;
        if (is_konst(fn, a, 0)) return b;
        return x->op == IR_OR && a == b ? a : -1;
    case IR_AND: return a == b ? a : -1;
    case IR_SUB: case IR_SHL: case IR_SHR: case IR_ROTL: case IR_ROTR: case IR_PADD: case IR_PSUB:
        return is_konst(fn, b, 0) ? a : -1;
    case IR_MUL:
        if (is_konst(fn, b, 1)) return a;
        return is_konst(fn, a, 1) ? b : -1;
    case IR_DIV: return is_konst(fn, b, 1) ? a : -1;
    case IR_CAST: return fn->ins[a].type == x->type ? a : -1;
    default: return -1;
    }
}

static void gvn_block(Vn* g, int b) {
    IrFunc* fn = g->fn;
    IrBlock* bb = &fn->blocks[b];
    int k = 0;
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = &fn->ins[v];
        for (int a = 0; a < x->nargs; a++) x->args[a] = vn_chase(g, x->args[a]);
        if (!is_pure(x)) { bb->ins[k++] = v; continue; }
        int w = simplify(fn, x);
        if (w < 0) {
            int h = (int)(vn_hash(fn, x) & (uint64_t)g->mask);
            for (int e = g->head[h]; e >= 0 && w < 0; e = g->ent[e].next)
                if (vn_equal(fn, &fn->ins[g->ent[e].v], x)) w = g->ent[e].v;
            if (w < 0) {
                g->ent[g->nent] = (VnEnt){ v, g->head[h] };
                g->head[h] = g->nent;
                g->log[g->nent++] = h;
                bb->ins[k++] = v;
                continue;
            }
        }
        g->repl[v] = w;
        kill_ins(fn, v);
    }
    bb->nins = k;
}

static void gvn(IrFunc* fn) {
    Vn g;
    g.fn = fn;
    int n = fn->nblocks, size = 16;
    while (size < 2 * fn->nins) size *= 2;
    g.mask = size - 1;
    g.head = xalloc((size_t)size, sizeof(int));
    for (int i = 0; i < size; i++) g.head[i] = -1;
    g.ent = xalloc((size_t)fn->nins, sizeof(VnEnt));
    g.log = xalloc((size_t)fn->nins, sizeof(int));
    g.repl = xalloc((size_t)fn->nins, sizeof(int));
    for (int v = 0; v < fn->nins; v++) g.repl[v] = -1;
    g.nent = 0;
    int* rpo, nrpo;
    int* idom = ir_dominators(fn, &rpo, &nrpo);
    int* cstart = xalloc((size_t)n + 1, sizeof(int));
    for (int b = 1; b < n; b++) if (idom[b] >= 0) cstart[idom[b] + 1]++;
    for (int b = 0; b < n; b++) cstart[b + 1] += cstart[b];
    int* kids = xalloc((size_t)n, sizeof(int));
    int* fill = xalloc((size_t)n, sizeof(int));
    for (int b = 1; b < n; b++) if (idom[b] >= 0) kids[cstart[idom[b]] + fill[idom[b]]++] = b;
    int* sb = xalloc((size_t)n, sizeof(int)); // the walk's frames: block, next child, entries at entry
    int* si = xalloc((size_t)n, sizeof(int));
    int* sm = xalloc((size_t)n, sizeof(int));
    int sp = 0;
    sb[0] = 0; si[0] = 0; sm[0] = 0; sp = 1;
    gvn_block(&g, 0);
    while (sp) {
        int b = sb[sp - 1];
        if (si[sp - 1] < cstart[b + 1] - cstart[b]) {
            int c = kids[cstart[b] + si[sp - 1]++];
            sb[sp] = c; si[sp] = 0; sm[sp] = g.nent; sp++;
            gvn_block(&g, c);
            continue;
        }
        while (g.nent > sm[sp - 1]) { // leave the block: drop its entries
            g.nent--;
            g.head[g.log[g.nent]] = g.ent[g.head[g.log[g.nent]]].next;
        }
        sp--;
    }
    for (int b = 0; b < n; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            for (int a = 0; a < x->nargs; a++) x->args[a] = vn_chase(&g, x->args[a]);
        }
    free(g.head); free(g.ent); free(g.log); free(g.repl);
    free(rpo); free(idom); free(cstart); free(kids); free(fill); free(sb); free(si); free(sm);
}

// --- loops ------------------------------------------------------------------------------

// The predecessor index of the back edge into h when h heads a natural loop:
// two predecessors, a latch h dominates and an entry it does not. body then
// marks the loop, every block reaching the latch without passing h; -1 when h
// heads none.
static int natural_loop(const IrFunc* fn, const int* idom, int h, uint8_t* body, int* stack) {
    const IrBlock* hb = &fn->blocks[h];
    if (hb->npreds != 2) return -1;
    int li = -1;
    for (int i = 0; i < 2; i++)
        if (idom[hb->preds[i]] >= 0 && ir_dominates(idom, h, hb->preds[i])) li = li < 0 ? i : -2;
    if (li < 0) return -1;
    int latch = hb->preds[li], pre = hb->preds[1 - li];
    if (idom[pre] < 0 || ir_dominates(idom, h, pre)) return -1;
    memset(body, 0, (size_t)fn->nblocks);
    int sp = 0;
    body[h] = 1;
    if (!body[latch]) { body[latch] = 1; stack[sp++] = latch; }
    while (sp) {
        const IrBlock* bb = &fn->blocks[stack[--sp]];
        for (int i = 0; i < bb->npreds; i++)
            if (!body[bb->preds[i]]) { body[bb->preds[i]] = 1; stack[sp++] = bb->preds[i]; }
    }
    return li;
}

// A new instruction in b, before its terminator.
static int ins_before_term(IrFunc* fn, int b, IrOp op, IrType type) {
    int v = ir_ins_new(fn, b, op, type);
    IrBlock* bb = &fn->blocks[b];
    bb->ins[bb->nins - 1] = bb->ins[bb->nins - 2];
    bb->ins[bb->nins - 2] = v;
    return v;
}

// --- loop-invariant code motion ---------------------------------------------------------
//
// A natural loop's instruction whose operands are all defined outside it moves
// to the end of the loop's entry block, computed once per entry rather than
// once per iteration: a pure operation that cannot trap (not a division), and
// a load from a stack object when nothing in the loop stores, calls, or is
// atomic or asm. Neither faults when the loop runs no iteration. A constant
// operand gets its own copy there, as lowering makes one per use. Inner loops
// go first, so a value they hoist can leave the enclosing loop too.

static int clobbers(const IrIns* x) {
    switch (x->op) {
    case IR_STORE: case IR_CALL: case IR_CALLI: case IR_RTCALL: case IR_ATOMIC: case IR_ASM: return 1;
    default: return 0;
    }
}

static void licm(IrFunc* fn) {
    int n = fn->nblocks;
    int* rpo, nrpo;
    int* idom = ir_dominators(fn, &rpo, &nrpo);
    uint8_t* body = xalloc((size_t)n, 1);
    int* stack = xalloc((size_t)n, sizeof(int));
    for (int r = nrpo - 1; r >= 0; r--) {
        int h = rpo[r];
        int li = natural_loop(fn, idom, h, body, stack);
        if (li < 0) continue;
        int pre = fn->blocks[h].preds[1 - li], quiet = 1;
        for (int b = 0; b < n && quiet; b++)
            for (int i = 0; body[b] && i < fn->blocks[b].nins && quiet; i++) quiet = !clobbers(&fn->ins[fn->blocks[b].ins[i]]);
        for (int q = 0; q < nrpo; q++) { // dominators first: an operand hoists before its user
            int b = rpo[q];
            if (!body[b]) continue;
            int k = 0;
            for (int i = 0; i < fn->blocks[b].nins; i++) {
                int v = fn->blocks[b].ins[i];
                const IrIns* x = &fn->ins[v];
                int ok = (is_pure(x) && x->op != IR_DIV && x->op != IR_REM) ||
                         (x->op == IR_LOAD && !x->flags && quiet && fn->ins[x->args[0]].op == IR_SLOT);
                for (int a = 0; a < x->nargs && ok; a++) {
                    const IrIns* y = &fn->ins[x->args[a]];
                    ok = y->op == IR_CONST || !body[y->block];
                }
                if (!ok) {
                    fn->blocks[b].ins[k++] = v;
                    continue;
                }
                for (int a = 0; a < fn->ins[v].nargs; a++) {
                    int y = fn->ins[v].args[a];
                    if (!body[fn->ins[y].block]) continue;
                    int c = ins_before_term(fn, pre, IR_CONST, (IrType)fn->ins[y].type);
                    fn->ins[c].imm = fn->ins[y].imm;
                    fn->ins[v].args[a] = c;
                }
                IrBlock* pb = &fn->blocks[pre];
                pb->ins = xgrow(pb->ins, &pb->cins, pb->nins + 1, sizeof(int));
                pb->ins[pb->nins] = pb->ins[pb->nins - 1];
                pb->ins[pb->nins - 1] = v;
                pb->nins++;
                fn->ins[v].block = pre;
            }
            fn->blocks[b].nins = k;
        }
    }
    free(rpo); free(idom); free(body); free(stack);
}

// --- loop strength reduction ------------------------------------------------------------
//
// A pointer offset `base +/- i * S` of a basic induction variable i (a header
// 64-bit phi whose back-edge operand is i +/- a constant c, indexing directly
// or through a cast to u64, the same bits) and an invariant base
// becomes a pointer phi of its own, started at `base +/- init * S` before the
// loop and stepped by `c * S` at the latch. The two phis step together, so the
// pointer equals the offset everywhere the header dominates. Only a scale that
// is not a power of two gains: a power-of-two one is a shifted operand the
// backend folds into the access or an add, while any other needs a `mov` and
// a `madd` per iteration. Offsets with one base, scale, direction, and i share
// one pointer. Release wraps, so the rewrite holds for every trip count.

typedef struct { int base, op, phi, q; uint64_t scale; } LsrPtr;

// repl covers the values from before the pass; a pointer phi is never replaced.
static int lsr_chase(const int* repl, int nins, int v) {
    while (v < nins && repl[v] >= 0) v = repl[v];
    return v;
}

// The induction variable index v stands for: itself, or the 64-bit value a
// cast to u64 reinterprets.
static int lsr_index(const IrFunc* fn, int v) {
    const IrIns* x = &fn->ins[v];
    if (x->op == IR_CAST && x->type == IT_U64 && fn->ins[x->args[0]].type == IT_I64) return x->args[0];
    return v;
}

// v as the u64 index of a pointer offset in b.
static int lsr_u64(IrFunc* fn, int b, int v) {
    if (fn->ins[v].type == IT_U64) return v;
    int c = ins_before_term(fn, b, fn->ins[v].op == IR_CONST ? IR_CONST : IR_CAST, IT_U64);
    if (fn->ins[v].op == IR_CONST) fn->ins[c].imm = fn->ins[v].imm;
    else ir_arg_add(fn, c, v);
    return c;
}

static int same_base(const IrFunc* fn, int a, int b) {
    if (a == b) return 1;
    const IrIns* x = &fn->ins[a];
    const IrIns* y = &fn->ins[b];
    return x->op == IR_GLOBAL && y->op == IR_GLOBAL && same_global(x, y);
}

// The pointer standing for offset u of phi in the loop headed by h, entered
// from pre and closed by latch, or -1 when its base does not reach pre.
static int lsr_pointer(IrFunc* fn, const int* idom, const int* repl, int nins, LsrPtr** ptrs, int* nptr, int* cptr,
                       int h, int pre, int latch, int pi, int li, int phi, int u) {
    int base = lsr_chase(repl, nins, fn->ins[u].args[0]), op = fn->ins[u].op;
    uint64_t scale = fn->ins[u].imm;
    for (int i = 0; i < *nptr; i++) {
        LsrPtr* p = &(*ptrs)[i];
        if (p->phi == phi && p->op == op && p->scale == scale && same_base(fn, p->base, base)) return p->q;
    }
    if (fn->ins[base].op == IR_GLOBAL && !ir_dominates(idom, fn->ins[base].block, pre)) {
        int g = ins_before_term(fn, pre, IR_GLOBAL, IT_PTR);
        fn->ins[g].sym = fn->ins[base].sym;
        fn->ins[g].name = fn->ins[base].name;
        base = g;
    } else if (!ir_dominates(idom, fn->ins[base].block, pre)) {
        return -1;
    }
    int init = fn->ins[phi].args[pi], next = fn->ins[phi].args[li];
    int start = base;
    if (fn->ins[init].op != IR_CONST || fn->ins[init].imm != 0) {
        int k = lsr_u64(fn, pre, init);
        start = ins_before_term(fn, pre, (IrOp)op, IT_PTR);
        fn->ins[start].imm = scale;
        ir_arg_add(fn, start, base);
        ir_arg_add(fn, start, k);
    }
    int q = ir_phi_new(fn, h, IT_PTR);
    int up = (op == IR_PADD) == (fn->ins[next].op == IR_ADD);
    int c = lsr_u64(fn, latch, fn->ins[next].args[1]);
    int step = ins_before_term(fn, latch, up ? IR_PADD : IR_PSUB, IT_PTR);
    fn->ins[step].imm = scale;
    ir_arg_add(fn, step, q);
    ir_arg_add(fn, step, c);
    for (int i = 0; i < 2; i++) ir_arg_add(fn, q, i == pi ? start : step);
    *ptrs = xgrow(*ptrs, cptr, *nptr + 1, sizeof(LsrPtr));
    (*ptrs)[(*nptr)++] = (LsrPtr){ base, op, phi, q, scale };
    return q;
}

// The preheader-resident global that base already is or chases to, reusing
// one already hoisted in this loop for the same symbol; base unchanged when
// it already dominates pre, or -1 when it is some other loop-varying value a
// full pointer rewrite would be needed for (out of scope: no general LICM).
// Marked by phi = -1 in the shared table, so it never matches a real
// induction-pointer entry (phi is always a valid instruction id, >= 0).
static int lsr_hoist(IrFunc* fn, const int* idom, LsrPtr** ptrs, int* nptr, int* cptr, int pre, int base) {
    if (ir_dominates(idom, fn->ins[base].block, pre)) return base;
    if (fn->ins[base].op != IR_GLOBAL) return -1;
    for (int i = 0; i < *nptr; i++) {
        LsrPtr* p = &(*ptrs)[i];
        if (p->phi < 0 && same_base(fn, p->base, base)) return p->q;
    }
    int g = ins_before_term(fn, pre, IR_GLOBAL, IT_PTR);
    fn->ins[g].sym = fn->ins[base].sym;
    fn->ins[g].name = fn->ins[base].name;
    *ptrs = xgrow(*ptrs, cptr, *nptr + 1, sizeof(LsrPtr));
    (*ptrs)[(*nptr)++] = (LsrPtr){ base, 0, -1, g, 0 };
    return g;
}

static void lsr(IrFunc* fn) {
    int n = fn->nblocks, nins = fn->nins, any = 0;
    for (int v = 0; v < nins && !any; v++) {
        const IrIns* x = &fn->ins[v];
        any = (x->op == IR_PADD || x->op == IR_PSUB) && x->block >= 0 && !x->flags;
    }
    if (!any) return;
    int* rpo, nrpo;
    int* idom = ir_dominators(fn, &rpo, &nrpo);
    int* repl = xalloc((size_t)nins, sizeof(int));
    for (int v = 0; v < nins; v++) repl[v] = -1;
    uint8_t* body = xalloc((size_t)n, 1);
    int* stack = xalloc((size_t)n, sizeof(int));
    int* phis = NULL;
    int cphis = 0, changed = 0;
    LsrPtr* ptrs = NULL;
    int cptr = 0;
    for (int r = 0; r < nrpo; r++) {
        int h = rpo[r];
        IrBlock* hb = &fn->blocks[h];
        int li = natural_loop(fn, idom, h, body, stack);
        if (li < 0) continue;
        int pi = 1 - li, latch = hb->preds[li], pre = hb->preds[pi];
        int nphi = 0; // the header's basic induction variables, before any pointer phi joins them
        for (int i = 0; i < hb->nins && fn->ins[hb->ins[i]].op == IR_PHI; i++) {
            int phi = hb->ins[i];
            IrIns* px = &fn->ins[phi];
            IrIns* nx = &fn->ins[px->args[li]];
            if ((px->type != IT_U64 && px->type != IT_I64) || (nx->op != IR_ADD && nx->op != IR_SUB) || nx->flags || nx->args[0] != phi ||
                fn->ins[nx->args[1]].op != IR_CONST)
                continue;
            phis = xgrow(phis, &cphis, nphi + 1, sizeof(int));
            phis[nphi++] = phi;
        }
        int nptr = 0;
        for (int k = 0; k < nphi; k++)
            for (int b = 0; b < n; b++) {
                if (!body[b]) continue;
                for (int i = 0; i < fn->blocks[b].nins; i++) {
                    int u = fn->blocks[b].ins[i];
                    IrIns* x = &fn->ins[u];
                    if ((x->op != IR_PADD && x->op != IR_PSUB) || x->flags || lsr_index(fn, x->args[1]) != phis[k] || u >= nins)
                        continue;
                    if (x->imm & (x->imm - 1)) {
                        int q = lsr_pointer(fn, idom, repl, nins, &ptrs, &nptr, &cptr, h, pre, latch, pi, li, phis[k], u);
                        if (q < 0) continue;
                        repl[u] = q;
                    } else {
                        int base = lsr_chase(repl, nins, x->args[0]);
                        int g = lsr_hoist(fn, idom, &ptrs, &nptr, &cptr, pre, base);
                        if (g < 0 || g == base) continue;
                        fn->ins[u].args[0] = g; // lsr_hoist may have grown fn->ins, so x is stale
                    }
                    changed = 1;
                }
            }
    }
    if (changed) {
        for (int b = 0; b < fn->nblocks; b++) {
            IrBlock* bb = &fn->blocks[b];
            int k = 0;
            for (int i = 0; i < bb->nins; i++) {
                int v = bb->ins[i];
                if (v < nins && repl[v] >= 0) { kill_ins(fn, v); continue; }
                IrIns* x = &fn->ins[v];
                for (int a = 0; a < x->nargs; a++) x->args[a] = lsr_chase(repl, nins, x->args[a]);
                bb->ins[k++] = v;
            }
            bb->nins = k;
        }
    }
    free(rpo); free(idom); free(repl); free(body); free(stack); free(phis); free(ptrs);
}

// --- auto-vectorization -----------------------------------------------------------------
//
// spec/compiler.md Compiler #6. An innermost counted loop (a header holding only
// i = phi [0, pre], [i + 1, body], lt i, n, and its branch, for a u64 or i64
// i indexing directly or through its cast to u64, or, under a constant n that
// fits, through i32 lane value t = cast i32 i and its cast to u64; a body of
// blocks in sequence, where one may branch on a compare into arms of a single
// block each, or none, that meet at the next, whose phis become selects)
// whose body is lane-wise f32/i32 arithmetic over elements base[i] gains a
// vector loop ahead of it, four elements per iteration (two for f64, which
// takes only arithmetic, loads, and stores, never mixed with 32-bit lanes),
// when every store's
// object is provably another access's object only at the same element: a
// distinct global or stack object, or the same base at the same offset. The
// vector loop counts whole vectors, k = i / width, so the target folds the
// 16-byte address scale; the original loop then runs the remainder from
// i = width * k.
// A header phi other than i (a reduction or any carried value) keeps the loop
// scalar, as does a body value used after it. An arm holds no load or store,
// since the vector loop runs both; t lanes come from a vector phi counting
// <0, 1, 2, 3> up by 4.

// A struct array whose F same-typed fields each get the same computation
// (every field of a stored struct stored, each field reading only its own
// field) is the flat array of its fields: element e is field e % F of struct
// e / F, so the vector loop runs F vectors per width structs, the body's
// field-0 computation once per vector, and every lane still computes what the
// scalar loop does for that field. Its elements are base[i] as a padd scaled
// by the struct size S = F * the field size, or a header phi stepping by S
// (loop strength reduction's pointer for a size that is no power of two).

enum { VEC_BLOCKS = 32, VEC_PTRS = 8 };

// b is the latch; blk the body in order, each arm after the block branching to
// it; ptr the header's pointer phis, stepping by stride, which pstep holds
typedef struct { int h, b, pre, pi, i, step, n, lane, nb, width; // width: lanes, 4 or 2 (f64)
    int blk[VEC_BLOCKS]; uint8_t arm[VEC_BLOCKS];
    int np, stride, fields; // fields: F, 1 for contiguous elements
    int ptr[VEC_PTRS], pstep[VEC_PTRS]; } VecLoop;

static int vec_ptr(const VecLoop* L, int v) {
    for (int j = 0; j < L->np; j++)
        if (L->ptr[j] == v) return j;
    return -1;
}

// The object address p offsets: a padd's base, or a pointer's entry value.
static int vec_base(const IrFunc* fn, const VecLoop* L, int p) {
    return fn->ins[p].args[vec_ptr(L, p) >= 0 ? L->pi : 0];
}

static int vec_pstep(const VecLoop* L, int v) {
    for (int j = 0; j < L->np; j++)
        if (L->pstep[j] == v) return 1;
    return 0;
}

static int vec_at(const VecLoop* L, int b) {
    for (int j = 0; j < L->nb; j++)
        if (L->blk[j] == b) return j;
    return -1;
}

static IrType vec_of(IrType t) {
    return t == IT_F32 ? IT_V4F32 : t == IT_I32 ? IT_V4I32 : t == IT_F64 ? IT_V2F64 : IT_VOID;
}

// Whether lane type t fits L: every lane type of a loop has one width, set by
// the first; f64 takes only arithmetic, loads, and stores (ir.h).
static int vec_width(VecLoop* L, IrType t) {
    int w = t == IT_F64 ? 2 : 4;
    if (!L->width) L->width = w;
    return L->width == w;
}

// Whether bases a and b are distinct objects: other globals, other stack
// objects, or a global and a stack object.
static int distinct_objects(const IrFunc* fn, int a, int b) {
    int ga = fn->ins[a].op == IR_GLOBAL, gb = fn->ins[b].op == IR_GLOBAL;
    int sa = fn->ins[a].op == IR_SLOT, sb = fn->ins[b].op == IR_SLOT;
    if (ga && gb) return !same_base(fn, a, b);
    if (sa && sb) return a != b;
    return (ga && sb) || (sa && gb);
}

// Whether v, used in loop L, is the same in every iteration: defined outside
// it, or a constant or global address the body computes.
static int vec_invariant(const IrFunc* fn, const VecLoop* L, int v) {
    const IrIns* x = &fn->ins[v];
    if (x->block != L->h && vec_at(L, x->block) < 0) return 1;
    return x->block != L->h && (x->op == IR_CONST || x->op == IR_GLOBAL);
}

// A lane value of L's body: a load or lane-wise arithmetic.
static int vec_lane(const IrFunc* fn, const VecLoop* L, int v) {
    const IrIns* x = &fn->ins[v];
    return vec_at(L, x->block) >= 0 && vec_of((IrType)x->type) != IT_VOID && x->op != IR_CONST;
}

// Whether v is a padd index of L: i, or its cast to u64 or t's.
static int vec_index(const IrFunc* fn, const VecLoop* L, int v) {
    const IrIns* x = &fn->ins[v];
    return lsr_index(fn, v) == L->i || (L->lane >= 0 && x->op == IR_CAST && x->type == IT_U64 && x->args[0] == L->lane);
}

// The block an arm of b's branch leads to, or b itself when it is no arm.
static int vec_arm(const IrFunc* fn, int h, int b) {
    const IrBlock* x = &fn->blocks[b];
    return x->npreds == 1 && x->succ[1] < 0 && x->succ[0] != h && b != h ? x->succ[0] : b;
}

static int vec_operand(const IrFunc* fn, const VecLoop* L, int v, IrType t) {
    return fn->ins[v].type == t && (vec_lane(fn, L, v) || vec_invariant(fn, L, v));
}

// Whether the loop headed by h has the accepted shape; fills *L.
static int vec_shape(IrFunc* fn, int h, VecLoop* L) {
    IrBlock* hb = &fn->blocks[h];
    if (hb->npreds != 2 || hb->nins < 3 || hb->nins > 3 + VEC_PTRS || hb->succ[1] < 0) return 0;
    int first = hb->succ[0], b = first, nb = 0;
    if (first == h || fn->blocks[first].npreds != 1) return 0;
    for (;;) { // the body's blocks in sequence, each fork's arms after it
        if (nb >= VEC_BLOCKS - 2) return 0;
        L->arm[nb] = 0;
        L->blk[nb++] = b;
        IrBlock* xb = &fn->blocks[b];
        if (xb->succ[0] == h && xb->succ[1] < 0) break;
        int j = xb->succ[0];
        if (j < 0) return 0; // leaves through a return, so h heads no loop
        if (xb->succ[1] >= 0) { // a fork: arms t and f meet at j
            int t = xb->succ[0], f = xb->succ[1];
            j = vec_arm(fn, h, t);
            if (j < 0 || j == b || j != vec_arm(fn, h, f) || (t == j && f == j) || fn->blocks[j].npreds != 2) return 0;
            if (t != j) { L->arm[nb] = 1; L->blk[nb++] = t; }
            if (f != j) { L->arm[nb] = 1; L->blk[nb++] = f; }
        } else if (fn->blocks[j].npreds != 1) {
            return 0;
        }
        if (j == h || j == first || vec_at(L, j) >= 0) return 0;
        b = j;
    }
    L->nb = nb;
    L->lane = -1;
    int li = hb->preds[0] == b ? 0 : hb->preds[1] == b ? 1 : -1;
    if (li < 0) return 0;
    int pre = hb->preds[1 - li];
    if (pre == h || vec_at(L, pre) >= 0) return 0;
    IrIns* cmp = &fn->ins[hb->ins[hb->nins - 2]];
    IrIns* br = &fn->ins[hb->ins[hb->nins - 1]];
    if (cmp->op != IR_LT || br->op != IR_BR || br->args[0] != hb->ins[hb->nins - 2]) return 0;
    IrIns* phi = &fn->ins[cmp->args[0]];
    if (phi->op != IR_PHI || phi->block != h || (phi->type != IT_U64 && phi->type != IT_I64)) return 0;
    L->np = L->stride = 0;
    for (int j = 0; j < hb->nins - 2; j++) { // i, and pointers stepping by one stride
        int p = hb->ins[j];
        if (p == cmp->args[0]) continue;
        const IrIns* px = &fn->ins[p];
        if (px->op != IR_PHI || px->type != IT_PTR) return 0;
        const IrIns* sx = &fn->ins[px->args[li]];
        const IrIns* ox = &fn->ins[px->args[1 - li]];
        if (sx->op != IR_PADD || sx->flags || sx->block != b || sx->args[0] != p || fn->ins[sx->args[1]].op != IR_CONST ||
            fn->ins[sx->args[1]].imm != 1 || !sx->imm || (L->stride && sx->imm != (uint64_t)L->stride) ||
            ox->block == h || vec_at(L, ox->block) >= 0)
            return 0;
        L->stride = (int)sx->imm;
        L->ptr[L->np] = p;
        L->pstep[L->np++] = px->args[li];
    }
    IrIns* init = &fn->ins[phi->args[1 - li]];
    IrIns* step = &fn->ins[phi->args[li]];
    if (init->op != IR_CONST || init->imm != 0 || step->op != IR_ADD || step->flags || step->block != b ||
        step->args[0] != hb->ins[0] || fn->ins[step->args[1]].op != IR_CONST || fn->ins[step->args[1]].imm != 1)
        return 0;
    L->h = h, L->b = b, L->pre = pre, L->pi = 1 - li, L->i = cmp->args[0], L->step = phi->args[li], L->n = cmp->args[1];
    if (!vec_invariant(fn, L, L->n)) return 0;
    const IrIns* n = &fn->ins[L->n];
    return n->op != IR_CONST || (int64_t)n->imm >= 4;
}

// Whether lane value w computes for field f what v does for field 0: the same
// operations over the same invariants, reading field f where v reads field 0.
static int vec_iso(const IrFunc* fn, const VecLoop* L, int v, int w, int f, int es) {
    const IrIns* x = &fn->ins[v];
    const IrIns* y = &fn->ins[w];
    int lv = vec_lane(fn, L, v), lw = vec_lane(fn, L, w);
    if (!lv || !lw) {
        if (lv || lw) return 0;
        return v == w || (x->op == IR_CONST && y->op == IR_CONST && x->type == y->type && x->imm == y->imm);
    }
    if (x->op != y->op || x->type != y->type || x->nargs != y->nargs) return 0;
    if (x->op == IR_LOAD) return x->aux == 0 && y->aux == f * es && x->args[0] == y->args[0];
    for (int a = 0; a < x->nargs; a++)
        if (!vec_iso(fn, L, x->args[a], y->args[a], f, es)) return 0;
    return 1;
}

// Whether L's F fields each get field 0's computation: a single block whose
// every store of a field f > 0 has a field-0 store to the same struct computing
// the same, and each field-0 store one in every field.
static int vec_fields(const IrFunc* fn, const VecLoop* L, const int* acc, int nacc, int es) {
    if (L->nb != 1 || L->lane >= 0) return 0;
    for (int s = 0; s < nacc; s++) {
        const IrIns* sx = &fn->ins[acc[s]];
        if (sx->aux % es || sx->aux < 0 || sx->aux >= L->stride) return 0;
        if (sx->op != IR_STORE) continue;
        int f = sx->aux / es;
        for (int g = 0; g < L->fields; g++) { // f's field-0 store, or field 0's in every field g
            if (f && g) break;
            int want = f ? 0 : g * es, found = 0;
            for (int t = 0; t < nacc && !found; t++) {
                const IrIns* tx = &fn->ins[acc[t]];
                const IrIns* z = f ? tx : sx; // the field-0 store
                const IrIns* o = f ? sx : tx;
                found = tx->op == IR_STORE && tx->aux == want && tx->args[0] == sx->args[0] &&
                        vec_iso(fn, L, z->args[1], o->args[1], f ? f : g, es);
            }
            if (!found) return 0;
        }
    }
    return 1;
}

// Whether L's body is lane-wise over independent elements, used only inside it.
static int vec_body(IrFunc* fn, VecLoop* L) {
    int nins = 0;
    for (int k = 0; k < L->nb; k++) nins += fn->blocks[L->blk[k]].nins;
    int* acc = xalloc((size_t)nins, sizeof(int)); // the loads and stores
    int nacc = 0, ok = 1, stores = 0;
    const IrIns* n = &fn->ins[L->n];
    L->width = 0;
    for (int k = 0; k < L->nb && ok; k++) {
        IrBlock* bb = &fn->blocks[L->blk[k]];
        for (int j = 0; j < bb->nins && ok; j++) {
            int u = bb->ins[j];
            IrIns* x = &fn->ins[u];
            IrType t = (IrType)x->type;
            if (u == L->step || vec_pstep(L, u) || x->op == IR_JMP || x->op == IR_CONST || x->op == IR_GLOBAL) continue;
            if (x->flags) { ok = 0; break; }
            switch (x->op) {
            case IR_BR: // a fork on a compare of its own block
                ok = fn->ins[x->args[0]].block == L->blk[k] && fn->ins[x->args[0]].type == IT_BOOL &&
                     fn->ins[x->args[0]].op != IR_PHI;
                break;
            case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
                IrType ot = (IrType)fn->ins[x->args[0]].type;
                ok = vec_of(ot) != IT_VOID && ot != IT_F64 && vec_width(L, ot) && vec_operand(fn, L, x->args[0], ot) && vec_operand(fn, L, x->args[1], ot);
                break;
            }
            case IR_PHI: // a join's: lane values or invariants
                ok = vec_of(t) != IT_VOID && t != IT_F64 && vec_width(L, t) && x->nargs == 2 && vec_operand(fn, L, x->args[0], t) &&
                     vec_operand(fn, L, x->args[1], t);
                break;
            case IR_CAST:
                if (x->args[0] == L->i && t == IT_I32 && L->lane < 0) { // t, which a constant n keeps exact
                    ok = n->op == IR_CONST && n->imm <= INT32_MAX && vec_width(L, IT_I32);
                    L->lane = u;
                } else {
                    ok = t == IT_U64 && ((x->args[0] == L->i && fn->ins[L->i].type == IT_I64) || x->args[0] == L->lane);
                }
                break;
            case IR_PADD: // every element one stride apart
                ok = x->imm && (!L->stride || x->imm == (uint64_t)L->stride) && vec_index(fn, L, x->args[1]) &&
                     vec_invariant(fn, L, x->args[0]);
                L->stride = (int)x->imm;
                break;
            case IR_LOAD: case IR_STORE: {
                IrIns* px = &fn->ins[x->args[0]];
                IrType et = x->op == IR_LOAD ? t : (IrType)fn->ins[x->args[1]].type;
                ok = !L->arm[k] && ((px->op == IR_PADD && vec_at(L, px->block) >= 0) || vec_ptr(L, x->args[0]) >= 0) &&
                     vec_of(et) != IT_VOID && vec_width(L, et) && (x->op == IR_LOAD || vec_operand(fn, L, x->args[1], et));
                acc[nacc++] = u;
                stores += x->op == IR_STORE;
                break;
            }
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
                ok = vec_of(t) != IT_VOID && vec_width(L, t) && (x->op != IR_DIV || t != IT_I32) && vec_operand(fn, L, x->args[0], t) &&
                     vec_operand(fn, L, x->args[1], t);
                break;
            case IR_AND: case IR_OR: case IR_XOR:
                ok = t == IT_I32 && vec_width(L, t) && vec_operand(fn, L, x->args[0], t) && vec_operand(fn, L, x->args[1], t);
                break;
            case IR_SHL: case IR_SHR: {
                const IrIns* c = &fn->ins[x->args[1]];
                ok = t == IT_I32 && vec_width(L, t) && vec_operand(fn, L, x->args[0], t) && c->op == IR_CONST && c->imm >= 1 && c->imm <= 31;
                break;
            }
            case IR_NEG:
                ok = vec_of(t) != IT_VOID && vec_width(L, t) && vec_operand(fn, L, x->args[0], t);
                break;
            default:
                ok = 0;
                break;
            }
        }
    }
    int es = L->width == 2 ? 8 : 4; // the element, or field, size
    int inner = L->stride > es; // aux offsets within one struct: its other fields, never another iteration's
    for (int s = 0; s < nacc && ok; s++) { // each store against every other access
        IrIns* sx = &fn->ins[acc[s]];
        if (sx->op != IR_STORE) continue;
        int sbase = vec_base(fn, L, sx->args[0]);
        IrType st = (IrType)fn->ins[sx->args[1]].type;
        for (int a = 0; a < nacc && ok; a++) {
            if (a == s) continue;
            IrIns* ax = &fn->ins[acc[a]];
            int abase = vec_base(fn, L, ax->args[0]);
            IrType at = (IrType)(ax->op == IR_LOAD ? ax->type : fn->ins[ax->args[1]].type);
            ok = distinct_objects(fn, sbase, abase) ||
                 (same_base(fn, sbase, abase) && ((sx->aux == ax->aux && st == at) ||
                                                 (inner && sx->aux != ax->aux && sx->aux >= 0 && ax->aux >= 0 &&
                                                  sx->aux < L->stride && ax->aux < L->stride &&
                                                  sx->aux % es == 0 && ax->aux % es == 0)));
        }
    }
    ok = ok && stores && L->stride % es == 0;
    L->fields = ok ? L->stride / es : 0;
    if (ok && L->fields > 1) ok = vec_fields(fn, L, acc, nacc, es);
    free(acc);
    if (!ok) return 0;
    // No body value leaves the loop; i only indexes, steps, and becomes t; a
    // u64 cast only indexes; a compare only forks.
    for (int b = 0; b < fn->nblocks && ok; b++) {
        IrBlock* ob = &fn->blocks[b];
        int in = vec_at(L, b) >= 0;
        for (int j = 0; j < ob->nins && ok; j++) {
            IrIns* x = &fn->ins[ob->ins[j]];
            for (int a = 0; a < x->nargs && ok; a++) {
                int v = x->args[a];
                IrIns* vx = &fn->ins[v];
                if (v == L->i && in && !(x->op == IR_PADD && a == 1) && x->op != IR_CAST && ob->ins[j] != L->step)
                    ok = 0;
                if (vec_ptr(L, v) >= 0 && b != L->h && !(in && a == 0 && (x->op == IR_LOAD || x->op == IR_STORE || vec_pstep(L, ob->ins[j]))))
                    ok = 0; // a pointer only addresses and steps
                if (vec_at(L, vx->block) < 0) continue;
                if (!in && b != L->h) ok = 0;
                if (vx->op == IR_CAST && v != L->lane && !(x->op == IR_PADD && a == 1)) ok = 0;
                if (vx->type == IT_BOOL && x->op != IR_BR) ok = 0;
            }
        }
    }
    return ok;
}

static int konst_in(IrFunc* fn, int b, IrType t, uint64_t imm) {
    int v = ins_before_term(fn, b, IR_CONST, t);
    fn->ins[v].imm = imm;
    return v;
}

static int ins2(IrFunc* fn, int b, IrOp op, IrType t, int x, int y) {
    int v = ir_ins_new(fn, b, op, t);
    ir_arg_add(fn, v, x);
    if (y >= 0) ir_arg_add(fn, v, y);
    return v;
}

// v as used from the preheader: itself, or a copy of the body's constant or global.
static int vec_scalar(IrFunc* fn, const VecLoop* L, int* pre_of, int v) {
    if (vec_at(L, fn->ins[v].block) < 0) return v;
    if (pre_of[v] < 0) {
        int c = ins_before_term(fn, L->pre, (IrOp)fn->ins[v].op, (IrType)fn->ins[v].type);
        fn->ins[c].imm = fn->ins[v].imm;
        fn->ins[c].sym = fn->ins[v].sym;
        fn->ins[c].name = fn->ins[v].name;
        pre_of[v] = c;
    }
    return pre_of[v];
}

// Lane value v in the vector loop: its clone, or an invariant splat in the preheader.
static int vec_value(IrFunc* fn, const VecLoop* L, int* map, int* pre_of, int* splat, int v) {
    if (map[v] >= 0) return map[v];
    if (splat[v] < 0 && fn->ins[v].op == IR_CONST) // one splat per constant, however many copies inlining made
        for (int c = 0; c < v && splat[v] < 0; c++)
            if (splat[c] >= 0 && fn->ins[c].op == IR_CONST && fn->ins[c].type == fn->ins[v].type &&
                fn->ins[c].imm == fn->ins[v].imm)
                splat[v] = splat[c];
    if (splat[v] < 0) {
        int s = vec_scalar(fn, L, pre_of, v);
        splat[v] = ins_before_term(fn, L->pre, IR_SPLAT, vec_of((IrType)fn->ins[v].type));
        ir_arg_add(fn, splat[v], s);
    }
    return splat[v];
}

// Whether L runs whole vectors only (a constant n, a multiple of 4) and
// nothing past it needs its header but the exit's phis, for i.
static int vec_whole(const IrFunc* fn, const VecLoop* L, int ex) {
    const IrIns* n = &fn->ins[L->n];
    if (n->op != IR_CONST || n->imm % (uint64_t)L->width) return 0;
    for (int b = 0; b < fn->nblocks; b++) {
        if (b == L->h || vec_at(L, b) >= 0) continue;
        const IrBlock* ob = &fn->blocks[b];
        for (int j = 0; j < ob->nins; j++) {
            const IrIns* x = &fn->ins[ob->ins[j]];
            for (int a = 0; a < x->nargs; a++) {
                int v = x->args[a];
                if (fn->ins[v].block != L->h) continue;
                if (!(b == ex && x->op == IR_PHI && ob->preds[a] == L->h && v == L->i)) return 0;
            }
        }
    }
    return 1;
}

static void vec_rewrite(IrFunc* fn, const VecLoop* L) {
    int nold = fn->nins;
    int* map = xalloc((size_t)nold, sizeof(int));
    int* pre_of = xalloc((size_t)nold, sizeof(int));
    int* splat = xalloc((size_t)nold, sizeof(int));
    for (int v = 0; v < nold; v++) map[v] = pre_of[v] = splat[v] = -1;
    int pre = L->pre, h = L->h;
    IrType it = (IrType)fn->ins[L->i].type; // k's type, so lt k, m compares as lt i, n does
    int lg = L->width == 4 ? 2 : 1; // log2 of the lanes a vector holds
    int zero = konst_in(fn, pre, it, 0), one = konst_in(fn, pre, it, 1), shift = konst_in(fn, pre, it, (uint64_t)lg);
    int m;
    if (fn->ins[L->n].op == IR_CONST) {
        m = konst_in(fn, pre, it, fn->ins[L->n].imm >> lg);
    } else { // an i64 n shifts arithmetically: a negative one skips the vector loop
        m = ins_before_term(fn, pre, IR_SHR, it);
        ir_arg_add(fn, m, vec_scalar(fn, L, pre_of, L->n));
        ir_arg_add(fn, m, shift);
    }
    int vh = ir_block_new(fn), vb = ir_block_new(fn), x = ir_block_new(fn);
    IrBlock* pb = &fn->blocks[pre];
    pb->succ[pb->succ[0] == h ? 0 : 1] = vh;
    IrBlock* hb = &fn->blocks[vh];
    hb->preds = xgrow(hb->preds, &hb->cpreds, 1, sizeof(int));
    hb->preds[hb->npreds++] = pre;
    int k = ir_phi_new(fn, vh, it);
    ir_arg_add(fn, k, zero);
    ins2(fn, vh, IR_BR, IT_VOID, ins2(fn, vh, IR_LT, IT_BOOL, k, m), -1);
    ir_edge_add(fn, vh, vb);
    ir_edge_add(fn, vh, x);
    int kx = it == IT_U64 ? k : ins2(fn, vb, IR_CAST, IT_U64, k, -1); // the u64 vector index
    int vi = -1;
    if (L->lane >= 0) { // t's lanes: <0, 1, 2, 3>, then 4 more each vector
        vi = ir_phi_new(fn, vh, IT_V4I32);
        ir_arg_add(fn, vi, ins_before_term(fn, pre, IR_LANES, IT_V4I32));
        map[L->lane] = vi;
    }
    int F = L->fields;
    uint8_t* keep = NULL; // for F > 1, field 0's computation
    if (F > 1) {
        keep = xalloc((size_t)nold, 1);
        int* stack = xalloc((size_t)nold, sizeof(int));
        int sp = 0;
        IrBlock* bb = &fn->blocks[L->blk[0]];
        for (int j = 0; j < bb->nins; j++) {
            int u = bb->ins[j];
            if (fn->ins[u].op == IR_STORE && fn->ins[u].aux == 0) { keep[u] = 1; stack[sp++] = u; }
        }
        while (sp) {
            IrIns* y = &fn->ins[stack[--sp]];
            for (int a = 0; a < y->nargs; a++) {
                int w = y->args[a];
                if (!keep[w] && vec_lane(fn, L, w)) { keep[w] = 1; stack[sp++] = w; }
            }
        }
        free(stack);
    }
    for (int j = 0; j < L->np; j++) { // a pointer's struct group: its entry value plus k * 16F
        int p = L->ptr[j];
        map[p] = ins2(fn, vb, IR_PADD, IT_PTR, vec_scalar(fn, L, pre_of, fn->ins[p].args[L->pi]), kx);
        fn->ins[map[p]].imm = 16 * (uint64_t)F;
    }
    for (int c = 0; c < F; c++) // F > 1: field 0's computation for each 16 bytes of the group
    for (int bk = 0; bk < L->nb; bk++)
    for (int j = 0; j < fn->blocks[L->blk[bk]].nins; j++) {
        int u = fn->blocks[L->blk[bk]].ins[j];
        IrIns x0 = fn->ins[u]; // fn->ins grows below
        if (u == L->step || vec_pstep(L, u) || x0.op == IR_JMP || x0.op == IR_BR || x0.op == IR_CONST ||
            x0.op == IR_GLOBAL || x0.op == IR_CAST || (keep && x0.op != IR_PADD && !keep[u]) || (c && x0.op == IR_PADD))
            continue;
        if (x0.op == IR_PHI) { // a join's: a select on the fork's mask
            IrBlock* jb = &fn->blocks[x0.block];
            int p = jb->preds[0], c = vec_at(L, p) >= 0 && L->arm[vec_at(L, p)] ? fn->blocks[p].preds[0] : p;
            int first = p == c ? fn->blocks[c].succ[0] == x0.block : fn->blocks[c].succ[0] == p; // arg 0 on true
            int mask = map[fn->ins[fn->blocks[c].ins[fn->blocks[c].nins - 1]].args[0]];
            int a = vec_value(fn, L, map, pre_of, splat, x0.args[first ? 0 : 1]);
            int b = vec_value(fn, L, map, pre_of, splat, x0.args[first ? 1 : 0]);
            map[u] = ins2(fn, vb, IR_SELECT, vec_of((IrType)x0.type), mask, a);
            ir_arg_add(fn, map[u], b);
        } else if (x0.type == IT_BOOL) { // a compare, to a mask
            int a = vec_value(fn, L, map, pre_of, splat, x0.args[0]);
            map[u] = ins2(fn, vb, (IrOp)x0.op, IT_V4I32, a, vec_value(fn, L, map, pre_of, splat, x0.args[1]));
        } else if (x0.op == IR_SHL || x0.op == IR_SHR) { // by the constant itself
            int a = vec_value(fn, L, map, pre_of, splat, x0.args[0]);
            map[u] = ins2(fn, vb, (IrOp)x0.op, IT_V4I32, a, vec_scalar(fn, L, pre_of, x0.args[1]));
        } else if (x0.op == IR_PADD) {
            map[u] = ins2(fn, vb, IR_PADD, IT_PTR, vec_scalar(fn, L, pre_of, x0.args[0]), kx);
            fn->ins[map[u]].imm = 16 * (uint64_t)F;
        } else if (x0.op == IR_LOAD) {
            map[u] = ins2(fn, vb, IR_LOAD, vec_of((IrType)x0.type), map[x0.args[0]], -1);
            fn->ins[map[u]].aux = x0.aux + 16 * c;
        } else if (x0.op == IR_STORE) {
            int s = ins2(fn, vb, IR_STORE, IT_VOID, map[x0.args[0]],
                         vec_value(fn, L, map, pre_of, splat, x0.args[1]));
            fn->ins[s].aux = x0.aux + 16 * c;
        } else {
            int a = vec_value(fn, L, map, pre_of, splat, x0.args[0]);
            int c = x0.nargs > 1 ? vec_value(fn, L, map, pre_of, splat, x0.args[1]) : -1;
            map[u] = ins2(fn, vb, (IrOp)x0.op, vec_of((IrType)x0.type), a, c);
        }
    }
    if (vi >= 0) {
        int c4 = konst_in(fn, pre, IT_I32, 4), four = ins_before_term(fn, pre, IR_SPLAT, IT_V4I32);
        ir_arg_add(fn, four, c4);
        ir_arg_add(fn, vi, ins2(fn, vb, IR_ADD, IT_V4I32, vi, four));
    }
    ir_arg_add(fn, k, ins2(fn, vb, IR_ADD, it, k, one));
    ir_ins_new(fn, vb, IR_JMP, IT_VOID);
    ir_edge_add(fn, vb, vh);
    int i0 = ins2(fn, x, IR_SHL, it, k, shift), ex = fn->blocks[h].succ[1];
    if (L->np) { // the scalar loop's pointers resume at struct i0
        int i64 = it == IT_U64 ? i0 : ins2(fn, x, IR_CAST, IT_U64, i0, -1);
        for (int j = 0; j < L->np; j++) {
            IrIns* px = &fn->ins[L->ptr[j]];
            int r = ins2(fn, x, IR_PADD, IT_PTR, vec_scalar(fn, L, pre_of, px->args[L->pi]), i64);
            fn->ins[r].imm = (uint64_t)L->stride;
            fn->ins[L->ptr[j]].args[L->pi] = r;
        }
    }
    fn->blocks[x].succ[0] = h;
    if (vec_whole(fn, L, ex)) { // no remainder: x leaves on a false branch, which cleanup folds with the scalar loop
        int no = ir_ins_new(fn, x, IR_CONST, IT_BOOL);
        ins2(fn, x, IR_BR, IT_VOID, no, -1);
        int hp = 0;
        while (fn->blocks[ex].preds[hp] != h) hp++;
        ir_edge_add(fn, x, ex);
        for (int j = 0; j < fn->blocks[ex].nins && fn->ins[fn->blocks[ex].ins[j]].op == IR_PHI; j++) {
            int ph = fn->blocks[ex].ins[j], a = fn->ins[ph].args[hp];
            ir_arg_add(fn, ph, a == L->i ? i0 : a);
        }
    } else {
        ir_ins_new(fn, x, IR_JMP, IT_VOID);
    }
    fn->blocks[h].preds[L->pi] = x;
    fn->ins[L->i].args[L->pi] = i0;
    free(map); free(pre_of); free(splat); free(keep);
}

// Returns whether any loop gained a vector loop.
static int vectorize(IrFunc* fn) {
    int n = fn->nblocks;
    VecLoop* loops = xalloc((size_t)n, sizeof(VecLoop));
    int nloops = 0;
    for (int h = 0; h < n; h++)
        if (vec_shape(fn, h, &loops[nloops]) && vec_body(fn, &loops[nloops])) nloops++;
    for (int i = 0; i < nloops; i++) vec_rewrite(fn, &loops[i]);
    free(loops);
    return nloops > 0;
}

// --- dead-code and control-flow cleanup -----------------------------------------------

typedef struct {
    IrFunc*  fn;
    uint8_t* dead; // per block: removed, awaiting compaction
    int*     repl; // per value: the value replacing it, or -1
    int      nrepl;
} Clean;

static int chase(const Clean* c, int v) {
    while (c->repl[v] >= 0) v = c->repl[v];
    return v;
}

static void apply_repl(Clean* c) {
    IrFunc* fn = c->fn;
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            for (int a = 0; a < x->nargs; a++) x->args[a] = chase(c, x->args[a]);
        }
}

// Remove one predecessor entry of t from `from`, with its operand in each phi.
static void drop_pred(IrFunc* fn, int t, int from) {
    IrBlock* tb = &fn->blocks[t];
    int i = 0;
    while (i < tb->npreds && tb->preds[i] != from) i++;
    if (i == tb->npreds) return;
    for (int k = 0; k < tb->nins && fn->ins[tb->ins[k]].op == IR_PHI; k++) {
        IrIns* p = &fn->ins[tb->ins[k]];
        memmove(p->args + i, p->args + i + 1, (size_t)(p->nargs - i - 1) * sizeof(int));
        p->nargs--;
    }
    memmove(tb->preds + i, tb->preds + i + 1, (size_t)(tb->npreds - i - 1) * sizeof(int));
    tb->npreds--;
}

static void kill_block(Clean* c, int b) {
    IrFunc* fn = c->fn;
    IrBlock* bb = &fn->blocks[b];
    for (int i = 0; i < bb->nins; i++) kill_ins(fn, bb->ins[i]);
    bb->nins = bb->npreds = 0;
    bb->succ[0] = bb->succ[1] = -1;
    c->dead[b] = 1;
}

static int drop_unreachable(Clean* c) {
    IrFunc* fn = c->fn;
    uint8_t* seen = xalloc((size_t)fn->nblocks, 1);
    int* stack = xalloc((size_t)fn->nblocks, sizeof(int));
    int n = 0, changed = 0;
    stack[n++] = 0;
    seen[0] = 1;
    while (n) {
        IrBlock* bb = &fn->blocks[stack[--n]];
        for (int k = 0; k < 2; k++)
            if (bb->succ[k] >= 0 && !seen[bb->succ[k]]) { seen[bb->succ[k]] = 1; stack[n++] = bb->succ[k]; }
    }
    for (int b = 0; b < fn->nblocks; b++) {
        if (seen[b] || c->dead[b]) continue;
        for (int k = 0; k < 2; k++) {
            int t = fn->blocks[b].succ[k];
            if (t >= 0 && !c->dead[t]) drop_pred(fn, t, b);
        }
        kill_block(c, b);
        changed = 1;
    }
    free(seen);
    free(stack);
    return changed;
}

// A branch on a constant, or to one block on both arms with equal phi
// operands, becomes a jump.
static int fold_branches(Clean* c) {
    IrFunc* fn = c->fn;
    int changed = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        if (c->dead[b]) continue;
        IrBlock* bb = &fn->blocks[b];
        IrIns* t = &fn->ins[bb->ins[bb->nins - 1]];
        if (t->op != IR_BR) continue;
        IrIns* cond = &fn->ins[t->args[0]];
        int keep = -1;
        uint64_t r;
        if (cond->op == IR_CONST) keep = cond->imm ? 0 : 1;
        else if (cond->nargs == 2 && fn->ins[cond->args[0]].op == IR_CONST && fn->ins[cond->args[1]].op == IR_CONST &&
                 is_int((IrType)fn->ins[cond->args[0]].type) &&
                 fold((IrOp)cond->op, (IrType)cond->type, (IrType)fn->ins[cond->args[0]].type,
                      fn->ins[cond->args[0]].imm, fn->ins[cond->args[1]].imm, &r))
            keep = r ? 0 : 1; // a comparison a threaded jump left constant
        else if (bb->succ[0] == bb->succ[1]) {
            IrBlock* sb = &fn->blocks[bb->succ[0]];
            int same = 1, i0 = -1, i1 = -1;
            for (int i = 0; i < sb->npreds; i++)
                if (sb->preds[i] == b) { if (i0 < 0) i0 = i; else i1 = i; }
            for (int k = 0; k < sb->nins && fn->ins[sb->ins[k]].op == IR_PHI; k++)
                same &= fn->ins[sb->ins[k]].args[i0] == fn->ins[sb->ins[k]].args[i1];
            if (same) keep = 0;
        }
        if (keep < 0) continue;
        drop_pred(fn, bb->succ[1 - keep], b);
        bb->succ[0] = bb->succ[keep];
        bb->succ[1] = -1;
        free(t->args);
        t->args = NULL;
        t->nargs = t->cargs = 0;
        t->op = IR_JMP;
        changed = 1;
    }
    return changed;
}

static int trivial_phis(Clean* c) {
    IrFunc* fn = c->fn;
    int changed = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        int k = 0;
        for (int i = 0; i < bb->nins; i++) {
            int v = bb->ins[i];
            IrIns* x = &fn->ins[v];
            if (x->op == IR_PHI) {
                int w = -1, trivial = 1;
                for (int a = 0; a < x->nargs && trivial; a++) {
                    int u = chase(c, x->args[a]);
                    if (u == v || u == w) continue;
                    if (w >= 0) trivial = 0;
                    else w = u;
                }
                if (trivial && w >= 0) {
                    c->repl[v] = w;
                    kill_ins(fn, v);
                    changed = 1;
                    continue;
                }
            }
            bb->ins[k++] = v;
        }
        bb->nins = k;
    }
    if (changed) apply_repl(c);
    return changed;
}

// A block that ends in a jump to a block with no other predecessor absorbs it.
static int merge_blocks(Clean* c) {
    IrFunc* fn = c->fn;
    int changed = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        if (c->dead[b]) continue;
        IrBlock* bb = &fn->blocks[b];
        for (;;) {
            int tid = bb->ins[bb->nins - 1];
            if (fn->ins[tid].op != IR_JMP) break;
            int s = bb->succ[0];
            IrBlock* sb = &fn->blocks[s];
            if (s == b || sb->npreds != 1) break;
            int k = 0;
            while (k < sb->nins && fn->ins[sb->ins[k]].op == IR_PHI) { // one operand: the value itself
                c->repl[sb->ins[k]] = fn->ins[sb->ins[k]].args[0];
                kill_ins(fn, sb->ins[k]);
                k++;
            }
            kill_ins(fn, tid);
            bb->nins--;
            bb->ins = xgrow(bb->ins, &bb->cins, bb->nins + sb->nins - k, sizeof(int));
            for (int i = k; i < sb->nins; i++) {
                fn->ins[sb->ins[i]].block = b;
                bb->ins[bb->nins++] = sb->ins[i];
            }
            bb->succ[0] = sb->succ[0];
            bb->succ[1] = sb->succ[1];
            for (int q = 0; q < 2; q++) {
                if (sb->succ[q] < 0) continue;
                IrBlock* tb = &fn->blocks[sb->succ[q]];
                for (int i = 0; i < tb->npreds; i++) if (tb->preds[i] == s) tb->preds[i] = b;
            }
            sb->nins = sb->npreds = 0;
            sb->succ[0] = sb->succ[1] = -1;
            c->dead[s] = 1;
            changed = 1;
        }
    }
    if (changed) apply_repl(c);
    return changed;
}

// A block holding only a jump is skipped: each predecessor goes straight to
// its target, when none already does (two edges from one block to another
// would need distinct phi operands the IR does not carry).
static int thread_jumps(Clean* c) {
    IrFunc* fn = c->fn;
    int changed = 0;
    for (int e = 1; e < fn->nblocks; e++) {
        IrBlock* eb = &fn->blocks[e];
        if (c->dead[e] || eb->nins != 1 || fn->ins[eb->ins[0]].op != IR_JMP || !eb->npreds) continue;
        int t = eb->succ[0];
        if (t == e) continue;
        IrBlock* tb = &fn->blocks[t];
        int ok = 1;
        for (int i = 0; i < eb->npreds && ok; i++) {
            int p = eb->preds[i];
            ok = p != e;
            for (int j = 0; j < i; j++) ok &= eb->preds[j] != p;
            for (int j = 0; j < tb->npreds; j++) ok &= tb->preds[j] != p;
        }
        if (!ok) continue;
        int idx = 0;
        while (tb->preds[idx] != e) idx++;
        int np = eb->npreds;
        int* ps = xalloc((size_t)np, sizeof(int));
        memcpy(ps, eb->preds, (size_t)np * sizeof(int));
        tb->preds[idx] = ps[0];
        tb->preds = xgrow(tb->preds, &tb->cpreds, tb->npreds + np - 1, sizeof(int));
        for (int i = 1; i < np; i++) {
            tb->preds[tb->npreds++] = ps[i];
            for (int k = 0; k < tb->nins && fn->ins[tb->ins[k]].op == IR_PHI; k++) {
                IrIns* ph = &fn->ins[tb->ins[k]];
                ph->args = xgrow(ph->args, &ph->cargs, ph->nargs + 1, sizeof(int));
                ph->args[ph->nargs++] = ph->args[idx];
            }
        }
        for (int i = 0; i < np; i++)
            for (int q = 0; q < 2; q++)
                if (fn->blocks[ps[i]].succ[q] == e) fn->blocks[ps[i]].succ[q] = t;
        free(ps);
        kill_block(c, e);
        changed = 1;
    }
    return changed;
}

// The block owning a use: an instruction's own, or for a phi operand the
// predecessor it arrives from.
static int use_owner(const IrFunc* fn, const IrIns* x, int a) {
    return x->op == IR_PHI ? fn->blocks[x->block].preds[a] : x->block;
}

// Whether every use of j's phi v outside j is owned by s, or by a block t
// dominates (t >= 0: j's other successor, which only j enters, so every path
// there still passes j).
static int phi_uses_in(const IrFunc* fn, const int* idom, int v, int j, int s, int t) {
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            const IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            for (int a = 0; a < x->nargs; a++) {
                if (x->args[a] != v) continue;
                int o = use_owner(fn, x, a);
                if (o != j && o != s && (t < 0 || (o != t && idom[o] < 0) || !ir_dominates(idom, t, o))) return 0;
            }
        }
    return 1;
}

// operand a of j's branch condition as it arrives from predecessor i: a
// constant's value, or 0 when it is not constant there
static int branch_operand(const IrFunc* fn, int j, int i, int a, uint64_t* out) {
    const IrIns* x = &fn->ins[a];
    if (x->op == IR_PHI && x->block == j) x = &fn->ins[x->args[i]];
    if (x->op != IR_CONST || !is_int((IrType)x->type)) return 0;
    *out = x->imm;
    return 1;
}

// A jump from p into block j whose branch condition is constant along that
// edge goes straight to the successor it selects, s. j holds only phis,
// constants, and the condition, a phi or an integer operation over its phis
// and constants, nothing of which but the phis is used outside j, and j heads
// no loop, so the edge enters no loop sideways. When s has j as its only
// predecessor and no phis, a phi of j used in s gets a phi in s joining it
// with its operand from p. Otherwise p must not already enter s, no phi of j
// is used in s, and each phi of s takes from p what it takes from j, through
// j's phi for p when that is the operand.
static int thread_branches(Clean* c) {
    IrFunc* fn = c->fn;
    int changed = 0;
    int* idom = NULL;
    int* rpo = NULL;
    int nrpo = 0;
    char* bad = NULL;
    for (int j = 1; j < fn->nblocks; j++) {
        if (c->dead[j]) continue;
        IrBlock* jb = &fn->blocks[j];
        IrIns* t = &fn->ins[jb->ins[jb->nins - 1]];
        if (t->op != IR_BR) continue;
        int cond = t->args[0], ok = 1, k = 0;
        while (k < jb->nins && fn->ins[jb->ins[k]].op == IR_PHI) k++;
        for (int i = k; i < jb->nins - 1 && ok; i++) {
            int v = jb->ins[i];
            ok = fn->ins[v].op == IR_CONST || v == cond;
        }
        IrIns* cx = &fn->ins[cond];
        if (!ok || (cx->block == j && cx->op != IR_PHI && cx->op != IR_CONST &&
                    (cx->nargs != 2 || !is_int((IrType)fn->ins[cx->args[0]].type))))
            continue;
        if (cx->block != j) continue;
        if (!bad) { // bad[b]: a non-phi value of b is used outside b
            bad = xalloc((size_t)fn->nblocks, sizeof(char));
            for (int b = 0; b < fn->nblocks; b++)
                for (int i = 0; i < fn->blocks[b].nins; i++) {
                    const IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
                    for (int a = 0; a < x->nargs; a++) {
                        const IrIns* y = &fn->ins[x->args[a]];
                        if (y->block >= 0 && y->block != b && y->op != IR_PHI) bad[y->block] = 1;
                    }
                }
        }
        if (bad[j]) continue;
        for (int i = 0; i < jb->npreds; i++) {
            int p = jb->preds[i];
            if (fn->blocks[p].succ[1] >= 0 || fn->ins[fn->blocks[p].ins[fn->blocks[p].nins - 1]].op != IR_JMP) continue;
            uint64_t x, y = 0, r;
            if (!branch_operand(fn, j, i, cx->op == IR_PHI || cx->op == IR_CONST ? cond : cx->args[0], &x)) continue;
            if (cx->op == IR_PHI || cx->op == IR_CONST) r = x != 0;
            else if (!branch_operand(fn, j, i, cx->args[1], &y) ||
                     !fold((IrOp)cx->op, (IrType)cx->type, (IrType)fn->ins[cx->args[0]].type, x, y, &r))
                continue;
            int s = jb->succ[r ? 0 : 1];
            IrBlock* sb = &fn->blocks[s];
            if (s == j) continue;
            int joins = sb->npreds != 1 || (sb->nins && fn->ins[sb->ins[0]].op == IR_PHI), dup = 0;
            for (int q = 0; q < sb->npreds; q++) dup |= sb->preds[q] == p;
            if (dup) continue;
            if (!idom) idom = ir_dominators(fn, &rpo, &nrpo);
            int t = jb->succ[r ? 1 : 0], fine = 1;
            if (t == s || fn->blocks[t].npreds != 1) t = -1;
            for (int q = 0; q < k && fine; q++) fine = phi_uses_in(fn, idom, jb->ins[q], j, joins ? j : s, t);
            if (!fine) continue;
            int header = 0;
            for (int q = 0; q < jb->npreds && !header; q++) header = idom[jb->preds[q]] >= 0 && ir_dominates(idom, j, jb->preds[q]);
            if (header) break;
            if (joins) { // each phi of s takes from p what it takes from j
                int js = 0;
                while (sb->preds[js] != j) js++;
                for (int q = 0; q < sb->nins && fn->ins[sb->ins[q]].op == IR_PHI; q++) {
                    int a = fn->ins[sb->ins[q]].args[js];
                    if (fn->ins[a].op == IR_PHI && fn->ins[a].block == j) a = fn->ins[a].args[i];
                    ir_arg_add(fn, sb->ins[q], a);
                }
            }
            for (int q = 0; q < k && !joins; q++) { // s's phi for each of j's it uses
                int v = fn->blocks[j].ins[q], w = -1;
                for (int b = 0; b < fn->nblocks; b++)
                    for (int u = 0; u < fn->blocks[b].nins; u++) {
                        int id = fn->blocks[b].ins[u];
                        for (int a = 0; a < fn->ins[id].nargs; a++) {
                            if (fn->ins[id].args[a] != v || id == w || use_owner(fn, &fn->ins[id], a) != s) continue;
                            if (w < 0) {
                                w = ir_phi_new(fn, s, (IrType)fn->ins[v].type);
                                ir_arg_add(fn, w, v);
                                ir_arg_add(fn, w, fn->ins[v].args[i]);
                                c->repl = xgrow(c->repl, &c->nrepl, fn->nins, sizeof(int));
                                c->repl[w] = -1;
                            }
                            fn->ins[id].args[a] = w;
                        }
                    }
            }
            sb = &fn->blocks[s];
            sb->preds = xgrow(sb->preds, &sb->cpreds, sb->npreds + 1, sizeof(int));
            sb->preds[sb->npreds++] = p;
            fn->blocks[p].succ[0] = s;
            drop_pred(fn, j, p);
            free(idom); free(rpo);
            idom = rpo = NULL;
            free(bad);
            bad = NULL;
            changed = 1;
            jb = &fn->blocks[j];
            cx = &fn->ins[cond]; // ir_phi_new may have moved fn->ins
            i--; // preds shifted down
        }
    }
    free(idom); free(rpo); free(bad);
    return changed;
}

static int has_effect(const IrIns* x) {
    switch (x->op) {
    case IR_STORE: case IR_CALL: case IR_CALLI: case IR_RTCALL: case IR_CHECK: case IR_PROJ: case IR_ASM:
    case IR_ATOMIC: case IR_PARAM: case IR_RESULT: case IR_JMP: case IR_BR: case IR_RET:
        return 1;
    case IR_LOAD: return (x->flags & IRF_VOLATILE) != 0;
    default: return 0;
    }
}

static int dce(Clean* c) {
    IrFunc* fn = c->fn;
    uint8_t* live = xalloc((size_t)fn->nins, 1);
    int* stack = xalloc((size_t)fn->nins, sizeof(int));
    int n = 0;
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int v = fn->blocks[b].ins[i];
            if (has_effect(&fn->ins[v])) { live[v] = 1; stack[n++] = v; }
        }
    while (n) {
        IrIns* x = &fn->ins[stack[--n]];
        for (int a = 0; a < x->nargs; a++)
            if (!live[x->args[a]]) { live[x->args[a]] = 1; stack[n++] = x->args[a]; }
    }
    int changed = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        int k = 0;
        for (int i = 0; i < bb->nins; i++) {
            int v = bb->ins[i];
            if (live[v]) { bb->ins[k++] = v; continue; }
            kill_ins(fn, v);
            changed = 1;
        }
        bb->nins = k;
    }
    free(live);
    free(stack);
    return changed;
}

// Renumber the blocks without the dead ones.
static void compact(Clean* c) {
    IrFunc* fn = c->fn;
    int* map = xalloc((size_t)fn->nblocks, sizeof(int));
    int n = 0;
    for (int b = 0; b < fn->nblocks; b++) map[b] = c->dead[b] ? -1 : n++;
    if (n == fn->nblocks) { free(map); return; }
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        if (map[b] < 0) {
            free(bb->ins);
            free(bb->preds);
            continue;
        }
        for (int i = 0; i < bb->npreds; i++) bb->preds[i] = map[bb->preds[i]];
        for (int k = 0; k < 2; k++) if (bb->succ[k] >= 0) bb->succ[k] = map[bb->succ[k]];
        for (int i = 0; i < bb->nins; i++) fn->ins[bb->ins[i]].block = map[b];
        fn->blocks[map[b]] = *bb;
    }
    fn->nblocks = n;
    memset(c->dead, 0, (size_t)n);
    free(map);
}

static void cleanup(IrFunc* fn) {
    Clean c;
    c.fn = fn;
    c.dead = xalloc((size_t)fn->nblocks, 1);
    c.repl = xalloc((size_t)fn->nins, sizeof(int));
    c.nrepl = fn->nins;
    for (int v = 0; v < fn->nins; v++) c.repl[v] = -1;
    for (int changed = 1; changed;) {
        changed = fold_branches(&c);
        changed |= drop_unreachable(&c);
        changed |= trivial_phis(&c);
        changed |= merge_blocks(&c);
        changed |= thread_jumps(&c);
        changed |= thread_branches(&c);
        changed |= dce(&c);
        compact(&c);
    }
    free(c.dead);
    free(c.repl);
}

void ir_optimize(IrFunc* fn) {
    sccp(fn);
    gvn(fn);
    licm(fn);
    lsr(fn);
    cleanup(fn);
    if (vectorize(fn)) cleanup(fn); // over the merged blocks, which it then tidies
}
