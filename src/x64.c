// x64.c — the x64 target of the IR backend for linux-x64 (spec/compiler.md
// Compiler #2, passes 7-10): instruction selection, leaving SSA, the frame,
// and encoding into the shared Obj under the System V AMD64 ABI, plus the
// runtime routines emit.c's arm64 ones mirror (x64_runtime): failures, the
// byte routines, and the debug allocator registry.
//
// Values: every value that is not a constant or a stack object lives where
// allocation (regalloc.c; "allocation and frame" below) put it: a general
// register, an xmm register (a float or a vector), or a spill slot, held in
// canonical form: a narrow integer sign- or zero-extended to 64 bits by its
// type, a bool 0 or 1, a float its bits (an f32's zero-extended), a vector
// whole. An instruction reads its operands in place and computes in its
// result's register in two-operand form, a constant operand as an immediate
// where the form takes one; a spilled operand or result goes through the
// scratch registers rax, rcx, and rdx (a float's through xmm0-xmm2, a
// vector's too), as do the forms with fixed registers (division, bit counts,
// vectors, fma, conversions to integers, checked pointer offsets). Constants
// and stack objects are rebuilt at each use, a stack object folded into a
// load or store as an rbp offset. Phi moves, call and byte routine arguments,
// parameters at entry, and asm bindings move in parallel.
//
// Frame: push rbp; mov rbp, rsp; sub rsp, N. The callee-saved registers the
// function uses, the spill slots, 16 scratch bytes, the words of aggregate
// parameters arriving in registers, and a saved result address sit below
// rbp, then the stack objects, then the outgoing area at [rsp]: stack
// arguments, register words of aggregate arguments, and a result buffer. N
// keeps rsp 16-byte aligned at every call.
//
// Calls (System V): integer scalars in rdi, rsi, rdx, rcx, r8, r9, floats in
// xmm0-xmm7, then 8-byte stack slots; an aggregate of up to 16 bytes in one
// register per eightbyte (SSE when every leaf in it is a float), or all on the
// stack when they run out; a larger one on the stack as a copy, and a result
// through memory at the address in rdi, returned in rax. Leaves meet register
// words in memory. A variadic call sets al to the SSE registers it uses. A
// scalar result arrives in rax or xmm0 and is re-canonicalized (a C callee
// may leave a narrow result's upper bits unspecified); an aggregate's words
// in rax and rdx, xmm0 and xmm1, by class. Calls go through R_X86_64_PLT32;
// the address of an extern symbol loads from the GOT, an internal one is
// rip-relative. Byte routines take their own convention (x64_runtime).
//
// Asm (Inline Assembly #1-3): the "in" values move to their registers, the
// instruction is encoded (nop, syscall, int3), and the "out" registers move
// to their projections' locations. Allocation keeps a value live across the
// statement out of its reserved registers; a reserved rbx or r12-r15 is the
// caller's, saved at entry and restored before leave with those allocation
// uses.
//
// The target requires x86-64-v3: floats use SSE (roundsd for floor and ceil)
// and vfmadd (FMA3) for fma, vectors SSE up to SSE4.1 (pmulld, blendvps).
//
// Release arithmetic follows the arm64 target: shifts count modulo 32 for a
// type up to 32 bits and 64 above, a division by zero yields 0, and MIN / -1
// wraps (its remainder is 0), so neither traps. Debug checks branch to the
// shared failure routines.
#include "backend.h"
#include "ir.h"
#include "regalloc.h"

#include <stdio.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
// the callee-saved registers an asm statement may reserve
#define CALLEE_SAVED (1u << RBX | 1u << R12 | 1u << R13 | 1u << R14 | 1u << R15)
enum { CC_O = 0, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
       CC_S = 8, CC_P = 10, CC_NP = 11, CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15 };
enum { R_PC32 = 2, R_PLT32 = 4, R_GOTPCREL = 9 };
// jump targets below 0: the arithmetic, null, and contract failure routines,
// then the function's located check stubs (LFAIL - k for stub k)
enum { FAIL = -1, NFAIL = -2, CFAIL = -3, LFAIL = -4 };

static const int ARG_REG[6] = { RDI, RSI, RDX, RCX, R8, R9 };

typedef struct { uint32_t site; int target; } Jump;

// The address a load or store folds, which rm reaches as base AFOLD:
// [base + index * (1 << scale) + disp], or [rip + sym + disp] with imm bytes
// of immediate following the displacement.
enum { AFOLD = 32 };
typedef struct { int base, index, scale, imm; const char* sym; } Fold;

typedef struct {
    Obj*     o;
    IrFunc*  fn;
    Buf*     t;
    int32_t* home;    // value -> rbp offset of its slot (negative), 0 for none
    int32_t  scratch; // rbp offset of 16 bytes where register words meet leaves
    int32_t  sret;    // rbp offset of the saved result address, 0 for a result in registers
    int32_t  saved;   // rbp offset of the callee-saved registers the function uses (keep), ascending
    uint32_t keep;    // those registers, a mask by encoding
    uint32_t epi;     // text offset + 1 of the function's one epilogue, once emitted
    int32_t  spill;   // rbp offset of spill slot 0; slot k is at spill - 8k
    int32_t  aggs;    // rbp offset of 16 bytes per aggregate parameter arriving in registers
    Ra       ra;      // layout, liveness, and allocation (regalloc.h)
    uint32_t out;     // outgoing stack argument bytes
    uint32_t frame_at; // text offset of the sub rsp imm32
    uint32_t* label;  // block -> text offset
    Jump*    jmp;
    int      njmp, cjmp;
    IrIns**  stub;    // located check sites, one per (site, name) pair
    int      nstub, cstub;
    Node*    last_at;
    int      failed;
    int      main;     // the C entry main: store argc and argv (process_words)
    Fold     fold;
} X64;

static void* xcalloc(size_t n, size_t size) {
    void* p = calloc(n ? n : 1, size);
    if (!p) oom();
    return p;
}

static IrIns* ins_of(X64* a, int v) { return &a->fn->ins[v]; }
static uint32_t here(X64* a) { return (uint32_t)a->t->len; }
static void put(X64* a, uint8_t b) { buf_put8(a->t, b); }
static void put32(X64* a, uint32_t v) { buf_put32(a->t, v); }

static void refuse(X64* a, const char* what) {
    if (a->failed) return;
    a->failed = 1;
    Node* d = a->fn->decl;
    fprintf(stderr, "%s:%d:%d: error: ", d && d->path ? d->path : a->o->d->path, d ? d->line : 0, d ? d->col : 0);
    diag_printf("cannot compile '%.*s' for linux-x64: %s not supported yet\n", d ? (int)d->len : 0,
                d ? d->str : "", what);
    a->o->d->errors++;
}

// --- encoding -------------------------------------------------------------------------

// REX prefix for w (64-bit operand), reg (ModRM.reg), and base (ModRM.rm);
// force emits it even when empty, to reach sil and dil as bytes.
static void rex(X64* a, int w, int reg, int base, int force) {
    uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((base >> 3) & 1));
    if (r != 0x40 || force) put(a, r);
}

// op reg, rm (register form)
static void rr(X64* a, int w, const uint8_t* op, int nop, int reg, int rm) {
    rex(a, w, reg, rm, 0);
    for (int i = 0; i < nop; i++) put(a, op[i]);
    put(a, (uint8_t)(0xC0 | (reg & 7) << 3 | (rm & 7)));
}

static void reloc_at(X64* a, const char* name, uint32_t type, int32_t addend);

// op reg, [base + disp] (disp32; a SIB byte for rsp and r12), or a->fold's address
static void rm(X64* a, int w, const uint8_t* op, int nop, int reg, int base, int32_t disp, int force) {
    Fold* f = &a->fold;
    if (base == AFOLD && f->sym) {
        rex(a, w, reg, 0, force);
        for (int i = 0; i < nop; i++) put(a, op[i]);
        put(a, (uint8_t)(0x05 | (reg & 7) << 3));
        reloc_at(a, f->sym, R_PC32, disp - 4 - f->imm);
        return;
    }
    if (base == AFOLD) {
        uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 | ((f->index >> 3) & 1) << 1 | ((f->base >> 3) & 1));
        if (r != 0x40 || force) put(a, r);
        for (int i = 0; i < nop; i++) put(a, op[i]);
        put(a, (uint8_t)(0x84 | (reg & 7) << 3));
        put(a, (uint8_t)(f->scale << 6 | (f->index & 7) << 3 | (f->base & 7)));
        put32(a, (uint32_t)disp);
        return;
    }
    rex(a, w, reg, base, force);
    for (int i = 0; i < nop; i++) put(a, op[i]);
    put(a, (uint8_t)(0x80 | (reg & 7) << 3 | (base & 7)));
    if ((base & 7) == RSP) put(a, 0x24);
    put32(a, (uint32_t)disp);
}

static void mov_rr(X64* a, int dst, int src) { rr(a, 1, (const uint8_t[]){ 0x89 }, 1, src, dst); }
static void ld64(X64* a, int r, int base, int32_t disp) { rm(a, 1, (const uint8_t[]){ 0x8B }, 1, r, base, disp, 0); }
static void st64(X64* a, int r, int base, int32_t disp) { rm(a, 1, (const uint8_t[]){ 0x89 }, 1, r, base, disp, 0); }
static void lea(X64* a, int r, int base, int32_t disp) { rm(a, 1, (const uint8_t[]){ 0x8D }, 1, r, base, disp, 0); }

static void mov_imm(X64* a, int r, uint64_t v) {
    if (v == 0) {
        rr(a, 0, (const uint8_t[]){ 0x31 }, 1, r, r); // xor r32, r32
    } else if (v <= 0xFFFFFFFFu) {
        rex(a, 0, 0, r, 0);
        put(a, (uint8_t)(0xB8 + (r & 7)));
        put32(a, (uint32_t)v);
    } else if ((int64_t)v >= INT32_MIN && (int64_t)v < 0) {
        rex(a, 1, 0, r, 0);
        put(a, 0xC7);
        put(a, (uint8_t)(0xC0 | (r & 7)));
        put32(a, (uint32_t)v);
    } else {
        rex(a, 1, 0, r, 0);
        put(a, (uint8_t)(0xB8 + (r & 7)));
        buf_put64(a->t, v);
    }
}

// The canonical 64-bit form of v at type t.
static uint64_t canon(IrType t, uint64_t v) {
    if (t == IT_BOOL) return v & 1;
    int bits = ir_type_bits(t);
    if (bits >= 64) return v;
    uint64_t m = (1ull << bits) - 1;
    v &= m;
    if (ir_type_signed(t) && (v >> (bits - 1))) v |= ~m;
    return v;
}

// Restore r's canonical form at type t from its low bits.
static void normalize(X64* a, IrType t, int r) {
    switch (t) {
    case IT_I8: rr(a, 1, (const uint8_t[]){ 0x0F, 0xBE }, 2, r, r); break;  // movsx r64, r8
    case IT_U8: case IT_BOOL: rex(a, 0, r, r, r >= RSP); put(a, 0x0F); put(a, 0xB6);
        put(a, (uint8_t)(0xC0 | (r & 7) << 3 | (r & 7))); break;              // movzx r32, r8
    case IT_I16: rr(a, 1, (const uint8_t[]){ 0x0F, 0xBF }, 2, r, r); break; // movsx r64, r16
    case IT_U16: rr(a, 0, (const uint8_t[]){ 0x0F, 0xB7 }, 2, r, r); break; // movzx r32, r16
    case IT_I32: rr(a, 1, (const uint8_t[]){ 0x63 }, 1, r, r); break;       // movsxd r64, r32
    case IT_U32: case IT_F32: rr(a, 0, (const uint8_t[]){ 0x89 }, 1, r, r); break; // mov r32, r32
    default: break;
    }
}

// jcc rel32 / jmp rel32 to a block or failure target, patched at the end.
static void jump_add(X64* a, int cc, int target) {
    if (a->njmp == a->cjmp) {
        a->cjmp = a->cjmp ? a->cjmp * 2 : 16;
        a->jmp = realloc(a->jmp, (size_t)a->cjmp * sizeof(Jump));
        if (!a->jmp) oom();
    }
    if (cc < 0) {
        put(a, 0xE9);
    } else {
        put(a, 0x0F);
        put(a, (uint8_t)(0x80 | cc));
    }
    a->jmp[a->njmp++] = (Jump){ here(a), target };
    put32(a, 0);
}

static void reloc_at(X64* a, const char* name, uint32_t type, int32_t addend) {
    Sec* t = &a->o->secs[SEC_TEXT];
    reloc_add(t, here(a), sym_intern(&a->o->syms, name, (uint32_t)strlen(name)), 1, 2, 1, type);
    put32(a, (uint32_t)addend);
}

static void reloc32(X64* a, const char* name, uint32_t type) { reloc_at(a, name, type, -4); }

static void call_sym(X64* a, const char* name) {
    put(a, 0xE8);
    reloc32(a, name, R_PLT32);
}

// r = the address of name: from the GOT for a symbol outside the object
static void addr_of(X64* a, int r, const char* name, int ext) {
    rex(a, 1, r, 0, 0);
    put(a, ext ? 0x8B : 0x8D);
    put(a, (uint8_t)(0x05 | (r & 7) << 3));
    reloc32(a, name, ext ? R_GOTPCREL : R_PC32);
}

// --- operands ---------------------------------------------------------------------------
//
// A value's location: its register (xmm k numbered X + k), its spill slot's
// memory (a location from LMEM, any rbp offset), or NOLOC for a constant, a
// stack object, or an unused value.

enum { X = 16, NOLOC = -1, LMEM = 64 };
enum { XMM0, XMM1, XMM2 };

static int mem_loc(int32_t disp) { return LMEM + (1 << 28) + disp; }
static int32_t loc_disp(int l) { return (int32_t)(l - LMEM - (1 << 28)); }
static IrType type_of(X64* a, int v) { return (IrType)ins_of(a, v)->type; }

// Spill slot k is 8 bytes at spill - 8k; a vector's pair k, k + 1 is 16
// bytes from k + 1's.
static int loc(X64* a, int v) {
    if (a->ra.reg[v] >= 0) return a->ra.reg[v];
    int s = a->ra.slot[v];
    if (s < 0) return NOLOC;
    return mem_loc(a->spill - 8 * (s + ir_type_vector(type_of(a, v))));
}

static void xop(X64* a, int pfx, int w, uint8_t op, int reg, int r);
static void xmm_from(X64* a, int x, int r);
static void xmm_to(X64* a, IrType t, int r, int x);
static void xld(X64* a, int x, int base, int32_t disp);
static void xst(X64* a, int x, int base, int32_t disp);
static void xmove(X64* a, int load, int aligned, int x, int base, int32_t disp);

// Copy the bits of a value of type t from location s to d. A float in an
// xmm register is its low lane: an f32 leaves it zero-extended (movd), as its
// canonical form in a general register or memory is; a vector moves whole.
// Memory to memory goes through r11, or xmm1 for a vector.
static void move1(X64* a, int d, int s, IrType t) {
    if (d == s) return;
    int vec = ir_type_vector(t);
    if (d >= LMEM && s >= LMEM) {
        if (vec) {
            xmove(a, 1, 0, XMM1, RBP, loc_disp(s));
            xmove(a, 0, 0, XMM1, RBP, loc_disp(d));
        } else {
            ld64(a, R11, RBP, loc_disp(s));
            st64(a, R11, RBP, loc_disp(d));
        }
    } else if (d >= LMEM) {
        int32_t o = loc_disp(d);
        if (s < X) st64(a, s, RBP, o);
        else if (vec) xmove(a, 0, 0, s - X, RBP, o);
        else if (t == IT_F32) { // movss, then the upper half zeroed
            put(a, 0xF3);
            rm(a, 0, (const uint8_t[]){ 0x0F, 0x11 }, 2, s - X, RBP, o, 0);
            rm(a, 0, (const uint8_t[]){ 0xC7 }, 1, 0, RBP, o + 4, 0);
            put32(a, 0);
        }
        else xst(a, s - X, RBP, o);
    } else if (s >= LMEM) {
        int32_t o = loc_disp(s);
        if (d < X) ld64(a, d, RBP, o);
        else if (vec) xmove(a, 1, 0, d - X, RBP, o);
        else xld(a, d - X, RBP, o);
    } else if (d < X && s < X) {
        mov_rr(a, d, s);
    } else if (d < X) {
        xmm_to(a, t, d, s - X);
    } else if (s < X) {
        xmm_from(a, d - X, s);
    } else {
        xop(a, 0, 0, 0x28, d - X, s - X); // movaps
    }
}

// r = v's canonical bits: a constant or a stack object's address rebuilt.
static void load_val(X64* a, int r, int v) {
    IrIns* x = ins_of(a, v);
    if (x->op == IR_CONST) mov_imm(a, r, canon((IrType)x->type, x->imm));
    else if (x->op == IR_SLOT) lea(a, r, RBP, a->home[v]);
    else move1(a, r, loc(a, v), (IrType)x->type);
}

