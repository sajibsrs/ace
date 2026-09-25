// arm64.c — the AArch64 target of the IR backend (SPEC.md Decision Queue,
// "Per-function SSA IR backend", passes 7-10): instruction selection, leaving
// SSA, linear-scan register allocation, the frame, and encoding into the
// shared Obj, under the AST emitter's ABI (emit.c header).
//
// Values: a 64-bit type lives in a whole x register. A narrower one lives in
// the low 32 bits, extended to 32 by its signedness (the Apple PCS argument
// form); bits 63:32 are unspecified, so 32-bit instructions operate on it and
// only a widening cast extends it.
//
// Registers: x0-x14 and x19-x28 are allocated. x15, x16, and x17 are scratch
// for spilled and constant operands, check temporaries, and far spill slots;
// x18 belongs to the platform.
//
// Allocation (Poletto & Sarkar): each value has one interval, the hull of the
// positions where it is live in block layout order (a reverse postorder),
// found by walking up from each use to the definition. Constants take no
// register: each use materializes them. An interval that crosses a call takes
// a callee-saved register or a stack slot. A phi's operands move into it in
// parallel at the end of each predecessor; critical edges into a phi's block
// are split first, and hints steer a phi and its operands to one register so
// most of those moves vanish.
#include "backend.h"
#include "ir.h"

#include <stdio.h>

enum { S0 = 16, S1 = 17, S2 = 15 }; // scratch, never allocated
enum { C_EQ, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE };
enum { NOREG = -1, SLOT = 32 }; // a move location: register r, or SLOT + stack slot
enum { FAIL = -1 };             // branch target: the arithmetic failure stub

typedef struct { uint32_t site, w; int target; uint8_t imm19; } Branch;

// A branch condition: the flags satisfy cond, or (cbz) reg is zero / nonzero.
typedef struct { uint8_t cbz, cond, reg, sf, nz; } Cond;

typedef struct { int dst, src; uint64_t imm; uint8_t is_const, sf; } Move;

typedef struct {
    Obj*      o;
    IrFunc*   fn;
    Buf*      t;
    int       is_main, far;
    // layout
    int*      order;   // blocks in layout order
    int*      lix;     // block -> layout index
    int*      bfirst;  // block -> index of its first instruction
    int*      blast;   // block -> index of its terminator
    int*      idx;     // value -> instruction index
    // analysis
    int*      nuse;
    uint8_t*  fused;   // emitted by its only user
    uint8_t*  madd;    // ADD/SUB fused with a MUL operand: 1 + its index
    int*      start, *end;
    int*      hint, *phi_of;
    int*      calls;   // call use positions, ascending
    int       ncalls;
    // allocation
    int8_t*   reg;
    int*      slot;
    int       nslots, ncs;
    int8_t    cs[10];  // callee-saved registers used, ascending
    uint32_t  frame, slot_base;
    int       has_frame;
    // emission
    uint32_t* label;   // block -> text offset
    Branch*   br;
    int       nbr, cbr;
    uint32_t  fail_at;
    Node*     last_at;
} A64;

static void* xcalloc(size_t n, size_t size) {
    void* p = calloc(n ? n : 1, size);
    if (!p) oom();
    return p;
}

static IrIns* ins_of(A64* a, int v) { return &a->fn->ins[v]; }
static int is64(IrType t) { return ir_type_bits(t) == 64; }
static uint32_t sfb(IrType t) { return is64(t) ? 0x80000000u : 0; }
static int is_const(A64* a, int v) { return ins_of(a, v)->op == IR_CONST; }
static int narrow(IrType t) { return t != IT_BOOL && ir_type_bits(t) < 32; }

// --- encoding -------------------------------------------------------------------------

static void put(A64* a, uint32_t w) { buf_put32(a->t, w); }
static uint32_t here(A64* a) { return (uint32_t)a->t->len; }

static void mov_reg(A64* a, int rd, int rn) {
    if (rd != rn) put(a, 0xAA0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd);
}

// rd = v; a 32-bit form sets only what a narrow value's register form needs
static void mov_imm(A64* a, int rd, uint64_t v, int sf) {
    int n = sf ? 4 : 2;
    uint32_t s = sf ? 0x80000000u : 0;
    if (!sf) v &= 0xFFFFFFFFu;
    int zeros = 0, ones = 0;
    for (int i = 0; i < n; i++) {
        uint16_t c = (uint16_t)(v >> (16 * i));
        zeros += c == 0;
        ones += c == 0xFFFF;
    }
    if (n - (zeros > ones ? zeros : ones) > 1) { // several moves: a bitmask is one
        uint32_t f = logical_imm_field(v, sf);
        if (f) {
            put(a, s | 0x32000000u | (f << 10) | (31u << 5) | (uint32_t)rd); // orr rd, zr, #v
            return;
        }
    }
    int inv = ones > zeros, first = 1;
    uint16_t skip = inv ? 0xFFFF : 0;
    for (int i = 0; i < n; i++) {
        uint16_t c = (uint16_t)(v >> (16 * i));
        if (c == skip) continue;
        uint32_t hw = (uint32_t)i << 21;
        if (first) put(a, s | (inv ? 0x12800000u : 0x52800000u) | hw | ((uint32_t)(inv ? (uint16_t)~c : c) << 5) | (uint32_t)rd);
        else put(a, s | 0x72800000u | hw | ((uint32_t)c << 5) | (uint32_t)rd); // movk
        first = 0;
    }
    if (first) put(a, s | (inv ? 0x12800000u : 0x52800000u) | (uint32_t)rd); // movz #0 / movn #0
}

static void sp_adjust(A64* a, uint32_t base, uint32_t f) { // add/sub sp, sp, #f
    if (f >> 12) put(a, base | 0x400000u | ((f >> 12) << 10));
    if (f & 0xFFFu) put(a, base | ((f & 0xFFFu) << 10));
}

static uint32_t slot_off(A64* a, int s) { return a->slot_base + 8u * (uint32_t)s; }

static void ld_slot(A64* a, int rt, int s) {
    uint32_t off = slot_off(a, s);
    if (off / 8 <= 4095) {
        put(a, 0xF94003E0u | ((off / 8) << 10) | (uint32_t)rt);
        return;
    }
    put(a, 0x914003E0u | ((off >> 12) << 10) | (uint32_t)rt); // add rt, sp, #hi, lsl 12
    put(a, 0xF9400000u | (((off & 0xFFFu) / 8) << 10) | ((uint32_t)rt << 5) | (uint32_t)rt);
}

