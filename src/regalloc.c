// regalloc.c — the target-neutral half of register allocation (regalloc.h):
// block order, use counts, liveness, location sharing, and the linear scan
// (Poletto & Sarkar) over the classes a target describes. Each value has one
// interval, the hull of the positions where it is live in block layout order
// (a reverse postorder), found by walking up from each use to the definition.
// Constants and stack objects take no register: each use rebuilds them. A
// parameter or result address is live from entry, where both targets move
// them, and is allocated before any other value. An
// interval that crosses a call takes a callee-saved register or a stack slot;
// hints steer a phi and its operands to one register.
#include "regalloc.h"
#include "backend.h"

static void* xcalloc(size_t n, size_t size) {
    void* p = calloc(n ? n : 1, size);
    if (!p) oom();
    return p;
}

// Split each edge from a two-way branch into a block with phis.
void ra_split_critical_edges(IrFunc* fn) {
    int n = fn->nblocks;
    for (int b = 0; b < n; b++) {
        for (int k = 0; k < 2; k++) {
            IrBlock* bb = &fn->blocks[b];
            int s = bb->succ[k];
            if (s < 0 || bb->succ[1 - k] < 0) continue;
            IrBlock* sb = &fn->blocks[s];
            if (sb->npreds < 2 || fn->ins[sb->ins[0]].op != IR_PHI) continue;
            Node* at = fn->ins[bb->ins[bb->nins - 1]].at;
            int mid = ir_block_new(fn);
            int j = ir_ins_new(fn, mid, IR_JMP, IT_VOID);
            fn->ins[j].at = at;
            IrBlock* mb = &fn->blocks[mid];
            mb->succ[0] = s;
            mb->preds = malloc(sizeof(int));
            if (!mb->preds) oom();
            mb->preds[0] = b;
            mb->npreds = mb->cpreds = 1;
            fn->blocks[b].succ[k] = mid;
            sb = &fn->blocks[s];
            for (int p = 0; p < sb->npreds; p++) // one edge: the first entry still from b
                if (sb->preds[p] == b) { sb->preds[p] = mid; break; }
        }
    }
}

// A loop whose header only tests (a two-way branch with one successor in the
// loop, one outside) and whose one back edge is a plain jump: cloning the
// reverse postorder and moving such a header after its latch, so an iteration
// ends in the test's own conditional branch to the body instead of a jump to
// the test, then a jump to the body once at entry. This only reorders the
// blocks emission walks (a->layout); analysis (a->order, a->bfirst, a->idx,
// liveness, allocation, coalescing) keeps every value's position as if
// nothing moved, so none of their ordering assumptions change.
static void emission_order(Ra* a) {
    IrFunc* fn = a->fn;
    int n = fn->nblocks;
    a->layout = xcalloc((size_t)n, sizeof(int));
    a->eix = xcalloc((size_t)n, sizeof(int));
    memcpy(a->layout, a->order, (size_t)n * sizeof(int));
    int* pos = xcalloc((size_t)n, sizeof(int));
    int* after = xcalloc((size_t)n, sizeof(int)); // latch -> its deferred header, or -1
    uint8_t* deferred = xcalloc((size_t)n, 1);
    uint8_t* in = xcalloc((size_t)n, 1);
    int* stack = xcalloc((size_t)n, sizeof(int));
    int moved = 0;
    for (int b = 0; b < n; b++) after[b] = -1;
    for (int i = 0; i < n; i++) pos[a->layout[i]] = i;
    for (int i = 1; i < n; i++) { // the entry stays first
        int h = a->layout[i];
        IrBlock* hb = &fn->blocks[h];
        if (hb->succ[1] < 0) continue;
        int latch = -1, back = 0;
        for (int p = 0; p < hb->npreds; p++)
            if (pos[hb->preds[p]] >= i) { latch = hb->preds[p]; back++; }
        if (back != 1 || fn->blocks[latch].succ[1] >= 0 || fn->blocks[latch].succ[0] != h || after[latch] >= 0)
            continue;
        memset(in, 0, (size_t)n); // the natural loop: what reaches the latch without passing h
        int sp = 0;
        in[h] = 1;
        in[latch] = 1;
        stack[sp++] = latch;
        while (sp) {
            IrBlock* bb = &fn->blocks[stack[--sp]];
            for (int p = 0; p < bb->npreds; p++)
                if (!in[bb->preds[p]]) {
                    in[bb->preds[p]] = 1;
                    stack[sp++] = bb->preds[p];
                }
        }
        int s0 = hb->succ[0], s1 = hb->succ[1];
        if ((in[s0] && s0 != h) == (in[s1] && s1 != h)) continue;
        after[latch] = h;
        deferred[h] = 1;
        moved++;
    }
    if (moved) {
        int* old = xcalloc((size_t)n, sizeof(int));
        memcpy(old, a->layout, (size_t)n * sizeof(int));
        int k = 0;
        for (int i = 0; i < n; i++) {
            int b = old[i];
            if (deferred[b]) continue;
            a->layout[k++] = b;
            if (after[b] >= 0) a->layout[k++] = after[b];
        }
        free(old);
    }
    // A cold arm: a block making a call to a @cold function whose one
    // predecessor is a two-way branch, with each block reached only from cold
    // blocks, follows every other block in its emission order, so the common
    // path falls through its branch.
    uint8_t* cold = a->cold = xcalloc((size_t)n, 1);
    int ncold = 0;
    for (int i = 1; i < n; i++) { // the entry stays first
        int b = a->layout[i];
        IrBlock* bb = &fn->blocks[b];
        int c = bb->npreds > 0;
        for (int p = 0; p < bb->npreds && c; p++) c = cold[bb->preds[p]];
        if (!c && bb->npreds == 1 && fn->blocks[bb->preds[0]].succ[1] >= 0)
            for (int j = 0; j < bb->nins && !c; j++)
                c = fn->ins[bb->ins[j]].op == IR_CALL && (fn->ins[bb->ins[j]].flags & IRF_COLD);
        cold[b] = (uint8_t)c;
        ncold += c;
    }
    if (ncold) {
        int* old = xcalloc((size_t)n, sizeof(int));
        memcpy(old, a->layout, (size_t)n * sizeof(int));
        int k = 0;
        for (int i = 0; i < n; i++) if (!cold[old[i]]) a->layout[k++] = old[i];
        for (int i = 0; i < n; i++) if (cold[old[i]]) a->layout[k++] = old[i];
        free(old);
    }
    for (int i = 0; i < n; i++) a->eix[a->layout[i]] = i;
    free(pos); free(after); free(deferred); free(in); free(stack);
}