static void store_val(X64* a, int v, int r) {
    int l = loc(a, v);
    if (l != NOLOC) move1(a, l, r, type_of(a, v));
}

// Operands in place: a general register holding v (its own, or scratch s
// loaded), the register v's result is computed in (its own, or s), and that
// result moved to v's location when it is not already there.
static int gp_in(X64* a, int v, int s) {
    int r = a->ra.reg[v];
    if (r >= 0 && r < X) return r;
    load_val(a, s, v);
    return s;
}

static int gp_out(X64* a, int v, int s) {
    int r = a->ra.reg[v];
    return r >= 0 && r < X ? r : s;
}

static void gp_done(X64* a, int v, int r) {
    if (r != a->ra.reg[v]) store_val(a, v, r);
}

// v, a constant whose canonical form a sign-extended imm32 gives
static int imm32_of(X64* a, int v, int32_t* k) {
    IrIns* x = ins_of(a, v);
    if (x->op != IR_CONST) return 0;
    uint64_t c = canon((IrType)x->type, x->imm);
    if ((int64_t)c != (int32_t)c) return 0;
    *k = (int32_t)c;
    return 1;
}

// op r, imm of the 83 / 81 group (ext: 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp)
static void alu_imm(X64* a, int ext, int r, int32_t k) {
    if (k == (int8_t)k) {
        rr(a, 1, (const uint8_t[]){ 0x83 }, 1, ext, r);
        put(a, (uint8_t)k);
    } else {
        rr(a, 1, (const uint8_t[]){ 0x81 }, 1, ext, r);
        put32(a, (uint32_t)k);
    }
}

// imul d, s, imm
static void imul_imm(X64* a, int d, int s, int32_t k) {
    if (k == (int8_t)k) {
        rr(a, 1, (const uint8_t[]){ 0x6B }, 1, d, s);
        put(a, (uint8_t)k);
    } else {
        rr(a, 1, (const uint8_t[]){ 0x69 }, 1, d, s);
        put32(a, (uint32_t)k);
    }
}

// A move of a parallel set: the value v from location src to dst, or, when
// src is NOLOC, v rebuilt (a constant or a stack object's address).
typedef struct { int dst, src, v; IrType t; } Move;

static Move move_of(X64* a, int dst, int v) {
    IrOp op = (IrOp)ins_of(a, v)->op;
    return (Move){ dst, ra_remat(op) ? NOLOC : loc(a, v), v, type_of(a, v) };
}

static void move_emit(X64* a, Move m) {
    if (m.src != NOLOC) {
        move1(a, m.dst, m.src, m.t);
    } else if (m.dst < X) {
        load_val(a, m.dst, m.v);
    } else { // through rax: r11 may hold an indirect call's target, and no xmm or memory move is asm's
        load_val(a, RAX, m.v);
        move1(a, m.dst, RAX, m.t);
    }
}

// Perform moves as if every source were read before any destination is
// written. A cycle of general registers is broken by xchg; any other by
// parking the blocked destination's value in the 16 scratch bytes.
static void parallel_move(X64* a, Move* m, int n) {
    int left = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].dst == m[i].src) m[i].dst = NOLOC;
        if (m[i].dst != NOLOC) left++;
    }
    while (left) {
        int progress = 0;
        for (int i = 0; i < n; i++) {
            if (m[i].dst == NOLOC) continue;
            int blocked = 0;
            for (int j = 0; j < n && !blocked; j++)
                blocked = j != i && m[j].dst != NOLOC && m[j].src == m[i].dst;
            if (blocked) continue;
            move_emit(a, m[i]);
            m[i].dst = NOLOC;
            left--;
            progress = 1;
        }
        if (progress) continue;
        for (int i = 0; i < n; i++) { // every pending move is on a cycle
            if (m[i].dst == NOLOC) continue;
            int d = m[i].dst, s = m[i].src;
            if (d < X && s >= 0 && s < X) { // d = s, s = d: readers of either follow the swap
                rr(a, 1, (const uint8_t[]){ 0x87 }, 1, s, d);
                m[i].dst = NOLOC;
                left--;
                for (int j = 0; j < n; j++)
                    if (m[j].dst != NOLOC) m[j].src = m[j].src == d ? s : m[j].src == s ? d : m[j].src;
                break;
            }
            IrType t = m[i].t;
            for (int j = 0; j < n; j++)
                if (m[j].dst != NOLOC && m[j].src == d) t = m[j].t;
            int park = mem_loc(a->scratch);
            move1(a, park, d, t);
            for (int j = 0; j < n; j++)
                if (m[j].dst != NOLOC && m[j].src == d) m[j].src = park;
            break;
        }
    }
}

static int fail_target(X64* a, int target) {
    if (target == FAIL) a->o->arithmetic_fail_used = 1;
    else if (target == NFAIL) a->o->null_fail_used = 1;
    else if (target == CFAIL) a->o->contract_fail_used = 1;
    return target;
}

static void test_rr(X64* a, int r) { rr(a, 1, (const uint8_t[]){ 0x85 }, 1, r, r); }
static void cmp_rr(X64* a, int x, int y) { rr(a, 1, (const uint8_t[]){ 0x39 }, 1, y, x); } // cmp x, y
// setcc r8 (a REX reaches spl-dil as bytes)
static void setcc(X64* a, int cc, int r) {
    rex(a, 0, 0, r, r >= RSP && r < R8);
    put(a, 0x0F); put(a, (uint8_t)(0x90 | cc)); put(a, (uint8_t)(0xC0 | (r & 7)));
}
// op r8, reg8 (0x20 and, 0x08 or)
static void op8(X64* a, uint8_t op, int reg, int r) {
    rex(a, 0, reg, r, (r >= RSP && r < R8) || (reg >= RSP && reg < R8));
    put(a, op); put(a, (uint8_t)(0xC0 | (reg & 7) << 3 | (r & 7)));
}

static void fail_if(X64* a, int cc, int target) { jump_add(a, cc, fail_target(a, target)); }

// Debug: r (the raw result) must equal its canonical form at t.
static void check_fits(X64* a, IrType t, int r) {
    mov_rr(a, R11, r);
    normalize(a, t, R11);
    cmp_rr(a, r, R11);
    fail_if(a, CC_NE, FAIL);
}

// --- selection --------------------------------------------------------------------------

static int cc_of(int op, IrType t) {
    int s = ir_type_signed(t);
    switch (op) {
    case IR_EQ: return CC_E;
    case IR_NE: return CC_NE;
    case IR_LT: return s ? CC_L : CC_B;
    case IR_LE: return s ? CC_LE : CC_BE;
    case IR_GT: return s ? CC_G : CC_A;
    default: return s ? CC_GE : CC_AE;
    }
}

// d = l op r in two-operand form, in d = v's register: r in place or as an
// immediate, l moved into d first (a commutative op exchanges its operands
// when r already is in d; sub moves such an r aside to rcx). A checked
// unsigned 64-bit multiply takes mul's rdx:rax.
static void arith(X64* a, int v) {
    static const uint8_t op[] = { [IR_ADD] = 0x01, [IR_SUB] = 0x29, [IR_AND] = 0x21, [IR_OR] = 0x09, [IR_XOR] = 0x31 };
    static const uint8_t ext[] = { [IR_ADD] = 0, [IR_SUB] = 5, [IR_AND] = 4, [IR_OR] = 1, [IR_XOR] = 6 };
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int chk = x->flags & IRF_CHECKED, wide = ir_type_bits(t) == 64 || t == IT_PTR;
    int l = x->args[0], r = x->args[1], d;
    int32_t k;
    if (x->op == IR_MUL && chk && wide && !ir_type_signed(t)) {
        load_val(a, RAX, l);
        load_val(a, RCX, r);
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 4, RCX); // mul rcx
        d = RAX;
    } else {
        d = gp_out(a, v, RAX);
        if (x->op != IR_SUB && !imm32_of(a, r, &k) && (imm32_of(a, l, &k) || a->ra.reg[r] == d)) {
            int s = l; l = r; r = s;
        }
        if (imm32_of(a, r, &k)) {
            if (x->op == IR_MUL) {
                imul_imm(a, d, gp_in(a, l, d), k);
            } else {
                load_val(a, d, l);
                alu_imm(a, ext[x->op], d, k);
            }
        } else {
            int s = gp_in(a, r, RCX);
            if (s == d && l != r) {
                mov_rr(a, RCX, s);
                s = RCX;
            }
            load_val(a, d, l);
            if (x->op == IR_MUL) rr(a, 1, (const uint8_t[]){ 0x0F, 0xAF }, 2, d, s); // imul d, s
            else rr(a, 1, (const uint8_t[]){ op[x->op] }, 1, s, d);
        }
    }
    if (chk && x->op != IR_AND && x->op != IR_OR && x->op != IR_XOR) {
        if (!wide) check_fits(a, t, d);
        else if (ir_type_signed(t) || x->op == IR_MUL) fail_if(a, CC_O, FAIL);
        else fail_if(a, CC_B, FAIL); // unsigned carry or borrow
    }
    normalize(a, t, d);
    gp_done(a, v, d);
}

// shl (4), shr (5), or sar (7) r, k
static void shift_imm(X64* a, int ext, int r, int k) {
    if (!k) return;
    rr(a, 1, (const uint8_t[]){ 0xC1 }, 1, ext, r);
    put(a, (uint8_t)k);
}

// Constant division (emit.c div_magic, as arm64.c const_divide): rax = n / c
// or n % c for n canonical at t, through rcx and rdx. A type up to 32 bits
// divides at width 32, where the product of two 32-bit values is exact in 64
// bits; a 64-bit type takes the high half of mul or imul. 0 when c needs the
// hardware divide (zero, or signed -1 with its overflow).
static int const_divide(X64* a, int op, int n, uint64_t c, IrType t) {
    int w = ir_type_bits(t) > 32 ? 64 : 32, sig = ir_type_signed(t);
    uint64_t mask = w == 64 ? UINT64_MAX : UINT32_MAX;
    int64_t sd = (int64_t)c;
    uint64_t d = (sig && sd < 0 ? (uint64_t)0 - c : c) & mask;
    if (d == 0 || (sig && sd == -1)) return 0;
    load_val(a, RCX, n);
    if (d == 1) {
        if (op == IR_DIV) mov_rr(a, RAX, RCX);
        else mov_imm(a, RAX, 0);
        return 1;
    }
    if ((d & (d - 1)) == 0) {
        int k = __builtin_ctzll(d);
        mov_rr(a, RAX, RCX);
        if (!sig) {
            if (op == IR_DIV) {
                shift_imm(a, 5, RAX, k);
            } else if (d - 1 <= INT32_MAX) {
                alu_imm(a, 4, RAX, (int32_t)(d - 1));
            } else {
                mov_imm(a, RDX, d - 1);
                rr(a, 1, (const uint8_t[]){ 0x21 }, 1, RDX, RAX); // and rax, rdx
            }
            return 1;
        }
        shift_imm(a, 7, RAX, 63);     // round toward zero:
        shift_imm(a, 5, RAX, 64 - k); // bias a negative n by d - 1
        rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RCX, RAX); // add rax, rcx
        shift_imm(a, 7, RAX, k);
        if (sd < 0) rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 3, RAX); // neg rax
    } else if (!sig) {
        DivMagic m = div_magic(d, w, 0, 0);
        mov_imm(a, RAX, m.magic);
        if (w == 32) {
            rr(a, 1, (const uint8_t[]){ 0x0F, 0xAF }, 2, RAX, RCX); // imul rax, rcx
            shift_imm(a, 5, RAX, 32 + (m.add ? 0 : m.shift));
        } else {
            rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 4, RCX); // mul rcx
            mov_rr(a, RAX, RDX);
            if (!m.add) shift_imm(a, 5, RAX, m.shift);
        }
        if (m.add) { // t + ((n - t) >> 1)
            mov_rr(a, RDX, RCX);
            rr(a, 1, (const uint8_t[]){ 0x29 }, 1, RAX, RDX); // sub rdx, rax
            shift_imm(a, 5, RDX, 1);
            rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RDX, RAX); // add rax, rdx
            shift_imm(a, 5, RAX, m.shift);
        }
    } else {
        DivMagic m = div_magic(d, w, 1, sd < 0);
        if (w == 32) {
            mov_imm(a, RAX, (uint64_t)(int64_t)(int32_t)(uint32_t)m.magic);
            rr(a, 1, (const uint8_t[]){ 0x0F, 0xAF }, 2, RAX, RCX); // imul rax, rcx
            shift_imm(a, 7, RAX, 32 + (m.add ? 0 : m.shift));
        } else {
            mov_imm(a, RAX, m.magic);
            rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 5, RCX); // imul rcx
            mov_rr(a, RAX, RDX);
            if (!m.add) shift_imm(a, 7, RAX, m.shift);
        }
        if (m.add) {
            rr(a, 1, (const uint8_t[]){ m.neg ? 0x29 : 0x01 }, 1, RCX, RAX); // t -= n or t += n
            shift_imm(a, 7, RAX, m.shift);
        }
        mov_rr(a, RDX, RAX); // q += q < 0
        shift_imm(a, 5, RDX, 63);
        rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RDX, RAX);
    }
    if (op == IR_REM) { // r = n - q * c
        if (sd == (int32_t)sd) {
            imul_imm(a, RAX, RAX, (int32_t)sd);
        } else {
            mov_imm(a, RDX, c);
            rr(a, 1, (const uint8_t[]){ 0x0F, 0xAF }, 2, RAX, RDX); // imul rax, rdx
        }
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 3, RAX); // neg rax
        rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RCX, RAX); // add rax, rcx
    }
    return 1;
}

// mul rcx: the high 64 bits of the unsigned product arrive in rdx
static void mul_high(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    load_val(a, RAX, x->args[0]);
    load_val(a, RCX, x->args[1]);
    rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 4, RCX);
    store_val(a, v, RDX);
}

static void div_rem(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int sg = ir_type_signed(t), chk = x->flags & IRF_CHECKED, rem = x->op == IR_REM;
    int bits = ir_type_bits(t);
    IrIns* y = ins_of(a, x->args[1]);
    if (y->op == IR_CONST && const_divide(a, x->op, x->args[0], canon(t, y->imm), t)) {
        store_val(a, v, RAX); // a quotient or remainder of canonical values is canonical
        return;
    }
    load_val(a, RAX, x->args[0]);
    load_val(a, RCX, x->args[1]);
    uint32_t zero = 0, minus = 0, done_at[2] = { 0, 0 };
    test_rr(a, RCX);
    if (chk) {
        fail_if(a, CC_E, FAIL);
    } else {
        put(a, 0x75); put(a, 0); zero = here(a); // jnz over: a zero divisor yields 0
        mov_imm(a, RAX, 0);
        put(a, 0xEB); put(a, 0); done_at[0] = here(a);
        a->t->d[zero - 1] = (uint8_t)(here(a) - zero);
    }
    if (sg) { // MIN / -1: debug fails, release wraps to MIN (remainder 0)
        rr(a, 1, (const uint8_t[]){ 0x83 }, 1, 7, RCX); put(a, 0xFF); // cmp rcx, -1
        put(a, 0x75); put(a, 0); minus = here(a);
        if (chk) {
            mov_imm(a, RDX, canon(t, 1ull << (bits - 1)));
            cmp_rr(a, RAX, RDX);
            fail_if(a, CC_E, FAIL);
        }
        if (rem) mov_imm(a, RAX, 0);
        else rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 3, RAX); // neg rax
        put(a, 0xEB); put(a, 0); done_at[1] = here(a);
        a->t->d[minus - 1] = (uint8_t)(here(a) - minus);
        put(a, 0x48); put(a, 0x99);                           // cqo
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 7, RCX);       // idiv rcx
    } else {
        mov_imm(a, RDX, 0);
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 6, RCX);       // div rcx
    }
    if (rem) mov_rr(a, RAX, RDX);
    if (!chk) a->t->d[done_at[0] - 1] = (uint8_t)(here(a) - done_at[0]);
    if (sg) a->t->d[done_at[1] - 1] = (uint8_t)(here(a) - done_at[1]);
    normalize(a, t, RAX);
    store_val(a, v, RAX);
}

