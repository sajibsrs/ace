// ir.h — the per-function SSA IR between the checker and the targets
// (SPEC.md Decision Queue, "Per-function SSA IR backend"). lower.c builds it
// from a checked function; the passes and each target consume it.
#ifndef IR_H
#define IR_H

#include "ace.h"
#include "sema.h"

#include <stdio.h>

// Value types. Every integer value is held in range for its type: a narrow
// result is wrapped (release) or checked (debug) at the operation that makes it.
// A float CONST's imm is its IEEE bit pattern (an f32's in the low 32 bits).
typedef enum {
    IT_VOID, IT_BOOL,
    IT_I8, IT_I16, IT_I32, IT_I64,
    IT_U8, IT_U16, IT_U32, IT_U64,
    IT_PTR, // an address: pointers, function values, variable homes; compares unsigned
    IT_F32, IT_F64,
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
    X(RESULT, "result") /* ptr to the caller's memory for this function's    \
                           result of over IR_MAX_LEAVES leaves */             \
    X(PROJ, "proj")     /* leaf imm of the aggregate result of call args[0],  \
                           which it follows directly with the call's others */ \
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
// of aggregate type agg, or NULL for a scalar (a by-address one included).
typedef struct {
    SType* agg;
    int    first, n;
} IrPart;

// How a call's arguments (after a calli's target) or a function's parameters
// group into parts, and its result; absent (NULL) when every part is a scalar.
typedef struct {
    IrPart* parts;
    int     nparts;
    IrPart  ret;   // n = 0 when there is no result
} IrSig;

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
    const char* name; // CALL target's link symbol; STR bytes; CHECK diagnostic
    IrSig*   sig;    // CALL, CALLI: the parts of its arguments and result, or NULL
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
// The IR type of scalar t, or -1 when t is not a scalar the IR covers.
int      ir_type_of(SType* t);
// The leaves of t in offset order (a scalar is its own one leaf), or -1 with
// *bad set to the first type the IR does not cover.
int      ir_leaves(SType* t, IrLeaf* out, SType** bad);
int      ir_leaf_count(SType* t); // uncapped; -1 when some leaf is not covered
int      ir_holds_union(SType* t); // a union in a struct, tuple, or array member, at any depth
// v wrapped into t: truncated, then extended by t's signedness
uint64_t ir_wrap(IrType t, uint64_t v);

// Lower a checked function to IR, or return NULL and set *why when it uses
// something the IR does not cover yet (the AST emitter compiles it instead).
IrFunc* ir_lower(Node* f, EmitMode mode, const char** why);
// Inline expansion (Functions & ABI #8), shared with the AST emitter: the
// inline function a direct call names, with the type arguments its body is
// stamped under, or NULL.
Node*   inline_target(Node* call, SType*** targs, int* ntargs);
// f's parameters (bind, one per parameter) and body copied with fresh local
// symbols, so each expansion has locals of its own; a generic body is stamped
// under targs first, and the copy keeps that stamp's types.
void    inline_copy(Node* f, SType** targs, int ntargs, Node** bind, Node** body);
void    ir_free(IrFunc* fn);
// Structural invariants; returns 0 and prints the first violation to stderr.
int     ir_verify(IrFunc* fn);
void    ir_print(IrFunc* fn, FILE* out);
// --ir: print the IR of every function in the root file, or why it was not lowered.
void    ir_dump_file(Node* file, EmitMode mode);

#endif