static void st_slot(A64* a, int rt, int s) { // a far slot is addressed through x15
    uint32_t off = slot_off(a, s);
    if (off / 8 <= 4095) {
        put(a, 0xF90003E0u | ((off / 8) << 10) | (uint32_t)rt);
        return;
    }
    put(a, 0x914003E0u | ((off >> 12) << 10) | (uint32_t)S2);
    put(a, 0xF9000000u | (((off & 0xFFFu) / 8) << 10) | ((uint32_t)S2 << 5) | (uint32_t)rt);
}

// the 32-bit extension that wraps a narrow value in place
static uint32_t ext_of(IrType t) {
    switch (t) {
    case IT_I8: return 0x13001C00u;  // sxtb
    case IT_I16: return 0x13003C00u; // sxth
    case IT_U8: return 0x53001C00u;  // uxtb
    default: return 0x53003C00u;     // uxth
    }
}

static void ext(A64* a, IrType t, int rd, int rn) {
    put(a, ext_of(t) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static void wrap(A64* a, IrType t, int rd) {
    if (narrow(t)) ext(a, t, rd, rd);
}

// --- branches ---------------------------------------------------------------------------

static void branch_add(A64* a, uint32_t w, int target, int imm19) {
    if (a->nbr == a->cbr) {
        a->cbr = a->cbr ? a->cbr * 2 : 16;
        a->br = realloc(a->br, (size_t)a->cbr * sizeof(Branch));
        if (!a->br) oom();
    }
    a->br[a->nbr++] = (Branch){ here(a), w, target, (uint8_t)imm19 };
    put(a, w);
}

static uint32_t cond_word(Cond c) {
    if (c.cbz) return (c.nz ? 0x35000000u : 0x34000000u) | (c.sf ? 0x80000000u : 0) | c.reg;
    return 0x54000000u | c.cond;
}

static Cond invert(Cond c) {
    if (c.cbz) c.nz ^= 1;
    else c.cond ^= 1;
    return c;
}

static void bl_to(A64* a, const char* name) {
    obj_fixup(a->o, FK_BRANCH, here(a), sym_intern(&a->o->syms, name, (uint32_t)strlen(name)));
    put(a, 0x94000000u);
}

// Branch to block target when c holds. Far code (over 1 MiB) skips an
// unconditional branch on the inverse instead.
static void branch_if(A64* a, Cond c, int target) {
    if (target == FAIL) a->o->arithmetic_fail_used = 1;
    if (!a->far) {
        branch_add(a, cond_word(c), target, 1);
        return;
    }
    put(a, cond_word(invert(c)) | (2u << 5)); // over the next instruction
    if (target == FAIL) bl_to(a, "__ace$arithmetic_fail");
    else branch_add(a, 0x14000000u, target, 0);
}

static void fail_if(A64* a, int cond) { branch_if(a, (Cond){ 0, (uint8_t)cond, 0, 0, 0 }, FAIL); }

static void fail_if_zero(A64* a, int reg, int sf, int nz) {
    branch_if(a, (Cond){ 1, 0, (uint8_t)reg, (uint8_t)sf, (uint8_t)nz }, FAIL);
}

// cmp wd, wd, sxtb (etc.): a narrow result that its extension changes is out of range
static void check_narrow(A64* a, IrType t, int rd) {
    static const uint32_t opt[] = { [IT_I8] = 4, [IT_I16] = 5, [IT_U8] = 0, [IT_U16] = 1 };
    put(a, 0x6B20001Fu | ((uint32_t)rd << 16) | (opt[t] << 13) | ((uint32_t)rd << 5));
    fail_if(a, C_NE);
}

// --- operands ---------------------------------------------------------------------------

// The register holding v for reading; a constant or spilled value goes to scratch.
static int opnd(A64* a, int v, int scratch) {
    IrIns* x = ins_of(a, v);
    if (x->op == IR_CONST) {
        mov_imm(a, scratch, x->imm, is64((IrType)x->type));
        return scratch;
    }
    if (a->reg[v] >= 0) return a->reg[v];
    ld_slot(a, scratch, a->slot[v]);
    return scratch;
}

// The register v's result is computed into; dst_done stores a spilled one.
static int dst(A64* a, int v) { return a->reg[v] >= 0 ? a->reg[v] : S0; }

static void dst_done(A64* a, int v, int r) {
    if (a->reg[v] < 0 && a->slot[v] >= 0) st_slot(a, r, a->slot[v]);
}

static int loc(A64* a, int v) { // a move location, or NOREG for an unused value
    if (a->reg[v] >= 0) return a->reg[v];
    return a->slot[v] >= 0 ? SLOT + a->slot[v] : NOREG;
}

static void move1(A64* a, Move m) {
    if (m.is_const) {
        if (m.dst < SLOT) {
            mov_imm(a, m.dst, m.imm, m.sf);
        } else {
            mov_imm(a, S1, m.imm, m.sf);
            st_slot(a, S1, m.dst - SLOT);
        }
    } else if (m.dst < SLOT && m.src < SLOT) {
        mov_reg(a, m.dst, m.src);
    } else if (m.dst < SLOT) {
        ld_slot(a, m.dst, m.src - SLOT);
    } else if (m.src < SLOT) {
        st_slot(a, m.src, m.dst - SLOT);
    } else {
        ld_slot(a, S1, m.src - SLOT);
        st_slot(a, S1, m.dst - SLOT);
    }
}

// Perform moves as if every source were read before any destination is
// written; a cycle is broken through x16.
static void parallel_move(A64* a, Move* m, int n) {
    int left = 0;
    for (int i = 0; i < n; i++) {
        if (!m[i].is_const && m[i].src == m[i].dst) m[i].dst = NOREG;
        if (m[i].dst != NOREG) left++;
    }
    while (left) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (m[i].dst == NOREG) continue;
            int blocked = 0;
            for (int j = 0; j < n && !blocked; j++)
                blocked = j != i && m[j].dst != NOREG && !m[j].is_const && m[j].src == m[i].dst;
            if (blocked) continue;
            move1(a, m[i]);
            m[i].dst = NOREG;
            left--;
            progress = 1;
        }
        if (progress) continue;
        for (int i = 0; i < n; i++) { // every pending move is on a cycle
            if (m[i].dst == NOREG) continue;
            int d = m[i].dst;
            move1(a, (Move){ S0, d, 0, 0, 1 });
            for (int j = 0; j < n; j++)
                if (m[j].dst != NOREG && !m[j].is_const && m[j].src == d) m[j].src = S0;
            break;
        }
    }
}