static void shift(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int bits = ir_type_bits(t), w = bits > 32, chk = x->flags & IRF_CHECKED;
    int ext = x->op == IR_SHL ? 4 : ir_type_signed(t) ? 7 : 5; // shl, sar, shr
    int d = gp_out(a, v, RAX);
    int32_t k;
    // a constant count as the imm8 form (masked as the hardware masks cl), unless out of range in debug
    if (imm32_of(a, x->args[1], &k) && (!chk || (uint64_t)(int64_t)k < (uint64_t)bits)) {
        load_val(a, d, x->args[0]);
        k &= w ? 63 : 31;
        if (k) {
            rr(a, w, (const uint8_t[]){ 0xC1 }, 1, ext, d);
            put(a, (uint8_t)k);
        }
    } else {
        int c = gp_in(a, x->args[1], RCX);
        if (c != RCX) mov_rr(a, RCX, c);
        if (chk) { // a negative count is a large unsigned one
            mov_imm(a, RDX, (uint64_t)bits);
            cmp_rr(a, RCX, RDX);
            fail_if(a, CC_AE, FAIL);
        }
        load_val(a, d, x->args[0]);
        rr(a, w, (const uint8_t[]){ 0xD3 }, 1, ext, d);
    }
    normalize(a, t, d); // a 32-bit op zeroed bits 63:32
    gp_done(a, v, d);
}

static void unary(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int d = gp_out(a, v, RAX);
    load_val(a, d, x->args[0]);
    if (x->op == IR_NOT && t == IT_BOOL) {
        alu_imm(a, 6, d, 1);                                     // xor d, 1
    } else if (x->op == IR_NOT) {
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 2, d);            // not d
    } else {
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 3, d);            // neg d
        if ((x->flags & IRF_CHECKED) && ir_type_signed(t)) {
            if (ir_type_bits(t) == 64) fail_if(a, CC_O, FAIL);
            else check_fits(a, t, d);
        }
    }
    normalize(a, t, d);
    gp_done(a, v, d);
}

// The flags of cmp l, r: r in place or as an immediate, test for zero.
static void cmp_vals(X64* a, int l, int r) {
    int s = gp_in(a, l, RAX);
    int32_t k;
    if (!imm32_of(a, r, &k)) cmp_rr(a, s, gp_in(a, r, RCX));
    else if (k) alu_imm(a, 7, s, k);
    else test_rr(a, s);
}

static void compare(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)ins_of(a, x->args[0])->type;
    cmp_vals(a, x->args[0], x->args[1]);
    int d = gp_out(a, v, RAX);
    setcc(a, cc_of(x->op, t), d);
    normalize(a, IT_BOOL, d);
    gp_done(a, v, d);
}

static void cast(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    if (a->ra.with[v] == x->args[0]) return; // in place: the operand's location already holds it
    int d = gp_out(a, v, RAX);
    if (x->type == IT_BOOL) { // nonzero is true
        test_rr(a, gp_in(a, x->args[0], RAX));
        setcc(a, CC_NE, d);
    } else {
        load_val(a, d, x->args[0]);
    }
    normalize(a, (IrType)x->type, d);
    gp_done(a, v, d);
}

static void bitop(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type, at = (IrType)ins_of(a, x->args[0])->type;
    int bits = ir_type_bits(at);
    load_val(a, RAX, x->args[0]);
    switch (x->op) {
    case IR_POPCNT:
        put(a, 0xF3); rr(a, 1, (const uint8_t[]){ 0x0F, 0xB8 }, 2, RAX, RAX);
        break;
    case IR_CLZ: case IR_CTZ: // bsr / bsf index; zero: clz the width (index -1), ctz the width
        mov_imm(a, RCX, x->op == IR_CLZ ? ~0ull : (uint64_t)bits);
        rr(a, 1, x->op == IR_CLZ ? (const uint8_t[]){ 0x0F, 0xBD } : (const uint8_t[]){ 0x0F, 0xBC }, 2, RDX, RAX);
        rr(a, 1, (const uint8_t[]){ 0x0F, 0x44 }, 2, RDX, RCX); // cmovz rdx, rcx
        if (x->op == IR_CLZ) {
            mov_imm(a, RAX, (uint64_t)(bits - 1));
            rr(a, 1, (const uint8_t[]){ 0x29 }, 1, RDX, RAX); // rax = bits - 1 - index
        } else {
            mov_rr(a, RAX, RDX);
        }
        break;
    case IR_BSWAP:
        if (bits == 8) {
            break;
        } else if (bits == 16) {
            put(a, 0x66); put(a, 0xC1); put(a, 0xC0); put(a, 8); // rol ax, 8
        } else {
            rex(a, bits == 64, 0, RAX, 0); put(a, 0x0F); put(a, 0xC8); // bswap
        }
        break;
    default: { // IR_ROTL, IR_ROTR by u64 args[1] modulo the width
        load_val(a, RCX, x->args[1]);
        int ext = x->op == IR_ROTL ? 0 : 1;
        if (bits == 8) {
            put(a, 0xD2); put(a, (uint8_t)(0xC0 | ext << 3));
        } else if (bits == 16) {
            put(a, 0x66); put(a, 0xD3); put(a, (uint8_t)(0xC0 | ext << 3));
        } else {
            rr(a, bits == 64, (const uint8_t[]){ 0xD3 }, 1, ext, RAX);
        }
        break;
    }
    }
    normalize(a, at, RAX);
    normalize(a, t, RAX);
    store_val(a, v, RAX);
}

// --- floats -----------------------------------------------------------------------------

// A float's slot holds its bits in canonical form, an f32's zero-extended (as
// canon gives a constant), so constants, phis, loads, stores, and aggregate
// leaves take the integer paths. Each operation moves its operands through
// rax into xmm0-xmm2 and its result back; xmm10-xmm15 stay the byte
// routines' own.

// op reg, rm after the mandatory prefix pfx (0 for none); w sets REX.W
static void xop(X64* a, int pfx, int w, uint8_t op, int reg, int r) {
    if (pfx) put(a, (uint8_t)pfx);
    rr(a, w, (const uint8_t[]){ 0x0F, op }, 2, reg, r);
}

static int fpfx(IrType t) { return t == IT_F64 ? 0xF2 : 0xF3; } // the sd or ss form

static void xmm_from(X64* a, int x, int r) { xop(a, 0x66, 1, 0x6E, x, r); } // movq xmm, r64

// r = the bits of xmm x at type t: movq, or movd zero-extending an f32
static void xmm_to(X64* a, IrType t, int r, int x) { xop(a, 0x66, t == IT_F64, 0x7E, x, r); }

// xmm x = v's bits: from its location, or a constant through rax.
static void fload(X64* a, int x, int v) {
    if (ra_remat((IrOp)ins_of(a, v)->op)) {
        load_val(a, RAX, v);
        xmm_from(a, x, RAX);
    } else {
        move1(a, X + x, loc(a, v), type_of(a, v));
    }
}

static void fstore(X64* a, int v, IrType t, int x) {
    int l = loc(a, v);
    if (l != NOLOC) move1(a, l, X + x, t);
}

// The xmm counterparts of gp_in, gp_out, and gp_done: xmm k as k.
static int x_in(X64* a, int v, int s) {
    int r = a->ra.reg[v];
    if (r >= X) return r - X;
    fload(a, s, v);
    return s;
}

static int x_out(X64* a, int v, int s) {
    int r = a->ra.reg[v];
    return r >= X ? r - X : s;
}

static void x_done(X64* a, int v, int x) {
    if (X + x != a->ra.reg[v]) fstore(a, v, type_of(a, v), x);
}

static void movaps(X64* a, int d, int s) {
    if (d != s) xop(a, 0, 0, 0x28, d, s);
}

static void xorps(X64* a, int x) { xop(a, 0, 0, 0x57, x, x); }
static void ucomis(X64* a, IrType t, int x, int y) { xop(a, t == IT_F64 ? 0x66 : 0, 0, 0x2E, x, y); }
static void cmov(X64* a, int cc, int dst, int src) { rr(a, 1, (const uint8_t[]){ 0x0F, (uint8_t)(0x40 | cc) }, 2, dst, src); }

// A short jcc (jmp when cc < 0) whose rel8 land8 patches to here.
static uint32_t jmp8(X64* a, int cc) {
    put(a, cc < 0 ? 0xEB : (uint8_t)(0x70 | cc));
    put(a, 0);
    return here(a);
}

static void land8(X64* a, uint32_t at) { a->t->d[at - 1] = (uint8_t)(here(a) - at); }

// d = l op r in two-operand form, as arith: in place in v's register.
static void farith(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    static const uint8_t op[] = { [IR_ADD] = 0x58, [IR_SUB] = 0x5C, [IR_MUL] = 0x59, [IR_DIV] = 0x5E };
    int l = x->args[0], r = x->args[1], d = x_out(a, v, XMM0);
    if ((x->op == IR_ADD || x->op == IR_MUL) && a->ra.reg[r] == X + d) {
        int s = l; l = r; r = s;
    }
    int s = x_in(a, r, XMM1);
    if (s == d && l != r) {
        movaps(a, XMM1, s);
        s = XMM1;
    }
    if (a->ra.reg[l] >= X) movaps(a, d, a->ra.reg[l] - X);
    else fload(a, d, l);
    xop(a, fpfx(t), 0, op[x->op], d, s);
    x_done(a, v, d);
}

// The sign bit flipped (btc), as fneg does: a NaN keeps its payload.
static void fneg(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    load_val(a, RAX, x->args[0]);
    rr(a, t == IT_F64, (const uint8_t[]){ 0x0F, 0xBA }, 2, 7, RAX);
    put(a, t == IT_F64 ? 63 : 31);
    store_val(a, v, RAX);
}

// ucomis, lt and le as gt and ge with the operands exchanged: seta and setae
// fail on an unordered result (CF and ZF set), eq also needs PF clear, and ne
// holds on PF set.
static void fcompare(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    int l = x->args[0], r = x->args[1], op = x->op;
    IrType t = (IrType)ins_of(a, l)->type;
    if (op == IR_LT || op == IR_LE) {
        int s = l; l = r; r = s;
        op = op == IR_LT ? IR_GT : IR_GE;
    }
    int xl = x_in(a, l, XMM0);
    ucomis(a, t, xl, x_in(a, r, XMM1));
    int d = gp_out(a, v, RAX);
    setcc(a, op == IR_EQ ? CC_E : op == IR_NE ? CC_NE : op == IR_GT ? CC_A : CC_AE, d);
    if (op == IR_EQ || op == IR_NE) {
        setcc(a, op == IR_EQ ? CC_NP : CC_P, RCX);
        op8(a, op == IR_EQ ? 0x20 : 0x08, RCX, d); // and d8, cl, or or d8, cl
    }
    normalize(a, IT_BOOL, d);
    gp_done(a, v, d);
}

// A conversion to or from a float, as arm64's: cvtss2sd or cvtsd2ss between
// widths; cvtsi2s from an integer's canonical 64-bit form, a u64 with its top
// bit set halved (the low bit kept sticky), converted, and doubled; to an
// integer cvtts2si, toward zero, saturating at 64 bits for a 64-bit result
// and at 32 bits for a narrower one, which then wraps. cvtts2si yields MIN
// for a NaN or an out-of-range value, which the saturation corrects: a NaN
// is 0.
static void fcast(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType to = (IrType)x->type, from = (IrType)ins_of(a, x->args[0])->type;
    if (ir_type_float(from) && ir_type_float(to)) {
        int d = x_out(a, v, XMM0), s = x_in(a, x->args[0], d);
        if (from != to) xop(a, fpfx(from), 0, 0x5A, d, s);
        else movaps(a, d, s);
        x_done(a, v, d);
        return;
    }
    if (ir_type_float(to) && from != IT_U64) {
        int s = gp_in(a, x->args[0], RAX), d = x_out(a, v, XMM0);
        xorps(a, d); // cvtsi2s writes only the low lane: no false dependency
        xop(a, fpfx(to), 1, 0x2A, d, s);
        x_done(a, v, d);
        return;
    }
    if (ir_type_float(to)) { // a u64 with its top bit set halved, converted, and doubled
        load_val(a, RAX, x->args[0]);
        xorps(a, XMM0);
        test_rr(a, RAX);
        uint32_t big = jmp8(a, CC_S);
        xop(a, fpfx(to), 1, 0x2A, XMM0, RAX);
        uint32_t done = jmp8(a, -1);
        land8(a, big);
        mov_rr(a, RCX, RAX);
        rr(a, 1, (const uint8_t[]){ 0xD1 }, 1, 5, RCX);            // shr rcx, 1
        rr(a, 0, (const uint8_t[]){ 0x83 }, 1, 4, RAX); put(a, 1); // and eax, 1
        rr(a, 1, (const uint8_t[]){ 0x09 }, 1, RAX, RCX);          // or rcx, rax
        xop(a, fpfx(to), 1, 0x2A, XMM0, RCX);
        xop(a, fpfx(to), 0, 0x58, XMM0, XMM0);
        land8(a, done);
        fstore(a, v, to, XMM0);
        return;
    }
    fload(a, XMM0, x->args[0]);
    if (to == IT_U64) { // at or past 2^63: less 2^63, converted, the top bit set
        mov_imm(a, RCX, from == IT_F64 ? 0x43E0000000000000ull : 0x5F000000u);
        xmm_from(a, XMM1, RCX);
        ucomis(a, from, XMM0, XMM1);
        uint32_t big = jmp8(a, CC_AE);  // a NaN is unordered: below
        mov_imm(a, RDX, 0);
        xop(a, fpfx(from), 1, 0x2C, RAX, XMM0);
        test_rr(a, RAX);
        cmov(a, CC_S, RAX, RDX);        // negative or NaN: 0
        uint32_t done = jmp8(a, -1);
        land8(a, big);
        xop(a, fpfx(from), 0, 0x5C, XMM0, XMM1);
        xop(a, fpfx(from), 1, 0x2C, RAX, XMM0);
        mov_imm(a, RCX, ~0ull);
        test_rr(a, RAX);
        cmov(a, CC_S, RAX, RCX);        // at or past 2^64: the maximum
        rr(a, 1, (const uint8_t[]){ 0x0F, 0xBA }, 2, 5, RAX); put(a, 63); // bts rax, 63
        land8(a, done);
    } else if (ir_type_signed(to)) {
        int w = to == IT_I64;
        xop(a, fpfx(from), w, 0x2C, RAX, XMM0);
        mov_imm(a, RCX, w ? 1ull << 63 : 0x80000000u);
        cmp_rr(a, RAX, RCX);
        uint32_t done = jmp8(a, CC_NE);
        mov_imm(a, RCX, w ? (uint64_t)INT64_MAX : 0x7FFFFFFFu);
        mov_imm(a, RDX, 0);
        xorps(a, XMM1);
        ucomis(a, from, XMM0, XMM1);
        cmov(a, CC_A, RAX, RCX);        // positive: the maximum
        cmov(a, CC_P, RAX, RDX);        // NaN: 0
        land8(a, done);
    } else { // at most 32 bits: saturate at u32 from a 64-bit conversion
        mov_imm(a, RCX, 0xFFFFFFFFu);
        mov_imm(a, RDX, 0);
        xorps(a, XMM1);
        xop(a, fpfx(from), 1, 0x2C, RAX, XMM0);
        ucomis(a, from, XMM0, XMM1);
        cmov(a, CC_A, RDX, RCX);        // what MIN means: the maximum when positive, else 0
        test_rr(a, RAX);
        cmov(a, CC_S, RAX, RDX);
        cmp_rr(a, RAX, RCX);
        cmov(a, CC_A, RAX, RCX);
    }
    normalize(a, to, RAX);
    store_val(a, v, RAX);
}

// The same bits in the result type's canonical form.
static void bitcast(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    load_val(a, RAX, x->args[0]);
    normalize(a, (IrType)x->type, RAX);
    store_val(a, v, RAX);
}

// Exact IEEE operations: sqrts, rounds toward negative or positive infinity
// (inexact suppressed), and vfmadd213s, which rounds once (FMA3).
static void fexact(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    if (x->op == IR_FMA) { // xmm0 = xmm1 * xmm0 + xmm2
        fload(a, XMM0, x->args[0]);
        fload(a, XMM1, x->args[1]);
        fload(a, XMM2, x->args[2]);
        put(a, 0xC4); put(a, 0xE2); put(a, t == IT_F64 ? 0xF1 : 0x71); put(a, 0xA9); put(a, 0xC2);
        fstore(a, v, t, XMM0);
        return;
    }
    int d = x_out(a, v, XMM0), s = x_in(a, x->args[0], d);
    if (x->op == IR_SQRT) {
        xop(a, fpfx(t), 0, 0x51, d, s);
    } else {
        put(a, 0x66);
        rr(a, 0, (const uint8_t[]){ 0x0F, 0x3A, t == IT_F64 ? 0x0B : 0x0A }, 3, d, s);
        put(a, x->op == IR_FLOOR ? 9 : 10);
    }
    x_done(a, v, d);
}

// A vector value (ir.h) lives in its own 16-byte slot, aligned as rbp is, and
// moves whole into xmm0-xmm2. Each operation is lane-wise SSE (up to SSE4.1),
// matching the arm64 target's NEON lane for lane.

// movdqa (aligned) or movdqu of xmm x from (load) or to [base + disp]
static void xmove(X64* a, int load, int aligned, int x, int base, int32_t disp) {
    put(a, aligned ? 0x66 : 0xF3);
    rm(a, 0, (const uint8_t[]){ 0x0F, load ? 0x6F : 0x7F }, 2, x, base, disp, 0);
}

