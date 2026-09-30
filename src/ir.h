// ir.h — the per-function SSA IR between the checker and the targets
// (spec/compiler.md Compiler #2 and #4). lower.c builds it
// from a checked function; the passes and each target consume it.
#ifndef IR_H
#define IR_H

#include "ace.h"
#include "sema.h"

#include <stdio.h>

// Value types. Every integer value is held in range for its type: a narrow
// result is wrapped (release) or checked (debug) at the operation that makes it.
// A float CONST's imm is its IEEE bit pattern (an f32's in the low 32 bits).
// A vector type is four (or, for f64, two) lanes in 128 bits, internal to the
// optimizer (Compiler #6, auto-vectorization): no source type lowers to one,
// and none crosses a call, parameter, or result. A v4i32 lane compares signed
// and wraps. v2f64 takes arithmetic, splat, load, store, and phi only.
typedef enum {
    IT_VOID, IT_BOOL,
    IT_I8, IT_I16, IT_I32, IT_I64,
    IT_U8, IT_U16, IT_U32, IT_U64,
    IT_PTR, // an address: pointers, function values, variable homes; compares unsigned
    IT_F32, IT_F64,
    IT_V4F32, IT_V4I32, IT_V2F64,
} IrType;

#define IR_OPS(X)                                                              \
    X(CONST, "const")   /* imm */                                              \
    X(PARAM, "param")   /* imm = parameter index */                            \
    X(PHI, "phi")       /* args[i] flows in from block preds[i] */             \
    X(ADD, "add") X(SUB, "sub") X(MUL, "mul") X(DIV, "div") X(REM, "rem")      \
    X(AND, "and") X(OR, "or") X(XOR, "xor") X(SHL, "shl") X(SHR, "shr")        \
    X(NEG, "neg") X(NOT, "not")                                                \
    /* comparisons: signedness or float from the operand type; result bool.   \
       Float ones are IEEE: eq, lt, le, gt, ge are false and ne true when an   \
       operand is a NaN, so not(lt) is not ge */                               \
    X(EQ, "eq") X(NE, "ne") X(LT, "lt") X(LE, "le") X(GT, "gt") X(GE, "ge")    \
    X(CAST, "cast")     /* truncate or extend; to or from a float: convert,   \
                           toward zero and saturating to an integer (NaN: 0) */ \
    X(BITCAST, "bitcast") /* the same bits between a float and an integer of \
                           its width */                                        \
    /* bits of unsigned args[0] at its width, the result of its type:         \
       set bits, leading and trailing zeros (the width for zero), bytes       \
       reversed, and a rotation by u64 args[1] modulo the width */            \
    X(POPCNT, "popcnt") X(CLZ, "clz") X(CTZ, "ctz") X(BSWAP, "bswap")          \
    X(ROTL, "rotl") X(ROTR, "rotr")                                            \
    /* exact IEEE: the square root, rounding down or up to an integer, and    \
       args[0] * args[1] + args[2] rounded once */                            \
    X(SQRT, "sqrt") X(FLOOR, "floor") X(CEIL, "ceil") X(FMA, "fma")            \
    X(SLOT, "slot")     /* ptr to a stack object: imm = size, aux = alignment */ \
    X(GLOBAL, "global") /* ptr to sym: a global variable or a function */      \
    X(STR, "str")       /* ptr to the bytes of a string literal: name, imm long */ \
    X(LOAD, "load")     /* the result type's value at args[0] + aux */         \
    X(STORE, "store")   /* args[1] to args[0] + aux, at args[1]'s width */     \
    /* pointer offset: ptr args[0] +/- u64 args[1] * imm (the element size) */ \
    X(PADD, "padd") X(PSUB, "psub")                                            \
    X(CALL, "call")     /* to name (sym, when an Ace or extern function) */    \
    X(CALLI, "calli")   /* to the function value args[0]; arguments follow */ \
    X(RTCALL, "rtcall") /* to the byte routine name (mem_copy, mem_move,      \
                           mem_fill: ptr, ptr or u8, u64; mem_compare: ptr,   \
                           ptr, u64 to an i64 of -1, 0, or 1), which every    \
                           target keeps from disturbing any value */          \
    X(ASM, "asm")       /* verbatim instruction text in name; args are the   \
                           "in" bindings, each moved to asmb's matching x     \
                           register; an "out" binding's value follows as a   \
                           PROJ, read back from its own x register (Inline   \
                           Assembly #1-3) */                                  \
    X(RESULT, "result") /* ptr to the caller's memory for this function's    \
                           result of over IR_MAX_LEAVES leaves */             \
    X(PROJ, "proj")     /* leaf imm of the aggregate result of call args[0]  \
                           or an asm's out bindings, which it follows        \
                           directly with the others */                       \
    /* vectors: every lane of vector type the scalar args[0] of its lane    \
       type; each lane args[1]'s where v4i32 mask args[0]'s is all ones,     \
       else args[2]'s; the v4i32 <0, 1, 2, 3>. add, sub, mul, neg, div       \
       (float only), and, or, xor, shl and shr (v4i32 only, by an i32        \
       constant 1-31), compare (to a v4i32 mask of all-ones or zero lanes),  \
       cast (v4i32 to v4f32), load, store, and phi also take vectors,        \
       lane-wise and never checked */                                        \
    X(SPLAT, "splat") X(SELECT, "select") X(LANES, "lanes")                    \
    X(CHECK, "check")   /* debug: bool args[0] false traps, writing "path:    \
                           line:col: name" for site at (Safety & Debug #2),   \
                           or the call contract failure when name is NULL */  \
    /* terminators */                                                          \
    X(JMP, "jmp")       /* succ[0] */                                          \
    X(BR, "br")         /* args[0] ? succ[0] : succ[1] */                      \
    X(RET, "ret")       /* the result's leaves, if any */