static Move move_of(A64* a, int dst_loc, int v) {
    IrIns* x = ins_of(a, v);
    if (x->op == IR_CONST) return (Move){ dst_loc, 0, x->imm, 1, (uint8_t)is64((IrType)x->type) };
    return (Move){ dst_loc, loc(a, v), 0, 0, 1 };
}

// --- layout, liveness, allocation --------------------------------------------------------

// Split each edge from a two-way branch into a block with phis.
static void split_critical_edges(IrFunc* fn) {
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

// Reverse postorder, visiting the second successor first so that the first
// (a branch's true side, a loop's body) follows its block.
static void layout(A64* a) {
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
}

static int is_compare(int op) { return op >= IR_EQ && op <= IR_GE; }

static void count_uses(A64* a) {
    IrFunc* fn = a->fn;
    a->nuse = xcalloc((size_t)fn->nins, sizeof(int));
    a->fused = xcalloc((size_t)fn->nins, 1);
    a->madd = xcalloc((size_t)fn->nins, 1);
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            for (int k = 0; k < x->nargs; k++) a->nuse[x->args[k]]++;
        }
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int u = fn->blocks[b].ins[i];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_BR) {
                int c = x->args[0];
                if (is_compare(fn->ins[c].op) && a->nuse[c] == 1 && fn->ins[c].block == b) a->fused[c] = 1;
            } else if ((x->op == IR_ADD || x->op == IR_SUB) && !(x->flags & IRF_CHECKED)) {
                for (int k = x->op == IR_ADD ? 0 : 1; k < 2; k++) { // multiply-add, multiply-subtract
                    int m = x->args[k];
                    IrIns* mx = &fn->ins[m];
                    // a constant addend would need a register: mul + add #imm is no longer
                    if (mx->op != IR_MUL || (mx->flags & IRF_CHECKED) || a->nuse[m] != 1 || mx->block != b ||
                        fn->ins[x->args[1 - k]].op == IR_CONST)
                        continue;
                    a->fused[m] = 1;
                    a->madd[u] = (uint8_t)(1 + k);
                    break;
                }
            }
        }
}

typedef struct { int v, block, pos; } Use;

// The end of each value's interval: the furthest position, in layout order,
// where it is used or live out of a block.
static void liveness(A64* a) {
    IrFunc* fn = a->fn;
    int nv = fn->nins;
    a->start = xcalloc((size_t)nv, sizeof(int));
    a->end = xcalloc((size_t)nv, sizeof(int));
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int v = fn->blocks[b].ins[i];
            a->start[v] = fn->ins[v].op == IR_PHI ? 2 * a->bfirst[b] : 2 * a->idx[v] + 1;
            a->end[v] = a->start[v];
        }
    // uses by value (counting sort), a fused value's operands used at its user
    int nu = 0;
    Use* uses = NULL;
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
                    if (fn->ins[w].op == IR_CONST) continue;
                    if (nu == cu) {
                        cu = cu ? cu * 2 : 256;
                        uses = realloc(uses, (size_t)cu * sizeof(Use));
                        if (!uses) oom();
                    }
                    uses[nu++] = (Use){ w, ub, pos };
                }
            }
        }
    }
    int* first = xcalloc((size_t)nv + 1, sizeof(int));
    for (int i = 0; i < nu; i++) first[uses[i].v + 1]++;
    for (int v = 0; v < nv; v++) first[v + 1] += first[v];
    Use* by = xcalloc((size_t)nu, sizeof(Use));
    int* fill = xcalloc((size_t)nv, sizeof(int));
    for (int i = 0; i < nu; i++) by[first[uses[i].v] + fill[uses[i].v]++] = uses[i];
    free(uses);
    free(fill);
    int* stamp = xcalloc((size_t)fn->nblocks, sizeof(int));
    int* stack = xcalloc((size_t)fn->nblocks, sizeof(int));
    for (int v = 0; v < nv; v++) {
        int d = fn->ins[v].block, e = a->end[v];
        for (int i = first[v]; i < first[v + 1]; i++) {
            if (by[i].pos > e) e = by[i].pos;
            int b = by[i].block;
            if (b == d || stamp[b] == v + 1) continue;
            int sp = 0;
            stamp[b] = v + 1;
            stack[sp++] = b;
            while (sp) { // live in at b: live out of each predecessor
                IrBlock* bb = &fn->blocks[stack[--sp]];
                for (int p = 0; p < bb->npreds; p++) {
                    int q = bb->preds[p];
                    if (2 * a->blast[q] + 1 > e) e = 2 * a->blast[q] + 1;
                    if (q != d && stamp[q] != v + 1) {
                        stamp[q] = v + 1;
                        stack[sp++] = q;
                    }
                }
            }
        }
        a->end[v] = e;
    }
    free(stamp); free(stack); free(first); free(by);
}

// Does v's interval contain a call it must survive?
static int crosses_call(A64* a, int v) {
    int lo = 0, hi = a->ncalls; // first call after v's start
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (a->calls[mid] > a->start[v]) hi = mid;
        else lo = mid + 1;
    }
    return lo < a->ncalls && a->end[v] > a->calls[lo] + 1;
}

static int allocatable(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    return x->type != IT_VOID && x->op != IR_CONST && !a->fused[v] && a->nuse[v] > 0;
}