// Reverse postorder, visiting the second successor first so that the first
// (a branch's true side, a loop's body) follows its block.
void ra_layout(Ra* a) {
    IrFunc* fn = a->fn;
    int n = fn->nblocks, npost = 0, sp = 0;
    int* stack = xcalloc((size_t)n * 2, sizeof(int));
    uint8_t* seen = xcalloc((size_t)n, 1);
    a->order = xcalloc((size_t)n, sizeof(int));
    a->lix = xcalloc((size_t)n, sizeof(int));
    stack[sp++] = 0; stack[sp++] = 0;
    seen[0] = 1;
    while (sp) {
        int b = stack[sp - 2], k = stack[sp - 1];
        if (k < 2) {
            stack[sp - 1] = k + 1;
            int s = fn->blocks[b].succ[1 - k];
            if (s >= 0 && !seen[s]) {
                seen[s] = 1;
                stack[sp++] = s; stack[sp++] = 0;
            }
            continue;
        }
        a->order[n - 1 - npost++] = b;
        sp -= 2;
    }
    free(stack);
    free(seen);
    a->bfirst = xcalloc((size_t)n, sizeof(int));
    a->blast = xcalloc((size_t)n, sizeof(int));
    a->idx = xcalloc((size_t)fn->nins, sizeof(int));
    int k = 0;
    for (int i = 0; i < n; i++) {
        int b = a->order[i];
        IrBlock* bb = &fn->blocks[b];
        a->lix[b] = i;
        a->bfirst[b] = k;
        for (int j = 0; j < bb->nins; j++) a->idx[bb->ins[j]] = k++;
        a->blast[b] = k - 1;
    }
    emission_order(a);
}

void ra_count_uses(Ra* a) {
    IrFunc* fn = a->fn;
    a->nuse = xcalloc((size_t)fn->nins, sizeof(int));
    a->fused = xcalloc((size_t)fn->nins, 1);
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            for (int k = 0; k < x->nargs; k++) a->nuse[x->args[k]]++;
        }
}

