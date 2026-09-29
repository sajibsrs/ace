// x64.c — the x64 target of the IR backend for linux-x64 (SPEC.md
// Compiler #2, passes 7-10): instruction selection, leaving SSA, the frame,
// and encoding into the shared Obj under the System V AMD64 ABI, plus the
// runtime failure routines emit.c's arm64 ones mirror (x64_runtime).
//
// Values: every value that is not a constant or a stack object lives in its
// own 8-byte frame slot, held in canonical form: a narrow integer sign- or
// zero-extended to 64 bits by its type, a bool 0 or 1. Each instruction loads
// its operands into rax, rcx, and rdx, computes, restores the canonical form,
// and stores its result. Constants and stack objects are rebuilt at each use.
// There is no register allocation yet.
//
// Frame: push rbp; mov rbp, rsp; sub rsp, N. Value slots and a phi staging
// area sit below rbp, then the stack objects, then the outgoing stack
// arguments at [rsp]. N keeps rsp 16-byte aligned at every call.
//
// Calls: scalar arguments in rdi, rsi, rdx, rcx, r8, r9, then 8-byte stack
// slots; a variadic call sets al to 0 (no vector registers); the result
// arrives in rax and is re-canonicalized (a C callee may leave a narrow
// result's upper bits unspecified). Calls go through R_X86_64_PLT32; the
// address of an extern symbol loads from the GOT, an internal one is
// rip-relative. Aggregate parts, floats, vectors, byte routines, and asm are
// refused with a diagnostic until the target covers them.
//
// Release arithmetic follows the arm64 target: shifts count modulo 32 for a
// type up to 32 bits and 64 above, a division by zero yields 0, and MIN / -1
// wraps (its remainder is 0), so neither traps. Debug checks branch to the
// shared failure routines.
#include "backend.h"
#include "ir.h"

#include <stdio.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11 };
enum { CC_O = 0, CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7,
       CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15 };
enum { R_PC32 = 2, R_PLT32 = 4, R_GOTPCREL = 9 };
// jump targets below 0: the arithmetic, null, and contract failure routines,
// then the function's located check stubs (LFAIL - k for stub k)
enum { FAIL = -1, NFAIL = -2, CFAIL = -3, LFAIL = -4 };

static const int ARG_REG[6] = { RDI, RSI, RDX, RCX, R8, R9 };

typedef struct { uint32_t site; int target; } Jump;