static void hints(A64* a) {
    IrFunc* fn = a->fn;
    a->hint = xcalloc((size_t)fn->nins, sizeof(int));
    a->phi_of = xcalloc((size_t)fn->nins, sizeof(int));
    for (int v = 0; v < fn->nins; v++) a->hint[v] = a->phi_of[v] = -1;
    a->calls = xcalloc((size_t)fn->nins, sizeof(int));
    for (int i = 0; i < fn->nblocks; i++) {
        IrBlock* bb = &fn->blocks[a->order[i]];
        for (int j = 0; j < bb->nins; j++) {
            int u = bb->ins[j];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_PHI) {
                for (int k = 0; k < x->nargs; k++)
                    if (a->phi_of[x->args[k]] < 0) a->phi_of[x->args[k]] = u;
            } else if (x->op == IR_PARAM) {
                a->hint[u] = (int)x->imm;
            } else if (x->op == IR_CALL) {
                a->calls[a->ncalls++] = 2 * a->idx[u];
                a->hint[u] = 0;
                for (int k = 0; k < x->nargs; k++)
                    if (a->hint[x->args[k]] < 0) a->hint[x->args[k]] = k;
            } else if (x->op == IR_RET && x->nargs) {
                if (a->hint[x->args[0]] < 0) a->hint[x->args[0]] = 0;
            }
        }
    }
}