static void vload(X64* a, int x, int v) { move1(a, X + x, loc(a, v), type_of(a, v)); }

static void vstore(X64* a, int v, int x) {
    int l = loc(a, v);
    if (l != NOLOC) move1(a, l, X + x, type_of(a, v));
}

// op x, y after the prefix pfx (0 for none): 0F op, or 0F 38 op for op38
static void vop(X64* a, int pfx, int op38, uint8_t op, int x, int y) {
    if (pfx) put(a, (uint8_t)pfx);
    if (op38) rr(a, 0, (const uint8_t[]){ 0x0F, 0x38, op }, 3, x, y);
    else rr(a, 0, (const uint8_t[]){ 0x0F, op }, 2, x, y);
}

// op x, imm8 of the 66 0F 72/73 shift group (ext: 4 psra, 6 psll)
static void vshift(X64* a, uint8_t op, int ext, int x, int n) {
    vop(a, 0x66, 0, op, ext, x);
    put(a, (uint8_t)n);
}

// x = all ones (pcmpeqd x, x)
static void vones(X64* a, int x) { vop(a, 0x66, 0, 0x76, x, x); }

// A lane-wise vector operation (ir.h): a compare to a mask (float lt and le
// by cmpps, gt and ge as them with the operands exchanged, ne as NEQ_UQ,
// true on a NaN as arm64's inverted eq is; an integer lt as gt exchanged, ne,
// le, and ge as eq, gt, and lt inverted), neg as a sign flip or a subtraction
// from zero, and select through blendvps on xmm0's mask.
static void vector(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    if (x->op == IR_LANES) { // <0, 1, 2, 3> as two words, joined by punpcklqdq
        mov_imm(a, RAX, 1ull << 32);
        xmm_from(a, XMM0, RAX);
        mov_imm(a, RAX, 3ull << 32 | 2);
        xmm_from(a, XMM1, RAX);
        vop(a, 0x66, 0, 0x6C, XMM0, XMM1);
        vstore(a, v, XMM0);
        return;
    }
    if (x->op == IR_SPLAT) { // the scalar's bits in lane 0, copied by pshufd
        fload(a, XMM0, x->args[0]);
        vop(a, 0x66, 0, 0x70, XMM0, XMM0);
        put(a, t == IT_V2F64 ? 0x44 : 0x00);
        vstore(a, v, XMM0);
        return;
    }
    if (x->op == IR_SELECT) { // xmm2 = xmm1's lanes where xmm0's are set
        vload(a, XMM0, x->args[0]);
        vload(a, XMM1, x->args[1]);
        vload(a, XMM2, x->args[2]);
        vop(a, 0x66, 1, 0x14, XMM2, XMM1);
        vstore(a, v, XMM2);
        return;
    }
    vload(a, XMM0, x->args[0]);
    if (x->op == IR_SHL || x->op == IR_SHR) { // by an immediate (ir.h); shr is arithmetic, as i32's
        vshift(a, 0x72, x->op == IR_SHL ? 6 : 4, XMM0, (int)ins_of(a, x->args[1])->imm);
        vstore(a, v, XMM0);
        return;
    }
    if (x->op == IR_CAST) { // v4i32 to v4f32: cvtdq2ps
        vop(a, 0, 0, 0x5B, XMM0, XMM0);
        vstore(a, v, XMM0);
        return;
    }
    if (x->op == IR_NEG) {
        if (t == IT_V4I32) { // 0 - x
            vop(a, 0x66, 0, 0xEF, XMM1, XMM1);
            vop(a, 0x66, 0, 0xFA, XMM1, XMM0);
            vstore(a, v, XMM1);
            return;
        }
        vones(a, XMM1); // the sign bits flipped, as fneg does
        vshift(a, t == IT_V2F64 ? 0x73 : 0x72, 6, XMM1, t == IT_V2F64 ? 63 : 31);
        vop(a, 0, 0, 0x57, XMM0, XMM1);
        vstore(a, v, XMM0);
        return;
    }
    int l = XMM0, r = XMM1, op = x->op;
    IrType ot = (IrType)ins_of(a, x->args[0])->type;
    vload(a, XMM1, x->args[1]);
    int pd = ot == IT_V2F64 ? 0x66 : 0;
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV:
        if (ot == IT_V4I32) {
            if (op == IR_MUL) vop(a, 0x66, 1, 0x40, l, r); // pmulld
            else vop(a, 0x66, 0, op == IR_ADD ? 0xFE : 0xFA, l, r);
        } else {
            static const uint8_t fop[] = { [IR_ADD] = 0x58, [IR_SUB] = 0x5C, [IR_MUL] = 0x59, [IR_DIV] = 0x5E };
            vop(a, pd, 0, fop[op], l, r);
        }
        break;
    case IR_AND: vop(a, 0x66, 0, 0xDB, l, r); break;
    case IR_OR: vop(a, 0x66, 0, 0xEB, l, r); break;
    case IR_XOR: vop(a, 0x66, 0, 0xEF, l, r); break;
    default: // a compare
        if (ot == IT_V4F32) {
            static const uint8_t pred[] = { [IR_EQ] = 0, [IR_NE] = 4, [IR_LT] = 1, [IR_LE] = 2, [IR_GT] = 1, [IR_GE] = 2 };
            if (op == IR_GT || op == IR_GE) l = XMM1, r = XMM0;
            vop(a, 0, 0, 0xC2, l, r);
            put(a, pred[op]);
        } else {
            if (op == IR_LT || op == IR_GE) l = XMM1, r = XMM0;
            vop(a, 0x66, 0, op == IR_EQ || op == IR_NE ? 0x76 : 0x66, l, r); // pcmpeqd, pcmpgtd
            if (op == IR_NE || op == IR_LE || op == IR_GE) {
                vones(a, r);
                vop(a, 0x66, 0, 0xEF, l, r);
            }
        }
        break;
    }
    vstore(a, v, l);
}

// r = the value of type t at [base + off] in canonical form (a mov with extension)
static void ld_width(X64* a, IrType t, int r, int base, int32_t off) {
    int sg = ir_type_signed(t);
    switch (t == IT_BOOL ? 8 : ir_type_bits(t)) {
    case 8: rm(a, sg, sg ? (const uint8_t[]){ 0x0F, 0xBE } : (const uint8_t[]){ 0x0F, 0xB6 }, 2, r, base, off, 0); break;
    case 16: rm(a, sg, sg ? (const uint8_t[]){ 0x0F, 0xBF } : (const uint8_t[]){ 0x0F, 0xB7 }, 2, r, base, off, 0); break;
    case 32: rm(a, sg, sg ? (const uint8_t[]){ 0x63 } : (const uint8_t[]){ 0x8B }, 1, r, base, off, 0); break;
    default: ld64(a, r, base, off); break;
    }
    if (t == IT_BOOL) normalize(a, t, r);
}

// [base + off] = r at type t's width
static void st_width(X64* a, IrType t, int r, int base, int32_t off) {
    switch (t == IT_BOOL ? 8 : ir_type_bits(t)) {
    case 8: rm(a, 0, (const uint8_t[]){ 0x88 }, 1, r, base, off, (r & 7) >= RSP && r < R8); break; // sil, dil need a REX
    case 16: put(a, 0x66); rm(a, 0, (const uint8_t[]){ 0x89 }, 1, r, base, off, 0); break;
    case 32: rm(a, 0, (const uint8_t[]){ 0x89 }, 1, r, base, off, 0); break;
    default: st64(a, r, base, off); break;
    }
}

// Words w0..w1 of an aggregate's leaves lv[0..n), whose values are vals[k],
// to [base + at + 8w]: each word built whole in rax (rcx scratch) and stored
// once, so the 8-byte load that reads it back forwards from one store.
static void pack_words(X64* a, const IrLeaf* lv, const int* vals, int n, int words, int base, int32_t at) {
    for (int w = 0; w < words; w++) {
        int any = 0;
        for (int k = 0; k < n; k++) {
            if (lv[k].off / 8 != (uint32_t)w) continue;
            IrType t = (IrType)ins_of(a, vals[k])->type;
            int r = any ? RCX : RAX;
            load_val(a, r, vals[k]);
            int bits = t == IT_BOOL ? 8 : ir_type_bits(t);
            if (bits == 8) normalize(a, IT_U8, r);
            else if (bits == 16) normalize(a, IT_U16, r);
            else if (bits == 32) normalize(a, IT_U32, r);
            if (lv[k].off % 8) rr(a, 1, (const uint8_t[]){ 0xC1 }, 1, 4, r), put(a, (uint8_t)(lv[k].off % 8 * 8)); // shl r, imm8
            if (any) rr(a, 1, (const uint8_t[]){ 0x09 }, 1, RCX, RAX);                            // or rax, rcx
            any = 1;
        }
        if (!any) mov_imm(a, RAX, 0);
        st64(a, RAX, base, at + 8 * w);
    }
}

// [base + off] = imm at type t's width (mov m, imm; a 64-bit store sign-extends it)
static void st_imm(X64* a, IrType t, int32_t k, int base, int32_t off) {
    switch (t == IT_BOOL ? 8 : ir_type_bits(t)) {
    case 8: rm(a, 0, (const uint8_t[]){ 0xC6 }, 1, 0, base, off, 0); put(a, (uint8_t)k); break;
    case 16: put(a, 0x66); rm(a, 0, (const uint8_t[]){ 0xC7 }, 1, 0, base, off, 0); buf_put16(a->t, (uint16_t)k); break;
    case 32: rm(a, 0, (const uint8_t[]){ 0xC7 }, 1, 0, base, off, 0); put32(a, (uint32_t)k); break;
    default: rm(a, 1, (const uint8_t[]){ 0xC7 }, 1, 0, base, off, 0); put32(a, (uint32_t)k); break;
    }
}

// The base of an access at off into stack object s: rbp with off moved to
// s's rbp offset, or, when that sum overflows int32 (fold_address runs before
// the frame is laid out), s's address in rax with off as it is.
static int slot_base(X64* a, int s, int32_t* off) {
    int64_t at = (int64_t)*off + a->home[s];
    if (at == (int32_t)at) {
        *off = (int32_t)at;
        return RBP;
    }
    lea(a, RAX, RBP, a->home[s]);
    return RAX;
}

// A load or store of type t at [base + off]: base the pointer's register (a
// stack object's is rbp and its offset), or the address it folds (fuse): a
// global rip-relative, an unchecked pointer offset's constant in off or its
// register index scaled; the value in place in its register, general or xmm
// (movss, movq, movdqu), or a store's constant as an immediate.
static void access(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    int load = x->op == IR_LOAD, p = x->args[0], base;
    IrIns* px = ins_of(a, p);
    IrType t = (IrType)(load ? x->type : ins_of(a, x->args[1])->type);
    int32_t off = x->aux, k;
    if (px->op == IR_SLOT) { // never null
        base = slot_base(a, p, &off);
    } else if (a->ra.fused[p] && px->op == IR_GLOBAL) {
        a->fold = (Fold){ -1, -1, 0, 0, px->name };
        base = AFOLD;
    } else if (a->ra.fused[p]) {
        int b = px->args[0], n = px->args[1], index = -1;
        if (imm32_of(a, n, &k)) off += (int32_t)(px->op == IR_PSUB ? -(int64_t)k * (int64_t)px->imm : (int64_t)k * (int64_t)px->imm);
        else index = gp_in(a, n, RDX);
        base = ins_of(a, b)->op == IR_SLOT ? slot_base(a, b, &off) : gp_in(a, b, RAX);
        if (index >= 0) {
            a->fold = (Fold){ base, index, __builtin_ctzll(px->imm), 0, NULL };
            base = AFOLD;
        }
    } else {
        base = gp_in(a, p, RAX);
        if (x->flags & IRF_CHECKED) {
            test_rr(a, base);
            fail_if(a, CC_E, NFAIL);
        }
    }
    int u = load ? v : x->args[1];
    if (ir_type_vector(t) || a->ra.reg[u] >= X) { // movdqu, or an f32's movss (the upper lanes zeroed) or an f64's movq
        int r = load ? x_out(a, v, XMM0) : x_in(a, u, XMM0);
        if (ir_type_vector(t)) xmove(a, load, 0, r, base, off);
        else if (t == IT_F32) { put(a, 0xF3); rm(a, 0, (const uint8_t[]){ 0x0F, load ? 0x10 : 0x11 }, 2, r, base, off, 0); }
        else if (load) xld(a, r, base, off);
        else xst(a, r, base, off);
        if (load) x_done(a, v, r);
        return;
    }
    if (load) {
        int d = gp_out(a, v, RCX);
        ld_width(a, t, d, base, off);
        gp_done(a, v, d);
    } else if (imm32_of(a, u, &k)) {
        int w = t == IT_BOOL ? 8 : ir_type_bits(t);
        a->fold.imm = w == 8 ? 1 : w == 16 ? 2 : 4; // what follows a rip-relative displacement
        st_imm(a, t, k, base, off);
    } else {
        st_width(a, t, gp_in(a, u, RCX), base, off);
    }
}

// d = p +/- n * scale (Pointers #9): release folds a constant n into an
// immediate and scales a register n by shl or imul; debug checks a null
// start, the scaled offset's overflow, and a result past either end of the
// address space, through mul.
static void pointer_offset(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    int sub = x->op == IR_PSUB, chk = x->flags & IRF_CHECKED;
    int32_t k;
    int64_t off;
    if (!chk && imm32_of(a, x->args[1], &k) && !__builtin_mul_overflow((int64_t)k, (int64_t)x->imm, &off) &&
        off == (int32_t)off && off != INT32_MIN) {
        int d = gp_out(a, v, RAX), p = x->args[0];
        int64_t at = a->home[p] + (sub ? -off : off);
        if (ins_of(a, p)->op == IR_SLOT && at == (int32_t)at) { // into a stack object: one lea from rbp
            lea(a, d, RBP, (int32_t)at);
            gp_done(a, v, d);
            return;
        }
        load_val(a, d, p);
        if (off) alu_imm(a, 0, d, (int32_t)(sub ? -off : off));
        gp_done(a, v, d);
        return;
    }
    if (!chk && x->imm <= INT32_MAX) {
        int d = gp_out(a, v, RAX), n = gp_in(a, x->args[1], RCX);
        if (x->imm != 1 || n == d) { // rcx = n * scale
            if (!x->imm || (x->imm & (x->imm - 1))) {
                imul_imm(a, RCX, n, (int32_t)x->imm);
            } else {
                if (n != RCX) mov_rr(a, RCX, n);
                if (x->imm > 1) {
                    rr(a, 1, (const uint8_t[]){ 0xC1 }, 1, 4, RCX);
                    put(a, (uint8_t)__builtin_ctzll(x->imm));
                }
            }
            n = RCX;
        }
        load_val(a, d, x->args[0]);
        rr(a, 1, sub ? (const uint8_t[]){ 0x29 } : (const uint8_t[]){ 0x01 }, 1, n, d);
        gp_done(a, v, d);
        return;
    }
    load_val(a, RAX, x->args[1]);
    mov_imm(a, RCX, x->imm);
    rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 4, RCX); // mul rcx: rdx:rax = n * scale
    if (chk) fail_if(a, CC_O, FAIL);
    mov_rr(a, RCX, RAX);
    load_val(a, RAX, x->args[0]);
    if (chk) {
        test_rr(a, RAX);
        fail_if(a, CC_E, FAIL);
    }
    rr(a, 1, sub ? (const uint8_t[]){ 0x29 } : (const uint8_t[]){ 0x01 }, 1, RCX, RAX);
    if (chk) fail_if(a, CC_B, FAIL);
    store_val(a, v, RAX);
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// --- calling convention -------------------------------------------------------------

// Where one part of an argument, parameter, or result travels (System V
// AMD64): a scalar, or an aggregate of up to 16 bytes, in one register per
// eightbyte, an SSE one when every leaf in it is a float and an integer one
// otherwise; the whole part on the stack at soff when its registers run out;
// an aggregate over 16 bytes or with a misaligned (packed) leaf (MEMORY)
// always on the stack, as a copy. reg[w] is the register of eightbyte w, or -1
// on the stack; SSE + k is xmm k; NOCLASS marks padding, which takes none.
enum { SSE = 32, NOCLASS = 64 };
typedef struct { int8_t reg[2]; int8_t words; uint32_t soff, size; } Pass;

typedef struct { int ni, ns; uint32_t soff; } Cc;

static IrPart part_of(const IrSig* sig, int p) { return sig ? sig->parts[p] : (IrPart){ NULL, p, 1, 0 }; }

// A result System V returns through memory at an address the caller passes
// in rdi and the callee returns in rax: an aggregate over 16 bytes.
static int ret_in_memory(const IrSig* sig) {
    return sig && sig->ret.agg && (sema_size(sig->ret.agg) > 16 || ir_misaligned(sig->ret.agg));
}

