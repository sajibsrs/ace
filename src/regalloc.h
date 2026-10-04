// regalloc.h — the target-neutral half of register allocation (spec/compiler.md
// Compiler #2, pass 8), shared by the backends: block order, use counts,
// liveness intervals, phi and cast location sharing, and a linear scan over
// register classes a target describes (RaTarget).
//
// Positions: instruction i of the reverse postorder is at 2i (its uses) and
// 2i + 1 (its definition); a phi is defined at its block's first position,
// and its operands are used at each predecessor's terminator.
#ifndef ACE_REGALLOC_H
#define ACE_REGALLOC_H

#include "ir.h"

typedef struct RaUse { int v, block, pos; } RaUse;

typedef struct {
    IrFunc*   fn;
    // layout
    int*      order;   // blocks in reverse postorder (analysis: liveness, allocation)
    int*      lix;     // block -> reverse-postorder index
    int*      layout;  // blocks in emission order (a loop rotated after its body)
    int*      eix;     // block -> emission-order index
    uint8_t*  cold;    // block -> laid out last, as a @cold call's arm
    int*      bfirst;  // block -> index of its first instruction
    int*      blast;   // block -> index of its terminator
    int*      idx;     // value -> instruction index
    // analysis
    int*      nuse;
    uint8_t*  fused;   // emitted by its only user (the target marks these after ra_count_uses)
    int*      start, *end;
    int*      ufirst;  // value -> its first use in uses; a remat value has none
    RaUse*    uses;    // by value
    int*      hint, *phi_of;
    int*      with;    // value -> the value it shares a location with, or -1
    int*      calls;   // call use positions, ascending
    int       ncalls;
    int*      rtcalls; // byte routine call positions, ascending
    int       nrtcalls;
    int*      asm_pos;  // asm instruction positions, ascending
    uint32_t* asm_mask; // that instruction's asmb->reserved, parallel to asm_pos
    int       nasms;
    uint32_t  asm_all;  // every asm statement's reserved registers
    // allocation
    int8_t*   reg;     // value -> register, or -1
    int*      slot;    // value -> spill slot (8 bytes; a vector takes two), or -1
    int       nslots;
} Ra;

// A target's registers. Numbers below fp_base are the integer class, from
// fp_base up the float and vector class, all below RA_NREGS; asm masks name
// integer registers below 32.
enum { RA_NREGS = 64 };
typedef struct {
    int           fp_base;
    const int8_t* caller[2]; // caller-saved registers in preference order, by class
    int           ncaller[2];
    const int8_t* callee[2]; // callee-saved, by class
    int           ncallee[2];
    uint64_t      callee_saved;    // mask
    uint64_t      rtcall_clobbers; // what a byte routine disturbs that allocation hands out
    int           vec_cross_spills; // a vector live across a call spills (its callee-saved half is short)
    int         (*cast_in_place)(IrType to, IrType from); // the cast's register form is its operand's
} RaTarget;

static inline int ra_remat(IrOp op) { return op == IR_CONST || op == IR_SLOT; }

void ra_split_critical_edges(IrFunc* fn);
void ra_layout(Ra* r);       // order, lix, bfirst, blast, idx, layout, eix, cold
void ra_count_uses(Ra* r);   // nuse; fused all clear
void ra_liveness(Ra* r);     // start, end, ufirst, uses, honoring fused
void ra_positions(Ra* r);    // phi_of, calls, rtcalls, asm; hint all -1
void ra_coalesce(Ra* r, const RaTarget* t);
void ra_allocate(Ra* r, const RaTarget* t);
int  ra_crosses(Ra* r, int v, const int* at, int n);
void ra_free(Ra* r);

#endif