static const int8_t caller_regs[] = { 9, 10, 11, 12, 13, 14, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const int8_t callee_regs[] = { 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };

static int callee_saved(int r) { return r >= 19 && r <= 28; }

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

static void allocate(A64* a) {
    IrFunc* fn = a->fn;
    int nv = fn->nins;
    a->reg = xcalloc((size_t)nv, 1);
    a->slot = xcalloc((size_t)nv, sizeof(int));
    int* seq = xcalloc((size_t)nv, sizeof(int)); // allocated values in start order
    int nseq = 0;
    for (int v = 0; v < nv; v++) { a->reg[v] = -1; a->slot[v] = -1; }
    for (int i = 0; i < fn->nblocks; i++) {
        IrBlock* bb = &fn->blocks[a->order[i]];
        for (int j = 0; j < bb->nins; j++)
            if (allocatable(a, bb->ins[j])) seq[nseq++] = bb->ins[j];
    }
    int owner[32], active[32], nactive = 0;
    uint8_t* spilled = xcalloc((size_t)nv, 1);
    for (int r = 0; r < 32; r++) owner[r] = -1;
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
        int cross = crosses_call(a, v), r = -1;
        int want[8], nwant = 0; // preferred registers, best first
        IrIns* x = ins_of(a, v);
        if (x->op == IR_PHI)
            for (int k = 0; k < x->nargs && nwant < 6; k++)
                if (a->reg[x->args[k]] >= 0) want[nwant++] = a->reg[x->args[k]];
        if (a->phi_of[v] >= 0 && a->reg[a->phi_of[v]] >= 0) want[nwant++] = a->reg[a->phi_of[v]];
        if (a->hint[v] >= 0) want[nwant++] = a->hint[v];
        for (int k = 0; k < nwant && r < 0; k++)
            if (owner[want[k]] < 0 && (!cross || callee_saved(want[k]))) r = want[k];
        if (!cross)
            for (size_t k = 0; k < sizeof caller_regs && r < 0; k++)
                if (owner[caller_regs[k]] < 0) r = caller_regs[k];
        for (size_t k = 0; k < sizeof callee_regs && r < 0; k++)
            if (owner[callee_regs[k]] < 0) r = callee_regs[k];
        if (r < 0) { // spill whichever interval ends last
            int far = -1;
            for (int k = 0; k < nactive; k++) {
                int w = active[k];
                if (cross && !callee_saved(a->reg[w])) continue;
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
        while (nheap && heap[0].end < a->start[v]) free_slots[nfree++] = heap_pop(heap, &nheap).slot;
        int s = nfree ? free_slots[--nfree] : a->nslots++;
        a->slot[v] = s;
        heap_push(heap, &nheap, (SlotUse){ a->end[v], s });
    }
    uint32_t used = 0;
    for (int i = 0; i < nseq; i++)
        if (a->reg[seq[i]] >= 0) used |= 1u << a->reg[seq[i]];
    for (int r = 19; r <= 28; r++)
        if (used & (1u << r)) a->cs[a->ncs++] = (int8_t)r;
    free(seq); free(spilled); free(heap); free(free_slots);
}

// --- frame ------------------------------------------------------------------------------

static void prologue(A64* a) {
    if (!a->has_frame) return;
    uint32_t f = a->frame;
    if (f <= 504) {
        put(a, 0xA9800000u | ((uint32_t)(-(int32_t)(f / 8)) & 0x7Fu) << 15 | (30u << 10) | (31u << 5) | 29u);
    } else {
        sp_adjust(a, 0xD10003FFu, f);
        put(a, 0xA9000000u | (30u << 10) | (31u << 5) | 29u); // stp x29, x30, [sp]
    }
    put(a, 0x910003FDu); // mov x29, sp
    for (int k = 0; k < a->ncs; k += 2) {
        uint32_t off = (16u + 8u * (uint32_t)k) / 8;
        if (k + 1 < a->ncs)
            put(a, 0xA9000000u | (off << 15) | ((uint32_t)a->cs[k + 1] << 10) | (31u << 5) | (uint32_t)a->cs[k]);
        else
            put(a, 0xF90003E0u | (off << 10) | (uint32_t)a->cs[k]);
    }
}

static void epilogue(A64* a) {
    if (a->has_frame) {
        for (int k = 0; k < a->ncs; k += 2) {
            uint32_t off = (16u + 8u * (uint32_t)k) / 8;
            if (k + 1 < a->ncs)
                put(a, 0xA9400000u | (off << 15) | ((uint32_t)a->cs[k + 1] << 10) | (31u << 5) | (uint32_t)a->cs[k]);
            else
                put(a, 0xF94003E0u | (off << 10) | (uint32_t)a->cs[k]);
        }
        uint32_t f = a->frame;
        if (f <= 504) {
            put(a, 0xA8C00000u | ((f / 8) << 15) | (30u << 10) | (31u << 5) | 29u); // ldp x29, x30, [sp], #f
        } else {
            put(a, 0xA9400000u | (30u << 10) | (31u << 5) | 29u);
            sp_adjust(a, 0x910003FFu, f);
        }
    }
    put(a, 0xD65F03C0u); // ret
}

// --- selection --------------------------------------------------------------------------

static int cond_of(int op, int sg) {
    switch (op) {
    case IR_EQ: return C_EQ;
    case IR_NE: return C_NE;
    case IR_LT: return sg ? C_LT : C_LO;
    case IR_LE: return sg ? C_LE : C_LS;
    case IR_GT: return sg ? C_GT : C_HI;
    default: return sg ? C_GE : C_HS;
    }
}

static int swap_cond(int c) { // the condition for the operands exchanged
    switch (c) {
    case C_LT: return C_GT;
    case C_GT: return C_LT;
    case C_LE: return C_GE;
    case C_GE: return C_LE;
    case C_LO: return C_HI;
    case C_HI: return C_LO;
    case C_LS: return C_HS;
    case C_HS: return C_LS;
    default: return c;
    }
}

// An add/sub/cmp immediate for constant c of type t: 0 when none, else 1 for
// c itself or -1 for its negation (cmn, or add and sub exchanged).
static int arith_imm(A64* a, int c, IrType t, uint32_t* imm) {
    if (!is_const(a, c)) return 0;
    uint64_t u = ins_of(a, c)->imm;
    if (u <= 4095) { *imm = (uint32_t)u; return 1; }
    int64_t s = (int64_t)u;
    if (ir_type_signed(t) && s < 0 && s >= -4095) { *imm = (uint32_t)-s; return -1; }
    return 0;
}

// Compare for v (EQ..GE); returns the condition that holds when v is true.
// A branch (cbz_ok) tests equality with zero by cbz/cbnz instead.
static Cond compare(A64* a, int v, int cbz_ok) {
    IrIns* x = ins_of(a, v);
    int l = x->args[0], r = x->args[1];
    IrType t = (IrType)ins_of(a, l)->type;
    uint32_t sf = sfb(t);
    int cond = cond_of(x->op, ir_type_signed(t));
    if (is_const(a, l) && !is_const(a, r)) {
        int s = l; l = r; r = s;
        cond = swap_cond(cond);
    }
    uint32_t imm;
    if (cbz_ok && is_const(a, r) && ins_of(a, r)->imm == 0 && (cond == C_EQ || cond == C_NE))
        return (Cond){ 1, 0, (uint8_t)opnd(a, l, S0), (uint8_t)is64(t), (uint8_t)(cond == C_NE) };
    int form = arith_imm(a, r, t, &imm);
    int rn = opnd(a, l, S0);
    if (form) put(a, sf | (form > 0 ? 0x7100001Fu : 0x3100001Fu) | (imm << 10) | ((uint32_t)rn << 5));
    else put(a, sf | 0x6B00001Fu | ((uint32_t)opnd(a, r, S1) << 16) | ((uint32_t)rn << 5));
    return (Cond){ 0, (uint8_t)cond, 0, 0, 0 };
}

// Constant division (emit.c emit_const_divide, after Granlund-Montgomery):
// dst = ra / c or ra % c through x16 and x17. 0 when the divisor needs the
// hardware divide (zero, or signed -1 with its overflow).
static int const_divide(A64* a, int op, int rd, int ra, uint64_t c, IrType t) {
    int w = is64(t) ? 64 : 32, sf = w == 64;
    uint32_t s = sf ? 0x80000000u : 0;
    int sig = ir_type_signed(t);
    uint64_t mask = sf ? UINT64_MAX : UINT32_MAX;
    int64_t sd = sf ? (int64_t)c : (int64_t)(int32_t)(uint32_t)c;
    uint64_t d = sig ? (sd < 0 ? (uint64_t)0 - (uint64_t)sd : (uint64_t)sd) & mask : c & mask;
    if (d == 0 || (sig && sd == -1)) return 0;
    uint32_t qd = op == IR_DIV ? (uint32_t)rd : S0; // the remainder keeps q in x16
    uint32_t lsr = sf ? 0xD340FC00u : 0x53007C00u, asr = sf ? 0x9340FC00u : 0x13007C00u;
    uint32_t n = (uint32_t)ra;
    if (d == 1) {
        if (op == IR_DIV) mov_reg(a, rd, ra);
        else mov_imm(a, rd, 0, sf);
        return 1;
    }
    if ((d & (d - 1)) == 0) {
        uint32_t k = (uint32_t)__builtin_ctzll(d);
        if (!sig) {
            if (op == IR_DIV) put(a, lsr | (k << 16) | (n << 5) | (uint32_t)rd);
            else put(a, (sf ? 0x92400000u : 0x12000000u) | ((k - 1) << 10) | (n << 5) | (uint32_t)rd);
            return 1;
        }
        put(a, asr | ((uint32_t)(w - 1) << 16) | (n << 5) | S0); // round toward zero:
        put(a, s | 0x0B400000u | (S0 << 16) | ((uint32_t)(w - (int)k) << 10) | (n << 5) | S0); // bias by d - 1
        put(a, asr | (k << 16) | (S0 << 5) | (sd < 0 ? S0 : qd));
        if (sd < 0) put(a, s | 0x4B0003E0u | (S0 << 16) | qd); // neg
    } else if (!sig) {
        DivMagic m = div_magic(d, w, 0, 0);
        mov_imm(a, S1, m.magic, sf);
        put(a, (sf ? 0x9BC07C00u : 0x9BA07C00u) | (S1 << 16) | (n << 5) | S0); // umulh / umull
        if (!m.add) {
            put(a, 0xD340FC00u | ((uint32_t)((sf ? 0 : 32) + m.shift) << 16) | (S0 << 5) | qd);
        } else {
            if (!sf) put(a, 0xD340FC00u | (32u << 16) | (S0 << 5) | S0);
            put(a, s | 0x4B000000u | (S0 << 16) | (n << 5) | S1);                 // a - t
            put(a, s | 0x0B400000u | (S1 << 16) | (1u << 10) | (S0 << 5) | S0);   // t + ((a - t) >> 1)
            put(a, lsr | ((uint32_t)m.shift << 16) | (S0 << 5) | qd);
        }
    } else {
        DivMagic m = div_magic(d, w, 1, sd < 0);
        mov_imm(a, S1, m.magic, sf);
        put(a, (sf ? 0x9B407C00u : 0x9B207C00u) | (S1 << 16) | (n << 5) | S0); // smulh / smull
        if (!m.add) {
            put(a, 0x9340FC00u | ((uint32_t)((sf ? 0 : 32) + m.shift) << 16) | (S0 << 5) | S0);
        } else {
            if (!sf) put(a, 0x9340FC00u | (32u << 16) | (S0 << 5) | S0);
            put(a, s | (m.neg ? 0x4B000000u : 0x0B000000u) | (n << 16) | (S0 << 5) | S0); // t -= a or t += a
            put(a, asr | ((uint32_t)m.shift << 16) | (S0 << 5) | S0);
        }
        put(a, s | 0x0B400000u | (S0 << 16) | ((uint32_t)(w - 1) << 10) | (S0 << 5) | qd); // q += q < 0
    }
    if (op == IR_REM) { // r = a - q * c
        mov_imm(a, S1, c & mask, sf);
        put(a, s | 0x1B008000u | (S1 << 16) | (n << 10) | (S0 << 5) | (uint32_t)rd);
    }
    return 1;
}

static void add_sub(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    uint32_t sf = sfb(t);
    int chk = x->flags & IRF_CHECKED, sub = x->op == IR_SUB;
    if (a->madd[v]) { // rd = other ± m0 * m1
        int k = a->madd[v] - 1;
        IrIns* m = ins_of(a, x->args[k]);
        int rn = opnd(a, m->args[0], S0), rm = opnd(a, m->args[1], S1), ra = opnd(a, x->args[1 - k], S2);
        int rd = dst(a, v);
        put(a, sf | (sub ? 0x1B008000u : 0x1B000000u) | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        wrap(a, t, rd);
        dst_done(a, v, rd);
        return;
    }
    int l = x->args[0], r = x->args[1];
    if (!sub && is_const(a, l) && !is_const(a, r)) { int s = l; l = r; r = s; }
    uint32_t imm;
    int form = arith_imm(a, r, t, &imm);
    int flags = chk && !narrow(t); // adds/subs set the overflow flags
    int rn = opnd(a, l, S0);
    if (form) {
        if (form < 0) sub = !sub;
        int rd = dst(a, v);
        put(a, sf | (sub ? 0x51000000u : 0x11000000u) | (flags ? 0x20000000u : 0) | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        rn = rd;
    } else {
        int rm = opnd(a, r, S1), rd = dst(a, v);
        put(a, sf | (sub ? 0x4B000000u : 0x0B000000u) | (flags ? 0x20000000u : 0) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
        rn = rd;
    }
    if (chk && narrow(t)) check_narrow(a, t, rn);
    else if (chk) fail_if(a, ir_type_signed(t) ? C_VS : sub ? C_LO : C_HS);
    else wrap(a, t, rn);
    dst_done(a, v, rn);
}

static void mul(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int sg = ir_type_signed(t);
    int rn = opnd(a, x->args[0], S0), rm = opnd(a, x->args[1], S1), rd = dst(a, v);
    uint32_t nm = ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
    if (!(x->flags & IRF_CHECKED) || narrow(t)) {
        put(a, sfb(t) | 0x1B007C00u | nm | (uint32_t)rd);
        if (x->flags & IRF_CHECKED) check_narrow(a, t, rd);
        else wrap(a, t, rd);
    } else if (!is64(t)) { // the exact 64-bit product must be its 32-bit extension
        put(a, (sg ? 0x9B207C00u : 0x9BA07C00u) | nm | (uint32_t)rd); // smull / umull
        put(a, 0xEB20001Fu | ((uint32_t)rd << 16) | ((sg ? 6u : 2u) << 13) | ((uint32_t)rd << 5));
        fail_if(a, C_NE);
    } else if (sg) { // the high half must be the low half's sign
        put(a, 0x9B407C00u | nm | S2);                  // smulh x15
        put(a, 0x9B007C00u | nm | (uint32_t)rd);
        put(a, 0xEB80001Fu | ((uint32_t)rd << 16) | (63u << 10) | (S2 << 5)); // cmp x15, rd, asr #63
        fail_if(a, C_NE);
    } else {
        put(a, 0x9BC07C00u | nm | S2);                  // umulh x15
        put(a, 0x9B007C00u | nm | (uint32_t)rd);
        fail_if_zero(a, S2, 1, 1);
    }
    dst_done(a, v, rd);
}

static void div_rem(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    uint32_t sf = sfb(t);
    int sg = ir_type_signed(t), chk = x->flags & IRF_CHECKED;
    if (is_const(a, x->args[1])) {
        uint64_t c = ins_of(a, x->args[1])->imm;
        int64_t sd = is64(t) ? (int64_t)c : (int64_t)(int32_t)(uint32_t)c;
        if (c && !(sg && sd == -1)) {
            int rn = opnd(a, x->args[0], S2), rd = dst(a, v);
            const_divide(a, x->op, rd, rn, c, t);
            dst_done(a, v, rd);
            return;
        }
    }
    int rn = opnd(a, x->args[0], S0), rm = opnd(a, x->args[1], S1);
    if (chk) {
        fail_if_zero(a, rm, is64(t), 0);
        if (sg && !narrow(t)) { // MIN / -1: a - 1 overflows exactly when a is MIN
            put(a, sf | 0x3100001Fu | (1u << 10) | ((uint32_t)rm << 5));                  // cmn rm, #1
            put(a, sf | 0x7A400800u | (1u << 16) | ((uint32_t)C_EQ << 12) | ((uint32_t)rn << 5)); // ccmp rn, #1, #0, eq
            fail_if(a, C_VS);
        }
    }
    uint32_t div = sf | (sg ? 0x1AC00C00u : 0x1AC00800u) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
    int rd = dst(a, v);
    if (x->op == IR_DIV) {
        put(a, div | (uint32_t)rd);
        if (sg && narrow(t)) {
            if (chk) check_narrow(a, t, rd); // MIN / -1 leaves the range
            else wrap(a, t, rd);
        }
    } else {
        put(a, div | S2);
        if (chk && sg && narrow(t)) check_narrow(a, t, S2);
        put(a, sf | 0x1B008000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 10) | (S2 << 5) | (uint32_t)rd); // msub
    }
    dst_done(a, v, rd);
}

static void shift(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int sf = is64(t), sg = ir_type_signed(t), bits = ir_type_bits(t);
    int rn = opnd(a, x->args[0], S0);
    int c = x->args[1];
    if (is_const(a, c) && ins_of(a, c)->imm < (uint64_t)bits) {
        uint32_t k = (uint32_t)ins_of(a, c)->imm, w = sf ? 64 : 32;
        int rd = dst(a, v);
        if (x->op == IR_SHL) // ubfm rd, rn, #(-k mod w), #(w - 1 - k)
            put(a, (sf ? 0xD3400000u : 0x53000000u) | (((w - k) & (w - 1)) << 16) | ((w - 1 - k) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        else if (sg)
            put(a, (sf ? 0x9340FC00u : 0x13007C00u) | (k << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
        else
            put(a, (sf ? 0xD340FC00u : 0x53007C00u) | (k << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
        if (x->op == IR_SHL) wrap(a, t, rd);
        dst_done(a, v, rd);
        return;
    }
    int rm = opnd(a, c, S1);
    if (x->flags & IRF_CHECKED) { // a negative count is a large unsigned one
        put(a, sfb(t) | 0x7100001Fu | ((uint32_t)bits << 10) | ((uint32_t)rm << 5));
        fail_if(a, C_HS);
    }
    int rd = dst(a, v);
    uint32_t op = x->op == IR_SHL ? 0x1AC02000u : sg ? 0x1AC02800u : 0x1AC02400u;
    put(a, sfb(t) | op | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
    if (x->op == IR_SHL) wrap(a, t, rd);
    dst_done(a, v, rd);
}

static void logic(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    uint32_t sf = sfb(t);
    static const uint32_t reg_op[] = { [IR_AND] = 0x0A000000u, [IR_OR] = 0x2A000000u, [IR_XOR] = 0x4A000000u };
    static const uint32_t imm_op[] = { [IR_AND] = 0x12000000u, [IR_OR] = 0x32000000u, [IR_XOR] = 0x52000000u };
    int l = x->args[0], r = x->args[1];
    if (is_const(a, l) && !is_const(a, r)) { int s = l; l = r; r = s; }
    uint32_t field = is_const(a, r) ? logical_imm_field(ins_of(a, r)->imm, is64(t)) : 0;
    int rn = opnd(a, l, S0);
    int rd;
    if (field) {
        rd = dst(a, v);
        put(a, sf | imm_op[x->op] | (field << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
    } else {
        int rm = opnd(a, r, S1);
        rd = dst(a, v);
        put(a, sf | reg_op[x->op] | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
    }
    dst_done(a, v, rd);
}

static void unary(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    uint32_t sf = sfb(t);
    int rn = opnd(a, x->args[0], S0), rd = dst(a, v);
    if (x->op == IR_NEG) {
        int flags = (x->flags & IRF_CHECKED) && !narrow(t);
        put(a, sf | (flags ? 0x6B0003E0u : 0x4B0003E0u) | ((uint32_t)rn << 16) | (uint32_t)rd);
        if (flags) fail_if(a, C_VS);
        else if (x->flags & IRF_CHECKED) check_narrow(a, t, rd);
        else wrap(a, t, rd);
    } else if (t == IT_BOOL || t == IT_U8 || t == IT_U16) { // eor with the type's ones
        uint64_t ones = t == IT_BOOL ? 1 : t == IT_U8 ? 0xFF : 0xFFFF;
        put(a, 0x52000000u | (logical_imm_field(ones, 0) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
    } else {
        put(a, sf | 0x2A2003E0u | ((uint32_t)rn << 16) | (uint32_t)rd); // mvn
    }
    dst_done(a, v, rd);
}

// Truncate, then extend by the target's signedness (ir_wrap), in register form.
static void cast(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType to = (IrType)x->type, from = (IrType)ins_of(a, x->args[0])->type;
    int fb = ir_type_bits(from), tb = ir_type_bits(to);
    int rn = opnd(a, x->args[0], S0), rd = dst(a, v);
    if (tb == 64 && fb < 64) {
        if (ir_type_signed(from)) put(a, 0x93407C00u | ((uint32_t)rn << 5) | (uint32_t)rd); // sxtw
        else put(a, 0x2A0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd);                      // mov w
    } else if (tb < 32 && !(fb < tb && (!ir_type_signed(from) || ir_type_signed(to))) &&
               !(fb == tb && ir_type_signed(from) == ir_type_signed(to))) {
        ext(a, to, rd, rn);
    } else {
        mov_reg(a, rd, rn); // the register form already holds the result
    }
    dst_done(a, v, rd);
}

static void call(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    Move m[8];
    for (int k = 0; k < x->nargs; k++) m[k] = move_of(a, k, x->args[k]);
    parallel_move(a, m, x->nargs);
    if (x->fn) {
        Sym* s = x->fn;
        bl_to(a, fn_mangle(s->name, s->len, s->type, s->decl, NULL, 0, 0));
    } else {
        bl_to(a, x->name);
    }
    if (loc(a, v) == NOREG) return;
    int rd = dst(a, v);
    IrType t = (IrType)x->type;
    if (narrow(t)) ext(a, t, rd, 0); // an AST-emitted callee may leave it unwrapped
    else mov_reg(a, rd, 0);
    dst_done(a, v, rd);
}

// --- emission ---------------------------------------------------------------------------

static void line_row(A64* a, Node* at) {
    if (!at || at == a->last_at || at->line <= 0) return;
    a->last_at = at;
    obj_line(a->o, here(a), at->line, at->col, at->path ? at->path : a->o->d->path);
}

static int next_block(A64* a, int b) {
    int i = a->lix[b] + 1;
    return i < a->fn->nblocks ? a->order[i] : -1;
}

static void jump(A64* a, int from, int to) {
    if (next_block(a, from) != to) branch_add(a, 0x14000000u, to, 0);
}

static void phi_moves(A64* a, int b) {
    IrFunc* fn = a->fn;
    int s = fn->blocks[b].succ[0];
    IrBlock* sb = &fn->blocks[s];
    int p = 0;
    while (sb->preds[p] != b) p++;
    int n = 0;
    while (n < sb->nins && fn->ins[sb->ins[n]].op == IR_PHI) n++;
    if (!n) return;
    Move* m = xcalloc((size_t)n, sizeof(Move));
    int k = 0;
    for (int i = 0; i < n; i++) {
        int phi = sb->ins[i];
        int d = loc(a, phi);
        if (d != NOREG) m[k++] = move_of(a, d, fn->ins[phi].args[p]);
    }
    parallel_move(a, m, k);
    free(m);
}

static void terminator(A64* a, int b, int v) {
    IrFunc* fn = a->fn;
    IrIns* x = ins_of(a, v);
    IrBlock* bb = &fn->blocks[b];
    if (x->op == IR_JMP) {
        phi_moves(a, b);
        jump(a, b, bb->succ[0]);
        return;
    }
    if (x->op == IR_RET) {
        if (x->nargs) {
            int r = x->args[0];
            if (is_const(a, r)) mov_imm(a, 0, ins_of(a, r)->imm, is64((IrType)ins_of(a, r)->type));
            else if (a->reg[r] >= 0) mov_reg(a, 0, a->reg[r]);
            else ld_slot(a, 0, a->slot[r]);
        }
        epilogue(a);
        return;
    }
    int c = x->args[0];
    Cond k = a->fused[c] ? compare(a, c, 1) : (Cond){ 1, 0, (uint8_t)opnd(a, c, S0), 0, 1 };
    int t = bb->succ[0], f = bb->succ[1], next = next_block(a, b);
    if (f == next) {
        branch_if(a, k, t);
    } else if (t == next) {
        branch_if(a, invert(k), f);
    } else {
        branch_if(a, k, t);
        branch_add(a, 0x14000000u, f, 0);
    }
}

static void emit_block(A64* a, int b) {
    IrFunc* fn = a->fn;
    IrBlock* bb = &fn->blocks[b];
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = &fn->ins[v];
        line_row(a, x->at);
        if (a->fused[v]) continue;
        switch (x->op) {
        case IR_CONST: case IR_PARAM: case IR_PHI:
            break;
        case IR_ADD: case IR_SUB:
            add_sub(a, v);
            break;
        case IR_MUL:
            mul(a, v);
            break;
        case IR_DIV: case IR_REM:
            div_rem(a, v);
            break;
        case IR_SHL: case IR_SHR:
            shift(a, v);
            break;
        case IR_AND: case IR_OR: case IR_XOR:
            logic(a, v);
            break;
        case IR_NEG: case IR_NOT:
            unary(a, v);
            break;
        case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
            Cond k = compare(a, v, 0);
            int rd = dst(a, v);
            put(a, 0x1A9F07E0u | ((uint32_t)(k.cond ^ 1) << 12) | (uint32_t)rd); // cset
            dst_done(a, v, rd);
            break;
        }
        case IR_CAST:
            cast(a, v);
            break;
        case IR_CALL:
            call(a, v);
            break;
        default:
            terminator(a, b, v);
            break;
        }
    }
}

// Entry: incoming parameters, a narrow one wrapped where it arrived (an
// AST-emitted caller may pass it unwrapped), then moved to their homes.
static void params(A64* a) {
    IrBlock* bb = &a->fn->blocks[0];
    Move m[8];
    int n = 0;
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = ins_of(a, v);
        if (x->op != IR_PARAM || loc(a, v) == NOREG) continue;
        if (narrow((IrType)x->type)) ext(a, (IrType)x->type, (int)x->imm, (int)x->imm);
        m[n++] = (Move){ loc(a, v), (int)x->imm, 0, 0, 1 };
    }
    parallel_move(a, m, n);
}

static int is_loop_head(A64* a, int b) {
    IrBlock* bb = &a->fn->blocks[b];
    for (int p = 0; p < bb->npreds; p++)
        if (a->lix[bb->preds[p]] >= a->lix[b]) return 1;
    return 0;
}

// Emit the body; 0 when a conditional branch is out of range.
static int emit_body(A64* a) {
    IrFunc* fn = a->fn;
    a->nbr = 0;
    a->fail_at = UINT32_MAX;
    a->last_at = NULL;
    line_row(a, fn->decl);
    prologue(a);
    params(a);
    for (int i = 0; i < fn->nblocks; i++) {
        int b = a->order[i];
        if (i && is_loop_head(a, b))
            while (here(a) % LOOP_ALIGN) put(a, 0xD503201Fu);
        a->label[b] = here(a);
        emit_block(a, b);
    }
    for (int i = 0; i < a->nbr; i++) {
        if (a->br[i].target != FAIL) continue;
        a->fail_at = here(a);
        bl_to(a, "__ace$arithmetic_fail");
        break;
    }
    for (int i = 0; i < a->nbr; i++) {
        Branch* br = &a->br[i];
        uint32_t to = br->target == FAIL ? a->fail_at : a->label[br->target];
        int64_t d = ((int64_t)to - (int64_t)br->site) / 4;
        uint32_t w = br->w;
        if (br->imm19) {
            if (d < -(1 << 18) || d >= (1 << 18)) return 0;
            w |= ((uint32_t)d & 0x7FFFFu) << 5;
        } else {
            w |= (uint32_t)d & 0x3FFFFFFu;
        }
        buf_patch32(a->t, br->site, w);
    }
    return 1;
}

void arm64_func(Obj* o, IrFunc* fn, const char* name, uint32_t len, int is_main) {
    A64 a;
    memset(&a, 0, sizeof a);
    a.o = o;
    a.fn = fn;
    a.t = obj_text(o);
    a.is_main = is_main;
    split_critical_edges(fn);
    layout(&a);
    count_uses(&a);
    liveness(&a);
    hints(&a);
    allocate(&a);
    a.slot_base = 16u + 8u * (uint32_t)a.ncs;
    a.frame = (a.slot_base + 8u * (uint32_t)a.nslots + 15u) & ~15u;
    a.has_frame = a.ncalls || a.nslots || a.ncs;
    a.label = xcalloc((size_t)fn->nblocks, sizeof(uint32_t));

    while (a.t->len % FUNC_ALIGN) put(&a, 0xD503201Fu);
    uint32_t start = here(&a);
    sym_define(&o->syms, o->secs, name, len, is_main, &o->secs[SEC_TEXT]);
    size_t nfix = o->nfix;
    int nlines = o->nlines;
    LineRow last = nlines ? o->lines[nlines - 1] : (LineRow){ 0 };
    if (!emit_body(&a)) { // over 1 MiB: again with far conditional branches
        a.t->len = start;
        o->nfix = nfix;
        o->nlines = nlines;
        if (nlines) o->lines[nlines - 1] = last;
        a.far = 1;
        emit_body(&a);
    }
    obj_debug_fn(o, start, here(&a) - start, fn->decl->str, fn->decl->len);

    free(a.order); free(a.lix); free(a.bfirst); free(a.blast); free(a.idx);
    free(a.nuse); free(a.fused); free(a.madd); free(a.start); free(a.end);
    free(a.hint); free(a.phi_of); free(a.calls); free(a.reg); free(a.slot);
    free(a.label); free(a.br);
}