// Stamp tag on each block where v is live in (never its own block), walking
// up from each use to the definition; returns the furthest position, in
// layout order, where v is used or live out of a block.
static int live_in(Ra* a, int v, int* stamp, int* stack, int tag) {
    IrFunc* fn = a->fn;
    int d = fn->ins[v].block, e = a->start[v];
    for (int i = a->ufirst[v]; i < a->ufirst[v + 1]; i++) {
        RaUse* u = &a->uses[i];
        if (u->pos > e) e = u->pos;
        int b = u->block;
        if (b == d || stamp[b] == tag) continue;
        int sp = 0;
        stamp[b] = tag;
        stack[sp++] = b;
        while (sp) { // live in at b: live out of each predecessor
            IrBlock* bb = &fn->blocks[stack[--sp]];
            for (int p = 0; p < bb->npreds; p++) {
                int q = bb->preds[p];
                if (2 * a->blast[q] + 1 > e) e = 2 * a->blast[q] + 1;
                if (q != d && stamp[q] != tag) {
                    stamp[q] = tag;
                    stack[sp++] = q;
                }
            }
        }
    }
    return e;
}

// The end of each value's interval: the furthest position, in layout order,
// where it is used or live out of a block.
// A parameter or a result address arrives at entry, wherever lowering placed it.
static int entry_value(IrOp op) { return op == IR_PARAM || op == IR_RESULT; }

void ra_liveness(Ra* a) {
    IrFunc* fn = a->fn;
    int nv = fn->nins;
    a->start = xcalloc((size_t)nv, sizeof(int));
    a->end = xcalloc((size_t)nv, sizeof(int));
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int v = fn->blocks[b].ins[i];
            IrIns* x = &fn->ins[v];
            // a proj's value arrives at its call; a parameter or result address, at entry
            a->start[v] = x->op == IR_PHI ? 2 * a->bfirst[b] : x->op == IR_PROJ ? 2 * a->idx[x->args[0]] + 1
                        : entry_value(x->op) ? 1 : 2 * a->idx[v] + 1;
            a->end[v] = a->start[v];
        }
    // uses by value (counting sort), a fused value's operands used at its user
    int nu = 0;
    RaUse* uses = NULL;
    int cu = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        for (int i = 0; i < bb->nins; i++) {
            int u = bb->ins[i];
            IrIns* x = &fn->ins[u];
            if (a->fused[u]) continue;
            for (int k = 0; k < x->nargs; k++) {
                int v = x->args[k];
                int ub = x->op == IR_PHI ? bb->preds[k] : b;
                int pos = x->op == IR_PHI ? 2 * a->blast[ub] : 2 * a->idx[u];
                IrIns* vx = &fn->ins[v];
                int nsub = a->fused[v] ? vx->nargs : 1;
                for (int s = 0; s < nsub; s++) {
                    int w = a->fused[v] ? vx->args[s] : v;
                    if (ra_remat((IrOp)fn->ins[w].op)) continue;
                    if (nu == cu) {
                        cu = cu ? cu * 2 : 256;
                        uses = realloc(uses, (size_t)cu * sizeof(RaUse));
                        if (!uses) oom();
                    }
                    uses[nu++] = (RaUse){ w, ub, pos };
                }
            }
        }
    }
    int* first = xcalloc((size_t)nv + 1, sizeof(int));
    for (int i = 0; i < nu; i++) first[uses[i].v + 1]++;
    for (int v = 0; v < nv; v++) first[v + 1] += first[v];
    RaUse* by = xcalloc((size_t)nu, sizeof(RaUse));
    int* fill = xcalloc((size_t)nv, sizeof(int));
    for (int i = 0; i < nu; i++) by[first[uses[i].v] + fill[uses[i].v]++] = uses[i];
    free(uses);
    free(fill);
    a->ufirst = first;
    a->uses = by;
    int* stamp = xcalloc((size_t)fn->nblocks, sizeof(int));
    int* stack = xcalloc((size_t)fn->nblocks, sizeof(int));
    for (int v = 0; v < nv; v++) a->end[v] = live_in(a, v, stamp, stack, v + 1);
    free(stamp); free(stack);
}

// Does v's interval contain one of the call positions at[0..n) it must survive?
int ra_crosses(Ra* a, int v, const int* at, int n) {
    int lo = 0, hi = n; // first call after v's start
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (at[mid] > a->start[v]) hi = mid;
        else lo = mid + 1;
    }
    return lo < n && a->end[v] > at[lo] + 1;
}


static int allocatable(Ra* a, int v) {
    IrIns* x = &a->fn->ins[v];
    return x->type != IT_VOID && !ra_remat((IrOp)x->op) && !a->fused[v] && a->nuse[v] > 0;
}