typedef enum {
#define X(name, _) IR_##name,
    IR_OPS(X)
#undef X
} IrOp;

enum {
    // debug build (Safety & Debug Builds #6): the operation traps through the
    // shared arithmetic failure routine when its result is not representable
    // (add, sub, mul, signed neg; div and rem by MIN / -1), its divisor is
    // zero, or its shift count is negative or at least the operand width
    IRF_CHECKED = 1,
    // also IRF_CHECKED, on load, store, and calli: a null address or function
    // value traps through the null failure routine (Safety & Debug Builds
    // #8); on padd and psub: a null start, an overflowing scaled offset, or an
    // address past either end of the address space (Pointers #9)
    IRF_VOLATILE = 2, // load, store: an access-site volatile access
    IRF_VARARGS = 4,  // call: arguments from index aux on are variadic
    // call, calli: the result, of over IR_MAX_LEAVES leaves, goes to the
    // memory whose address is the last argument (AAPCS64 passes it in x8)
    IRF_SRET = 8,
};

// Aggregates are never IR values: lowering splits every struct, tuple, and
// array, and every string and slice (a ptr and a u64 length), into its scalar
// leaves (SSA values, or loads and stores for one in memory), and each target
// maps leaves onto its ABI. One of over IR_MAX_LEAVES leaves stays in memory.
enum { IR_MAX_LEAVES = 16 };

typedef struct {
    uint32_t off;  // byte offset in the aggregate
    uint8_t  type; // IrType
} IrLeaf;

// One argument, parameter, or result as the ABI sees it: n leaves from first,
// of aggregate type agg, or NULL for a scalar (a mut or receiver address
// included). addr marks an aggregate of over 16 bytes passed by value whose
// one leaf is its address in the IR: the address of the caller's copy.
typedef struct {
    SType*  agg;
    int     first, n;
    uint8_t addr;
} IrPart;

// How a call's arguments (after a calli's target) or a function's parameters
// group into parts, and its result; absent (NULL) when every part is a scalar.
typedef struct {
    IrPart* parts;
    int     nparts;
    IrPart  ret;   // n = 0 when there is no result
} IrSig;

// An asm statement's real-register bindings (Inline Assembly #2): reg[k] is
// the x register args[k] ("in") moves to before the instruction; out[k] is
// the x register the k-th following PROJ reads after it. reserved is every
// register named in any "in", "out", or "clobbers" binding — none of them
// may hold a value live across the instruction.
typedef struct {
    int*     reg;    // nargs of them, parallel to args
    int      nout;
    int*     out;
    uint32_t reserved;
} IrAsm;

typedef struct {
    uint8_t  op;     // IrOp
    uint8_t  type;   // IrType of the result
    uint8_t  flags;  // IRF_*
    int      block;  // owning block, or -1 once removed
    int*     args;   // value ids
    int      nargs, cargs;
    uint64_t imm;    // CONST value (in range for type), PARAM index, SLOT size, PADD/PSUB scale
    int32_t  aux;    // LOAD/STORE offset, SLOT alignment, CALL first variadic argument
    Sym*     sym;    // GLOBAL symbol; CALL target function, when not a runtime routine
    const char* name; // CALL target's link symbol; STR bytes; CHECK diagnostic; ASM instruction text
    IrSig*   sig;    // CALL, CALLI: the parts of its arguments and result, or NULL
    IrAsm*   asmb;   // ASM: its register bindings
    Node*    at;     // source position, for line rows; CHECK: the failing site
} IrIns;