typedef struct {
    Obj*     o;
    IrFunc*  fn;
    Buf*     t;
    int32_t* home;    // value -> rbp offset of its slot (negative), 0 for none
    int32_t  stage;   // rbp offset of the phi staging area
    uint32_t out;     // outgoing stack argument bytes
    uint32_t frame_at; // text offset of the sub rsp imm32
    uint32_t* label;  // block -> text offset
    Jump*    jmp;
    int      njmp, cjmp;
    IrIns**  stub;    // located check sites, one per (site, name) pair
    int      nstub, cstub;
    Node*    last_at;
    int      failed;
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
    fprintf(stderr, "%s:%d:%d: error: cannot compile '%.*s' for linux-x64: %s not supported yet\n",
            d && d->path ? d->path : a->o->d->path, d ? d->line : 0, d ? d->col : 0,
            d ? (int)d->len : 0, d ? d->str : "", what);
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

// op reg, [base + disp] (disp32; a SIB byte for rsp and r12)
static void rm(X64* a, int w, const uint8_t* op, int nop, int reg, int base, int32_t disp, int force) {
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
    case IT_U32: rr(a, 0, (const uint8_t[]){ 0x89 }, 1, r, r); break;       // mov r32, r32
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

static void reloc32(X64* a, const char* name, uint32_t type) {
    Sec* t = &a->o->secs[SEC_TEXT];
    reloc_add(t, here(a), sym_intern(&a->o->syms, name, (uint32_t)strlen(name)), 1, 2, 1, type);
    put32(a, (uint32_t)-4);
}

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

static void load_val(X64* a, int r, int v) {
    IrIns* x = ins_of(a, v);
    if (x->op == IR_CONST) mov_imm(a, r, canon((IrType)x->type, x->imm));
    else if (x->op == IR_SLOT) lea(a, r, RBP, a->home[v]);
    else ld64(a, r, RBP, a->home[v]);
}

static void store_val(X64* a, int v, int r) {
    if (a->home[v]) st64(a, r, RBP, a->home[v]);
}

static int fail_target(X64* a, int target) {
    if (target == FAIL) a->o->arithmetic_fail_used = 1;
    else if (target == NFAIL) a->o->null_fail_used = 1;
    else if (target == CFAIL) a->o->contract_fail_used = 1;
    return target;
}

static void test_rr(X64* a, int r) { rr(a, 1, (const uint8_t[]){ 0x85 }, 1, r, r); }
static void cmp_rr(X64* a, int x, int y) { rr(a, 1, (const uint8_t[]){ 0x39 }, 1, y, x); } // cmp x, y

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

static void arith(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int chk = x->flags & IRF_CHECKED, wide = ir_type_bits(t) == 64 || t == IT_PTR;
    load_val(a, RAX, x->args[0]);
    load_val(a, RCX, x->args[1]);
    switch (x->op) {
    case IR_ADD: rr(a, 1, (const uint8_t[]){ 0x01 }, 1, RCX, RAX); break;
    case IR_SUB: rr(a, 1, (const uint8_t[]){ 0x29 }, 1, RCX, RAX); break;
    case IR_AND: rr(a, 1, (const uint8_t[]){ 0x21 }, 1, RCX, RAX); break;
    case IR_OR: rr(a, 1, (const uint8_t[]){ 0x09 }, 1, RCX, RAX); break;
    case IR_XOR: rr(a, 1, (const uint8_t[]){ 0x31 }, 1, RCX, RAX); break;
    default: // IR_MUL
        if (chk && wide && !ir_type_signed(t)) rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 4, RCX); // mul rcx
        else rr(a, 1, (const uint8_t[]){ 0x0F, 0xAF }, 2, RAX, RCX);                        // imul rax, rcx
        break;
    }
    if (chk && x->op != IR_AND && x->op != IR_OR && x->op != IR_XOR) {
        if (!wide) check_fits(a, t, RAX);
        else if (ir_type_signed(t) || x->op == IR_MUL) fail_if(a, CC_O, FAIL);
        else fail_if(a, CC_B, FAIL); // unsigned carry or borrow
    }
    normalize(a, t, RAX);
    store_val(a, v, RAX);
}

static void div_rem(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    int sg = ir_type_signed(t), chk = x->flags & IRF_CHECKED, rem = x->op == IR_REM;
    int bits = ir_type_bits(t);
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
    int bits = ir_type_bits(t), w = bits > 32;
    load_val(a, RAX, x->args[0]);
    load_val(a, RCX, x->args[1]);
    if (x->flags & IRF_CHECKED) { // a negative count is a large unsigned one
        mov_imm(a, RDX, (uint64_t)bits);
        cmp_rr(a, RCX, RDX);
        fail_if(a, CC_AE, FAIL);
    }
    int ext = x->op == IR_SHL ? 4 : ir_type_signed(t) ? 7 : 5; // shl, sar, shr
    rr(a, w, (const uint8_t[]){ 0xD3 }, 1, ext, RAX);
    normalize(a, t, RAX); // a 32-bit op zeroed bits 63:32
    store_val(a, v, RAX);
}

static void unary(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)x->type;
    load_val(a, RAX, x->args[0]);
    if (x->op == IR_NOT && t == IT_BOOL) {
        rr(a, 1, (const uint8_t[]){ 0x83 }, 1, 6, RAX); put(a, 1); // xor rax, 1
    } else if (x->op == IR_NOT) {
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 2, RAX);            // not rax
    } else {
        rr(a, 1, (const uint8_t[]){ 0xF7 }, 1, 3, RAX);            // neg rax
        if ((x->flags & IRF_CHECKED) && ir_type_signed(t)) {
            if (ir_type_bits(t) == 64) fail_if(a, CC_O, FAIL);
            else check_fits(a, t, RAX);
        }
    }
    normalize(a, t, RAX);
    store_val(a, v, RAX);
}

