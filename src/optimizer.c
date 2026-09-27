// optimizer.c — pass 6, the release-only optimizer, over one function's IR
// (SPEC.md Decision Queue, "Per-function SSA IR backend"). Each pass runs
// once, in the order the SPEC gives: sparse conditional constant propagation,
// global value numbering, loop strength reduction, and dead-code and
// control-flow cleanup.
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
    case IR_NEG: case IR_NOT:
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
    case IR_POPCNT: case IR_CLZ: case IR_CTZ: case IR_BSWAP: case IR_ROTL: case IR_ROTR:
    case IR_SQRT: case IR_FLOOR: case IR_CEIL: case IR_FMA:
    case IR_PADD: case IR_PSUB:
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
    case IR_ADD: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR: case IR_EQ: case IR_NE:
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
    if (commutes(fn, x)) return mix(h, arg_hash(fn, x->args[0]) + arg_hash(fn, x->args[1]));
    for (int i = 0; i < x->nargs; i++) h = mix(h, arg_hash(fn, x->args[i]));
    return h;
}

static int vn_equal(const IrFunc* fn, const IrIns* x, const IrIns* y) {
    if (x->op != y->op || x->type != y->type || x->imm != y->imm || x->aux != y->aux || x->nargs != y->nargs) return 0;
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

// A new instruction in b, before its terminator.
static int ins_before_term(IrFunc* fn, int b, IrOp op, IrType type) {
    int v = ir_ins_new(fn, b, op, type);
    IrBlock* bb = &fn->blocks[b];
    bb->ins[bb->nins - 1] = bb->ins[bb->nins - 2];
    bb->ins[bb->nins - 2] = v;
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
    return x->op == IR_GLOBAL && y->op == IR_GLOBAL && x->sym == y->sym &&
           (x->name == y->name || (x->name && y->name && !strcmp(x->name, y->name)));
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

static void lsr(IrFunc* fn) {
    int n = fn->nblocks, nins = fn->nins, any = 0;
    for (int v = 0; v < nins && !any; v++) {
        const IrIns* x = &fn->ins[v];
        any = (x->op == IR_PADD || x->op == IR_PSUB) && x->block >= 0 && !x->flags && (x->imm & (x->imm - 1));
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
        if (hb->npreds != 2) continue;
        int li = -1;
        for (int i = 0; i < 2; i++)
            if (idom[hb->preds[i]] >= 0 && ir_dominates(idom, h, hb->preds[i])) li = li < 0 ? i : -2;
        if (li < 0) continue;
        int pi = 1 - li, latch = hb->preds[li], pre = hb->preds[pi];
        if (idom[pre] < 0 || ir_dominates(idom, h, pre)) continue;
        memset(body, 0, (size_t)n); // the natural loop: every block reaching the latch without passing h
        int sp = 0;
        body[h] = 1;
        if (!body[latch]) { body[latch] = 1; stack[sp++] = latch; }
        while (sp) {
            IrBlock* bb = &fn->blocks[stack[--sp]];
            for (int i = 0; i < bb->npreds; i++)
                if (!body[bb->preds[i]]) { body[bb->preds[i]] = 1; stack[sp++] = bb->preds[i]; }
        }
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
                    if ((x->op != IR_PADD && x->op != IR_PSUB) || x->flags || lsr_index(fn, x->args[1]) != phis[k] || u >= nins ||
                        !(x->imm & (x->imm - 1)))
                        continue;
                    int q = lsr_pointer(fn, idom, repl, nins, &ptrs, &nptr, &cptr, h, pre, latch, pi, li, phis[k], u);
                    if (q < 0) continue;
                    repl[u] = q;
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

// --- dead-code and control-flow cleanup -----------------------------------------------

typedef struct {
    IrFunc*  fn;
    uint8_t* dead; // per block: removed, awaiting compaction
    int*     repl; // per value: the value replacing it, or -1
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
        if (cond->op == IR_CONST) keep = cond->imm ? 0 : 1;
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

static int has_effect(const IrIns* x) {
    switch (x->op) {
    case IR_STORE: case IR_CALL: case IR_CALLI: case IR_RTCALL: case IR_CHECK: case IR_PROJ: case IR_ASM:
    case IR_PARAM: case IR_RESULT: case IR_JMP: case IR_BR: case IR_RET:
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
    for (int v = 0; v < fn->nins; v++) c.repl[v] = -1;
    for (int changed = 1; changed;) {
        changed = fold_branches(&c);
        changed |= drop_unreachable(&c);
        changed |= trivial_phis(&c);
        changed |= merge_blocks(&c);
        changed |= thread_jumps(&c);
        changed |= dce(&c);
        compact(&c);
    }
    free(c.dead);
    free(c.repl);
}

void ir_optimize(IrFunc* fn) {
    sccp(fn);
    gvn(fn);
    lsr(fn);
    cleanup(fn);
}