int x64_result_in_memory(SType* t) { return t && ir_type_of(t) < 0 && (sema_size(t) > 16 || ir_misaligned(t)); }

// Assign part pt, whose first leaf has type t0, from the next registers or
// the stack; lv receives an aggregate's leaves.
static void pass_part(Cc* c, IrPart pt, IrType t0, IrLeaf* lv, Pass* o) {
    o->reg[0] = o->reg[1] = -1;
    if (!pt.agg) { // a scalar
        o->words = 1;
        o->size = 8;
        if (ir_type_float(t0) || ir_type_vector(t0)) {
            if (c->ns < 8) { o->reg[0] = (int8_t)(SSE + c->ns++); return; }
        } else if (c->ni < 6) {
            o->reg[0] = (int8_t)ARG_REG[c->ni++];
            return;
        }
        o->soff = c->soff;
        c->soff += 8;
        return;
    }
    SType* bad;
    if (!pt.addr) ir_leaves(pt.agg, lv, &bad);
    o->size = sema_size(pt.agg);
    o->words = (int8_t)((o->size + 7) / 8);
    if (!pt.addr && o->size <= 16 && !ir_misaligned(pt.agg)) {
        int ni = 0, ns = 0, cls[2];
        for (int w = 0; w < o->words; w++) {
            cls[w] = ir_word_class(pt.agg, (uint32_t)w);
            ns += cls[w] == IR_WORD_SSE;
            ni += cls[w] == IR_WORD_INT;
        }
        if (c->ni + ni <= 6 && c->ns + ns <= 8) {
            for (int w = 0; w < o->words; w++)
                o->reg[w] = (int8_t)(cls[w] == IR_WORD_NONE ? NOCLASS : cls[w] == IR_WORD_SSE ? SSE + c->ns++ : ARG_REG[c->ni++]);
            return;
        }
    }
    c->soff = align_up(c->soff, sema_align(pt.agg) > 8 ? 16 : 8);
    o->soff = c->soff;
    c->soff += align_up(o->size, 8);
}

// A call's plan: its parts, and the outgoing bytes at [rsp]: the stack
// arguments, then 16 bytes of register words per aggregate part in
// registers (at img), then a result through memory (at buf).
typedef struct {
    Pass*    pass;
    IrLeaf*  lv;      // IR_MAX_LEAVES per part
    int      k0, n, np, sret, mem_ret, nsse;
    uint32_t img, buf, bytes;
} Plan;

static void plan_call(X64* a, IrIns* x, Plan* p) {
    p->k0 = x->op == IR_CALLI;
    p->sret = (x->flags & IRF_SRET) != 0;
    p->n = x->nargs - p->k0 - p->sret;
    p->np = x->sig ? x->sig->nparts : p->n;
    p->mem_ret = p->sret || ret_in_memory(x->sig);
    p->pass = xcalloc((size_t)p->np, sizeof(Pass));
    p->lv = xcalloc((size_t)p->np * IR_MAX_LEAVES, sizeof(IrLeaf));
    Cc c = { p->mem_ret, 0, 0 };
    int nimg = 0;
    for (int q = 0; q < p->np; q++) {
        IrPart pt = part_of(x->sig, q);
        pass_part(&c, pt, (IrType)ins_of(a, x->args[p->k0 + pt.first])->type, p->lv + q * IR_MAX_LEAVES, &p->pass[q]);
        if (pt.agg && p->pass[q].reg[0] >= 0) nimg++;
    }
    p->nsse = c.ns;
    p->img = align_up(c.soff, 16);
    p->buf = p->img + 16 * (uint32_t)(nimg ? nimg : 1); // one image also holds a result's registers
    p->bytes = p->buf + (p->mem_ret && !p->sret ? align_up(sema_size(x->sig->ret.agg), 16) : 0);
}

static void plan_free(Plan* p) {
    free(p->pass);
    free(p->lv);
}

// movq xmm x, [base + disp] and back
static void xld(X64* a, int x, int base, int32_t disp) { put(a, 0xF3); rm(a, 0, (const uint8_t[]){ 0x0F, 0x7E }, 2, x, base, disp, 0); }
static void xst(X64* a, int x, int base, int32_t disp) { put(a, 0x66); rm(a, 0, (const uint8_t[]){ 0x0F, 0xD6 }, 2, x, base, disp, 0); }

// The register of a result's eightbyte, planned as an argument's: the k-th
// integer one in rax or rdx, the k-th SSE one in xmm0 or xmm1.
static int ret_reg(int r) { return r >= SSE ? r : r == ARG_REG[0] ? RAX : RDX; }

// The call at bb->ins[i], whose aggregate result's projections follow it.
static void call(X64* a, IrBlock* bb, int i) {
    int v = bb->ins[i];
    IrIns* x = ins_of(a, v);
    Plan p;
    plan_call(a, x, &p);
    const IrSig* sig = x->sig;
    // the stack parts and the register images first: they use rax, rcx, rsi, and rdi
    uint32_t img = p.img;
    for (int q = 0; q < p.np && !a->failed; q++) {
        IrPart pt = part_of(sig, q);
        Pass* ps = &p.pass[q];
        const IrLeaf* lv = p.lv + q * IR_MAX_LEAVES;
        int first = p.k0 + pt.first;
        if (!pt.agg) {
            if (ps->reg[0] < 0) {
                load_val(a, RAX, x->args[first]);
                st64(a, RAX, RSP, (int32_t)ps->soff);
            }
            continue;
        }
        if (pt.addr) { // copy the aggregate at the address: rep movsb, keeping rsi and rdi
            put(a, 0x56); put(a, 0x57); // push rsi, rdi
            load_val(a, RSI, x->args[first]);
            lea(a, RDI, RSP, (int32_t)ps->soff + 16);
            mov_imm(a, RCX, ps->size);
            put(a, 0xF3); put(a, 0xA4);
            put(a, 0x5F); put(a, 0x5E); // pop rdi, rsi
            continue;
        }
        uint32_t at = ps->soff;
        if (ps->reg[0] >= 0) {
            at = img;
            img += 16;
        }
        if (ps->reg[0] >= 0) {
            pack_words(a, lv, x->args + first, pt.n, ps->words, RSP, (int32_t)at);
            continue;
        }
        for (int k = 0; k < pt.n; k++) {
            IrType t = (IrType)ins_of(a, x->args[first + k])->type;
            load_val(a, RAX, x->args[first + k]);
            st_width(a, t, RAX, RSP, (int32_t)(at + lv[k].off));
        }
    }
    if (x->op == IR_CALLI) {
        load_val(a, R11, x->args[0]);
        if (x->flags & IRF_CHECKED) {
            test_rr(a, R11);
            fail_if(a, CC_E, NFAIL);
        }
    }
    // scalar arguments to their registers in parallel, then the aggregates'
    // words, whose registers no scalar move reads any more
    Move* m = xcalloc((size_t)p.np + 1, sizeof(Move));
    int nm = 0;
    for (int q = 0; q < p.np; q++) {
        IrPart pt = part_of(sig, q);
        Pass* ps = &p.pass[q];
        if (ps->reg[0] < 0 || pt.agg) continue;
        m[nm++] = move_of(a, ps->reg[0] >= SSE ? X + ps->reg[0] - SSE : ps->reg[0], x->args[p.k0 + pt.first]);
    }
    if (p.sret) m[nm++] = move_of(a, RDI, x->args[x->nargs - 1]);
    parallel_move(a, m, nm);
    free(m);
    img = p.img;
    for (int q = 0; q < p.np; q++) {
        IrPart pt = part_of(sig, q);
        Pass* ps = &p.pass[q];
        if (ps->reg[0] < 0 || !pt.agg) continue;
        for (int w = 0; w < ps->words; w++) {
            int32_t at = (int32_t)(img + 8 * (uint32_t)w);
            if (ps->reg[w] == NOCLASS) continue;
            if (ps->reg[w] >= SSE) xld(a, ps->reg[w] - SSE, RSP, at);
            else ld64(a, ps->reg[w], RSP, at);
        }
        img += 16;
    }
    if (p.mem_ret && !p.sret) lea(a, RDI, RSP, (int32_t)p.buf);
    if (x->flags & IRF_VARARGS) mov_imm(a, RAX, (uint64_t)p.nsse); // al: the vector registers used
    if (x->op == IR_CALLI) {
        put(a, 0x41); put(a, 0xFF); put(a, 0xD3); // call r11
    } else {
        call_sym(a, x->name);
    }
    if (x->op == IR_CALL && !x->sym && strncmp(x->name, "__ace$registry_", 15) == 0 &&
        strcmp(x->name, "__ace$registry_leaks") != 0)
        a->o->allocator_used = 1;
    if (x->op == IR_CALL && !x->sym && strcmp(x->name, "__ace$arithmetic_fail") == 0) a->o->arithmetic_fail_used = 1;
    if (x->op == IR_CALL && !x->sym && strcmp(x->name, "__ace$null_fail") == 0) a->o->null_fail_used = 1;
    if (sig && sig->ret.agg && !p.sret) { // the projections read the result's leaves
        IrLeaf lv[IR_MAX_LEAVES];
        SType* bad;
        int n = ir_leaves(sig->ret.agg, lv, &bad);
        uint32_t at = p.buf;
        if (!p.mem_ret) {
            Cc c = { 0, 0, 0 };
            Pass r;
            pass_part(&c, sig->ret, (IrType)lv[0].type, lv, &r);
            for (int w = 0; w < r.words; w++) {
                int32_t to = (int32_t)(p.img + 8 * (uint32_t)w);
                if (r.reg[w] == NOCLASS) continue;
                int rg = ret_reg(r.reg[w]);
                if (rg >= SSE) xst(a, rg - SSE, RSP, to);
                else st64(a, rg, RSP, to);
            }
            at = p.img;
        }
        for (int j = i + 1; j < bb->nins && ins_of(a, bb->ins[j])->op == IR_PROJ; j++) {
            IrIns* pj = ins_of(a, bb->ins[j]);
            int k = (int)pj->imm;
            if (k >= n) break;
            ld_width(a, (IrType)pj->type, RAX, RSP, (int32_t)(at + lv[k].off));
            store_val(a, bb->ins[j], RAX);
        }
        plan_free(&p);
        return;
    }
    plan_free(&p);
    IrType t = (IrType)x->type;
    if (t == IT_VOID) return;
    if (ir_type_float(t)) {
        fstore(a, v, t, XMM0);
        return;
    }
    normalize(a, t, RAX);
    store_val(a, v, RAX);
}

// A byte routine call under its private convention (x64_runtime): the
// operands in rdi, rsi, rdx; mem_compare's result in rax.
static void rtcall(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    Move m[3] = { move_of(a, RDI, x->args[0]), move_of(a, RSI, x->args[1]), move_of(a, RDX, x->args[2]) };
    parallel_move(a, m, 3);
    call_sym(a, x->name);
    if (strcmp(x->name, "__ace$mem_fill") == 0) a->o->mem_fill_used = 1;
    else if (strcmp(x->name, "__ace$mem_compare") == 0) a->o->mem_compare_used = 1;
    else a->o->mem_copy_used = 1;
    if (x->type != IT_VOID) store_val(a, v, RAX);
}

// --- emission ---------------------------------------------------------------------------

static void line_row(X64* a, Node* at) {
    if (!at || at == a->last_at || at->line <= 0) return;
    a->last_at = at;
    obj_line(a->o, here(a), at->line, at->col, at->path ? at->path : a->o->d->path);
}

// Each phi of b's successor takes its operand from b, in parallel.
static void phi_moves(X64* a, int b) {
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
        int ph = sb->ins[i], d = loc(a, ph);
        if (d != NOLOC) m[k++] = move_of(a, d, fn->ins[ph].args[p]);
    }
    parallel_move(a, m, k);
    free(m);
}

// Save (at entry) or restore (before leave) the callee-saved registers the
// function's asm statements reserve.
static void callee_saved(X64* a, int save) {
    int32_t at = a->saved;
    for (int r = RBX; r <= R15; r++) {
        if (!(a->keep & 1u << r)) continue;
        if (save) st64(a, r, RBP, at);
        else ld64(a, r, RBP, at);
        at += 8;
    }
}

// asm "instruction text" { bindings } (Inline Assembly #2-3), bb->ins[i]:
// loads each "in" value into its register, emits the instruction (sema
// accepted only nop, syscall, and int3), then stores each "out" register,
// narrowed to its projection's type, to the projection's slot.
static void asm_select(X64* a, IrBlock* bb, int i) {
    int v = bb->ins[i];
    IrIns* x = ins_of(a, v);
    Move* m = xcalloc((size_t)(x->nargs + x->asmb->nout), sizeof(Move));
    for (int k = 0; k < x->nargs; k++) m[k] = move_of(a, x->asmb->reg[k], x->args[k]);
    parallel_move(a, m, x->nargs);
    const char* p = x->name;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] == 's') { put(a, 0x0F); put(a, 0x05); } // syscall
    else if (p[0] == 'i') put(a, 0xCC);            // int3
    else put(a, 0x90);                             // nop
    int n = 0;
    for (int j = i + 1; j < bb->nins && ins_of(a, bb->ins[j])->op == IR_PROJ; j++) {
        int u = bb->ins[j], l = loc(a, u);
        if (l == NOLOC) continue;
        int r = x->asmb->out[ins_of(a, u)->imm];
        normalize(a, type_of(a, u), r);
        m[n++] = (Move){ l, r, u, type_of(a, u) };
    }
    parallel_move(a, m, n);
    free(m);
}

// A branch condition: cc after the flags, which parity may qualify (an
// unordered float compare sets PF): with NP, cc holds only when PF is clear;
// with P, cc also holds when PF is set.
typedef struct { int cc, par; } Cond;
enum { PAR_NONE, PAR_NP, PAR_P };

static Cond invert(Cond k) { return (Cond){ k.cc ^ 1, k.par == PAR_NP ? PAR_P : k.par == PAR_P ? PAR_NP : PAR_NONE }; }

// The flags a branch or check on c tests: a fused compare's own, as compare
// and fcompare set them, or c's register tested for nonzero.
static Cond flags(X64* a, int c) {
    IrIns* x = ins_of(a, c);
    if (!a->ra.fused[c]) {
        test_rr(a, gp_in(a, c, RAX));
        return (Cond){ CC_NE, PAR_NONE };
    }
    int l = x->args[0], r = x->args[1], op = x->op;
    IrType t = type_of(a, l);
    if (!ir_type_float(t)) {
        cmp_vals(a, l, r);
        return (Cond){ cc_of(op, t), PAR_NONE };
    }
    if (op == IR_LT || op == IR_LE) {
        int s = l; l = r; r = s;
        op = op == IR_LT ? IR_GT : IR_GE;
    }
    int xl = x_in(a, l, XMM0);
    ucomis(a, t, xl, x_in(a, r, XMM1));
    if (op == IR_EQ) return (Cond){ CC_E, PAR_NP };
    if (op == IR_NE) return (Cond){ CC_NE, PAR_P };
    return (Cond){ op == IR_GT ? CC_A : CC_AE, PAR_NONE };
}

static void jump_if(X64* a, Cond k, int target) {
    if (k.par == PAR_NP) {
        uint32_t skip = jmp8(a, CC_P);
        jump_add(a, k.cc, target);
        land8(a, skip);
        return;
    }
    if (k.par == PAR_P) jump_add(a, CC_P, target);
    jump_add(a, k.cc, target);
}