static void compare(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    IrType t = (IrType)ins_of(a, x->args[0])->type;
    load_val(a, RAX, x->args[0]);
    load_val(a, RCX, x->args[1]);
    cmp_rr(a, RAX, RCX);
    put(a, 0x0F); put(a, (uint8_t)(0x90 | cc_of(x->op, t))); put(a, 0xC0); // setcc al
    normalize(a, IT_BOOL, RAX);
    store_val(a, v, RAX);
}

static void cast(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    load_val(a, RAX, x->args[0]);
    if (x->type == IT_BOOL) { // nonzero is true
        test_rr(a, RAX);
        put(a, 0x0F); put(a, 0x95); put(a, 0xC0); // setne al
    }
    normalize(a, (IrType)x->type, RAX);
    store_val(a, v, RAX);
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

// mov (load, with extension) or store of type t at [rax + off] with rcx
static void access(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    int load = x->op == IR_LOAD;
    IrType t = (IrType)(load ? x->type : ins_of(a, x->args[1])->type);
    load_val(a, RAX, x->args[0]);
    if (x->flags & IRF_CHECKED) {
        test_rr(a, RAX);
        fail_if(a, CC_E, NFAIL);
    }
    int32_t off = x->aux;
    int bits = t == IT_BOOL ? 8 : ir_type_bits(t);
    if (load) {
        switch (bits) {
        case 8: rm(a, ir_type_signed(t), ir_type_signed(t) ? (const uint8_t[]){ 0x0F, 0xBE } : (const uint8_t[]){ 0x0F, 0xB6 }, 2, RCX, RAX, off, 0); break;
        case 16: rm(a, ir_type_signed(t), ir_type_signed(t) ? (const uint8_t[]){ 0x0F, 0xBF } : (const uint8_t[]){ 0x0F, 0xB7 }, 2, RCX, RAX, off, 0); break;
        case 32: rm(a, ir_type_signed(t), ir_type_signed(t) ? (const uint8_t[]){ 0x63 } : (const uint8_t[]){ 0x8B }, 1, RCX, RAX, off, 0); break;
        default: ld64(a, RCX, RAX, off); break;
        }
        if (t == IT_BOOL) normalize(a, t, RCX);
        store_val(a, v, RCX);
        return;
    }
    load_val(a, RCX, x->args[1]);
    switch (bits) {
    case 8: rm(a, 0, (const uint8_t[]){ 0x88 }, 1, RCX, RAX, off, 0); break;
    case 16: put(a, 0x66); rm(a, 0, (const uint8_t[]){ 0x89 }, 1, RCX, RAX, off, 0); break;
    case 32: rm(a, 0, (const uint8_t[]){ 0x89 }, 1, RCX, RAX, off, 0); break;
    default: st64(a, RCX, RAX, off); break;
    }
}

// rax = p +/- n * scale (Pointers #9): debug checks a null start, the scaled
// offset's overflow, and a result past either end of the address space.
static void pointer_offset(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    int sub = x->op == IR_PSUB, chk = x->flags & IRF_CHECKED;
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

static int scalar_sig(const IrSig* sig) {
    if (!sig) return 1;
    for (int k = 0; k < sig->nparts; k++) if (sig->parts[k].agg) return 0;
    return !sig->ret.agg;
}

static void call(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    if (!scalar_sig(x->sig) || (x->flags & IRF_SRET)) {
        refuse(a, "an aggregate argument or result");
        return;
    }
    int k0 = x->op == IR_CALLI, n = x->nargs - k0;
    for (int k = 0; k < n; k++) {
        IrType t = (IrType)ins_of(a, x->args[k0 + k])->type;
        if (ir_type_float(t) || ir_type_vector(t)) {
            refuse(a, "a float argument");
            return;
        }
    }
    for (int k = 6; k < n; k++) {
        load_val(a, RAX, x->args[k0 + k]);
        st64(a, RAX, RSP, 8 * (k - 6));
    }
    if (x->op == IR_CALLI) {
        load_val(a, R11, x->args[0]);
        if (x->flags & IRF_CHECKED) {
            test_rr(a, R11);
            fail_if(a, CC_E, NFAIL);
        }
    }
    for (int k = 0; k < n && k < 6; k++) load_val(a, ARG_REG[k], x->args[k0 + k]);
    if (x->flags & IRF_VARARGS) mov_imm(a, RAX, 0); // al: no vector registers
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
    IrType t = (IrType)x->type;
    if (t == IT_VOID) return;
    if (ir_type_float(t)) {
        refuse(a, "a float result");
        return;
    }
    normalize(a, t, RAX);
    store_val(a, v, RAX);
}

// --- emission ---------------------------------------------------------------------------

static void line_row(X64* a, Node* at) {
    if (!at || at == a->last_at || at->line <= 0) return;
    a->last_at = at;
    obj_line(a->o, here(a), at->line, at->col, at->path ? at->path : a->o->d->path);
}

static void phi_moves(X64* a, int b) {
    IrFunc* fn = a->fn;
    int s = fn->blocks[b].succ[0];
    IrBlock* sb = &fn->blocks[s];
    int p = 0;
    while (sb->preds[p] != b) p++;
    int n = 0;
    while (n < sb->nins && fn->ins[sb->ins[n]].op == IR_PHI) n++;
    for (int i = 0; i < n; i++) { // stage every source first: the moves are parallel
        load_val(a, RAX, fn->ins[sb->ins[i]].args[p]);
        st64(a, RAX, RBP, a->stage - 8 * i);
    }
    for (int i = 0; i < n; i++) {
        ld64(a, RAX, RBP, a->stage - 8 * i);
        store_val(a, sb->ins[i], RAX);
    }
}

static void terminator(X64* a, int b, int v) {
    IrFunc* fn = a->fn;
    IrIns* x = ins_of(a, v);
    IrBlock* bb = &fn->blocks[b];
    if (x->op == IR_JMP) {
        phi_moves(a, b);
        if (bb->succ[0] != b + 1) jump_add(a, -1, bb->succ[0]);
        return;
    }
    if (x->op == IR_RET) {
        if (x->nargs > 1) {
            refuse(a, "a result of several leaves");
            return;
        }
        if (x->nargs) {
            if (ir_type_float((IrType)ins_of(a, x->args[0])->type)) {
                refuse(a, "a float result");
                return;
            }
            load_val(a, RAX, x->args[0]);
        }
        put(a, 0xC9); // leave
        put(a, 0xC3); // ret
        return;
    }
    load_val(a, RAX, x->args[0]);
    test_rr(a, RAX);
    int t = bb->succ[0], f = bb->succ[1];
    if (f == b + 1) {
        jump_add(a, CC_NE, t);
    } else if (t == b + 1) {
        jump_add(a, CC_E, f);
    } else {
        jump_add(a, CC_NE, t);
        jump_add(a, -1, f);
    }
}

// A debug check: a branch to its site's stub when its condition is false.
static void check(X64* a, int v) {
    IrIns* x = ins_of(a, v);
    load_val(a, RAX, x->args[0]);
    test_rr(a, RAX);
    if (!x->name) {
        fail_if(a, CC_E, CFAIL);
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
    jump_add(a, CC_E, LFAIL - k);
}

static void emit_block(X64* a, int b) {
    IrFunc* fn = a->fn;
    IrBlock* bb = &fn->blocks[b];
    for (int i = 0; i < bb->nins && !a->failed; i++) {
        int v = bb->ins[i];
        IrIns* x = &fn->ins[v];
        if (x->op != IR_CHECK) line_row(a, x->at);
        IrType t = (IrType)x->type;
        if (ir_type_float(t) || ir_type_vector(t)) {
            refuse(a, "a float or vector value");
            return;
        }
        switch (x->op) {
        case IR_CONST: case IR_PARAM: case IR_PHI: case IR_SLOT:
            break;
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR: case IR_XOR:
            arith(a, v);
            break;
        case IR_DIV: case IR_REM:
            div_rem(a, v);
            break;
        case IR_SHL: case IR_SHR:
            shift(a, v);
            break;
        case IR_NEG: case IR_NOT:
            unary(a, v);
            break;
        case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
            if (ir_type_float((IrType)ins_of(a, x->args[0])->type)) refuse(a, "a float comparison");
            else compare(a, v);
            break;
        case IR_CAST:
            if (ir_type_float((IrType)ins_of(a, x->args[0])->type)) refuse(a, "a float conversion");
            else cast(a, v);
            break;
        case IR_POPCNT: case IR_CLZ: case IR_CTZ: case IR_BSWAP: case IR_ROTL: case IR_ROTR:
            bitop(a, v);
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
            if (x->op == IR_STORE && ir_type_float((IrType)ins_of(a, x->args[1])->type)) refuse(a, "a float store");
            else access(a, v);
            break;
        case IR_PADD: case IR_PSUB:
            pointer_offset(a, v);
            break;
        case IR_CALL: case IR_CALLI:
            call(a, v);
            break;
        case IR_CHECK:
            check(a, v);
            break;
        case IR_RTCALL:
            refuse(a, "a byte routine (memory copy, fill, or compare)");
            break;
        case IR_ASM:
            refuse(a, "inline assembly");
            break;
        case IR_RESULT: case IR_PROJ:
            refuse(a, "an aggregate result");
            break;
        case IR_BITCAST:
            refuse(a, "a float bitcast");
            break;
        case IR_SQRT: case IR_FLOOR: case IR_CEIL: case IR_FMA:
            refuse(a, "a float operation");
            break;
        default:
            terminator(a, b, v);
            break;
        }
    }
}

// Split each edge from a two-way branch into a block with phis, so phi moves
// happen only at the end of a jump's block.
static void split_critical_edges(IrFunc* fn) {
    int n = fn->nblocks;
    for (int b = 0; b < n; b++) {
        for (int k = 0; k < 2; k++) {
            IrBlock* bb = &fn->blocks[b];
            int s = bb->succ[k];
            if (s < 0 || bb->succ[1 - k] < 0) continue;
            IrBlock* sb = &fn->blocks[s];
            if (!sb->nins || fn->ins[sb->ins[0]].op != IR_PHI) continue;
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
            for (int p = 0; p < sb->npreds; p++)
                if (sb->preds[p] == b) { sb->preds[p] = mid; break; }
        }
    }
}

static uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Slots, the phi staging area, and stack objects below rbp; the frame size.
static uint32_t layout(X64* a) {
    IrFunc* fn = a->fn;
    a->home = xcalloc((size_t)fn->nins, sizeof(int32_t));
    uint32_t down = 0, maxphi = 0;
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        uint32_t nphi = 0;
        for (int i = 0; i < bb->nins; i++) {
            int v = bb->ins[i];
            IrIns* x = &fn->ins[v];
            if (x->op == IR_PHI) nphi++;
            if (x->op == IR_CALL || x->op == IR_CALLI) {
                int n = x->nargs - (x->op == IR_CALLI);
                if (n > 6 && 8u * (uint32_t)(n - 6) > a->out) a->out = 8u * (uint32_t)(n - 6);
            }
            if (x->type == IT_VOID || x->op == IR_CONST || x->op == IR_SLOT) continue;
            down += 8;
            a->home[v] = -(int32_t)down;
        }
        if (nphi > maxphi) maxphi = nphi;
    }
    down += 8 * maxphi;
    a->stage = -(int32_t)down + 8 * (int32_t)(maxphi ? maxphi - 1 : 0);
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

// Entry: register parameters, and the ones the caller stacked at rbp + 16,
// store to their slots in canonical form (a C caller may leave a narrow
// argument's upper bits unspecified).
static void params(X64* a) {
    IrFunc* fn = a->fn;
    if (!scalar_sig(fn->sig) || fn->ret_mem) {
        refuse(a, "an aggregate parameter or result");
        return;
    }
    IrBlock* bb = &fn->blocks[0];
    for (int i = 0; i < bb->nins; i++) {
        int v = bb->ins[i];
        IrIns* x = ins_of(a, v);
        if (x->op != IR_PARAM || !a->home[v]) continue;
        IrType t = (IrType)x->type;
        if (ir_type_float(t) || ir_type_vector(t)) {
            refuse(a, "a float parameter");
            return;
        }
        int k = (int)x->imm;
        int r = k < 6 ? ARG_REG[k] : RAX;
        if (k >= 6) ld64(a, RAX, RBP, 16 + 8 * (k - 6));
        normalize(a, t, r);
        store_val(a, v, r);
    }
}

static void emit_body(X64* a) {
    IrFunc* fn = a->fn;
    line_row(a, fn->decl);
    put(a, 0x55);                        // push rbp
    mov_rr(a, RBP, RSP);                 // mov rbp, rsp
    rr(a, 1, (const uint8_t[]){ 0x81 }, 1, 5, RSP); // sub rsp, imm32
    a->frame_at = here(a);
    put32(a, 0);
    params(a);
    for (int b = 0; b < fn->nblocks && !a->failed; b++) {
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
    split_critical_edges(fn);
    uint32_t frame = layout(&a);
    a.label = xcalloc((size_t)fn->nblocks, sizeof(uint32_t));
    while (a.t->len % FUNC_ALIGN) put(&a, 0x90);
    uint32_t start = here(&a);
    sym_define(&o->syms, o->secs, name, len, global, &o->secs[SEC_TEXT]);
    emit_body(&a);
    buf_patch32(a.t, a.frame_at, frame);
    obj_debug_fn(o, start, here(&a) - start, fn->decl->str, fn->decl->len);
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

void x64_runtime(Obj* o) {
    if (o->bounds_fail_used) fail_routine(o, "__ace$bounds_fail", NULL);
    if (o->arithmetic_fail_used) fail_routine(o, "__ace$arithmetic_fail", "ace: arithmetic violation\n");
    if (o->contract_fail_used) fail_routine(o, "__ace$contract_fail", "ace: call contract violation\n");
    if (o->null_fail_used) fail_routine(o, "__ace$null_fail", "ace: null use violation\n");
    if (o->allocator_used) {
        fprintf(stderr, "%s: error: the allocator is not supported for linux-x64 yet\n", o->d->path);
        o->d->errors++;
    } else if (o->mode == EMIT_DEBUG) { // no allocation intrinsic exists in this unit
        Buf* t = obj_text(o);
        while (t->len % FUNC_ALIGN) buf_put8(t, 0x90);
        sym_define(&o->syms, o->secs, "__ace$registry_leaks", 20, 0, &o->secs[SEC_TEXT]);
        buf_put8(t, 0xC3);
    }
    if (o->mem_copy_used || o->mem_fill_used || o->mem_compare_used) {
        fprintf(stderr, "%s: error: byte routines are not supported for linux-x64 yet\n", o->d->path);
        o->d->errors++;
    }
}
