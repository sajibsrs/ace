// opt.c — pass 6, the release-only optimizer, over one function's IR
// (SPEC.md Decision Queue, "Per-function SSA IR backend"). Each pass runs
// once, in the order the SPEC gives; those present so far are sparse
// conditional constant propagation and dead-code and control-flow cleanup.
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
    case IR_STORE: case IR_CALL: case IR_CALLI: case IR_RTCALL: case IR_CHECK: case IR_PROJ:
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
    cleanup(fn);
}