void ra_positions(Ra* a) {
    IrFunc* fn = a->fn;
    a->hint = xcalloc((size_t)fn->nins, sizeof(int));
    a->phi_of = xcalloc((size_t)fn->nins, sizeof(int));
    for (int v = 0; v < fn->nins; v++) a->hint[v] = a->phi_of[v] = -1;
    a->calls = xcalloc((size_t)fn->nins, sizeof(int));
    a->rtcalls = xcalloc((size_t)fn->nins, sizeof(int));
    a->asm_pos = xcalloc((size_t)fn->nins, sizeof(int));
    a->asm_mask = xcalloc((size_t)fn->nins, sizeof(uint32_t));
    for (int i = 0; i < fn->nblocks; i++) {
        IrBlock* bb = &fn->blocks[a->order[i]];
        for (int j = 0; j < bb->nins; j++) {
            int u = bb->ins[j];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_PHI) {
                for (int k = 0; k < x->nargs; k++)
                    if (a->phi_of[x->args[k]] < 0) a->phi_of[x->args[k]] = u;
            } else if (x->op == IR_RTCALL) {
                a->rtcalls[a->nrtcalls++] = 2 * a->idx[u];
            } else if (x->op == IR_GLOBAL && x->sym && x->sym->is_thread) {
                a->asm_pos[a->nasms] = 2 * a->idx[u];
                a->asm_mask[a->nasms] = a->thread_reserved;
                a->nasms++;
            } else if (x->op == IR_ASM) {
                a->asm_pos[a->nasms] = 2 * a->idx[u];
                a->asm_mask[a->nasms] = x->asmb->reserved;
                a->nasms++;
                a->asm_all |= x->asmb->reserved;
            } else if (x->op == IR_CALL || x->op == IR_CALLI) {
                a->calls[a->ncalls++] = 2 * a->idx[u];
            }
        }
    }
}

// Share a phi's location with an operand defined after it, typically the
// value a loop carries around its back edge, when their intervals overlap only
// as hulls: the phi is dead from the operand's definition on (neither used
// after it in its block nor live out of it) and the operand is not live into
// the phi's block. The pair is allocated as the phi, over both intervals, so
// the operand is computed where the phi lives and its phi move vanishes.
// The operand may itself be a phi, as where an `if` inside a loop joins: it is
// written by its moves at its predecessors' ends, so the phi must also be
// unread there except as that operand's own argument, whose move then vanishes
// too. A value shares with one phi and leads no pair of its own, so no chain
// forms.
void ra_coalesce(Ra* a, const RaTarget* t) {
    IrFunc* fn = a->fn;
    a->with = xcalloc((size_t)fn->nins, sizeof(int));
    for (int v = 0; v < fn->nins; v++) a->with[v] = -1;
    uint8_t* leads = xcalloc((size_t)fn->nins, 1);
    int* stamp = xcalloc((size_t)fn->nblocks, sizeof(int));
    int* stack = xcalloc((size_t)fn->nblocks, sizeof(int));
    int tag = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        for (int i = 0; i < bb->nins && fn->ins[bb->ins[i]].op == IR_PHI; i++) {
            int p = bb->ins[i];
            if (!allocatable(a, p) || a->with[p] >= 0) continue;
            int shared = 0;
            for (int k = 0; k < fn->ins[p].nargs && !shared; k++) {
                int v = fn->ins[p].args[k];
                if (!allocatable(a, v) || a->with[v] >= 0 || leads[v] ||
                    a->start[v] <= a->start[p] || a->start[v] > a->end[p]) // disjoint: a hint suffices
                    continue;
                int bv = fn->ins[v].block, live = 0;
                live_in(a, v, stamp, stack, ++tag);
                if (stamp[b] == tag) continue;
                if (fn->ins[v].op == IR_PHI) { // read by a sibling phi's move into bv
                    IrBlock* vb = &fn->blocks[bv];
                    for (int j = 0; j < vb->nins && fn->ins[vb->ins[j]].op == IR_PHI && !live; j++) {
                        IrIns* w = &fn->ins[vb->ins[j]];
                        for (int q = 0; q < w->nargs && !live && vb->ins[j] != v; q++) live = w->args[q] == p;
                    }
                }
                live_in(a, p, stamp, stack, ++tag);
                for (int j = a->ufirst[p]; j < a->ufirst[p + 1] && !live; j++)
                    live = a->uses[j].block == bv && a->uses[j].pos > a->start[v];
                for (int s = 0; s < 2 && !live; s++)
                    live = fn->blocks[bv].succ[s] >= 0 && stamp[fn->blocks[bv].succ[s]] == tag;
                if (live) continue;
                a->with[v] = p;
                leads[p] = 1;
                if (a->end[v] > a->end[p]) a->end[p] = a->end[v];
                shared = 1;
            }
        }
    }
    free(stamp); free(stack); free(leads);
    // A cast its operand's register form already holds shares the operand's
    // location when its interval lies within the operand's, which only the
    // operand's definition writes then: the operand leads no sharing pair of
    // its own (a phi's moves come at its predecessors' ends, where in SSA the
    // cast is not live), and no value computed into it is defined in the
    // cast's interval.
    int* partner = xcalloc((size_t)fn->nins, sizeof(int)); // a phi -> the one value computed into it + 1
    for (int v = 0; v < fn->nins; v++) if (a->with[v] >= 0) partner[a->with[v]] = v + 1;
    for (int v = 0; v < fn->nins; v++) {
        IrIns* x = &fn->ins[v];
        if (x->op != IR_CAST || x->block < 0 || !allocatable(a, v) || a->with[v] >= 0) continue;
        int s = x->args[0];
        if (!allocatable(a, s) || a->with[s] >= 0 || a->end[v] > a->end[s] ||
            !t->cast_in_place((IrType)x->type, (IrType)fn->ins[s].type))
            continue;
        int w = partner[s] - 1;
        if (w < 0 || a->start[w] <= a->start[v] || a->start[w] > a->end[v]) a->with[v] = s;
    }
    free(partner);
}

