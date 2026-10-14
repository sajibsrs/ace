// arm64.c — the AArch64 target of the IR backend (spec/compiler.md Compiler #2,
// passes 7-10): instruction selection, leaving
// SSA, linear-scan register allocation, the frame, and encoding into the
// shared Obj, under the platform ABI (emit.c header).
//
// Values: a 64-bit type lives in a whole x register. A narrower one lives in
// the low 32 bits, extended to 32 by its signedness (the Apple PCS argument
// form); bits 63:32 are unspecified, so 32-bit instructions operate on it and
// only a widening cast extends it. A float lives in the low bits of a v
// register (s for f32, d for f64); a vector (ir.h), in the whole q register.
// A vector never takes a callee-saved register across a call, whose low 64
// bits alone the callee preserves, and spills to a pair of slots.
//
// Registers: x0-x14 and x19-x28 are allocated. x15, x16, and x17 are scratch
// for spilled and constant operands, check temporaries, and far spill slots;
// x18 belongs to the platform. v0-v28 are allocated; v29-v31 are the float
// scratch. Register numbers 0-31 name x registers and V + n names vn, in one
// linear scan; a value takes a register of its type's class.
//
// Allocation (regalloc.c, Poletto & Sarkar; this file supplies the register
// sets, hints, and fused values): each value has one interval, the hull of the
// positions where it is live in block layout order (a reverse postorder),
// found by walking up from each use to the definition. Constants take no
// register: each use materializes them. An interval that crosses a call takes
// a callee-saved register or a stack slot. A phi's operands move into it in
// parallel at the end of each predecessor; critical edges into a phi's block
// are split first, and hints steer a phi and its operands to one register so
// most of those moves vanish. A cast whose register form is its operand's
// shares the operand's location, costing no instruction.
//
// Float constants are rematerialized too: fmov from zero or an 8-bit
// immediate, else a literal load from the function's pool after its code.
#include "backend.h"
#include "ir.h"
#include "regalloc.h"

#include <stdio.h>

enum { S0 = 16, S1 = 17, S2 = 15 }; // scratch, never allocated
enum { V = 32, FS0 = V + 31, FS1 = V + 30, FS2 = V + 29 }; // v registers; the float scratch
enum { C_EQ, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE };
enum { NOREG = -1, SLOT = 64 }; // a move location: register r, or SLOT + stack slot
// branch targets: the arithmetic, null, and call contract failure stubs, then
// the located check stubs (LFAIL - k for the function's stub k)
enum { FAIL = -1, NFAIL = -2, CFAIL = -3, LFAIL = -4 };

typedef struct { uint32_t site, w; int target; uint8_t imm19; } Branch;

// A branch condition: the flags satisfy cond, or (cbz) reg is zero / nonzero.
typedef struct { uint8_t cbz, cond, reg, sf, nz; } Cond;

// is_const: 1 moves imm, 2 moves the address sp + imm (a stack object)
// q: a vector, moved whole (never a constant)
typedef struct { int dst, src; uint64_t imm; uint8_t is_const, sf, q; } Move;