static void terminator(X64* a, int b, int v) {
    IrFunc* fn = a->fn;
    IrIns* x = ins_of(a, v);
    IrBlock* bb = &fn->blocks[b];
    int next = a->ra.eix[b] + 1 < fn->nblocks ? a->ra.layout[a->ra.eix[b] + 1] : -1;
    if (x->op == IR_JMP) {
        phi_moves(a, b);
        if (bb->succ[0] != next) jump_add(a, -1, bb->succ[0]);
        return;
    }
    if (x->op == IR_RET) {
        const IrSig* sig = fn->sig;
        if (sig && sig->ret.agg) { // its leaves meet in memory: the caller's, or the words of rax and rdx
            IrLeaf lv[IR_MAX_LEAVES];
            SType* bad;
            ir_leaves(sig->ret.agg, lv, &bad);
            int base = RBP;
            int32_t at = a->scratch;
            if (a->sret) {
                ld64(a, RCX, RBP, a->sret);
                base = RCX;
                at = 0;
            }
            if (!a->sret) {
                Cc c = { 0, 0, 0 };
                Pass r;
                pass_part(&c, sig->ret, (IrType)lv[0].type, lv, &r);
                pack_words(a, lv, x->args, x->nargs, r.words, base, at);
                for (int w = 0; w < r.words; w++) {
                    if (r.reg[w] == NOCLASS) continue;
                    int rg = ret_reg(r.reg[w]);
                    if (rg >= SSE) xld(a, rg - SSE, RBP, a->scratch + 8 * w);
                    else ld64(a, rg, RBP, a->scratch + 8 * w);
                }
            } else {
                for (int k = 0; k < x->nargs; k++) {
                    load_val(a, RAX, x->args[k]);
                    st_width(a, (IrType)ins_of(a, x->args[k])->type, RAX, base, at + (int32_t)lv[k].off);
                }
            }
        } else if (x->nargs && ir_type_float((IrType)ins_of(a, x->args[0])->type)) {
            movaps(a, XMM0, x_in(a, x->args[0], XMM0));
        } else if (x->nargs) {
            load_val(a, RAX, x->args[0]);
        }
        // with registers to restore, the epilogue is emitted once and later returns jump to it
        if (a->keep && a->epi) {
            int32_t d = (int32_t)(a->epi - 1) - (int32_t)(here(a) + 2);
            if (d >= -128) { put(a, 0xEB); put(a, (uint8_t)d); } // jmp rel8
            else { put(a, 0xE9); put32(a, (uint32_t)(d - 3)); } // jmp rel32
            return;
        }
        if (a->keep) a->epi = here(a) + 1;
        if (a->sret) ld64(a, RAX, RBP, a->sret); // the result's address, as the caller passed it
        callee_saved(a, 0);
        put(a, 0xC9); // leave
        put(a, 0xC3); // ret
        return;
    }
    Cond k = flags(a, x->args[0]);
    int t = bb->succ[0], f = bb->succ[1];
    if (f == next) {
        jump_if(a, k, t);
    } else if (t == next) {
        jump_if(a, invert(k), f);
    } else {
        jump_if(a, k, t);
        jump_add(a, -1, f);
    }
}

// A debug check: a branch to its site's stub when its condition is false.
static void check(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    Cond t = invert(flags(a, x->args[0]));
    if (!x->name) {
        jump_if(a, t, fail_target(a, CFAIL));
        return;
    }
    int k = 0;
    while (k < a->nstub && (a->stub[k]->at != x->at || strcmp(a->stub[k]->name, x->name) != 0)) k++;
    if (k == a->nstub) {
        if (a->nstub == a->cstub) {
            a->cstub = a->cstub ? a->cstub * 2 : 8;
            a->stub = realloc(a->stub, (size_t)a->cstub * sizeof(IrIns*));
            if (!a->stub) oom();
        }
        a->stub[a->nstub++] = x;
    }
    jump_if(a, t, LFAIL - k);
}

static void emit_block(X64* a, int b) {
    IrFunc* fn = a->fn;
    IrBlock* bb = &fn->blocks[b];
    for (int i = 0; i < bb->nins && !a->failed; i++) {
        int v = bb->ins[i];
        IrIns* x = &fn->ins[v];
        if (x->op != IR_CHECK) line_row(a, x->at);
        if (a->ra.fused[v]) continue;
        IrType t = (IrType)x->type;
        if (ir_type_vector(t) && x->op != IR_LOAD && x->op != IR_PHI) {
            vector(a, v);
            continue;
        }
        int fp = x->nargs && ir_type_float((IrType)ins_of(a, x->args[0])->type);
        switch (x->op) {
        case IR_CONST: case IR_PARAM: case IR_PHI: case IR_SLOT: case IR_RESULT: case IR_PROJ:
            break; // a result's address arrives at entry, a projection at its call
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
            if (ir_type_float(t)) farith(a, v);
            else arith(a, v);
            break;
        case IR_DIV: case IR_REM:
            if (ir_type_float(t)) farith(a, v);
            else div_rem(a, v);
            break;
        case IR_SHL: case IR_SHR:
            shift(a, v);
            break;
        case IR_NEG: case IR_NOT:
            if (ir_type_float(t)) fneg(a, v);
            else unary(a, v);
            break;
        case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
            if (fp) fcompare(a, v);
            else compare(a, v);
            break;
        case IR_CAST:
            if (fp || ir_type_float(t)) fcast(a, v);
            else cast(a, v);
            break;
        case IR_POPCNT: case IR_CLZ: case IR_CTZ: case IR_BSWAP: case IR_ROTL: case IR_ROTR:
            bitop(a, v);
            break;
        case IR_MULH:
            mul_high(a, v);
            break;
        case IR_GLOBAL:
            addr_of(a, RAX, x->name, x->sym && x->sym->is_extern);
            store_val(a, v, RAX);
            break;
        case IR_STR: {
            ESym* s = obj_string(a->o, x->name, (uint32_t)x->imm);
            addr_of(a, RAX, s->name, 0);
            store_val(a, v, RAX);
            break;
        }
        case IR_LOAD: case IR_STORE:
            access(a, v);
            break;
        case IR_PADD: case IR_PSUB:
            pointer_offset(a, v);
            break;
        case IR_CALL: case IR_CALLI:
            call(a, bb, i);
            break;
        case IR_CHECK:
            check(a, v);
            break;
        case IR_RTCALL:
            rtcall(a, v);
            break;
        case IR_ASM:
            asm_select(a, bb, i);
            break;
        case IR_BITCAST:
            bitcast(a, v);
            break;
        case IR_SQRT: case IR_FLOOR: case IR_CEIL: case IR_FMA:
            fexact(a, v);
            break;
        default:
            terminator(a, b, v);
            break;
        }
    }
}

// --- allocation and frame ---------------------------------------------------------------
//
// Allocation (regalloc.c) hands out rbx and r12-r15 (callee-saved), r10, r9,
// r8, rsi, and rdi, and xmm3-xmm15, none of which a callee preserves; rax,
// rcx, rdx, and r11, and xmm0-xmm2, stay the selection's scratch. A byte
// routine disturbs rsi, rdi, and xmm10-xmm15.

static const int8_t GP_CALLER[] = { R10, R9, R8, RSI, RDI };
static const int8_t GP_CALLEE[] = { RBX, R12, R13, R14, R15 };
static const int8_t XMM_CALLER[] = { X + 8, X + 9, X + 10, X + 11, X + 12, X + 13, X + 14, X + 15,
                                     X + 7, X + 6, X + 5, X + 4, X + 3 };

// A cast whose operand's canonical form already is its own: to 64 bits, or
// wider with an extension the operand's signedness gives, or from a bool.
static int cast_in_place(IrType to, IrType from) {
    if (ir_type_float(to) || ir_type_float(from) || ir_type_vector(to) || ir_type_vector(from) || to == IT_BOOL)
        return 0;
    int bt = ir_type_bits(to), bf = ir_type_bits(from);
    if (bt >= 64 || from == IT_BOOL) return 1;
    if (bt > bf) return !ir_type_signed(from) || ir_type_signed(to);
    return bt == bf && ir_type_signed(to) == ir_type_signed(from);
}

static const RaTarget X64T = {
    X,
    { GP_CALLER, XMM_CALLER }, { (int)sizeof GP_CALLER, (int)sizeof XMM_CALLER },
    { GP_CALLEE, NULL }, { (int)sizeof GP_CALLEE, 0 },
    CALLEE_SAVED,
    1u << RSI | 1u << RDI | UINT64_C(0x3F) << (X + 10),
    0,
    cast_in_place,
};

// r, when allocation hands it out, as v's preference
static void hint(X64* a, int v, int r) {
    int ok = r >= X + 3 && r < X + 16;
    for (size_t k = 0; k < sizeof GP_CALLER && !ok; k++) ok = GP_CALLER[k] == r;
    if (ok && a->ra.hint[v] < 0) a->ra.hint[v] = r;
}

// Registers a value would like: where a parameter or a result address
// arrives, and where a call's argument goes.
static void hints(X64* a) {
    IrFunc* fn = a->fn;
    Cc c = { 0, 0, 0 };
    if (fn->ret_mem || ret_in_memory(fn->sig)) c.ni = 1;
    int np = fn->sig ? fn->sig->nparts : fn->nparams;
    Pass* ps = xcalloc((size_t)(np ? np : 1), sizeof(Pass));
    IrLeaf lv[IR_MAX_LEAVES];
    for (int q = 0; q < np; q++) pass_part(&c, part_of(fn->sig, q), fn->params[part_of(fn->sig, q).first], lv, &ps[q]);
    for (int i = 0; i < fn->nblocks; i++) {
        IrBlock* bb = &fn->blocks[a->ra.order[i]];
        for (int j = 0; j < bb->nins; j++) {
            int u = bb->ins[j];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_PARAM) {
                for (int q = 0; q < np; q++) {
                    IrPart pt = part_of(fn->sig, q);
                    if (!pt.agg && !pt.addr && pt.first == (int)x->imm && ps[q].reg[0] >= 0)
                        hint(a, u, ps[q].reg[0] >= SSE ? X + ps[q].reg[0] - SSE : ps[q].reg[0]);
                }
            } else if (x->op == IR_RESULT) {
                hint(a, u, RDI);
            } else if (x->op == IR_CALL || x->op == IR_CALLI) {
                Plan p;
                plan_call(a, x, &p);
                for (int q = 0; q < p.np; q++) {
                    IrPart pt = part_of(x->sig, q);
                    int r = p.pass[q].reg[0];
                    if (!pt.agg && r >= 0) hint(a, x->args[p.k0 + pt.first], r >= SSE ? X + r - SSE : r);
                }
                if (p.sret) hint(a, x->args[x->nargs - 1], RDI);
                plan_free(&p);
            }
        }
    }
    free(ps);
}

// Whether load or store u computes its address itself (access): from a
// global in this object, rip-relative, or from an unchecked pointer offset
// (a constant folded into the displacement, or a register index scaled by
// 1, 2, 4, or 8).
static int fold_address(X64* a, int u) {
    IrIns* x = ins_of(a, u);
    int p = x->args[0];
    IrIns* px = ins_of(a, p);
    if (a->ra.nuse[p] != 1 || px->block != x->block || (x->flags & IRF_CHECKED)) return 0;
    if (px->op == IR_GLOBAL) return !(px->sym && px->sym->is_extern);
    if ((px->op != IR_PADD && px->op != IR_PSUB) || (px->flags & IRF_CHECKED)) return 0;
    int32_t k;
    int64_t d;
    if (imm32_of(a, px->args[1], &k))
        return !__builtin_mul_overflow((int64_t)k, (int64_t)px->imm, &d) && d == (int32_t)d && d != INT32_MIN &&
               (int64_t)x->aux + d == (int32_t)((int64_t)x->aux + d) && (int64_t)x->aux - d == (int32_t)((int64_t)x->aux - d);
    return px->op == IR_PADD && (px->imm == 1 || px->imm == 2 || px->imm == 4 || px->imm == 8);
}

// The values their only user emits (ra.fused): a compare its branch or
// check in its own block tests, and an address its load or store folds.
static void fuse(X64* a) {
    IrFunc* fn = a->fn;
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            int u = fn->blocks[b].ins[i];
            IrIns* x = &fn->ins[u];
            if (x->op == IR_BR || x->op == IR_CHECK) {
                int c = x->args[0];
                IrIns* cx = &fn->ins[c];
                if (cx->op >= IR_EQ && cx->op <= IR_GE && a->ra.nuse[c] == 1 && cx->block == b) a->ra.fused[c] = 1;
            } else if ((x->op == IR_LOAD || x->op == IR_STORE) && fold_address(a, u)) {
                a->ra.fused[x->args[0]] = 1;
            }
        }
}

// Allocate, then lay out the frame below rbp: the callee-saved registers the
// function uses, the spill slots, 16 scratch bytes, 16 bytes per aggregate
// parameter arriving in registers, the saved result address, and the stack
// objects; then the outgoing area at [rsp]. Returns the frame size.
static uint32_t layout(X64* a) {
    IrFunc* fn = a->fn;
    a->ra.fn = fn;
    ra_split_critical_edges(fn);
    ra_layout(&a->ra);
    ra_count_uses(&a->ra);
    fuse(a);
    ra_liveness(&a->ra);
    ra_positions(&a->ra);
    hints(a);
    ra_coalesce(&a->ra, &X64T);
    ra_allocate(&a->ra, &X64T);
    a->keep = a->ra.asm_all & CALLEE_SAVED;
    for (int v = 0; v < fn->nins; v++)
        if (a->ra.reg[v] >= 0 && a->ra.reg[v] < X) a->keep |= (1u << a->ra.reg[v]) & CALLEE_SAVED;
    a->home = xcalloc((size_t)fn->nins, sizeof(int32_t));
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) {
            IrIns* x = &fn->ins[fn->blocks[b].ins[i]];
            if (x->op != IR_CALL && x->op != IR_CALLI) continue;
            Plan p;
            plan_call(a, x, &p);
            if (p.bytes > a->out) a->out = p.bytes;
            plan_free(&p);
        }
    uint32_t down = 8 * (uint32_t)__builtin_popcount(a->keep);
    a->saved = -(int32_t)down;
    a->spill = -(int32_t)down - 8;
    down = align_up(down + 8 * (uint32_t)a->ra.nslots, 16) + 16;
    a->scratch = -(int32_t)down;
    int np = fn->sig ? fn->sig->nparts : fn->nparams;
    down += 16 * (uint32_t)np;
    a->aggs = -(int32_t)down;
    if (fn->ret_mem || ret_in_memory(fn->sig)) {
        down += 8;
        a->sret = -(int32_t)down;
    }
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        for (int i = 0; i < bb->nins; i++) {
            int v = bb->ins[i];
            IrIns* x = &fn->ins[v];
            if (x->op != IR_SLOT) continue;
            uint32_t al = x->aux > 0 ? (uint32_t)x->aux : 1;
            if (al > 16) refuse(a, "a stack object aligned beyond 16 bytes");
            down = align_up(down + (uint32_t)x->imm, al < 16 ? al : 16);
            a->home[v] = -(int32_t)down;
        }
    }
    a->out = align_up(a->out, 16);
    return align_up(down + a->out, 16);
}

// Entry: each parameter's leaves to their locations in canonical form (a C
// caller may leave a narrow argument's upper bits unspecified). Scalars in
// registers are normalized in place and moved in parallel with a result
// address arriving in rdi; then, with no arrival register still to read,
// the stacked scalars (at rbp + 16), each aggregate's leaves from its words
// (stored to its own 16 bytes at entry when in registers), and the address
// of a caller's stack copy for a parameter passed at its address.
static void params(X64* a) {
    IrFunc* fn = a->fn;
    IrBlock* bb = &fn->blocks[0];
    int* pv = xcalloc((size_t)(fn->nparams ? fn->nparams : 1), sizeof(int));
    int np = fn->sig ? fn->sig->nparts : fn->nparams;
    Move* m = xcalloc((size_t)np + 1, sizeof(Move));
    int nm = 0;
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = ins_of(a, v);
        if (x->op == IR_PARAM && loc(a, v) != NOLOC) pv[x->imm] = v + 1;
        if (x->op == IR_RESULT && loc(a, v) != NOLOC) m[nm++] = (Move){ loc(a, v), RDI, v, IT_PTR };
    }
    Cc c = { 0, 0, 0 };
    if (a->sret) {
        st64(a, RDI, RBP, a->sret);
        c.ni = 1;
    }
    Pass* ps = xcalloc((size_t)(np ? np : 1), sizeof(Pass));
    IrLeaf* lv = xcalloc((size_t)(np ? np : 1) * IR_MAX_LEAVES, sizeof(IrLeaf));
    for (int q = 0; q < np; q++) {
        IrPart pt = part_of(fn->sig, q);
        IrLeaf* ql = lv + q * IR_MAX_LEAVES;
        pass_part(&c, pt, fn->params[pt.first], ql, &ps[q]);
        if (pt.agg && !pt.addr && ps[q].reg[0] >= 0) {
            for (int w = 0; w < ps[q].words; w++) {
                int32_t at = a->aggs + 16 * q + 8 * w;
                if (ps[q].reg[w] == NOCLASS) continue;
                if (ps[q].reg[w] >= SSE) xst(a, ps[q].reg[w] - SSE, RBP, at);
                else st64(a, ps[q].reg[w], RBP, at);
            }
        }
        if (pt.agg || pt.addr || ps[q].reg[0] < 0 || !pv[pt.first]) continue;
        IrType t = fn->params[pt.first];
        int r = ps[q].reg[0];
        if (r >= SSE) r = X + r - SSE;
        else normalize(a, t, r);
        m[nm++] = (Move){ loc(a, pv[pt.first] - 1), r, pv[pt.first] - 1, t };
    }
    parallel_move(a, m, nm);
    for (int q = 0; q < np && !a->failed; q++) {
        IrPart pt = part_of(fn->sig, q);
        int32_t stacked = 16 + (int32_t)ps[q].soff;
        if (pt.addr) {
            if (!pv[pt.first]) continue;
            lea(a, RAX, RBP, stacked);
            store_val(a, pv[pt.first] - 1, RAX);
        } else if (!pt.agg) {
            if (!pv[pt.first] || ps[q].reg[0] >= 0) continue;
            ld64(a, RAX, RBP, stacked);
            normalize(a, fn->params[pt.first], RAX);
            store_val(a, pv[pt.first] - 1, RAX);
        } else {
            int32_t at = ps[q].reg[0] >= 0 ? a->aggs + 16 * q : stacked;
            for (int k = 0; k < pt.n; k++) {
                int v = pv[pt.first + k];
                if (!v) continue;
                ld_width(a, fn->params[pt.first + k], RAX, RBP, at + (int32_t)lv[q * IR_MAX_LEAVES + k].off);
                store_val(a, v - 1, RAX);
            }
        }
    }
    free(pv); free(m); free(ps); free(lv);
}

