// sema.h — shared semantic model. sema.c owns and annotates; emit.c consumes.
// Types are arena-allocated and stable after sema_check.
#ifndef SEMA_H
#define SEMA_H

#include "ace.h"

// --- type kinds ---------------------------------------------------------------

enum {
    SK_PRIM, SK_PTR, SK_SLICE, SK_ARRAY, SK_TUPLE, SK_FUNC,
    SK_STRUCT, SK_ENUM, SK_CONTRACT, SK_DISTINCT, SK_ERROR_DOMAIN,
    SK_TYPEPARAM, SK_ERROR,
    SK_UNION, SK_BITS, SK_STATICMAP, SK_VOID, SK_UT, SK_TEMPLATE, SK_ERR
};

enum {
    P_BOOL, P_I8, P_I16, P_I32, P_I64, P_U8, P_U16, P_U32, P_U64,
    P_F32, P_F64, P_STRING, P_USIZE, P_TYPEID
};

typedef struct SType SType;
typedef struct {
    const char* name;
    SType* t;
    Node* default_value; // struct-literal default; never storage initialization
    uint32_t requested_align; // zero means contextual default
    uint32_t requested_offset; // @offset(N), meaningful when has_offset
    uint8_t has_offset;
    uint32_t effective_align;
    uint32_t offset;
    uint8_t bit_offset, bit_width, bit_signed, bit_anonymous;
} SField;

struct SType {
    uint8_t     kind;
    uint8_t     prim;
    uint8_t     is_mut;
    uint8_t     calc; // layout in progress (cycle guard)
    uint8_t     building; // aggregate fields are currently being constructed
    uint8_t     invalid_layout;
    uint8_t     is_packed;
    uint8_t     is_overlap; // SK_STRUCT: some fields share bytes (@offset)
    uint8_t     is_c_abi; // SK_FUNC: explicitly extern "c" callable shape
    uint32_t    size, align;
    uint32_t    requested_align; // explicit aggregate @align, zero when absent
    const char* name;
    SType*      base;
    uint64_t    count;
    SField*     fields; int nfields; // struct/tuple fields or union variants
    SType**     params; int nparams; // function params
    SType**     args; int nargs;     // nominal generic instantiation arguments
    SType*      ret;
    Node*       decl;
    uint32_t    type_id; // compiler-assigned nonzero identity used by typeid/Error
    struct STypeInst* insts; // instantiation memo (generic decls)
};

typedef struct STypeInst {
    SType** args;
    SType*  result;
    struct STypeInst* next;
} STypeInst;

// --- symbols --------------------------------------------------------------------

enum { SYM_VAR, SYM_FUNC, SYM_TYPE, SYM_MODULE };

typedef struct Sym Sym;
struct Sym {
    const char* name; uint32_t len;
    uint8_t     kind;
    uint8_t     is_const, is_mut, is_pub, is_generic, is_extern;
    uint8_t     is_param, is_const_backed, is_global;
    // top-level declarations are entered before any is resolved; a lookup
    // resolves one still DECL_ENTERED on demand (Compiler #5)
    uint8_t     decl_state, in_cycle;
    uint8_t     is_literal_backed; // string binding initialized from a literal
    uint8_t     is_root; // top-level in the root file: unseen from imported files
    SType*      type;
    Node*       decl;   // binder/declaration node (slot key for the emitter)
    Node*       init;   // top-level initializer (static Error validation/emission)
    Node*       top;    // top-level declaration resolved on demand
    Sym*        next;
    Sym*        overload; // same-scope function overloads; not part of scope chain
    // generic specialization (Generics #9–14): a template chains the
    // specializations bound to it; a specialization records its exact
    // type-argument tuple (spec_args != NULL marks a specialization)
    Sym*        specs;     // template: head of its bound specializations
    // set head: specializations entered but not yet bound (Compiler #5)
    Sym*        entered_specs;
    Sym*        spec_next; // specialization: next bound to the same template
    SType**     spec_args; // specialization: exact type-argument tuple
    int         spec_nargs;
    // generic function: the scope its body is checked under at stamp time;
    // NULL means the shared file scope
    struct Scope* decl_scope;
};

// --- helpers used by the emitter --------------------------------------------------

// Size of a type (layout computed on demand).
uint32_t sema_size(SType* t);
uint32_t sema_align(SType* t);

/*
 * Byte offset of a tagged union's inline payload storage after the i32 tag,
 * aligned to the largest payload alignment. Defined for SK_UNION types.
 */
uint32_t sema_union_payload_off(SType* t);

// Build a type from a syntax node against the checked file scope (e.g. for
// contract-method signatures at dispatch sites).
SType* sema_build_type(Node* t);

// Re-check (and re-annotate) a generic function body under a type-argument
// substitution, immediately before the emitter emits that stamp.
SType* sema_stamp_body(Node* generic_fn, SType** args, int nargs); // the substituted return type

// The instantiation memo of a generic declaration (chain on its tparams node).
STypeInst* sema_insts(Node* generic_decl);

static inline SType* node_ty(Node* n) { return (SType*)n->ty; }
static inline Sym*   node_sym(Node* n) { return (Sym*)n->sym; }

#endif