typedef struct {
    Obj*      o;
    IrFunc*   fn;
    Buf*      t;
    int       far;
    Ra        ra;      // layout, liveness, and allocation (regalloc.h)
    uint8_t*  madd;    // ADD/SUB fused with a MUL operand: 1 + its index
    uint32_t  out;     // outgoing bytes at [sp]: stack arguments, then call memory
    struct Loc* pin;   // where each parameter leaf arrives
    struct Loc* rin;   // where each result leaf leaves
    uint32_t  nsave, save_at; // the save area for incoming words and addresses
    uint32_t* obj;     // SLOT value -> its object's offset from sp
    // allocation
    int       ncs;
    int8_t    cs[18];  // callee-saved registers used, ascending: x19-x28, then d8-d15
    uint32_t  frame, slot_base;
    int       has_frame;
    uint32_t  epi;     // text offset + 1 of the function's one epilogue, once emitted
    // emission
    uint32_t* label;   // block -> text offset
    Branch*   br;
    int       nbr, cbr;
    uint64_t* pool;    // float constants of the function, 8 bytes each
    int       npool, cpool;
    uint32_t* psite;   // literal loads: (text offset, pool index) pairs
    int       npsite, cpsite;
    Node*     last_at;
    IrIns**   stub;    // located check stubs: a failing site and its diagnostic, one per pair
    uint32_t* stub_at; // each stub's text offset
    int       nstub, cstub;
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
// a value no register holds: rebuilt at each use (a constant, sp + a stack object's offset)
static int narrow(IrType t) { return t != IT_BOOL && ir_type_bits(t) < 32; }
static int is_fp(IrType t) { return ir_type_float(t); }
static int fp_reg(int r) { return r >= V && r < SLOT; }
static int is_vec(IrType t) { return ir_type_vector(t); }
static int fp_val(A64* a, int v) { IrType t = (IrType)ins_of(a, v)->type; return is_fp(t) || is_vec(t); }
static int vec_val(A64* a, int v) { return is_vec((IrType)ins_of(a, v)->type); }
static uint32_t R(int r) { return (uint32_t)r & 31; } // a register's encoding field
static uint32_t ftype(IrType t) { return t == IT_F64 ? 0x00400000u : 0; } // the d form of an s instruction

// Whether an integer cast's register form is its operand's: no instruction.
static int cast_in_place(IrType to, IrType from) {
    if (is_fp(to) || is_fp(from) || is_vec(to) || is_vec(from)) return 0;
    int fb = ir_type_bits(from), tb = ir_type_bits(to);
    if (tb == 64 && fb < 64) return !ir_type_signed(from) && fb < 32; // a u8, u16, or bool is held zero-extended
    return !(tb < 32 && !(fb < tb && (!ir_type_signed(from) || ir_type_signed(to))) &&
             !(fb == tb && ir_type_signed(from) == ir_type_signed(to)));
}

// --- encoding -------------------------------------------------------------------------

static void put(A64* a, uint32_t w) { buf_put32(a->t, w); }
static uint32_t here(A64* a) { return (uint32_t)a->t->len; }

static void mem(A64* a, uint32_t op, int size, int rt, int rn, int64_t off);

// mov vd.16b, vn.16b: a whole vector
static void mov_q(A64* a, int rd, int rn) {
    if (rd != rn) put(a, 0x4EA01C00u | (R(rn) << 16) | (R(rn) << 5) | R(rd));
}

// rd = rn in either class; across classes, the bits (fmov d, x / x, d)
static void mov_reg(A64* a, int rd, int rn) {
    if (rd == rn) return;
    if (fp_reg(rd) && fp_reg(rn)) put(a, 0x1E604000u | (R(rn) << 5) | R(rd));
    else if (fp_reg(rd)) put(a, 0x9E670000u | (R(rn) << 5) | R(rd));
    else if (fp_reg(rn)) put(a, 0x9E660000u | (R(rn) << 5) | R(rd));
    else put(a, 0xAA0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd);
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

// rd = rn + imm (either may be sp), imm below 2^24
static void add_imm(A64* a, int rd, int rn, uint32_t imm) {
    uint32_t lo = imm & 0xFFFu, hi = imm >> 12;
    if (hi) {
        put(a, 0x91400000u | (hi << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
        rn = rd;
    }
    if (lo || !hi) put(a, 0x91000000u | (lo << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

static uint32_t slot_off(A64* a, int s) { return a->slot_base + 8u * (uint32_t)s; }

// A slot holds 8 bytes of either class: ldr/str x, or d for a v register.
static void ld_slot(A64* a, int rt, int s) { // a far slot through rt, or x15 for a v register
    uint32_t off = slot_off(a, s), op = fp_reg(rt) ? 0xFD400000u : 0xF9400000u;
    if (off / 8 <= 4095) {
        put(a, op | ((off / 8) << 10) | (31u << 5) | R(rt));
        return;
    }
    uint32_t base = fp_reg(rt) ? S2 : (uint32_t)rt;
    put(a, 0x914003E0u | ((off >> 12) << 10) | base); // add base, sp, #hi, lsl 12
    put(a, op | (((off & 0xFFFu) / 8) << 10) | (base << 5) | R(rt));
}

// A vector's pair of slots from s: ldr/str q, a far one through x15.
static void ld_slotq(A64* a, int rt, int s) { mem(a, 0x3DC00000u, 16, rt, 31, slot_off(a, s)); }
static void st_slotq(A64* a, int rt, int s) { mem(a, 0x3D800000u, 16, rt, 31, slot_off(a, s)); }

static void st_slot(A64* a, int rt, int s) { // a far slot is addressed through x15
    uint32_t off = slot_off(a, s), op = fp_reg(rt) ? 0xFD000000u : 0xF9000000u;
    if (off / 8 <= 4095) {
        put(a, op | ((off / 8) << 10) | (31u << 5) | R(rt));
        return;
    }
    put(a, 0x914003E0u | ((off >> 12) << 10) | (uint32_t)S2);
    put(a, op | (((off & 0xFFFu) / 8) << 10) | ((uint32_t)S2 << 5) | R(rt));
}

// vd = the float with bit pattern bits: fmov from zero or an 8-bit immediate,
// else a literal load from the pool (in far code, the bits through x15)
static void fmov_const(A64* a, int rd, uint64_t bits, int is64) {
    uint32_t d = R(rd);
    if (!bits) {
        put(a, (is64 ? 0x9E6703E0u : 0x1E2703E0u) | d); // fmov d, xzr / s, wzr
        return;
    }
    int imm8 = fmov_imm8(bits, is64);
    if (imm8 >= 0) {
        put(a, (is64 ? 0x1E601000u : 0x1E201000u) | ((uint32_t)imm8 << 13) | d);
        return;
    }
    if (a->far) {
        mov_imm(a, S2, bits, is64);
        put(a, (is64 ? 0x9E670000u : 0x1E270000u) | (S2 << 5) | d);
        return;
    }
    int k = 0;
    while (k < a->npool && a->pool[k] != bits) k++;
    if (k == a->npool) {
        if (a->npool == a->cpool) {
            a->cpool = a->cpool ? a->cpool * 2 : 8;
            a->pool = realloc(a->pool, (size_t)a->cpool * sizeof(uint64_t));
            if (!a->pool) oom();
        }
        a->pool[a->npool++] = bits;
    }
    if (a->npsite + 2 > a->cpsite) {
        a->cpsite = a->cpsite ? a->cpsite * 2 : 16;
        a->psite = realloc(a->psite, (size_t)a->cpsite * sizeof(uint32_t));
        if (!a->psite) oom();
    }
    a->psite[a->npsite++] = here(a);
    a->psite[a->npsite++] = (uint32_t)k;
    put(a, (is64 ? 0x5C000000u : 0x1C000000u) | d); // ldr d / s, literal
}

// the 32-bit extension that wraps a narrow value in place
static uint32_t ext_of(IrType t) {
    switch (t) {
    case IT_I8: return 0x13001C00u;  // sxtb
    case IT_I16: return 0x13003C00u; // sxth
    case IT_U8: case IT_BOOL: return 0x53001C00u; // uxtb
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

static void bl_extern(A64* a, const char* name) {
    reloc_add(&a->o->secs[SEC_TEXT], here(a), sym_intern(&a->o->syms, name, (uint32_t)strlen(name)), 1, 2, 1, RM_BRANCH26);
    put(a, 0x94000000u);
}

static const char* fail_routine(A64* a, int target) {
    if (target == FAIL) {
        a->o->arithmetic_fail_used = 1;
        return "__ace$arithmetic_fail";
    }
    if (target == CFAIL) {
        a->o->contract_fail_used = 1;
        return "__ace$contract_fail";
    }
    a->o->null_fail_used = 1;
    return "__ace$null_fail";
}

// Branch to block target when c holds. Far code (over 1 MiB) skips an
// unconditional branch on the inverse instead.
static void branch_if(A64* a, Cond c, int target) {
    const char* fail = target < 0 && target > LFAIL ? fail_routine(a, target) : NULL;
    if (!a->far) {
        branch_add(a, cond_word(c), target, 1);
        return;
    }
    put(a, cond_word(invert(c)) | (2u << 5)); // over the next instruction
    if (fail) bl_to(a, fail);
    else branch_add(a, 0x14000000u, target, 0);
}

static void null_fail_if_zero(A64* a, int reg) {
    branch_if(a, (Cond){ 1, 0, (uint8_t)reg, 1, 0 }, NFAIL);
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

// The register holding v for reading; a constant or spilled value goes to
// scratch, of v's class.
static int opnd(A64* a, int v, int scratch) {
    IrIns* x = ins_of(a, v);
    if (x->op == IR_CONST) {
        if (is_fp((IrType)x->type)) fmov_const(a, scratch, x->imm, x->type == IT_F64);
        else mov_imm(a, scratch, x->imm, is64((IrType)x->type));
        return scratch;
    }
    if (x->op == IR_SLOT) {
        add_imm(a, scratch, 31, a->obj[v]);
        return scratch;
    }
    if (a->ra.reg[v] >= 0) return a->ra.reg[v];
    if (vec_val(a, v)) ld_slotq(a, scratch, a->ra.slot[v]);
    else ld_slot(a, scratch, a->ra.slot[v]);
    return scratch;
}

// The register v's result is computed into; dst_done stores a spilled one.
static int dst(A64* a, int v) { return a->ra.reg[v] >= 0 ? a->ra.reg[v] : fp_val(a, v) ? FS0 : S0; }

static void dst_done(A64* a, int v, int r) {
    if (a->ra.reg[v] < 0 && a->ra.slot[v] >= 0) {
        if (vec_val(a, v)) st_slotq(a, r, a->ra.slot[v]);
        else st_slot(a, r, a->ra.slot[v]);
    }
}

static int loc(A64* a, int v) { // a move location, or NOREG for an unused value
    if (a->ra.reg[v] >= 0) return a->ra.reg[v];
    return a->ra.slot[v] >= 0 ? SLOT + a->ra.slot[v] : NOREG;
}

// A constant into a v register is a float; into a slot, any constant is its bits.
static void move1(A64* a, Move m) {
    if (m.q) {
        if (m.dst < SLOT && m.src < SLOT) mov_q(a, m.dst, m.src);
        else if (m.dst < SLOT) ld_slotq(a, m.dst, m.src - SLOT);
        else if (m.src < SLOT) st_slotq(a, m.src, m.dst - SLOT);
        else { ld_slotq(a, FS1, m.src - SLOT); st_slotq(a, FS1, m.dst - SLOT); }
        return;
    }
    if (m.is_const) {
        int r = m.dst < SLOT ? m.dst : S1;
        if (m.is_const == 2) add_imm(a, r, 31, (uint32_t)m.imm);
        else if (fp_reg(r)) fmov_const(a, r, m.imm, m.sf);
        else mov_imm(a, r, m.imm, m.sf);
        if (m.dst >= SLOT) st_slot(a, S1, m.dst - SLOT);
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
// written; a cycle is broken through x16, or v31 for a v register (a slot's
// bits park in x16, whichever class reads them; a vector's in v31). avoid is 0 for every
// ordinary caller (an ABI register set never includes x16); asm's own moves
// pass its reserved set, since a binding may target x16 directly. Any other
// register may hold a value live across the statement, so such a cycle is
// broken with no scratch at all: a stuck move set is only whole register
// cycles, and three eors swap a move's destination with its source in place.
static void parallel_move(A64* a, Move* m, int n, uint32_t avoid) {
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
            int d = m[i].dst, q = 0; // parked whole when a vector reads it
            for (int j = 0; j < n; j++) q |= m[j].dst != NOREG && !m[j].is_const && m[j].src == d && m[j].q;
            int fp = fp_reg(d) || q, park = fp ? FS0 : S0;
            if (!fp && (avoid & (1u << park))) { // d = s, s = d; readers of either follow the swap
                uint32_t s = (uint32_t)m[i].src, rd = (uint32_t)d;
                put(a, 0xCA000000u | (s << 16) | (rd << 5) | rd);
                put(a, 0xCA000000u | (s << 16) | (rd << 5) | s);
                put(a, 0xCA000000u | (s << 16) | (rd << 5) | rd);
                m[i].dst = NOREG;
                left--;
                for (int j = 0; j < n; j++)
                    if (m[j].dst != NOREG && !m[j].is_const)
                        m[j].src = m[j].src == d ? (int)s : m[j].src == (int)s ? d : m[j].src;
                break;
            }
            move1(a, (Move){ park, d, 0, 0, 1, (uint8_t)q });
            for (int j = 0; j < n; j++)
                if (m[j].dst != NOREG && !m[j].is_const && m[j].src == d) m[j].src = park;
            break;
        }
    }
}

static Move move_of(A64* a, int dst_loc, int v) {
    IrIns* x = ins_of(a, v);
    if (x->op == IR_CONST) return (Move){ dst_loc, 0, x->imm, 1, (uint8_t)is64((IrType)x->type), 0 };
    if (x->op == IR_SLOT) return (Move){ dst_loc, 0, a->obj[v], 2, 1, 0 };
    return (Move){ dst_loc, loc(a, v), 0, 0, 1, (uint8_t)is_vec((IrType)x->type) };
}

// Where a leaf of an argument, parameter, or result travels (emit.c
// assign_cc): in register reg (L_REG); on the stack at soff in size bytes
// (L_STACK); in registers from reg on that it shares with other leaves, at
// byte off of their memory at mem (L_WORD: nreg registers of rsz bytes each,
// one 8-byte x register when nreg is 0); or at off from an
// address that travels in register reg, or on the stack at soff when reg < 0
// (L_ADDR), whose memory is at mem. head marks the first leaf of a word or of
// an addressed aggregate. mem is an offset in the caller's outgoing area (a
// copy of the aggregate, a result buffer, a word) or in the callee's save
// area (the saved address or word). word marks an L_REG leaf alone in a word
// of an aggregate, whose other bits AAPCS64 leaves unspecified.
enum { L_REG, L_STACK, L_WORD, L_ADDR };
typedef struct Loc { uint8_t kind, size, head; int8_t reg; uint32_t soff, off, mem; uint8_t word, nreg, rsz; } Loc;

static uint32_t align_to(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

typedef struct { int xn, vn; uint32_t soff, area; int caller; } Cc;

// An aggregate on the stack, its leaves at their natural size: an HFA at its
// own alignment, any other at least 8-aligned (Apple arm64).
static void agg_stack(Cc* c, const IrLeaf* lv, int n, SType* agg, int hfa, Loc* out) {
    uint32_t al = sema_align(agg);
    c->soff = align_to(c->soff, al > 16 ? 16 : al < 8 && !hfa ? 8 : al ? al : 1);
    for (int k = 0; k < n; k++)
        out[k] = (Loc){ L_STACK, (uint8_t)(ir_type_bits((IrType)lv[k].type) / 8), 0, -1, c->soff + lv[k].off, 0, 0, 0, 0, 0 };
    c->soff += align_to(sema_size(agg), 8);
}

// Memory for an aggregate that travels by address: the caller's copy, or the
// callee's saved address.
static uint32_t addr_mem(Cc* c, SType* agg) {
    c->area = align_to(c->area, c->caller && sema_align(agg) > 8 ? 16 : 8);
    uint32_t m = c->area;
    c->area += c->caller ? align_to(sema_size(agg), 8) : 8;
    return m;
}

// The 8-byte word past a scalar leaf's last byte (a packed leaf may cross into
// the next word).
static uint32_t word_end(IrLeaf l) {
    uint32_t b = (uint32_t)ir_type_bits((IrType)l.type) / 8;
    return b <= 8 ? (l.off + b + 7) / 8 : l.off / 8 + 1;
}

// An aggregate in 8-byte words from x register first: a word holding one leaf
// at its start is that leaf's register, else the leaves of the words they
// span share those registers through memory.
static void agg_words(Cc* c, const IrLeaf* lv, int n, int first, Loc* out) {
    for (int k = 0; k < n;) {
        uint32_t w = lv[k].off / 8, end = word_end(lv[k]);
        int e = k + 1;
        for (; e < n && lv[e].off / 8 < end; e++)
            if (word_end(lv[e]) > end) end = word_end(lv[e]);
        int8_t reg = (int8_t)(first + (int)w);
        if (e == k + 1 && lv[k].off % 8 == 0 && end == w + 1) {
            out[k] = (Loc){ L_REG, 8, 1, reg, 0, 0, 0, 1, 0, 0 };
        } else {
            uint32_t m = c->area = align_to(c->area, 8);
            c->area += 8 * (end - w);
            for (int j = k; j < e; j++)
                out[j] = (Loc){ L_WORD, 8, (uint8_t)(j == k), reg, 0, lv[j].off - 8 * w, m, 0, (uint8_t)(end - w), 8 };
        }
        k = e;
    }
}

// Assign the leaves t[0..n), grouped by sig's parts (each leaf alone when sig
// is NULL), from x0-x7, v0-v7, and the stack; scalars from leaf varargs_from
// on take 8-byte stack slots. Returns the stack bytes; c->area ends past the
// memory the leaves need.
static uint32_t assign_leaves(Cc* c, const IrType* t, int n, const IrSig* sig, int varargs_from, Loc* out) {
    int np = sig ? sig->nparts : n;
    for (int p = 0; p < np; p++) {
        IrPart pt = sig ? sig->parts[p] : (IrPart){ NULL, p, 1, 0 };
        Loc* o = out + pt.first;
        if (!pt.agg || pt.addr) { // a by-value aggregate's address travels as a scalar (AAPCS64)
            IrType tt = t[pt.first];
            uint32_t w = (uint32_t)ir_type_bits(tt) / 8;
            int* rn = is_fp(tt) ? &c->vn : &c->xn;
            if (varargs_from >= 0 && pt.first >= varargs_from) w = 8;
            else if (*rn < 8) {
                o[0] = (Loc){ L_REG, 8, 1, (int8_t)((rn == &c->vn ? V : 0) + (*rn)++), 0, 0, 0, 0, 0, 0 };
                continue;
            }
            if (w < 4) w = 4;
            c->soff = align_to(c->soff, w);
            o[0] = (Loc){ L_STACK, (uint8_t)w, 1, -1, c->soff, 0, 0, 0, 0, 0 };
            c->soff += w;
            continue;
        }
        IrLeaf lv[IR_MAX_LEAVES];
        SType* bad;
        ir_leaves(pt.agg, lv, &bad);
        uint32_t size = sema_size(pt.agg);
        IrType bt;
        int h = ir_hfa(pt.agg, &bt);
        if (h) {
            if (c->vn + h > 8) {
                agg_stack(c, lv, pt.n, pt.agg, 1, o);
                c->vn = 8;
            } else if (pt.n == h && t[pt.first] == bt) { // its leaves are its elements
                for (int k = 0; k < pt.n; k++) o[k] = (Loc){ L_REG, 8, 1, (int8_t)(V + c->vn++), 0, 0, 0, 0, 0, 0 };
            } else { // an overlay's words, through memory into its elements' registers
                uint32_t m = c->area = align_to(c->area, 8);
                c->area += align_to(size, 8);
                for (int k = 0; k < pt.n; k++)
                    o[k] = (Loc){ L_WORD, 8, (uint8_t)(k == 0), (int8_t)(V + c->vn), 0, lv[k].off, m, 0, (uint8_t)h,
                                  (uint8_t)(ir_type_bits(bt) / 8) };
                c->vn += h;
            }
        } else if (size > 16) {
            uint32_t m = addr_mem(c, pt.agg);
            int8_t reg = -1;
            uint32_t so = 0;
            if (c->xn < 8) {
                reg = (int8_t)c->xn++;
            } else {
                so = c->soff = align_to(c->soff, 8);
                c->soff += 8;
            }
            for (int k = 0; k < pt.n; k++) o[k] = (Loc){ L_ADDR, 8, (uint8_t)(k == 0), reg, so, lv[k].off, m, 0, 0, 0 };
        } else if (c->xn + (int)((size + 7) / 8) > 8) {
            agg_stack(c, lv, pt.n, pt.agg, 0, o);
            c->xn = 8;
        } else {
            agg_words(c, lv, pt.n, c->xn, o);
            c->xn += (int)((size + 7) / 8);
        }
    }
    return c->soff;
}

// The result's leaves: a scalar in x0 or v0; an aggregate as an argument
// would travel from the first registers, except that one over 16 bytes (not
// an HFA) is stored through the address in x8.
static void assign_result(Cc* c, const IrType* t, int n, SType* agg, Loc* out) {
    if (!n) return;
    if (!agg) {
        out[0] = (Loc){ L_REG, 8, 1, (int8_t)(is_fp(t[0]) ? V : 0), 0, 0, 0, 0, 0, 0 };
        return;
    }
    if (!ir_hfa(agg, NULL) && sema_size(agg) > 16) {
        IrLeaf lv[IR_MAX_LEAVES];
        SType* bad;
        ir_leaves(agg, lv, &bad);
        uint32_t m = addr_mem(c, agg);
        for (int k = 0; k < n; k++) out[k] = (Loc){ L_ADDR, 8, (uint8_t)(k == 0), 8, 0, lv[k].off, m, 0, 0, 0 };
        return;
    }
    Cc r = { 0, 0, 0, c->area, c->caller };
    IrSig one = { &(IrPart){ agg, 0, n, 0 }, 1, { NULL, 0, 0, 0 } };
    assign_leaves(&r, t, n, &one, -1, out);
    c->area = r.area;
}

// A call's plan: its arguments' leaves (after a calli's target) and its
// result's, with the outgoing bytes they need at [sp]: the stack arguments,
// then the memory of copies, words, and a result buffer.
typedef struct {
    Loc*     arg;
    Loc*     res;
    int      k0, n, nres;
    uint32_t bytes;
} Plan;

static void plan_call(IrFunc* fn, IrIns* x, Plan* p) {
    p->k0 = x->op == IR_CALLI;
    p->n = x->nargs - p->k0 - ((x->flags & IRF_SRET) != 0); // a result memory's address travels in x8
    IrType* t = xcalloc((size_t)p->n, sizeof(IrType));
    for (int k = 0; k < p->n; k++) t[k] = (IrType)fn->ins[x->args[p->k0 + k]].type;
    p->arg = xcalloc((size_t)p->n, sizeof(Loc));
    Cc c = { 0, 0, 0, 0, 1 };
    uint32_t base = align_to(assign_leaves(&c, t, p->n, x->sig, (x->flags & IRF_VARARGS) ? x->aux : -1, p->arg), 16);
    free(t);
    SType* agg = x->sig ? x->sig->ret.agg : NULL;
    IrLeaf lv[IR_MAX_LEAVES];
    IrType rt[IR_MAX_LEAVES];
    p->nres = 0;
    if (agg) {
        SType* bad;
        p->nres = ir_leaves(agg, lv, &bad);
        for (int k = 0; k < p->nres; k++) rt[k] = (IrType)lv[k].type;
    } else if (x->type != IT_VOID) {
        p->nres = 1;
        rt[0] = (IrType)x->type;
    }
    p->res = xcalloc((size_t)(p->nres ? p->nres : 1), sizeof(Loc));
    assign_result(&c, rt, p->nres, agg, p->res);
    for (int k = 0; k < p->n; k++) p->arg[k].mem += base; // the memory follows the stack arguments
    for (int k = 0; k < p->nres; k++) p->res[k].mem += base;
    p->bytes = align_to(base + c.area, 16);
}

static void plan_free(Plan* p) {
    free(p->arg);
    free(p->res);
}

static int is_call(int op) { return op == IR_CALL || op == IR_CALLI; }

// --- layout, liveness, allocation --------------------------------------------------------

static int is_compare(int op) { return op >= IR_EQ && op <= IR_GE; }

static int access_bytes(A64* a, IrIns* x) {
    IrType t = (IrType)(x->op == IR_LOAD ? x->type : ins_of(a, x->args[1])->type);
    return ir_type_bits(t) / 8;
}

// Whether global x is a C global, whose storage another image may own, so its
// address comes from the GOT rather than a direct page reference.
static int got_data(IrIns* x) { return x->sym && x->sym->kind == SYM_VAR && x->sym->is_extern; }

// Whether load or store u computes its address itself: from a global (adrp
// and a page offset), or from an unchecked pointer offset (a folded constant,
// or a register index scaled by the access size).
static int fold_address(A64* a, int u) {
    IrIns* x = ins_of(a, u);
    int p = x->args[0];
    IrIns* px = ins_of(a, p);
    if (a->ra.nuse[p] != 1 || px->block != x->block || (x->flags & IRF_CHECKED)) return 0;
    if (px->op == IR_GLOBAL) return x->aux == 0 && !got_data(px);
    if ((px->op != IR_PADD && px->op != IR_PSUB) || (px->flags & IRF_CHECKED)) return 0;
    int n = px->args[1];
    uint64_t d;
    if (is_const(a, n)) return !__builtin_mul_overflow(ins_of(a, n)->imm, px->imm, &d) && d <= INT32_MAX;
    return px->op == IR_PADD && x->aux == 0 && !ra_remat((IrOp)ins_of(a, px->args[0])->op) &&
           (px->imm == 1 || px->imm == (uint64_t)access_bytes(a, x));
}

// The values their only user emits (ra.fused): a compare its branch or
// check tests, an address its load or store folds, and a multiply its add
// or subtract fuses.
static void fuse(A64* a) {
    IrFunc* fn = a->fn;
    a->madd = xcalloc((size_t)fn->nins, 1);
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int u = fn->blocks[b].ins[i];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_BR || x->op == IR_CHECK) {
                int c = x->args[0];
                if (is_compare(fn->ins[c].op) && a->ra.nuse[c] == 1 && fn->ins[c].block == b) a->ra.fused[c] = 1;
            } else if (x->op == IR_LOAD || x->op == IR_STORE) {
                if (fold_address(a, u)) a->ra.fused[x->args[0]] = 1;
            } else if ((x->op == IR_ADD || x->op == IR_SUB) && !(x->flags & IRF_CHECKED) &&
                       !is_fp((IrType)x->type) && !is_vec((IrType)x->type)) { // a float multiply-add is never contracted
                for (int k = x->op == IR_ADD ? 0 : 1; k < 2; k++) { // multiply-add, multiply-subtract
                    int m = x->args[k];
                    IrIns* mx = &fn->ins[m];
                    // a constant addend would need a register: mul + add #imm is no longer
                    if (mx->op != IR_MUL || (mx->flags & IRF_CHECKED) || a->ra.nuse[m] != 1 || mx->block != b ||
                        fn->ins[x->args[1 - k]].op == IR_CONST)
                        continue;
                    a->ra.fused[m] = 1;
                    a->madd[u] = (uint8_t)(1 + k);
                    break;
                }
            }
        }
}

// Registers a value would like: where a parameter arrives, a call's argument
// goes or its result arrives, and a returned value leaves.
static void hints(A64* a) {
    IrFunc* fn = a->fn;
    for (int i = 0; i < fn->nblocks; i++) {
        IrBlock* bb = &fn->blocks[a->ra.order[i]];
        for (int j = 0; j < bb->nins; j++) {
            int u = bb->ins[j];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_PARAM) {
                if (a->pin[x->imm].kind == L_REG) a->ra.hint[u] = a->pin[x->imm].reg;
            } else if (x->op == IR_RESULT) {
                a->ra.hint[u] = 8;
            } else if (is_call(x->op)) {
                if ((x->flags & IRF_SRET) && a->ra.hint[x->args[x->nargs - 1]] < 0) a->ra.hint[x->args[x->nargs - 1]] = 8;
                Plan p;
                plan_call(fn, x, &p);
                for (int k = 0; k < p.n; k++)
                    if (p.arg[k].kind == L_REG && a->ra.hint[x->args[p.k0 + k]] < 0) a->ra.hint[x->args[p.k0 + k]] = p.arg[k].reg;
                if (p.nres && p.res[0].kind == L_REG) a->ra.hint[u] = p.res[0].reg;
                for (int i = j + 1; i < bb->nins && fn->ins[bb->ins[i]].op == IR_PROJ; i++) {
                    Loc r = p.res[fn->ins[bb->ins[i]].imm];
                    if (r.kind == L_REG) a->ra.hint[bb->ins[i]] = r.reg;
                }
                if (p.bytes > a->out) a->out = p.bytes;
                plan_free(&p);
            } else if (x->op == IR_RET) {
                for (int k = 0; k < x->nargs; k++)
                    if (a->rin[k].kind == L_REG && a->ra.hint[x->args[k]] < 0) a->ra.hint[x->args[k]] = a->rin[k].reg;
            }
        }
    }
}

static const int8_t caller_regs[] = { 9, 10, 11, 12, 13, 14, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
static const int8_t callee_regs[] = { 19, 20, 21, 22, 23, 24, 25, 26, 27, 28 };
// v16-v28, then the argument registers; d8-d15 are callee-saved (their low 64 bits)
static const int8_t fcaller_regs[] = { V + 16, V + 17, V + 18, V + 19, V + 20, V + 21, V + 22, V + 23, V + 24,
                                       V + 25, V + 26, V + 27, V + 28, V + 7, V + 6, V + 5, V + 4, V + 3,
                                       V + 2, V + 1, V + 0 };
static const int8_t fcallee_regs[] = { V + 8, V + 9, V + 10, V + 11, V + 12, V + 13, V + 14, V + 15 };

static int callee_saved(int r) { return (r >= 19 && r <= 28) || (r >= V + 8 && r <= V + 15); }

// Allocation hands out x0-x14, x19-x28, and v0-v28. A byte routine disturbs
// x8 and v24-v28; a callee preserves only a v register's low half, so a
// vector live across a call spills.
static const RaTarget ARM64 = {
    V,
    { caller_regs, fcaller_regs }, { (int)sizeof caller_regs, (int)sizeof fcaller_regs },
    { callee_regs, fcallee_regs }, { (int)sizeof callee_regs, (int)sizeof fcallee_regs },
    UINT64_C(0x1FF80000) | UINT64_C(0xFF) << (V + 8),
    UINT64_C(1) << 8 | UINT64_C(0x1F) << (V + 24),
    1,
    cast_in_place,
};

static void allocate(A64* a) {
    ra_allocate(&a->ra, &ARM64);
    uint64_t used = 0;
    for (int v = 0; v < a->fn->nins; v++)
        if (a->ra.reg[v] >= 0) used |= UINT64_C(1) << a->ra.reg[v];
    used |= a->ra.asm_all; // a reserved x19-x28 is the caller's, whatever asm does to it
    for (int r = 0; r < SLOT; r++)
        if (callee_saved(r) && (used & (UINT64_C(1) << r))) a->cs[a->ncs++] = (int8_t)r;
}

// --- frame ------------------------------------------------------------------------------
//
// [sp, sp + out): outgoing stack arguments; then the frame record (x29, x30),
// the callee-saved registers, the spill slots, and the stack objects.

// Save (or restore) the callee-saved registers, after the frame record (with
// it when record) at sp + off, within ldp/stp reach when off is at most 352
// bytes (18 registers follow). A pair never mixes x and d registers.
static void frame_pairs(A64* a, uint32_t off, int restore, int record) {
    uint32_t pair = restore ? 0xA9400000u : 0xA9000000u, one = restore ? 0xF9400000u : 0xF9000000u;
    uint32_t fpair = restore ? 0x6D400000u : 0x6D000000u, fone = restore ? 0xFD400000u : 0xFD000000u;
    if (record) put(a, pair | ((off / 8) << 15) | (30u << 10) | (31u << 5) | 29u);
    for (int k = 0; k < a->ncs;) {
        uint32_t o = (off + 16u + 8u * (uint32_t)k) / 8;
        int fp = fp_reg(a->cs[k]);
        if (k + 1 < a->ncs && fp_reg(a->cs[k + 1]) == fp) {
            put(a, (fp ? fpair : pair) | (o << 15) | (R(a->cs[k + 1]) << 10) | (31u << 5) | R(a->cs[k]));
            k += 2;
        } else {
            put(a, (fp ? fone : one) | (o << 10) | (31u << 5) | R(a->cs[k]));
            k++;
        }
    }
}

// A small frame without outgoing arguments saves the record with writeback.
static int short_frame(A64* a) { return !a->out && a->frame <= 504; }

// Outgoing bytes past ldp/stp reach of the saved registers: sp moves in two
// steps, the frame above them first, so the registers sit at its base.
static int far_out(A64* a) { return a->out > 352; }

static void prologue(A64* a) {
    if (!a->has_frame) return;
    uint32_t f = a->frame;
    if (short_frame(a)) {
        put(a, 0xA9800000u | ((uint32_t)(-(int32_t)(f / 8)) & 0x7Fu) << 15 | (30u << 10) | (31u << 5) | 29u);
        put(a, 0x910003FDu); // mov x29, sp
        frame_pairs(a, 0, 0, 0);
    } else if (far_out(a)) {
        sp_adjust(a, 0xD10003FFu, f - a->out);
        frame_pairs(a, 0, 0, 1);
        put(a, 0x910003FDu); // mov x29, sp
        sp_adjust(a, 0xD10003FFu, a->out);
    } else {
        sp_adjust(a, 0xD10003FFu, f);
        frame_pairs(a, a->out, 0, 1);
        add_imm(a, 29, 31, a->out);
    }
}

// A frame's restores are emitted once; every later return branches to them.
static void epilogue(A64* a) {
    if (a->has_frame && a->epi) {
        put(a, 0x14000000u | ((uint32_t)(((int64_t)a->epi - 1 - here(a)) / 4) & 0x3FFFFFFu)); // b epi
        return;
    }
    if (a->has_frame) {
        a->epi = here(a) + 1;
        uint32_t f = a->frame;
        if (short_frame(a)) {
            frame_pairs(a, 0, 1, 0);
            put(a, 0xA8C00000u | ((f / 8) << 15) | (30u << 10) | (31u << 5) | 29u); // ldp x29, x30, [sp], #f
        } else if (far_out(a)) {
            sp_adjust(a, 0x910003FFu, a->out);
            frame_pairs(a, 0, 1, 1);
            sp_adjust(a, 0x910003FFu, f - a->out);
        } else {
            frame_pairs(a, a->out, 1, 1);
            sp_adjust(a, 0x910003FFu, f);
        }
    }
    put(a, 0xD65F03C0u); // ret
}

// Lay out the stack objects above the spill slots; returns the frame size.
static uint32_t place_objects(A64* a) {
    IrFunc* fn = a->fn;
    a->obj = xcalloc((size_t)fn->nins, sizeof(uint32_t));
    uint32_t at = a->slot_base + 8u * (uint32_t)a->ra.nslots;
    if (a->nsave) {
        a->save_at = at;
        at += a->nsave;
        a->has_frame = 1;
    }
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int v = fn->blocks[b].ins[i];
            IrIns* x = &fn->ins[v];
            if (x->op != IR_SLOT) continue;
            uint32_t al = x->aux > 0 ? (uint32_t)x->aux : 1;
            at = (at + al - 1) & ~(al - 1);
            a->obj[v] = at;
            at += (uint32_t)(x->imm ? x->imm : 1);
            a->has_frame = 1;
        }
    return (at + 15u) & ~15u;
}

// --- selection --------------------------------------------------------------------------

// The condition after cmp or fcmp under which compare op holds. A float
// ordering must fail on an unordered fcmp (NZCV 0011): LO and LS test C, GT
// and GE test N == V, all false there; NE holds there, as IEEE says.
static int cond_of(int op, IrType t) {
    int sg = ir_type_signed(t), fp = is_fp(t);
    switch (op) {
    case IR_EQ: return C_EQ;
    case IR_NE: return C_NE;
    case IR_LT: return sg ? C_LT : C_LO;
    case IR_LE: return sg ? C_LE : C_LS;
    case IR_GT: return sg || fp ? C_GT : C_HI;
    default: return sg || fp ? C_GE : C_HS;
    }
}

static int swap_op(int op) { // the comparison with its operands exchanged
    switch (op) {
    case IR_LT: return IR_GT;
    case IR_GT: return IR_LT;
    case IR_LE: return IR_GE;
    case IR_GE: return IR_LE;
    default: return op;
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
    int l = x->args[0], r = x->args[1], op = x->op;
    IrType t = (IrType)ins_of(a, l)->type;
    uint32_t sf = sfb(t);
    if (is_const(a, l) && !is_const(a, r)) {
        int s = l; l = r; r = s;
        op = swap_op(op);
    }
    int cond = cond_of(op, t);
    if (is_fp(t)) { // fcmp, against #0.0 when it is +0.0
        int rn = opnd(a, l, FS0);
        if (is_const(a, r) && ins_of(a, r)->imm == 0) put(a, 0x1E202008u | ftype(t) | (R(rn) << 5));
        else put(a, 0x1E202000u | ftype(t) | (R(opnd(a, r, FS1)) << 16) | (R(rn) << 5));
        return (Cond){ 0, (uint8_t)cond, 0, 0, 0 };
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

// umulh: the high 64 bits of the unsigned product
static void mul_high(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    int rn = opnd(a, x->args[0], S0), rm = opnd(a, x->args[1], S1), rd = dst(a, v);
    put(a, 0x9BC07C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd);
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

// fadd, fsub, fmul, fdiv, fneg: IEEE, never trapping or fused
static void farith(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    uint32_t ty = ftype((IrType)x->type);
    int rn = opnd(a, x->args[0], FS0);
    if (x->op == IR_NEG) {
        int rd = dst(a, v);
        put(a, 0x1E214000u | ty | (R(rn) << 5) | R(rd));
        dst_done(a, v, rd);
        return;
    }
    static const uint32_t op[] = { [IR_ADD] = 0x1E202800u, [IR_SUB] = 0x1E203800u, [IR_MUL] = 0x1E200800u,
                                   [IR_DIV] = 0x1E201800u };
    int rm = opnd(a, x->args[1], FS1), rd = dst(a, v);
    put(a, op[x->op] | ty | (R(rm) << 16) | (R(rn) << 5) | R(rd));
    dst_done(a, v, rd);
}

// A conversion to or from a float (emit.c gen_cast): fcvt between widths;
// scvtf/ucvtf from an integer's register form (w for a narrow one); fcvtzs/
// fcvtzu, which round toward zero and saturate, then a narrow result wrapped.
static void fcast(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType to = (IrType)x->type, from = (IrType)ins_of(a, x->args[0])->type;
    int rn = opnd(a, x->args[0], is_fp(from) ? FS0 : S0), rd = dst(a, v);
    uint32_t nd = (R(rn) << 5) | R(rd);
    if (is_fp(from) && is_fp(to)) {
        if (from == to) mov_reg(a, rd, rn);
        else put(a, (to == IT_F64 ? 0x1E22C000u : 0x1E624000u) | nd);
    } else if (is_fp(to)) {
        uint32_t op = is64(from) ? (to == IT_F64 ? 0x9E620000u : 0x9E220000u) : (to == IT_F64 ? 0x1E620000u : 0x1E220000u);
        put(a, op | (ir_type_signed(from) ? 0 : 0x10000u) | nd);
    } else {
        uint32_t op = from == IT_F64 ? (is64(to) ? 0x9E780000u : 0x1E780000u) : (is64(to) ? 0x9E380000u : 0x1E380000u);
        put(a, op | (ir_type_signed(to) ? 0 : 0x10000u) | nd);
        wrap(a, to, rd);
    }
    dst_done(a, v, rd);
}

// A lane-wise vector operation (ir.h): NEON on .4s lanes, a compare to a mask
// (lt and le as gt and ge with the operands exchanged; ne as eq inverted),
// and select through bsl, or bit or bif when the result's register already
// holds the lanes it keeps.
static void vector(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    if (x->op == IR_SPLAT && t == IT_V2F64) { // dup .2d from d lane 0, or fmov .2d/movi for an 8-bit immediate
        int d = is_const(a, x->args[0]) ? fmov_imm8(ins_of(a, x->args[0])->imm, 1) : -1;
        int zero = is_const(a, x->args[0]) && !ins_of(a, x->args[0])->imm;
        if (zero || d >= 0) {
            int rd = dst(a, v), imm8 = zero ? 0 : d;
            put(a, (zero ? 0x4F000400u : 0x6F00F400u) | ((uint32_t)imm8 >> 5) << 16 | ((uint32_t)imm8 & 31) << 5 | R(rd));
            dst_done(a, v, rd);
            return;
        }
        int rn = opnd(a, x->args[0], FS0), rd = dst(a, v);
        put(a, 0x4E080400u | (R(rn) << 5) | R(rd));
        dst_done(a, v, rd);
        return;
    }
    if (x->op == IR_SPLAT) {
        int f = t == IT_V4F32;
        if (is_const(a, x->args[0])) { // an 8-bit lane immediate: movi .4s, or fmov .4s for a float
            uint64_t bits = ins_of(a, x->args[0])->imm & 0xFFFFFFFFu;
            int imm8 = !f && bits <= 0xFF ? (int)bits : f && !bits ? 0 : f ? fmov_imm8(bits, 0) : -1;
            if (imm8 >= 0) {
                int rd = dst(a, v);
                uint32_t op = f && bits ? 0x4F00F400u : 0x4F000400u;
                put(a, op | ((uint32_t)imm8 >> 5) << 16 | ((uint32_t)imm8 & 31) << 5 | R(rd));
                dst_done(a, v, rd);
                return;
            }
        }
        int rn = opnd(a, x->args[0], f ? FS0 : S0), rd = dst(a, v);
        put(a, (f ? 0x4E040400u : 0x4E040C00u) | (R(rn) << 5) | R(rd)); // dup .4s, from s lane 0 or w
        dst_done(a, v, rd);
        return;
    }
    if (x->op == IR_SELECT) {
        int m = opnd(a, x->args[0], FS0), rn = opnd(a, x->args[1], FS1), rm = opnd(a, x->args[2], FS2);
        int rd = dst(a, v);
        if (rd == rm && rd != m) {
            put(a, 0x6EA01C00u | (R(m) << 16) | (R(rn) << 5) | R(rd)); // bit: rn's lanes where m is set
        } else if (rd == rn && rd != m) {
            put(a, 0x6EE01C00u | (R(m) << 16) | (R(rm) << 5) | R(rd)); // bif: rm's lanes where m is clear
        } else {
            mov_q(a, rd, m);
            put(a, 0x6E601C00u | (R(rm) << 16) | (R(rn) << 5) | R(rd)); // bsl
        }
        dst_done(a, v, rd);
        return;
    }
    if (x->op == IR_LANES) { // <0, 1> into the low half by fmov, which zeroes the high; <2, 3> inserted
        int rd = dst(a, v);
        mov_imm(a, S0, 1ull << 32, 1);
        put(a, 0x9E670000u | (S0 << 5) | R(rd)); // fmov d, x
        mov_imm(a, S0, 3ull << 32 | 2, 1);
        put(a, 0x4E181C00u | (S0 << 5) | R(rd)); // ins .d[1], x
        dst_done(a, v, rd);
        return;
    }
    if (x->op == IR_SHL || x->op == IR_SHR) { // by an immediate (ir.h); shr is arithmetic, as i32's
        uint32_t n = (uint32_t)ins_of(a, x->args[1])->imm;
        int rn = opnd(a, x->args[0], FS0), rd = dst(a, v);
        put(a, (x->op == IR_SHL ? 0x4F005400u | (32 + n) << 16 : 0x4F000400u | (64 - n) << 16) | (R(rn) << 5) | R(rd));
        dst_done(a, v, rd);
        return;
    }
    int l = x->args[0], r = x->nargs > 1 ? x->args[1] : -1, op = x->op;
    IrType ot = (IrType)ins_of(a, l)->type;
    int f = ot == IT_V4F32 || ot == IT_V2F64;
    uint32_t w;
    switch (op) {
    case IR_ADD: w = f ? 0x4E20D400u : 0x4EA08400u; break;
    case IR_SUB: w = f ? 0x4EA0D400u : 0x6EA08400u; break;
    case IR_MUL: w = f ? 0x6E20DC00u : 0x4EA09C00u; break;
    case IR_DIV: w = 0x6E20FC00u; break;
    case IR_NEG: w = f ? 0x6EA0F800u : 0x6EA0B800u; break;
    case IR_CAST: w = 0x4E21D800u; break; // scvtf
    case IR_AND: w = 0x4E201C00u; break;
    case IR_OR: w = 0x4EA01C00u; break;
    case IR_XOR: w = 0x6E201C00u; break;
    case IR_LT: case IR_LE:
        r = l; l = x->args[1];
        op = swap_op(op);
        // fall through
    default: // a compare
        w = op == IR_GT ? (f ? 0x6EA0E400u : 0x4EA03400u) : op == IR_GE ? (f ? 0x6E20E400u : 0x4EA03C00u)
                        : (f ? 0x4E20E400u : 0x6EA08C00u); // eq, ne
        break;
    }
    if (ot == IT_V2F64) w |= 0x00400000u; // sz: .2d lanes (arithmetic only, ir.h)
    int rn = opnd(a, l, FS0), rm = r >= 0 ? opnd(a, r, FS1) : 0, rd = dst(a, v);
    put(a, w | (R(rm) << 16) | (R(rn) << 5) | R(rd));
    if (op == IR_NE) put(a, 0x6E205800u | (R(rd) << 5) | R(rd)); // mvn
    dst_done(a, v, rd);
}

// Truncate, then extend by the target's signedness (ir_wrap), in register form.
static void cast(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType to = (IrType)x->type, from = (IrType)ins_of(a, x->args[0])->type;
    if (is_fp(to) || is_fp(from)) {
        fcast(a, v);
        return;
    }
    int fb = ir_type_bits(from), tb = ir_type_bits(to);
    int rn = opnd(a, x->args[0], S0), rd = dst(a, v);
    if (cast_in_place(to, from)) mov_reg(a, rd, rn); // none when it shares its operand's location
    else if (tb == 64 && fb < 64 && ir_type_signed(from)) put(a, 0x93407C00u | ((uint32_t)rn << 5) | (uint32_t)rd); // sxtw
    else if (tb == 64 && fb < 64) put(a, 0x2A0003E0u | ((uint32_t)rn << 16) | (uint32_t)rd);                   // mov w
    else ext(a, to, rd, rn);
    dst_done(a, v, rd);
}

// fmov between a float and an integer register of its width (a 32-bit one
// zeroes the upper bits, so a u32 result is held zero-extended).
static void bitcast(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType to = (IrType)x->type, from = (IrType)ins_of(a, x->args[0])->type;
    int rn = opnd(a, x->args[0], is_fp(from) ? FS0 : S0), rd = dst(a, v);
    int w = to == IT_F32 || from == IT_F32;
    uint32_t op = is_fp(to) ? (w ? 0x1E270000u : 0x9E670000u) : (w ? 0x1E260000u : 0x9E660000u);
    put(a, op | (R(rn) << 5) | R(rd));
    dst_done(a, v, rd);
}

// Bit operations on an unsigned value's register form (emit.c
// emit_bits_integer): popcount through v31 (cnt, addv); clz, less the zeros
// a narrow value's 32-bit form adds; ctz as clz of the bits reversed, a
// narrow value first given a set bit past its width; rev16, rev, or none for
// a byte swap; a rotation by rorv, left by the negated count, a narrow value
// first repeated across the word so the hardware's count reduction is its own.
static void bitop(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int bits = ir_type_bits(t);
    uint32_t sf = sfb(t);
    int rn = opnd(a, x->args[0], S0);
    if (x->op == IR_ROTL || x->op == IR_ROTR) {
        int rm = opnd(a, x->args[1], S1);
        if (bits < 32) {
            put(a, 0x2A000000u | (bits == 8 ? 0x2000u : 0x4000u) | ((uint32_t)rn << 16) | ((uint32_t)rn << 5) | S0);
            if (bits == 8) put(a, 0x2A004000u | (S0 << 16) | (S0 << 5) | S0); // orr w16, w16, w16, lsl #16
            rn = S0;
        }
        if (x->op == IR_ROTL) {
            put(a, sf | 0x4B0003E0u | ((uint32_t)rm << 16) | S2); // neg
            rm = S2;
        }
        int rd = dst(a, v);
        put(a, sf | 0x1AC02C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd); // rorv
        if (bits < 32) wrap(a, t, rd);
        dst_done(a, v, rd);
        return;
    }
    int rd = dst(a, v);
    switch (x->op) {
    case IR_POPCNT:
        put(a, (sf ? 0x9E670000u : 0x1E270000u) | ((uint32_t)rn << 5) | R(FS0)); // fmov d31, x / s31, w
        put(a, 0x0E205800u | (R(FS0) << 5) | R(FS0));                           // cnt v31.8b, v31.8b
        put(a, 0x0E31B800u | (R(FS0) << 5) | R(FS0));                           // addv b31, v31.8b
        put(a, 0x0E013C00u | (R(FS0) << 5) | (uint32_t)rd);                     // umov w, v31.b[0]
        break;
    case IR_CLZ:
        put(a, sf | 0x5AC01000u | ((uint32_t)rn << 5) | (uint32_t)rd);
        if (bits < 32) put(a, 0x51000000u | ((uint32_t)(32 - bits) << 10) | ((uint32_t)rd << 5) | (uint32_t)rd);
        break;
    case IR_CTZ:
        if (bits < 32) {
            mov_imm(a, S1, UINT64_C(1) << bits, 0);
            put(a, 0x2A000000u | (S1 << 16) | ((uint32_t)rn << 5) | S1); // orr w17, wn, w17
            rn = S1;
        }
        put(a, sf | 0x5AC00000u | ((uint32_t)rn << 5) | (uint32_t)rd); // rbit
        put(a, sf | 0x5AC01000u | ((uint32_t)rd << 5) | (uint32_t)rd); // clz
        break;
    default: // IR_BSWAP
        if (bits == 8) mov_reg(a, rd, rn);
        else put(a, (bits == 16 ? 0x5AC00400u : bits == 32 ? 0x5AC00800u : 0xDAC00C00u) | ((uint32_t)rn << 5) | (uint32_t)rd);
        break;
    }
    dst_done(a, v, rd);
}

// Exact IEEE operations: fsqrt, frintm (floor), frintp (ceil), and fmadd,
// which rounds once.
static void fexact(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    uint32_t ty = ftype((IrType)x->type);
    int rn = opnd(a, x->args[0], FS0);
    if (x->op == IR_FMA) {
        int rm = opnd(a, x->args[1], FS1), ra = opnd(a, x->args[2], FS2), rd = dst(a, v);
        put(a, 0x1F000000u | ty | (R(rm) << 16) | (R(ra) << 10) | (R(rn) << 5) | R(rd));
        dst_done(a, v, rd);
        return;
    }
    static const uint32_t op[] = { [IR_SQRT] = 0x1E21C000u, [IR_FLOOR] = 0x1E254000u, [IR_CEIL] = 0x1E24C000u };
    int rd = dst(a, v);
    put(a, op[x->op] | ty | (R(rn) << 5) | R(rd));
    dst_done(a, v, rd);
}

// --- memory -----------------------------------------------------------------------------

// The unsigned-offset load or store opcode for a value of type t (a load
// extends a narrow value to its 32-bit register form; a float uses s or d).
static uint32_t mem_op(IrType t, int load) {
    if (is_vec(t)) return load ? 0x3DC00000u : 0x3D800000u; // q
    if (t == IT_F32) return load ? 0xBD400000u : 0xBD000000u;
    if (t == IT_F64) return load ? 0xFD400000u : 0xFD000000u;
    switch (ir_type_bits(t)) {
    case 8: return load ? (t == IT_I8 ? 0x39C00000u : 0x39400000u) : 0x39000000u;
    case 16: return load ? (t == IT_I16 ? 0x79C00000u : 0x79400000u) : 0x79000000u;
    case 32: return load ? 0xB9400000u : 0xB9000000u;
    default: return load ? 0xF9400000u : 0xF9000000u;
    }
}

// op rt, [rn + off]: scaled, unscaled, or through x15 as a register offset.
static void mem(A64* a, uint32_t op, int size, int rt, int rn, int64_t off) {
    uint32_t tn = ((uint32_t)rn << 5) | R(rt);
    if (off >= 0 && off % size == 0 && off / size <= 4095) {
        put(a, op | ((uint32_t)(off / size) << 10) | tn);
    } else if (off >= -256 && off <= 255) {
        put(a, (op & ~0x01000000u) | (((uint32_t)off & 0x1FFu) << 12) | tn);
    } else {
        mov_imm(a, S2, (uint64_t)off, 1);
        put(a, (op & ~0x01000000u) | 0x00206800u | (S2 << 16) | tn);
    }
}

// adrp rd, name@PAGE, then the page offset in the instruction w (add or a
// load/store with a zero offset)
static void page_ref(A64* a, int rd, const char* name, uint32_t w) {
    ESym* s = sym_intern(&a->o->syms, name, (uint32_t)strlen(name));
    Sec* t = &a->o->secs[SEC_TEXT];
    reloc_add(t, here(a), s, 1, 2, 1, RM_PAGE21);
    put(a, 0x90000000u | (uint32_t)rd);
    reloc_add(t, here(a), s, 0, 2, 1, RM_PAGEOFF12);
    put(a, w);
}

// Load or store v; the value register is chosen by the caller of mem.
static void access(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    int load = x->op == IR_LOAD;
    IrType t = (IrType)(load ? x->type : ins_of(a, x->args[1])->type);
    int size = access_bytes(a, x);
    uint32_t op = mem_op(t, load);
    int rt;
    if (load) {
        rt = dst(a, v);
    } else {
        int s = x->args[1];
        uint64_t bits = ins_of(a, s)->imm;
        if (is_fp(t) && is_const(a, s)) { // a float constant stores as its bits
            op = mem_op(t == IT_F64 ? IT_U64 : IT_U32, 0);
            if (bits) mov_imm(a, S1, bits, t == IT_F64);
            rt = bits ? S1 : 31;
        } else {
            rt = is_const(a, s) && bits == 0 ? 31 : opnd(a, s, is_fp(t) || is_vec(t) ? FS1 : S1); // zr
        }
    }
    int p = x->args[0];
    IrIns* px = ins_of(a, p);
    int64_t off = x->aux;
    int rn;
    if (px->op == IR_SLOT) {
        rn = 31;
        off += a->obj[p];
    } else if (a->ra.fused[p] && px->op == IR_GLOBAL) {
        page_ref(a, S0, px->name, op | (S0 << 5) | R(rt));
        goto done;
    } else if (a->ra.fused[p]) { // an unchecked pointer offset
        int n = px->args[1], base = px->args[0];
        if (is_const(a, n)) {
            int64_t d = (int64_t)(ins_of(a, n)->imm * px->imm);
            off += px->op == IR_PADD ? d : -d;
            if (ins_of(a, base)->op == IR_SLOT) {
                rn = 31;
                off += a->obj[base];
            } else {
                rn = opnd(a, base, S0);
            }
        } else {
            rn = opnd(a, base, S0);
            int ri = opnd(a, n, S2);
            put(a, (op & ~0x01000000u) | 0x00206800u | (px->imm > 1 ? 0x1000u : 0) | ((uint32_t)ri << 16) |
                       ((uint32_t)rn << 5) | R(rt));
            goto done;
        }
    } else {
        rn = opnd(a, p, S0);
        if (x->flags & IRF_CHECKED) null_fail_if_zero(a, rn);
    }
    mem(a, op, size, rt, rn, off);
done:
    if (load) dst_done(a, v, rt);
}

// rd = the address of a C global (C Interop #6) from its GOT slot, which the
// dynamic linker fills: adrp rd, name@GOTPAGE; ldr rd, [rd, name@GOTPAGEOFF].
static void got_ref(A64* a, int rd, const char* name) {
    ESym* s = sym_intern(&a->o->syms, name, (uint32_t)strlen(name));
    Sec* t = &a->o->secs[SEC_TEXT];
    reloc_add(t, here(a), s, 1, 2, 1, RM_GOT_LOAD_PAGE21);
    put(a, 0x90000000u | (uint32_t)rd);
    reloc_add(t, here(a), s, 0, 2, 1, RM_GOT_LOAD_PAGEOFF12);
    put(a, 0xF9400000u | ((uint32_t)rd << 5) | (uint32_t)rd);
}

static void global(A64* a, int v) {
    int rd = dst(a, v);
    if (got_data(ins_of(a, v))) got_ref(a, rd, ins_of(a, v)->name);
    else page_ref(a, rd, ins_of(a, v)->name, 0x91000000u | ((uint32_t)rd << 5) | (uint32_t)rd);
    dst_done(a, v, rd);
}

static void string_bytes(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    ESym* s = obj_string(a->o, x->name, (uint32_t)x->imm);
    int rd = dst(a, v);
    page_ref(a, rd, s->name, 0x91000000u | ((uint32_t)rd << 5) | (uint32_t)rd);
    dst_done(a, v, rd);
}

// rd = rp +/- rn * scale (Pointers #9): debug checks a null start, the scaled
// offset's overflow, and a result past either end of the address space.
static void pointer_offset(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    int sub = x->op == IR_PSUB, chk = x->flags & IRF_CHECKED;
    uint64_t scale = x->imm;
    int n = x->args[1];
    int rp = opnd(a, x->args[0], S0);
    if (chk) fail_if_zero(a, rp, 1, 0);
    uint64_t d;
    int k = scale && (scale & (scale - 1)) == 0 ? __builtin_ctzll(scale) : -1;
    uint32_t adds = chk ? 0x20000000u : 0;
    if (is_const(a, n) && !__builtin_mul_overflow(ins_of(a, n)->imm, scale, &d)) {
        int rd = dst(a, v);
        if (d <= 0xFFF || (d <= 0xFFF000 && !(d & 0xFFF))) {
            uint32_t imm = d <= 0xFFF ? (uint32_t)d << 10 : (0x400000u | (uint32_t)(d >> 12) << 10);
            put(a, (sub ? 0xD1000000u : 0x91000000u) | adds | imm | ((uint32_t)rp << 5) | (uint32_t)rd);
        } else {
            mov_imm(a, S1, d, 1);
            put(a, (sub ? 0xCB000000u : 0x8B000000u) | adds | (S1 << 16) | ((uint32_t)rp << 5) | (uint32_t)rd);
        }
        if (chk) fail_if(a, sub ? C_LO : C_HS);
        dst_done(a, v, rd);
        return;
    }
    int rn = opnd(a, n, S1);
    if (chk && scale != 1) { // the scaled offset must fit in 64 bits
        if (k >= 0) {
            put(a, 0xD340FC00u | ((uint32_t)(64 - k) << 16) | ((uint32_t)rn << 5) | S2); // lsr x15, rn, #(64 - k)
        } else {
            mov_imm(a, S2, scale, 1);
            put(a, 0x9BC07C00u | (S2 << 16) | ((uint32_t)rn << 5) | S2); // umulh
        }
        fail_if_zero(a, S2, 1, 1);
    }
    int rd = dst(a, v);
    if (k >= 0) { // add/sub rd, rp, rn, lsl #k
        put(a, (sub ? 0xCB000000u : 0x8B000000u) | adds | ((uint32_t)rn << 16) | ((uint32_t)k << 10) | ((uint32_t)rp << 5) | (uint32_t)rd);
    } else if (!chk) { // madd / msub rd, rn, scale, rp
        mov_imm(a, S2, scale, 1);
        put(a, (sub ? 0x9B008000u : 0x9B000000u) | (S2 << 16) | ((uint32_t)rp << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
    } else {
        mov_imm(a, S2, scale, 1);
        put(a, 0x9B007C00u | (S2 << 16) | ((uint32_t)rn << 5) | S2); // mul
        put(a, (sub ? 0xEB000000u : 0xAB000000u) | (S2 << 16) | ((uint32_t)rp << 5) | (uint32_t)rd);
    }
    if (chk) fail_if(a, sub ? C_LO : C_HS);
    dst_done(a, v, rd);
}

// Store leaf v to [base + off], through scratch when it is a constant or in
// a slot; wide stores a narrow integer as 32 bits (a scalar stack argument).
static void store_leaf(A64* a, int v, int base, int64_t off, int scratch, int wide) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int size = ir_type_bits(t) / 8, rt;
    uint32_t op = mem_op(t, 0);
    if (x->op == IR_CONST) { // a float constant stores as its bits
        if (is_fp(t)) op = mem_op(t == IT_F64 ? IT_U64 : IT_U32, 0);
        rt = x->imm ? scratch : 31;
        if (x->imm) mov_imm(a, scratch, x->imm, size == 8);
    } else {
        rt = opnd(a, v, is_fp(t) ? (scratch == S0 ? FS0 : FS1) : scratch);
    }
    if (wide && size < 4 && !is_fp(t)) {
        op = mem_op(IT_U32, 0);
        size = 4;
    }
    mem(a, op, size, rt, base, off);
}

// Load leaf v, when it is used, from [base + off].
static void load_leaf(A64* a, int v, int base, int64_t off) {
    if (loc(a, v) == NOREG) return;
    IrType t = (IrType)ins_of(a, v)->type;
    int rd = dst(a, v);
    mem(a, mem_op(t, 1), ir_type_bits(t) / 8, rd, base, off);
    dst_done(a, v, rd);
}

static void str_x(A64* a, int r, int base, uint32_t off) { mem(a, 0xF9000000u, 8, r, base, off); }
static void ldr_x(A64* a, int r, int base, uint32_t off) { mem(a, 0xF9400000u, 8, r, base, off); }

// A head L_WORD's registers loaded from, or stored to, its memory at base + off.
static void word_regs(A64* a, Loc g, int load, int base, uint32_t off) {
    int n = g.nreg ? g.nreg : 1, sz = g.rsz ? g.rsz : 8;
    uint32_t op = mem_op(g.reg >= V ? (sz == 4 ? IT_F32 : IT_F64) : IT_U64, load);
    for (int i = 0; i < n; i++) mem(a, op, sz, g.reg + i, base, off + (uint32_t)(i * sz));
}

// A direct, extern, or indirect call: memory first (stack arguments, copies
// of aggregates passed by address, shared words), then the register arguments
// moved in parallel and the shared words loaded; a calli's target waits in
// x17. The results then move to the call's value or its projections, the
// words and a result buffer through memory.
static void call(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    Plan p;
    plan_call(a->fn, x, &p);
    if (x->op == IR_CALLI) {
        mov_reg(a, S1, opnd(a, x->args[0], S1));
        if (x->flags & IRF_CHECKED) null_fail_if_zero(a, S1);
    }
    Move* m = xcalloc((size_t)(p.n + p.nres + 2), sizeof(Move));
    int nm = 0;
    for (int k = 0; k < p.n; k++) {
        Loc g = p.arg[k];
        int w = x->args[p.k0 + k];
        switch (g.kind) {
        case L_REG:
            m[nm++] = move_of(a, g.reg, w);
            break;
        case L_STACK:
            store_leaf(a, w, 31, g.soff, S0, g.size == 4);
            break;
        case L_WORD:
            store_leaf(a, w, 31, g.mem + g.off, S0, 0);
            break;
        default: // L_ADDR: the copy, and its address
            store_leaf(a, w, 31, g.mem + g.off, S0, 0);
            if (!g.head) break;
            if (g.reg >= 0) {
                m[nm++] = (Move){ g.reg, 0, g.mem, 2, 1, 0 };
            } else {
                add_imm(a, S0, 31, g.mem);
                str_x(a, S0, 31, g.soff);
            }
            break;
        }
    }
    if (p.nres && p.res[0].kind == L_ADDR) m[nm++] = (Move){ 8, 0, p.res[0].mem, 2, 1, 0 }; // the result buffer
    if (x->flags & IRF_SRET) m[nm++] = move_of(a, 8, x->args[x->nargs - 1]); // the result's memory
    parallel_move(a, m, nm, 0);
    for (int k = 0; k < p.n; k++)
        if (p.arg[k].kind == L_WORD && p.arg[k].head) word_regs(a, p.arg[k], 1, 31, p.arg[k].mem);
    if (x->op == IR_CALLI) put(a, 0xD63F0000u | (S1 << 5)); // blr x17
    else if (x->sym && x->sym->is_extern) bl_extern(a, x->name);
    else bl_to(a, x->name);
    if (x->op == IR_CALL && !x->sym && strncmp(x->name, "__ace$registry_", 15) == 0 &&
        strcmp(x->name, "__ace$registry_leaks") != 0)
        a->o->allocator_used = 1; // main's exit check alone needs no registry
    if (x->op == IR_CALL && !x->sym && strcmp(x->name, "__ace$arithmetic_fail") == 0)
        a->o->arithmetic_fail_used = 1; // a check lowering calls a failure routine directly
    if (x->op == IR_CALL && !x->sym && strcmp(x->name, "__ace$null_fail") == 0) a->o->null_fail_used = 1;
    if (!x->sig || !x->sig->ret.agg) {
        free(m);
        plan_free(&p);
        if (loc(a, v) == NOREG) return;
        int rd = dst(a, v);
        IrType t = (IrType)x->type;
        if (narrow(t)) ext(a, t, rd, 0); // an AST-emitted callee may leave it unwrapped
        else mov_reg(a, rd, is_fp(t) ? V : 0);
        dst_done(a, v, rd);
        return;
    }
    IrBlock* bb = &a->fn->blocks[x->block];
    int first = 0;
    while (bb->ins[first] != v) first++;
    int last = first + 1;
    while (last < bb->nins && ins_of(a, bb->ins[last])->op == IR_PROJ) last++;
    for (int k = 0; k < p.nres; k++)
        if (p.res[k].kind == L_WORD && p.res[k].head) word_regs(a, p.res[k], 0, 31, p.res[k].mem);
    nm = 0;
    for (int i = first + 1; i < last; i++) {
        int u = bb->ins[i];
        Loc r = p.res[ins_of(a, u)->imm];
        IrType t = (IrType)ins_of(a, u)->type;
        if (r.kind != L_REG || loc(a, u) == NOREG) continue;
        if (narrow(t) || (t == IT_BOOL && r.word)) ext(a, t, r.reg, r.reg); // an AST-emitted callee may leave it unwrapped
        m[nm++] = (Move){ loc(a, u), r.reg, 0, 0, 1, 0 };
    }
    parallel_move(a, m, nm, 0);
    for (int i = first + 1; i < last; i++) {
        int u = bb->ins[i];
        Loc r = p.res[ins_of(a, u)->imm];
        if (r.kind != L_REG) load_leaf(a, u, 31, r.mem + r.off);
    }
    free(m);
    plan_free(&p);
}

// A byte routine (emit.c emit_byte_helpers): its arguments in x15, x16, and
// x17, a result in x17. It disturbs only x8, x15-x17, x30, v24-v31, and the
// flags; allocation keeps a value live across it out of x8 and v24-v28.
static void rtcall(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    Move m[3];
    for (int k = 0; k < x->nargs; k++) m[k] = move_of(a, S2 + k, x->args[k]); // x15, x16, x17
    parallel_move(a, m, x->nargs, 0);
    bl_to(a, x->name);
    if (strcmp(x->name, "__ace$mem_fill") == 0) a->o->mem_fill_used = 1;
    else if (strcmp(x->name, "__ace$mem_compare") == 0) a->o->mem_compare_used = 1;
    else a->o->mem_copy_used = 1;
    if (loc(a, v) == NOREG) return;
    int rd = dst(a, v);
    mov_reg(a, rd, S1);
    dst_done(a, v, rd);
}

// The v1 mini-assembler's only recognized instructions (Inline Assembly #1):
// nop, svc #imm, brk #imm; lowering already refused anything else
// (asm_recognized), so this only encodes.
static void asm_encode(A64* a, const char* p, uint32_t n) {
    uint32_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    uint32_t start = i;
    while (i < n && p[i] != ' ' && p[i] != '\t' && p[i] != '#') i++;
    if (i - start == 3 && memcmp(p + start, "nop", 3) == 0) { put(a, 0xD503201Fu); return; }
    int is_svc = p[start] == 's';
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '#')) i++;
    uint64_t imm = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') { imm = imm * 10 + (uint64_t)(p[i] - '0'); i++; }
    put(a, (is_svc ? 0xD4000001u : 0xD4200000u) | ((uint32_t)imm << 5));
}

// asm "instruction text" { bindings } (Inline Assembly #2-3): moves each
// "in" binding into its named register, emits the instruction, then moves
// each "out" binding's register into its projection, narrowed to its
// declared type (an AST-emitted asm variable may have left it unwrapped, the
// same trust call() already makes for a callee's return leaf).
static void asm_select(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrAsm* ab = x->asmb;
    Move* m = xcalloc((size_t)(x->nargs ? x->nargs : 1), sizeof(Move));
    for (int k = 0; k < x->nargs; k++) m[k] = move_of(a, ab->reg[k], x->args[k]);
    parallel_move(a, m, x->nargs, ab->reserved);
    free(m);
    asm_encode(a, x->name, (uint32_t)strlen(x->name));
    if (!ab->nout) return;
    IrBlock* bb = &a->fn->blocks[x->block];
    int first = 0;
    while (bb->ins[first] != v) first++;
    int last = first + 1;
    while (last < bb->nins && ins_of(a, bb->ins[last])->op == IR_PROJ) last++;
    Move* om = xcalloc((size_t)ab->nout, sizeof(Move));
    int nm = 0;
    for (int i = first + 1; i < last; i++) {
        int u = bb->ins[i];
        if (loc(a, u) == NOREG) continue;
        int reg = ab->out[ins_of(a, u)->imm];
        IrType t = (IrType)ins_of(a, u)->type;
        if (narrow(t)) ext(a, t, reg, reg);
        om[nm++] = (Move){ loc(a, u), reg, 0, 0, 1, 0 };
    }
    parallel_move(a, om, nm, ab->reserved);
    free(om);
}

// --- emission ---------------------------------------------------------------------------

static void line_row(A64* a, Node* at) {
    if (!at || at == a->last_at || at->line <= 0) return;
    a->last_at = at;
    obj_line(a->o, here(a), at->line, at->col, at->path ? at->path : a->o->d->path);
}

static int next_block(A64* a, int b) {
    int i = a->ra.eix[b] + 1;
    return i < a->fn->nblocks ? a->ra.layout[i] : -1;
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
    parallel_move(a, m, k, 0);
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
        Move* m = xcalloc((size_t)(x->nargs ? x->nargs : 1), sizeof(Move));
        int nm = 0;
        if (x->nargs && a->rin[0].kind == L_ADDR) ldr_x(a, S0, 31, a->save_at + a->rin[0].mem); // the caller's buffer
        for (int k = 0; k < x->nargs; k++) {
            Loc r = a->rin[k];
            if (r.kind == L_REG) m[nm++] = move_of(a, r.reg, x->args[k]);
            else if (r.kind == L_WORD) store_leaf(a, x->args[k], 31, a->save_at + r.mem + r.off, S0, 0);
            else store_leaf(a, x->args[k], S0, r.off, S1, 0);
        }
        parallel_move(a, m, nm, 0);
        for (int k = 0; k < x->nargs; k++)
            if (a->rin[k].kind == L_WORD && a->rin[k].head) word_regs(a, a->rin[k], 1, 31, a->save_at + a->rin[k].mem);
        free(m);
        epilogue(a);
        return;
    }
    int c = x->args[0];
    Cond k = a->ra.fused[c] ? compare(a, c, 1) : (Cond){ 1, 0, (uint8_t)opnd(a, c, S0), 0, 1 };
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

// A debug check: a branch to its site's stub when its condition is false.
static void check(A64* a, int v) {
    IrIns* x = ins_of(a, v);
    int c = x->args[0];
    Cond t = a->ra.fused[c] ? compare(a, c, 1) : (Cond){ 1, 0, (uint8_t)opnd(a, c, S0), 0, 1 };
    if (!x->name) { // the call contract failure, which reports no site
        branch_if(a, invert(t), CFAIL);
        return;
    }
    int k = 0;
    while (k < a->nstub && (a->stub[k]->at != x->at || strcmp(a->stub[k]->name, x->name) != 0)) k++;
    if (k == a->nstub) {
        if (a->nstub == a->cstub) {
            a->cstub = a->cstub ? a->cstub * 2 : 8;
            a->stub = realloc(a->stub, (size_t)a->cstub * sizeof(IrIns*));
            a->stub_at = realloc(a->stub_at, (size_t)a->cstub * sizeof(uint32_t));
            if (!a->stub || !a->stub_at) oom();
        }
        a->stub[a->nstub++] = x;
    }
    branch_if(a, invert(t), LFAIL - k);
}

static void emit_block(A64* a, int b) {
    IrFunc* fn = a->fn;
    IrBlock* bb = &fn->blocks[b];
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = &fn->ins[v];
        if (x->op != IR_CHECK) line_row(a, x->at); // its at is the site its stub reports
        if (a->ra.fused[v]) continue;
        if (is_vec((IrType)x->type) && x->op != IR_LOAD && x->op != IR_PHI) {
            vector(a, v);
            continue;
        }
        if (is_fp((IrType)x->type) && (x->op == IR_ADD || x->op == IR_SUB || x->op == IR_MUL || x->op == IR_DIV ||
                                       x->op == IR_NEG)) {
            farith(a, v);
            continue;
        }
        switch (x->op) {
        case IR_CONST: case IR_PARAM: case IR_RESULT: case IR_PHI: case IR_PROJ: // a proj's value arrives at its call
            break;
        case IR_POPCNT: case IR_CLZ: case IR_CTZ: case IR_BSWAP: case IR_ROTL: case IR_ROTR:
            bitop(a, v);
            break;
        case IR_SQRT: case IR_FLOOR: case IR_CEIL: case IR_FMA:
            fexact(a, v);
            break;
        case IR_RTCALL:
            rtcall(a, v);
            break;
        case IR_ASM:
            asm_select(a, v);
            break;
        case IR_ADD: case IR_SUB:
            add_sub(a, v);
            break;
        case IR_MUL:
            mul(a, v);
            break;
        case IR_MULH:
            mul_high(a, v);
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
        case IR_BITCAST:
            bitcast(a, v);
            break;
        case IR_CALL: case IR_CALLI:
            call(a, v);
            break;
        case IR_SLOT:
            break;
        case IR_GLOBAL:
            global(a, v);
            break;
        case IR_STR:
            string_bytes(a, v);
            break;
        case IR_LOAD: case IR_STORE:
            access(a, v);
            break;
        case IR_PADD: case IR_PSUB:
            pointer_offset(a, v);
            break;
        case IR_CHECK:
            check(a, v);
            break;
        default:
            terminator(a, b, v);
            break;
        }
    }
}

// Entry: first the words and addresses that arrive in registers (and x8, a
// result buffer's) are saved; then the register parameters, a narrow one
// wrapped where it arrived (an AST-emitted caller may pass it unwrapped),
// move to their homes in parallel; then the rest load from memory: the saved
// words, through the saved or stacked addresses, and the caller's frame.
static void params(A64* a) {
    IrFunc* fn = a->fn;
    IrBlock* bb = &fn->blocks[0];
    if (fn->nrets && a->rin[0].kind == L_ADDR) str_x(a, 8, 31, a->save_at + a->rin[0].mem);
    for (int k = 0; k < fn->nparams; k++) {
        Loc g = a->pin[k];
        if (g.head && g.kind == L_WORD) word_regs(a, g, 0, 31, a->save_at + g.mem);
        else if (g.head && g.kind == L_ADDR && g.reg >= 0) str_x(a, g.reg, 31, a->save_at + g.mem);
    }
    Move* m = xcalloc((size_t)fn->nparams + 1, sizeof(Move));
    int n = 0;
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = ins_of(a, v);
        if (x->op == IR_RESULT && loc(a, v) != NOREG) m[n++] = (Move){ loc(a, v), 8, 0, 0, 1, 0 }; // the caller's memory
        if (x->op != IR_PARAM || loc(a, v) == NOREG || a->pin[x->imm].kind != L_REG) continue;
        int r = a->pin[x->imm].reg;
        if (narrow((IrType)x->type) || (x->type == IT_BOOL && a->pin[x->imm].word)) ext(a, (IrType)x->type, r, r);
        m[n++] = (Move){ loc(a, v), r, 0, 0, 1, 0 };
    }
    parallel_move(a, m, n, 0);
    free(m);
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = ins_of(a, v);
        if (x->op != IR_PARAM) continue;
        Loc g = a->pin[x->imm];
        if (g.kind == L_STACK) {
            load_leaf(a, v, 31, (int64_t)(a->frame + g.soff));
        } else if (g.kind == L_WORD) {
            load_leaf(a, v, 31, a->save_at + g.mem + g.off);
        } else if (g.kind == L_ADDR && loc(a, v) != NOREG) {
            if (g.reg >= 0) ldr_x(a, S1, 31, a->save_at + g.mem);
            else ldr_x(a, S1, 31, a->frame + g.soff);
            load_leaf(a, v, S1, g.off);
        }
    }
}

// A block a later one branches back to, other than a cold arm rejoining.
static int is_loop_head(A64* a, int b) {
    IrBlock* bb = &a->fn->blocks[b];
    for (int p = 0; p < bb->npreds; p++)
        if (a->ra.eix[bb->preds[p]] >= a->ra.eix[b] && !a->ra.cold[bb->preds[p]]) return 1;
    return 0;
}

// Emit the body; 0 when a conditional branch or a literal load is out of range.
static int emit_body(A64* a) {
    IrFunc* fn = a->fn;
    a->nbr = 0;
    a->npool = a->npsite = 0;
    a->nstub = 0;
    a->last_at = NULL;
    line_row(a, fn->decl);
    prologue(a);
    params(a);
    for (int i = 0; i < fn->nblocks; i++) {
        int b = a->ra.layout[i];
        if (i && is_loop_head(a, b))
            while (here(a) % LOOP_ALIGN) put(a, 0xD503201Fu);
        a->label[b] = here(a);
        emit_block(a, b);
    }
    uint32_t fail_at[3] = { UINT32_MAX, UINT32_MAX, UINT32_MAX }; // per function: bl to the routine
    for (int i = 0; i < a->nbr; i++) {
        int t = a->br[i].target;
        if (t >= 0 || t <= LFAIL || fail_at[-1 - t] != UINT32_MAX) continue;
        fail_at[-1 - t] = here(a);
        bl_to(a, fail_routine(a, t));
    }
    for (int k = 0; k < a->nstub; k++) { // write "path:line:col: what" to fd 2 and abort
        uint32_t n;
        ESym* msg = obj_located(a->o, a->stub[k]->at, a->stub[k]->name, &n);
        a->stub_at[k] = here(a);
        line_row(a, a->stub[k]->at);
        mov_imm(a, 0, 2, 1);
        page_ref(a, 1, msg->name, 0x91000021u); // add x1, x1, msg@PAGEOFF
        mov_imm(a, 2, n, 1);
        bl_to(a, "__ace$bounds_fail");
        a->o->bounds_fail_used = 1;
    }
    for (int i = 0; i < a->nbr; i++) {
        Branch* br = &a->br[i];
        uint32_t to = br->target <= LFAIL ? a->stub_at[LFAIL - br->target]
                    : br->target < 0 ? fail_at[-1 - br->target] : a->label[br->target];
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
    if (a->npool) { // the float pool, 8-byte aligned (an f32 in the low half), after code that never falls in
        if (here(a) & 7) put(a, 0); // udf
        uint32_t base = here(a);
        for (int i = 0; i < a->npool; i++) {
            put(a, (uint32_t)a->pool[i]);
            put(a, (uint32_t)(a->pool[i] >> 32));
        }
        for (int i = 0; i < a->npsite; i += 2) {
            uint32_t site = a->psite[i];
            int64_t d = ((int64_t)base + 8 * (int64_t)a->psite[i + 1] - (int64_t)site) / 4;
            if (d >= (1 << 18)) return 0;
            uint32_t w;
            memcpy(&w, a->t->d + site, 4);
            buf_patch32(a->t, site, w | ((uint32_t)d << 5));
        }
    }
    return 1;
}

void arm64_func(Obj* o, IrFunc* fn, const char* name, uint32_t len, int global) {
    A64 a;
    memset(&a, 0, sizeof a);
    a.o = o;
    a.fn = fn;
    a.t = obj_text(o);
    a.pin = xcalloc((size_t)(fn->nparams ? fn->nparams : 1), sizeof(Loc));
    a.rin = xcalloc((size_t)(fn->nrets ? fn->nrets : 1), sizeof(Loc));
    Cc c = { 0, 0, 0, 0, 0 };
    assign_leaves(&c, fn->params, fn->nparams, fn->sig, -1, a.pin);
    assign_result(&c, fn->rets, fn->nrets, fn->sig ? fn->sig->ret.agg : NULL, a.rin);
    a.nsave = c.area;
    a.ra.fn = fn;
    ra_split_critical_edges(fn);
    ra_layout(&a.ra);
    ra_count_uses(&a.ra);
    fuse(&a);
    ra_liveness(&a.ra);
    ra_positions(&a.ra);
    hints(&a);
    ra_coalesce(&a.ra, &ARM64);
    allocate(&a);
    a.slot_base = a.out + 16u + 8u * (uint32_t)a.ncs;
    // a byte routine's bl needs x30 saved; an asm statement naming x29 or x30
    // needs the return address protected regardless of its own leaf status
    a.has_frame = a.ra.ncalls || a.ra.nrtcalls || a.ra.nslots || a.ncs || a.nsave || (a.ra.asm_all & (3u << 29)); // an asm reserving x29 or x30 keeps the frame
    a.frame = place_objects(&a);
    if (!a.has_frame) a.frame = 0;
    a.label = xcalloc((size_t)fn->nblocks, sizeof(uint32_t));

    while (a.t->len % FUNC_ALIGN) put(&a, 0xD503201Fu);
    uint32_t start = here(&a);
    sym_define(&o->syms, o->secs, name, len, global, &o->secs[SEC_TEXT]);
    size_t nfix = o->nfix;
    int nrl = o->secs[SEC_TEXT].nrl;
    int nlines = o->nlines;
    LineRow last = nlines ? o->lines[nlines - 1] : (LineRow){ 0 };
    if (!emit_body(&a)) { // over 1 MiB: again with far conditional branches
        a.t->len = start;
        o->nfix = nfix;
        o->secs[SEC_TEXT].nrl = nrl;
        o->nlines = nlines;
        if (nlines) o->lines[nlines - 1] = last;
        a.far = 1;
        emit_body(&a);
    }
    obj_debug_fn(o, start, here(&a) - start, fn->decl->str, fn->decl->len);

    ra_free(&a.ra);
    free(a.madd);
    free(a.label); free(a.br); free(a.pin); free(a.rin); free(a.obj); free(a.pool); free(a.psite);
    free(a.stub); free(a.stub_at);
}