static void rip_op(X64* a, uint8_t op, int r, const char* name);

static void emit_body(X64* a) {
    IrFunc* fn = a->fn;
    line_row(a, fn->decl);
    put(a, 0x55);                        // push rbp
    mov_rr(a, RBP, RSP);                 // mov rbp, rsp
    rr(a, 1, (const uint8_t[]){ 0x81 }, 1, 5, RSP); // sub rsp, imm32
    a->frame_at = here(a);
    put32(a, 0);
    callee_saved(a, 1);
    if (a->main) { // glibc's argc and argv for sys.argc/arg (lower.c's process_word)
        rip_op(a, 0x89, RDI, "__ace$argc");
        rip_op(a, 0x89, RSI, "__ace$argv");
    }
    params(a);
    for (int i = 0; i < fn->nblocks && !a->failed; i++) { // in the allocator's emission order
        int b = a->ra.layout[i];
        a->label[b] = here(a);
        emit_block(a, b);
    }
    // located check stubs: write "path:line:col: what" to fd 2 and abort
    uint32_t* stub_at = xcalloc((size_t)a->nstub, sizeof(uint32_t));
    for (int k = 0; k < a->nstub; k++) {
        uint32_t n;
        ESym* msg = obj_located(a->o, a->stub[k]->at, a->stub[k]->name, &n);
        stub_at[k] = here(a);
        line_row(a, a->stub[k]->at);
        mov_imm(a, RDI, 2);
        addr_of(a, RSI, msg->name, 0);
        mov_imm(a, RDX, n);
        put(a, 0xE9);
        reloc32(a, "__ace$bounds_fail", R_PC32);
        a->o->bounds_fail_used = 1;
    }
    for (int i = 0; i < a->njmp; i++) {
        Jump* j = &a->jmp[i];
        if (j->target <= LFAIL) {
            buf_patch32(a->t, j->site, stub_at[LFAIL - j->target] - (j->site + 4));
        } else if (j->target < 0) { // straight to the shared routine
            static const char* routine[] = { "__ace$arithmetic_fail", "__ace$null_fail", "__ace$contract_fail" };
            const char* name = routine[-1 - j->target];
            reloc_add(&a->o->secs[SEC_TEXT], j->site, sym_intern(&a->o->syms, name, (uint32_t)strlen(name)), 1, 2, 1, R_PC32);
            buf_patch32(a->t, j->site, (uint32_t)-4);
        } else {
            buf_patch32(a->t, j->site, a->label[j->target] - (j->site + 4));
        }
    }
    free(stub_at);
}

void x64_func(Obj* o, IrFunc* fn, const char* name, uint32_t len, int global) {
    X64 a;
    memset(&a, 0, sizeof a);
    a.o = o;
    a.fn = fn;
    a.t = obj_text(o);
    uint32_t frame = layout(&a);
    a.label = xcalloc((size_t)fn->nblocks, sizeof(uint32_t));
    a.main = len == 5 && memcmp(name, "_main", 5) == 0;
    while (a.t->len % FUNC_ALIGN) put(&a, 0x90);
    uint32_t start = here(&a);
    sym_define(&o->syms, o->secs, name, len, global, &o->secs[SEC_TEXT]);
    emit_body(&a);
    buf_patch32(a.t, a.frame_at, frame);
    obj_debug_fn(o, start, here(&a) - start, fn->decl->str, fn->decl->len);
    ra_free(&a.ra);
    free(a.home); free(a.label); free(a.jmp); free(a.stub);
}

// --- runtime routines -------------------------------------------------------------------

// A failure routine: write its message (or, for bounds_fail, the caller's
// rdi, rsi, rdx) and abort. Reached by a jump or call from any stack depth,
// so it realigns rsp before calling C.
static void fail_routine(Obj* o, const char* name, const char* message) {
    Buf* t = obj_text(o);
    while (t->len % FUNC_ALIGN) buf_put8(t, 0x90);
    X64 a;
    memset(&a, 0, sizeof a);
    a.o = o;
    a.t = t;
    ESym* msg = NULL;
    if (message) {
        char nm[64];
        snprintf(nm, sizeof nm, "%s$message", name);
        Sec* c = &o->secs[SEC_CONST];
        msg = sym_define(&o->syms, o->secs, nm, (uint32_t)strlen(nm), 0, c);
        buf_write(&c->data, message, strlen(message));
    }
    sym_define(&o->syms, o->secs, name, (uint32_t)strlen(name), 0, &o->secs[SEC_TEXT]);
    rr(&a, 1, (const uint8_t[]){ 0x83 }, 1, 4, RSP); put(&a, 0xF0); // and rsp, -16
    if (message) {
        mov_imm(&a, RDI, 2);
        addr_of(&a, RSI, msg->name, 0);
        mov_imm(&a, RDX, strlen(message));
    }
    call_sym(&a, "_write");
    call_sym(&a, "_abort");
    put(&a, 0x0F); put(&a, 0x0B); // ud2 if abort unexpectedly returns
}

// The words an Ace main stores argc and argv in; zero when a C main is the entry.
static void process_words(Obj* o) {
    Sec* b = &o->secs[SEC_BSS];
    b->size = (b->size + 7) & ~(uint64_t)7;
    sym_define(&o->syms, o->secs, "__ace$argc", 10, 0, b);
    b->size += 8;
    sym_define(&o->syms, o->secs, "__ace$argv", 10, 0, b);
    b->size += 8;
}

// --- routine encoding: [base + index + disp] operands, SSE, local labels ---

enum { X10 = 10, X11, X12, X13, X14, X15 }; // the byte routines' xmm scratch

// op reg, [base + index + disp] after an optional mandatory prefix pfx;
// index < 0 for none; force emits an empty REX (sil, dil as bytes).
static void mop(X64* a, int pfx, int w, const uint8_t* op, int nop, int reg, int base, int index, int32_t disp, int force) {
    if (pfx) put(a, (uint8_t)pfx);
    uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) & 1) << 2 |
                          (index >= 0 ? ((index >> 3) & 1) << 1 : 0) | ((base >> 3) & 1));
    if (r != 0x40 || force) put(a, r);
    for (int i = 0; i < nop; i++) put(a, op[i]);
    int mod = disp == 0 && (base & 7) != RBP ? 0 : disp >= -128 && disp <= 127 ? 1 : 2;
    int sib = index >= 0 || (base & 7) == RSP;
    put(a, (uint8_t)(mod << 6 | (reg & 7) << 3 | (sib ? 4 : base & 7)));
    if (sib) put(a, (uint8_t)((index >= 0 ? index & 7 : 4) << 3 | (base & 7)));
    if (mod == 1) put(a, (uint8_t)disp);
    else if (mod == 2) put32(a, (uint32_t)disp);
}

static void ldq(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 1, (const uint8_t[]){ 0x8B }, 1, r, b, i, d, 0); }
static void stq(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 1, (const uint8_t[]){ 0x89 }, 1, r, b, i, d, 0); }
static void ldd(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 0, (const uint8_t[]){ 0x8B }, 1, r, b, i, d, 0); }
static void std_(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 0, (const uint8_t[]){ 0x89 }, 1, r, b, i, d, 0); }
static void ldb(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 0, (const uint8_t[]){ 0x0F, 0xB6 }, 2, r, b, i, d, 0); } // movzx r32, m8
static void stb(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 0, (const uint8_t[]){ 0x88 }, 1, r, b, i, d, r >= RSP && r < R8); }
static void leaq(X64* a, int r, int b, int i, int32_t d) { mop(a, 0, 1, (const uint8_t[]){ 0x8D }, 1, r, b, i, d, 0); }
static void ldx(X64* a, int x, int b, int i, int32_t d) { mop(a, 0xF3, 0, (const uint8_t[]){ 0x0F, 0x6F }, 2, x, b, i, d, 0); } // movdqu
static void stx(X64* a, int x, int b, int i, int32_t d) { mop(a, 0xF3, 0, (const uint8_t[]){ 0x0F, 0x7F }, 2, x, b, i, d, 0); } // movdqu
static void stxa(X64* a, int x, int b, int32_t d) { mop(a, 0x66, 0, (const uint8_t[]){ 0x0F, 0x7F }, 2, x, b, -1, d, 0); }   // movdqa

// op reg, rm with a 66 prefix: SSE2 integer forms
static void sse(X64* a, int w, uint8_t op, int reg, int r) {
    put(a, 0x66);
    rr(a, w, (const uint8_t[]){ 0x0F, op }, 2, reg, r);
}

// op r, imm8 of group 1 (ext: 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp)
static void grp(X64* a, int w, int ext, int r, int8_t imm) {
    rr(a, w, (const uint8_t[]){ 0x83 }, 1, ext, r);
    put(a, (uint8_t)imm);
}

static void ret_(X64* a) { put(a, 0xC3); }
static void bind(X64* a, int k) { a->label[k] = here(a); }

static void jmp_sym(X64* a, const char* name) {
    put(a, 0xE9);
    reloc32(a, name, R_PLT32);
}

// op r, [rip + name] (a local data symbol)
static void rip_op(X64* a, uint8_t op, int r, const char* name) {
    rex(a, 1, r, 0, 0);
    put(a, op);
    put(a, (uint8_t)(0x05 | (r & 7) << 3));
    reloc32(a, name, R_PC32);
}

// Pad with long nops to a 16-byte boundary: a loop head that no fetch block splits.
static void align_loop(X64* a) {
    static const uint8_t nop[9][9] = {
        { 0x90 }, { 0x66, 0x90 }, { 0x0F, 0x1F, 0x00 }, { 0x0F, 0x1F, 0x40, 0x00 },
        { 0x0F, 0x1F, 0x44, 0x00, 0x00 }, { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 }, { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };
    uint32_t n = (LOOP_ALIGN - here(a) % LOOP_ALIGN) % LOOP_ALIGN;
    while (n) {
        uint32_t k = n > 9 ? 9 : n;
        for (uint32_t i = 0; i < k; i++) put(a, nop[k - 1][i]);
        n -= k;
    }
}

// Begin routine name at a FUNC_ALIGN boundary, with fresh local labels.
static void routine(X64* a, Obj* o, const char* name) {
    Buf* t = obj_text(o);
    while (t->len % FUNC_ALIGN) buf_put8(t, 0x90);
    sym_define(&o->syms, o->secs, name, (uint32_t)strlen(name), 0, &o->secs[SEC_TEXT]);
    a->njmp = 0;
}

static void patch_local(X64* a) {
    for (int i = 0; i < a->njmp; i++)
        buf_patch32(a->t, a->jmp[i].site, a->label[a->jmp[i].target] - (a->jmp[i].site + 4));
    a->njmp = 0;
}

/*
 * Shared byte routines, emitted once per object when used. A private
 * convention keeps call sites from disturbing other values: rdi = destination
 * (or a), rsi = source, fill byte (zero-extended), or b, rdx = byte count;
 * mem_compare returns -1, 0, or 1 in rax. They clobber only rax, rcx, rdx,
 * rsi, rdi, r11, xmm10-xmm15, and flags, and use SSE2 alone (every x86-64).
 * As on arm64, each size class up to 64 bytes loads every byte before
 * storing, so it also serves overlapping moves, and tails reuse overlapping
 * chunks instead of byte loops. Larger runs move 32 bytes per iteration with
 * the far-end chunk loaded first; forward runs align the destination to 32.
 * From REP_BYTES, forward copies and fills use rep movsb / rep stosb.
 */
enum { L_COPY, L_BACKWARD, L_FORWARD, L_REP, L_LT16, L_LT8, L_LT4, L_DONE, L_GT32, L_BIG, L_LOOP, L_TAIL, L_BLOOP,
       L_WORDS, L_LAST, L_SAME, L_DIFF, L_FOUND, L_FAIL, L_NEXT, L_FOUND2, NLABELS };

// The byte count from which copies and fills take rep movsb / rep stosb:
// measured on an ERMS/FSRM core, the SSE2 loops win below it.
enum { REP_BYTES = 1024 };

static void rep_at(X64* a, int target) {
    rr(a, 1, (const uint8_t[]){ 0x81 }, 1, 7, RDX); // cmp rdx, imm32
    put32(a, REP_BYTES);
    jump_add(a, CC_AE, target);
}

static void emit_mem_copy(X64* a, Obj* o) {
    routine(a, o, "__ace$mem_move"); // at most 64 bytes is overlap-safe; above that,
    grp(a, 1, 7, RDX, 64);           // backward only when the destination starts inside the source
    jump_add(a, CC_BE, L_COPY);
    mov_rr(a, RAX, RDI);
    rr(a, 1, (const uint8_t[]){ 0x29 }, 1, RSI, RAX); // rax = dst - src
    cmp_rr(a, RAX, RDX);
    jump_add(a, CC_B, L_BACKWARD);
    rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 3, RAX);   // neg rax: src - dst
    cmp_rr(a, RAX, RDX);
    jump_add(a, CC_B, L_FORWARD);           // a leftward overlap never takes rep movsb
    sym_define(&o->syms, o->secs, "__ace$mem_copy", 14, 0, &o->secs[SEC_TEXT]);
    bind(a, L_COPY);
    grp(a, 1, 7, RDX, 32);
    jump_add(a, CC_A, L_GT32);
    grp(a, 1, 7, RDX, 16);
    jump_add(a, CC_B, L_LT16);
    ldx(a, X12, RSI, -1, 0);                // 16..32
    ldx(a, X13, RSI, RDX, -16);
    stx(a, X12, RDI, -1, 0);
    stx(a, X13, RDI, RDX, -16);
    ret_(a);
    bind(a, L_LT16);
    grp(a, 1, 7, RDX, 8);
    jump_add(a, CC_B, L_LT8);
    ldq(a, RAX, RSI, -1, 0);                // 8..15
    ldq(a, RCX, RSI, RDX, -8);
    stq(a, RAX, RDI, -1, 0);
    stq(a, RCX, RDI, RDX, -8);
    ret_(a);
    bind(a, L_LT8);
    grp(a, 1, 7, RDX, 4);
    jump_add(a, CC_B, L_LT4);
    ldd(a, RAX, RSI, -1, 0);                // 4..7
    ldd(a, RCX, RSI, RDX, -4);
    std_(a, RAX, RDI, -1, 0);
    std_(a, RCX, RDI, RDX, -4);
    ret_(a);
    bind(a, L_LT4);                         // 0..3: first, middle, last
    test_rr(a, RDX);
    jump_add(a, CC_E, L_DONE);
    mov_rr(a, R11, RDX);
    rr(a, 1, (const uint8_t[]){ 0xD1 }, 1, 5, R11); // shr r11, 1
    ldb(a, RAX, RSI, -1, 0);
    ldb(a, RCX, RSI, R11, 0);
    ldb(a, RSI, RSI, RDX, -1);
    stb(a, RAX, RDI, -1, 0);
    stb(a, RCX, RDI, R11, 0);
    stb(a, RSI, RDI, RDX, -1);
    bind(a, L_DONE);
    ret_(a);
    bind(a, L_GT32);
    grp(a, 1, 7, RDX, 64);
    jump_add(a, CC_A, L_BIG);
    ldx(a, X12, RSI, -1, 0);                // 33..64
    ldx(a, X13, RSI, -1, 16);
    ldx(a, X14, RSI, RDX, -32);
    ldx(a, X15, RSI, RDX, -16);
    stx(a, X12, RDI, -1, 0);
    stx(a, X13, RDI, -1, 16);
    stx(a, X14, RDI, RDX, -32);
    stx(a, X15, RDI, RDX, -16);
    ret_(a);
    // From REP_BYTES, rep movsb (fast strings) outruns 16-byte SSE2 stores.
    // Below it, forward: preload the tail and head, then continue from a
    // 32-byte-aligned destination so no loop store splits a cache line. The
    // first aligned chunk loads before the head store, which may overwrite
    // it in a leftward overlapping move.
    bind(a, L_BIG);
    rep_at(a, L_REP);
    bind(a, L_FORWARD);
    ldx(a, X14, RSI, RDX, -32);
    ldx(a, X15, RSI, RDX, -16);
    ldx(a, X12, RSI, -1, 0);
    ldx(a, X13, RSI, -1, 16);
    leaq(a, R11, RDI, RDX, -32);            // the tail's destination
    leaq(a, RAX, RDI, -1, 32);
    grp(a, 1, 4, RAX, -32);
    rr(a, 1, (const uint8_t[]){ 0x29 }, 1, RDI, RAX); // rax = step, 1..32
    rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RAX, RSI);
    ldx(a, X10, RSI, -1, 0);
    ldx(a, X11, RSI, -1, 16);
    stx(a, X12, RDI, -1, 0);
    stx(a, X13, RDI, -1, 16);
    rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RAX, RDI);
    stxa(a, X10, RDI, 0);
    stxa(a, X11, RDI, 16);
    rr(a, 1, (const uint8_t[]){ 0x29 }, 1, RAX, RDX);
    grp(a, 1, 5, RDX, 64);                  // bytes past the first aligned chunk, less 32
    jump_add(a, CC_BE, L_TAIL);
    align_loop(a);
    bind(a, L_LOOP);
    grp(a, 1, 0, RSI, 32);
    grp(a, 1, 0, RDI, 32);
    ldx(a, X10, RSI, -1, 0);
    ldx(a, X11, RSI, -1, 16);
    stxa(a, X10, RDI, 0);
    stxa(a, X11, RDI, 16);
    grp(a, 1, 5, RDX, 32);
    jump_add(a, CC_A, L_LOOP);
    bind(a, L_TAIL);
    stx(a, X14, R11, -1, 0);
    stx(a, X15, R11, -1, 16);
    ret_(a);
    bind(a, L_BACKWARD);                    // backward, head preloaded
    ldx(a, X14, RSI, -1, 0);
    ldx(a, X15, RSI, -1, 16);
    mov_rr(a, R11, RDI);
    rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RDX, RSI);
    rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RDX, RDI);
    grp(a, 1, 5, RDX, 32);
    align_loop(a);
    bind(a, L_BLOOP);
    grp(a, 1, 5, RSI, 32);
    grp(a, 1, 5, RDI, 32);
    ldx(a, X10, RSI, -1, 0);
    ldx(a, X11, RSI, -1, 16);
    stx(a, X10, RDI, -1, 0);
    stx(a, X11, RDI, -1, 16);
    grp(a, 1, 5, RDX, 32);
    jump_add(a, CC_A, L_BLOOP);
    stx(a, X14, R11, -1, 0);
    stx(a, X15, R11, -1, 16);
    ret_(a);
    bind(a, L_REP);
    mov_rr(a, RCX, RDX);
    put(a, 0xF3); put(a, 0xA4);             // rep movsb
    ret_(a);
    patch_local(a);
}