typedef struct {
    int* ins;        // instruction ids in order; phis first, terminator last
    int  nins, cins;
    int* preds;
    int  npreds, cpreds;
    int  succ[2];    // -1 when absent
    uint8_t sealed;  // every predecessor is known (lowering)
} IrBlock;

// A contract construction over a generic concrete type (Carved #5): no
// signing emits its pair's vtable, so the object gains it with the function.
typedef struct {
    SType* conc;
    SType* contract;
    Node*  conf;
} IrVtable;

typedef struct {
    Node*    decl;
    Sym*     sym;
    IrType*  params; // leaves, in order
    int      nparams;
    IrType*  rets;   // result leaves; none for a void function
    int      nrets;
    SType*   ret_mem; // a result of over IR_MAX_LEAVES leaves, stored through RESULT; else NULL
    IrSig*   sig;    // the parts of params and rets, or NULL when all are scalars
    IrIns*   ins;    // indexed by value id
    int      nins, cins;
    IrBlock* blocks; // blocks[0] is the entry
    int      nblocks, cblocks;
    IrVtable* vtables;
    int       nvtables, cvtables;
} IrFunc;

// --- construction (lower.c) -------------------------------------------------------
IrFunc*  ir_func_new(Node* decl, Sym* sym);
int      ir_block_new(IrFunc* fn);
int      ir_ins_new(IrFunc* fn, int block, IrOp op, IrType type); // appended
int      ir_phi_new(IrFunc* fn, int block, IrType type);          // after the block's phis
void     ir_arg_add(IrFunc* fn, int v, int arg);
void     ir_edge_add(IrFunc* fn, int from, int to);
int      ir_is_term(IrOp op);

int      ir_type_bits(IrType t);
int      ir_type_signed(IrType t);
int      ir_type_float(IrType t);
int      ir_type_vector(IrType t);
IrType   ir_lane_type(IrType t); // a vector's lane type; t itself for a scalar
// The IR type of scalar t, or -1 when t is not a scalar the IR covers.
int      ir_type_of(SType* t);
// The leaves of t in offset order (a scalar is its own one leaf), or -1 with
// *bad set to the first type the IR does not cover.
int      ir_leaves(SType* t, IrLeaf* out, SType** bad);
int      ir_leaf_count(SType* t); // uncapped; -1 when some leaf is not covered
int      ir_is_overlay(SType* t);  // a union or a struct whose fields share bytes
int      ir_holds_union(SType* t); // is or holds an overlay in a struct, tuple, or array member, at any depth
// v wrapped into t: truncated, then extended by t's signedness
uint64_t ir_wrap(IrType t, uint64_t v);

// Lower a checked function to IR, or return NULL and set *why when it uses
// something the IR does not cover (a compile error).
// file is the program, whose error domains an Error's name and message search.
// targs/ntargs are the instantiation's type arguments for a generic template
// (f->c set, not itself a specialization); pass NULL/0 otherwise.
IrFunc* ir_lower(Node* file, Node* f, EmitMode mode, SType** targs, int ntargs, const char** why);
// The seed and mask of static map s's perfect hash (emit.c); 0 when none was found.
int     staticmap_hash(Sym* s, uint64_t* seed, uint32_t* mask);
void    ir_free(IrFunc* fn);
// Pass 6 (release only, optimizer.c): constant propagation, value numbering, loop
// strength reduction, dead-code and control-flow cleanup, then auto-vectorization
// (cleaning up again after it), in place.
void    ir_optimize(IrFunc* fn);
// Immediate dominators (Cooper-Harvey-Kennedy) over a reverse postorder, which
// *rpo_out receives (malloc'd, *nrpo blocks); idom[0] = 0 and idom[b] = -1 for
// a block unreachable from the entry. Both results are the caller's to free.
int*    ir_dominators(IrFunc* fn, int** rpo_out, int* nrpo);
int     ir_dominates(const int* idom, int a, int b); // a dominates reachable block b
// Structural invariants; returns 0 and prints the first violation to stderr.
int     ir_verify(IrFunc* fn);
void    ir_print(IrFunc* fn, FILE* out);
// --ir: print the IR of every function in the root file, or why it was not lowered.
void    ir_dump_file(Node* file, EmitMode mode);

#endif