typedef struct { int end, slot; } SlotUse;

static void heap_push(SlotUse* h, int* n, SlotUse x) {
    int i = (*n)++;
    while (i && h[(i - 1) / 2].end > x.end) { h[i] = h[(i - 1) / 2]; i = (i - 1) / 2; }
    h[i] = x;
}

static SlotUse heap_pop(SlotUse* h, int* n) {
    SlotUse top = h[0], x = h[--*n];
    int i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= *n) break;
        if (c + 1 < *n && h[c + 1].end < h[c].end) c++;
        if (h[c].end >= x.end) break;
        h[i] = h[c];
        i = c;
    }
    if (*n) h[i] = x;
    return top;
}

// Whether the target's register r is callee-saved, disturbed by a byte
// routine, or reserved by an asm statement (mask az: integer registers).
static int bit(uint64_t m, int r) { return (m >> r) & 1; }
static int asm_clobbers(uint32_t az, int r) { return r < 32 && (az & (1u << r)) != 0; }

// The union of every asm statement's reserved registers (Inline Assembly
// #2-3) whose position falls inside v's interval: none of them may hold v.
static uint32_t asm_reserved(Ra* a, int v) {
    uint32_t m = 0;
    for (int i = 0; i < a->nasms; i++) {
        int at = a->asm_pos[i];
        if (at > a->start[v] && a->end[v] > at + 1) m |= a->asm_mask[i];
    }
    return m;
}