static void emit_mem_fill(X64* a, Obj* o) {
    routine(a, o, "__ace$mem_fill");
    mov_imm(a, RAX, 0x0101010101010101ull);
    rr(a, 1, (const uint8_t[]){ 0x0F, 0xAF }, 2, RAX, RSI); // imul rax, rsi: the byte in every lane
    sse(a, 1, 0x6E, X12, RAX);              // movq xmm12, rax
    sse(a, 0, 0x6C, X12, X12);              // punpcklqdq xmm12, xmm12
    grp(a, 1, 7, RDX, 32);
    jump_add(a, CC_A, L_GT32);
    grp(a, 1, 7, RDX, 16);
    jump_add(a, CC_B, L_LT16);
    stx(a, X12, RDI, -1, 0);                // 16..32
    stx(a, X12, RDI, RDX, -16);
    ret_(a);
    bind(a, L_LT16);
    grp(a, 1, 7, RDX, 8);
    jump_add(a, CC_B, L_LT8);
    stq(a, RAX, RDI, -1, 0);                // 8..15
    stq(a, RAX, RDI, RDX, -8);
    ret_(a);
    bind(a, L_LT8);
    grp(a, 1, 7, RDX, 4);
    jump_add(a, CC_B, L_LT4);
    std_(a, RAX, RDI, -1, 0);               // 4..7
    std_(a, RAX, RDI, RDX, -4);
    ret_(a);
    bind(a, L_LT4);                         // 0..3: first, middle, last
    test_rr(a, RDX);
    jump_add(a, CC_E, L_DONE);
    stb(a, RAX, RDI, -1, 0);
    stb(a, RAX, RDI, RDX, -1);
    rr(a, 1, (const uint8_t[]){ 0xD1 }, 1, 5, RDX); // shr rdx, 1
    stb(a, RAX, RDI, RDX, 0);
    bind(a, L_DONE);
    ret_(a);
    bind(a, L_GT32);                        // 33+: tail, then 32-byte runs
    rep_at(a, L_REP);
    stx(a, X12, RDI, RDX, -32);
    stx(a, X12, RDI, RDX, -16);
    grp(a, 1, 5, RDX, 32);
    align_loop(a);
    bind(a, L_LOOP);
    stx(a, X12, RDI, -1, 0);
    stx(a, X12, RDI, -1, 16);
    grp(a, 1, 0, RDI, 32);
    grp(a, 1, 5, RDX, 32);
    jump_add(a, CC_A, L_LOOP);
    ret_(a);
    bind(a, L_REP);                         // al: the byte
    mov_rr(a, RCX, RDX);
    put(a, 0xF3); put(a, 0xAA);             // rep stosb
    ret_(a);
    patch_local(a);
}

// eax = the mask of the 16 bytes of [rdi + rcx] and [rsi + rcx] that differ,
// branching to found when any does.
static void compare16(X64* a, int32_t d, int found) {
    ldx(a, X12, RDI, RCX, d);
    ldx(a, X13, RSI, RCX, d);
    sse(a, 0, 0x74, X12, X13);              // pcmpeqb
    sse(a, 0, 0xD7, RAX, X12);              // pmovmskb eax
    put(a, 0x35); put32(a, 0xFFFF);         // xor eax, 0xffff
    jump_add(a, CC_NE, found);
}

/*
 * compare: rdi = a, rsi = b, rdx = common length; returns -1, 0, or 1 in rax
 * by the first differing unsigned byte. pcmpeqb and pmovmskb give a mask of
 * equal bytes, whose first clear bit (bsf of its complement) indexes the
 * first difference directly. Lengths below 16 compare byte-reversed words,
 * since reversal puts the first byte in the high bits. Overlapping tail
 * chunks rescan only bytes already known equal.
 */
static void emit_mem_compare(X64* a, Obj* o) {
    routine(a, o, "__ace$mem_compare");
    grp(a, 1, 7, RDX, 16);
    jump_add(a, CC_B, L_LT16);
    grp(a, 1, 7, RDX, 32);
    jump_add(a, CC_A, L_GT32);
    mov_imm(a, RCX, 0);                     // 16..32: head, then tail
    compare16(a, 0, L_FOUND);
    leaq(a, RCX, RDX, -1, -16);
    compare16(a, 0, L_FOUND);
    bind(a, L_SAME);
    mov_imm(a, RAX, 0);
    ret_(a);
    bind(a, L_GT32);                        // 33+: 32-byte blocks, then the final one
    leaq(a, R11, RDX, -1, -32);
    mov_imm(a, RCX, 0);
    align_loop(a);
    bind(a, L_LOOP);
    ldx(a, X12, RDI, RCX, 0);
    ldx(a, X13, RDI, RCX, 16);
    ldx(a, X14, RSI, RCX, 0);
    ldx(a, X15, RSI, RCX, 16);
    sse(a, 0, 0x74, X12, X14);
    sse(a, 0, 0x74, X13, X15);
    sse(a, 0, 0xDB, X12, X13);              // pand
    sse(a, 0, 0xD7, RAX, X12);
    put(a, 0x3D); put32(a, 0xFFFF);         // cmp eax, 0xffff
    jump_add(a, CC_NE, L_FOUND2);
    grp(a, 1, 0, RCX, 32);
    cmp_rr(a, RCX, R11);
    jump_add(a, CC_B, L_LOOP);
    mov_rr(a, RCX, R11);
    compare16(a, 0, L_FOUND);
    compare16(a, 16, L_FOUND2);
    jump_add(a, -1, L_SAME);
    bind(a, L_FOUND2);                      // in [rcx, rcx + 32): which half
    compare16(a, 0, L_FOUND);
    grp(a, 1, 0, RCX, 16);
    compare16(a, 0, L_FOUND);
    bind(a, L_FOUND);                       // eax: the differing bytes of [rcx, rcx + 16)
    rr(a, 0, (const uint8_t[]){ 0x0F, 0xBC }, 2, RAX, RAX); // bsf eax, eax
    rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RAX, RCX);
    ldb(a, RAX, RDI, RCX, 0);
    ldb(a, RCX, RSI, RCX, 0);
    rr(a, 0, (const uint8_t[]){ 0x39 }, 1, RCX, RAX); // cmp eax, ecx
    jump_add(a, -1, L_DIFF);
    bind(a, L_LT16);                        // 8..15: two overlapping words
    grp(a, 1, 7, RDX, 8);
    jump_add(a, CC_B, L_LT8);
    ldq(a, RAX, RDI, -1, 0);
    ldq(a, RCX, RSI, -1, 0);
    cmp_rr(a, RAX, RCX);
    jump_add(a, CC_NE, L_WORDS);
    ldq(a, RAX, RDI, RDX, -8);
    ldq(a, RCX, RSI, RDX, -8);
    jump_add(a, -1, L_LAST);
    bind(a, L_LT8);                         // 4..7: two overlapping halves
    grp(a, 1, 7, RDX, 4);
    jump_add(a, CC_B, L_LT4);
    ldd(a, RAX, RDI, -1, 0);
    ldd(a, RCX, RSI, -1, 0);
    cmp_rr(a, RAX, RCX);
    jump_add(a, CC_NE, L_WORDS);
    ldd(a, RAX, RDI, RDX, -4);
    ldd(a, RCX, RSI, RDX, -4);
    bind(a, L_LAST);
    cmp_rr(a, RAX, RCX);
    jump_add(a, CC_E, L_SAME);
    bind(a, L_WORDS);                       // rax != rcx in memory order
    rex(a, 1, 0, RAX, 0); put(a, 0x0F); put(a, 0xC8); // bswap rax
    rex(a, 1, 0, RCX, 0); put(a, 0x0F); put(a, 0xC9); // bswap rcx
    cmp_rr(a, RAX, RCX);
    jump_add(a, -1, L_DIFF);
    bind(a, L_LT4);                         // 0..3: first, middle, last
    test_rr(a, RDX);
    jump_add(a, CC_E, L_SAME);
    ldb(a, RAX, RDI, -1, 0);
    ldb(a, RCX, RSI, -1, 0);
    cmp_rr(a, RAX, RCX);
    jump_add(a, CC_NE, L_DIFF);
    mov_rr(a, R11, RDX);
    rr(a, 1, (const uint8_t[]){ 0xD1 }, 1, 5, R11); // shr r11, 1
    ldb(a, RAX, RDI, R11, 0);
    ldb(a, RCX, RSI, R11, 0);
    cmp_rr(a, RAX, RCX);
    jump_add(a, CC_NE, L_DIFF);
    ldb(a, RAX, RDI, RDX, -1);
    ldb(a, RCX, RSI, RDX, -1);
    cmp_rr(a, RAX, RCX);
    jump_add(a, CC_E, L_SAME);
    bind(a, L_DIFF);                        // flags of an unsigned a <> b: -1 below, 1 above
    rr(a, 1, (const uint8_t[]){ 0x19 }, 1, RAX, RAX); // sbb rax, rax
    grp(a, 1, 1, RAX, 1);                   // or rax, 1
    ret_(a);
    patch_local(a);
}

/*
 * The debug allocator's registry (spec Memory): a list of live
 * allocations whose nodes are malloc'd ptr, size, align, next, headed at
 * __ace$registry_head. find and remove fail unless ptr and size match a
 * record; leaks fails while any record remains. Standard System V calls.
 */
static void emit_registry(X64* a, Obj* o) {
    Sec* b = &o->secs[SEC_BSS];
    b->size = align_up(b->size, 8);
    sym_define(&o->syms, o->secs, "__ace$registry_head", 19, 0, b);
    b->size += 8;
    fail_routine(o, "__ace$allocator_fail", "ace: allocator violation\n");

    routine(a, o, "__ace$registry_find");   // find(ptr, size) -> node
    rip_op(a, 0x8B, RAX, "__ace$registry_head");
    bind(a, L_LOOP);
    test_rr(a, RAX);
    jump_add(a, CC_E, L_FAIL);
    mop(a, 0, 1, (const uint8_t[]){ 0x39 }, 1, RDI, RAX, -1, 0, 0); // cmp [rax], rdi
    jump_add(a, CC_NE, L_NEXT);
    mop(a, 0, 1, (const uint8_t[]){ 0x39 }, 1, RSI, RAX, -1, 8, 0);
    jump_add(a, CC_E, L_DONE);
    bind(a, L_NEXT);
    ldq(a, RAX, RAX, -1, 24);
    jump_add(a, -1, L_LOOP);
    bind(a, L_DONE);
    ret_(a);
    bind(a, L_FAIL);
    jmp_sym(a, "__ace$allocator_fail");
    patch_local(a);

    routine(a, o, "__ace$registry_add");    // add(ptr, size, align)
    put(a, 0x53); put(a, 0x41); put(a, 0x54); put(a, 0x41); put(a, 0x55); // push rbx, r12, r13: rsp aligned
    mov_rr(a, RBX, RDI);
    rr(a, 1, (const uint8_t[]){ 0x89 }, 1, RSI, 12);
    rr(a, 1, (const uint8_t[]){ 0x89 }, 1, RDX, 13);
    mov_imm(a, RDI, 32);
    call_sym(a, "_malloc");
    test_rr(a, RAX);
    jump_add(a, CC_E, L_FAIL);
    stq(a, RBX, RAX, -1, 0);
    stq(a, 12, RAX, -1, 8);
    stq(a, 13, RAX, -1, 16);
    rip_op(a, 0x8B, RCX, "__ace$registry_head");
    stq(a, RCX, RAX, -1, 24);
    rip_op(a, 0x89, RAX, "__ace$registry_head");
    put(a, 0x41); put(a, 0x5D); put(a, 0x41); put(a, 0x5C); put(a, 0x5B); // pop r13, r12, rbx
    ret_(a);
    bind(a, L_FAIL);
    jmp_sym(a, "__ace$allocator_fail");
    patch_local(a);

    routine(a, o, "__ace$registry_remove"); // remove(ptr, size): unlink its record and free it
    rip_op(a, 0x8D, RCX, "__ace$registry_head"); // rcx = the link to the record
    bind(a, L_LOOP);
    ldq(a, RAX, RCX, -1, 0);
    test_rr(a, RAX);
    jump_add(a, CC_E, L_FAIL);
    mop(a, 0, 1, (const uint8_t[]){ 0x39 }, 1, RDI, RAX, -1, 0, 0);
    jump_add(a, CC_NE, L_NEXT);
    mop(a, 0, 1, (const uint8_t[]){ 0x39 }, 1, RSI, RAX, -1, 8, 0);
    jump_add(a, CC_E, L_FOUND);
    bind(a, L_NEXT);
    leaq(a, RCX, RAX, -1, 24);
    jump_add(a, -1, L_LOOP);
    bind(a, L_FOUND);
    ldq(a, RDX, RAX, -1, 24);
    stq(a, RDX, RCX, -1, 0);
    mov_rr(a, RDI, RAX);
    jmp_sym(a, "_free");
    bind(a, L_FAIL);
    jmp_sym(a, "__ace$allocator_fail");
    patch_local(a);
}

static void emit_registry_leaks(X64* a, Obj* o, int registry) {
    routine(a, o, "__ace$registry_leaks");
    if (registry) { // any record left fails
        rip_op(a, 0x8B, RAX, "__ace$registry_head");
        test_rr(a, RAX);
        put(a, 0x0F); put(a, 0x85);
        reloc32(a, "__ace$allocator_fail", R_PC32); // jnz
    }
    ret_(a);
}

void x64_runtime(Obj* o) {
    process_words(o);
    if (o->bounds_fail_used) fail_routine(o, "__ace$bounds_fail", NULL);
    if (o->arithmetic_fail_used) fail_routine(o, "__ace$arithmetic_fail", "ace: arithmetic violation\n");
    if (o->contract_fail_used) fail_routine(o, "__ace$contract_fail", "ace: call contract violation\n");
    if (o->null_fail_used) fail_routine(o, "__ace$null_fail", "ace: null use violation\n");
    uint32_t label[NLABELS];
    X64 a;
    memset(&a, 0, sizeof a);
    a.o = o;
    a.t = obj_text(o);
    a.label = label;
    if (o->allocator_used) emit_registry(&a, o);
    if (o->allocator_used || o->mode == EMIT_DEBUG) emit_registry_leaks(&a, o, o->allocator_used);
    if (o->mem_copy_used) emit_mem_copy(&a, o);
    if (o->mem_fill_used) emit_mem_fill(&a, o);
    if (o->mem_compare_used) emit_mem_compare(&a, o);
    free(a.jmp);
}
