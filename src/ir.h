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
typedef enum {
    IT_VOID, IT_BOOL,
    IT_I8, IT_I16, IT_I32, IT_I64,
    IT_U8, IT_U16, IT_U32, IT_U64,
    IT_PTR, // an address: pointers, function values, variable homes; compares unsigned
} IrType;

#define IR_OPS(X)                                                              \
    X(CONST, "const")   /* imm */                                              \
    X(PARAM, "param")   /* imm = parameter index */                            \
    X(PHI, "phi")       /* args[i] flows in from block preds[i] */             \
    X(ADD, "add") X(SUB, "sub") X(MUL, "mul") X(DIV, "div") X(REM, "rem")      \
    X(AND, "and") X(OR, "or") X(XOR, "xor") X(SHL, "shl") X(SHR, "shr")        \
    X(NEG, "neg") X(NOT, "not")                                                \
    /* comparisons: signedness from the operand type; result bool */          \
    X(EQ, "eq") X(NE, "ne") X(LT, "lt") X(LE, "le") X(GT, "gt") X(GE, "ge")    \
    X(CAST, "cast")     /* truncate or extend to the result type */           \
    X(SLOT, "slot")     /* ptr to a stack object: imm = size, aux = alignment */ \
    X(GLOBAL, "global") /* ptr to sym: a global variable or a function */      \
    X(LOAD, "load")     /* the result type's value at args[0] + aux */         \
    X(STORE, "store")   /* args[1] to args[0] + aux, at args[1]'s width */     \
    /* pointer offset: ptr args[0] +/- u64 args[1] * imm (the element size) */ \
    X(PADD, "padd") X(PSUB, "psub")                                            \
    X(CALL, "call")     /* to name (sym, when an Ace or extern function) */    \
    X(CALLI, "calli")   /* to the function value args[0]; arguments follow */ \
    /* terminators */                                                          \
    X(JMP, "jmp")       /* succ[0] */                                          \
    X(BR, "br")         /* args[0] ? succ[0] : succ[1] */                      \
    X(RET, "ret")       /* optional args[0] */

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
};

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
    const char* name; // CALL target's link symbol
    Node*    at;     // source position, for line rows
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
    IrType   ret;
    IrType*  params;
    int      nparams;
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
// v wrapped into t: truncated, then extended by t's signedness
uint64_t ir_wrap(IrType t, uint64_t v);

// Lower a checked function to IR, or return NULL and set *why when it uses
// something the IR does not cover yet (the AST emitter compiles it instead).
IrFunc* ir_lower(Node* f, EmitMode mode, const char** why);
void    ir_free(IrFunc* fn);
// Structural invariants; returns 0 and prints the first violation to stderr.
int     ir_verify(IrFunc* fn);
void    ir_print(IrFunc* fn, FILE* out);
// --ir: print the IR of every function in the root file, or why it was not lowered.
void    ir_dump_file(Node* file, EmitMode mode);

#endif