void ra_allocate(Ra* a, const RaTarget* t) {
    IrFunc* fn = a->fn;
    int nv = fn->nins;
    a->reg = xcalloc((size_t)nv, 1);
    a->slot = xcalloc((size_t)nv, sizeof(int));
    int* seq = xcalloc((size_t)nv, sizeof(int)); // allocated values in start order
    int nseq = 0;
    for (int v = 0; v < nv; v++) { a->reg[v] = -1; a->slot[v] = -1; }
    for (int pass = 0; pass < 2; pass++) // the values arriving at entry first
        for (int i = 0; i < fn->nblocks; i++) {
            IrBlock* bb = &fn->blocks[a->order[i]];
            for (int j = 0; j < bb->nins; j++) {
                int v = bb->ins[j];
                if (allocatable(a, v) && a->with[v] < 0 && entry_value((IrOp)fn->ins[v].op) == !pass) seq[nseq++] = v;
            }
        }
    int owner[RA_NREGS], active[RA_NREGS], nactive = 0;
    uint8_t* spilled = xcalloc((size_t)nv, 1);
    for (int r = 0; r < RA_NREGS; r++) owner[r] = -1;
    for (int i = 0; i < nseq; i++) {
        int v = seq[i];
        for (int k = 0; k < nactive;) { // expire
            int w = active[k];
            if (a->end[w] < a->start[v]) {
                owner[a->reg[w]] = -1;
                active[k] = active[--nactive];
            } else {
                k++;
            }
        }
        IrType vt = (IrType)a->fn->ins[v].type;
        int cross = ra_crosses(a, v, a->calls, a->ncalls), r = -1, fp = ir_type_float(vt) || ir_type_vector(vt);
        if (cross && ir_type_vector(vt) && t->vec_cross_spills) {
            spilled[v] = 1;
            continue;
        }
        int rt = ra_crosses(a, v, a->rtcalls, a->nrtcalls); // not in a register a byte routine disturbs
        uint32_t az = asm_reserved(a, v); // not in a register an asm statement reserves
        const int8_t* caller = t->caller[fp];
        const int8_t* callee = t->callee[fp];
        int ncaller = t->ncaller[fp], ncallee = t->ncallee[fp];
        int want[8], nwant = 0; // preferred registers, best first
        IrIns* x = &a->fn->ins[v];
        if (x->op == IR_PHI)
            for (int k = 0; k < x->nargs && nwant < 6; k++)
                if (a->reg[x->args[k]] >= 0) want[nwant++] = a->reg[x->args[k]];
        if (a->phi_of[v] >= 0 && a->reg[a->phi_of[v]] >= 0) want[nwant++] = a->reg[a->phi_of[v]];
        if (a->hint[v] >= 0) want[nwant++] = a->hint[v];
        for (int k = 0; k < nwant && r < 0; k++)
            if (owner[want[k]] < 0 && (want[k] >= t->fp_base) == fp && (!cross || bit(t->callee_saved, want[k])) &&
                !(rt && bit(t->rtcall_clobbers, want[k])) && !asm_clobbers(az, want[k]))
                r = want[k];
        if (!cross)
            for (int k = 0; k < ncaller && r < 0; k++)
                if (owner[caller[k]] < 0 && !(rt && bit(t->rtcall_clobbers, caller[k])) && !asm_clobbers(az, caller[k]))
                    r = caller[k];
        for (int k = 0; k < ncallee && r < 0; k++)
            if (owner[callee[k]] < 0 && !asm_clobbers(az, callee[k])) r = callee[k];
        if (r < 0) { // spill whichever interval of the class ends last
            int far = -1;
            for (int k = 0; k < nactive; k++) {
                int w = active[k];
                if ((a->reg[w] >= t->fp_base) != fp || (cross && !bit(t->callee_saved, a->reg[w])) ||
                    (rt && bit(t->rtcall_clobbers, a->reg[w])) || asm_clobbers(az, a->reg[w]))
                    continue;
                if (far < 0 || a->end[w] > a->end[active[far]]) far = k;
            }
            if (far >= 0 && a->end[active[far]] > a->end[v]) {
                int w = active[far];
                r = a->reg[w];
                a->reg[w] = -1;
                spilled[w] = 1;
                active[far] = active[--nactive];
            } else {
                spilled[v] = 1;
                continue;
            }
        }
        a->reg[v] = (int8_t)r;
        owner[r] = v;
        active[nactive++] = v;
    }
    // stack slots, reused once their interval ends
    SlotUse* heap = xcalloc((size_t)nseq, sizeof(SlotUse));
    int* free_slots = xcalloc((size_t)nseq, sizeof(int));
    int nheap = 0, nfree = 0;
    for (int i = 0; i < nseq; i++) {
        int v = seq[i];
        if (!spilled[v]) continue;
        if (ir_type_vector((IrType)a->fn->ins[v].type)) { // a pair of its own
            a->slot[v] = a->nslots;
            a->nslots += 2;
            continue;
        }
        while (nheap && heap[0].end < a->start[v]) free_slots[nfree++] = heap_pop(heap, &nheap).slot;
        int s = nfree ? free_slots[--nfree] : a->nslots++;
        a->slot[v] = s;
        heap_push(heap, &nheap, (SlotUse){ a->end[v], s });
    }
    for (int v = 0; v < nv; v++)
        if (a->with[v] >= 0) {
            a->reg[v] = a->reg[a->with[v]];
            a->slot[v] = a->slot[a->with[v]];
        }
    free(seq); free(spilled); free(heap); free(free_slots);
}

void ra_free(Ra* a) {
    free(a->order); free(a->lix); free(a->layout); free(a->eix); free(a->cold); free(a->bfirst); free(a->blast); free(a->idx);
    free(a->nuse); free(a->fused); free(a->start); free(a->end); free(a->ufirst); free(a->uses);
    free(a->hint); free(a->phi_of); free(a->with); free(a->calls); free(a->rtcalls);
    free(a->asm_pos); free(a->asm_mask); free(a->reg); free(a->slot);
}
