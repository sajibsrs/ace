// backend.h — state shared by the code generators and the object writer.
// macho.c owns the Mach-O arm64 object: sections, symbols, relocations,
// fixups, DWARF, and the file image. Each backend emits into one Obj.
#ifndef BACKEND_H
#define BACKEND_H

#include "ace.h"
#include "ir.h"

#include <stdlib.h>
#include <string.h>

void oom(void);

// --- growable byte buffer -------------------------------------------------------

typedef struct {
    uint8_t* d;
    size_t   len, cap;
} Buf;

static inline void buf_reserve(Buf* b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + extra) cap *= 2;
    b->d = realloc(b->d, cap);
    if (!b->d) oom();
    b->cap = cap;
}

static inline void buf_put8(Buf* b, uint8_t v) {
    buf_reserve(b, 1);
    b->d[b->len++] = v;
}

static inline void buf_put16(Buf* b, uint16_t v) {
    buf_put8(b, (uint8_t)v);
    buf_put8(b, (uint8_t)(v >> 8));
}

static inline void buf_put32(Buf* b, uint32_t v) {
    for (int i = 0; i < 4; i++) buf_put8(b, (uint8_t)(v >> (8 * i)));
}

static inline void buf_put64(Buf* b, uint64_t v) {
    for (int i = 0; i < 8; i++) buf_put8(b, (uint8_t)(v >> (8 * i)));
}

static inline void buf_write(Buf* b, const void* p, size_t n) {
    buf_reserve(b, n);
    memcpy(b->d + b->len, p, n);
    b->len += n;
}

static inline void buf_patch32(Buf* b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) b->d[off + (size_t)i] = (uint8_t)(v >> (8 * i));
}

// --- sections & symbols ----------------------------------------------------------

enum {
    SEC_TEXT, SEC_CONST, SEC_DATA, SEC_BSS,
    SEC_DEBUG_ABBREV, SEC_DEBUG_INFO, SEC_DEBUG_LINE, NSECS,
    // Mach-O thread-local variables (Values & Passing #13): the templates and
    // descriptors, written after the others only by an object that has one
    SEC_THREAD_DATA = NSECS, SEC_THREAD_VARS, SEC_THREAD_BSS, NSECS_THREAD,
    // nlist_64 n_sect is 1-based: SEC_TEXT -> 1
};

enum { // section types (the low byte) and flags (mach-o/loader.h)
    S_ZEROFILL               = 0x00000001,
    S_THREAD_LOCAL_REGULAR   = 0x00000011,
    S_THREAD_LOCAL_ZEROFILL  = 0x00000012,
    S_THREAD_LOCAL_VARIABLES = 0x00000013,
    S_ATTR_SOME_INSTRUCTIONS = 0x00000400,
    S_ATTR_DEBUG             = 0x02000000,
    S_ATTR_PURE_INSTRUCTIONS = 0x80000000,
};

enum { // arm64 relocation types
    RM_UNSIGNED  = 0,
    RM_PAIR      = 1,
    RM_BRANCH26  = 2,
    RM_PAGE21    = 3,
    RM_PAGEOFF12 = 4,
    RM_GOT_LOAD_PAGE21    = 5,
    RM_GOT_LOAD_PAGEOFF12 = 6,
    RM_TLVP_LOAD_PAGE21    = 8,
    RM_TLVP_LOAD_PAGEOFF12 = 9,
};

enum { // n_type bits
    N_EXT_  = 0x01,
    N_SECT_ = 0x0e,
};

typedef struct { char* name; uint32_t strx, ord; uint8_t type, sect; uint64_t value; uint32_t hash, len; } ESym;
// v keeps emission order; slots is an open-addressing index over it by name.
typedef struct { ESym** v; size_t n, cap; ESym** slots; size_t slot_cap; } SymTab;

// Structured relocation; serialized once symbol indices are known at write.
typedef struct {
    uint32_t off;
    ESym*    sym;
    uint8_t  pcrel, len, ext, type;
} Rel;

typedef struct {
    const char* sect, *seg;
    uint32_t    flags;
    uint8_t     align; // exponent: 2^align
    Buf         data;
    Rel*        rl;
    int         nrl, crl;
    uint64_t    size;  // bss: reserved bytes; else set to data.len at layout
    uint64_t    addr;
    uint32_t    offset, reloff;
} Sec;

// Whether s reserves its bytes rather than holding them (bss).
static inline int sec_zerofill(const Sec* s) {
    uint32_t type = s->flags & 0xFFu;
    return type == S_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
}

void  reloc_add(Sec* s, uint32_t off, ESym* sym, int pcrel, int len, int ext, uint32_t type);
ESym* sym_intern(SymTab* t, const char* p, uint32_t len);
// Define at the section cursor; bss grows its size cursor instead of data.
ESym* sym_define(SymTab* t, Sec* secs, const char* p, uint32_t len, int ext, Sec* in);

// --- the object under construction ------------------------------------------------

enum { FK_SUBSP, FK_BRANCH };

typedef struct {
    uint8_t  kind;   // FK_*
    uint32_t site;   // offset of the instruction word in __text
    ESym*    target; // FK_BRANCH: call target (interned by name)
    uint32_t frame;  // FK_SUBSP: final frame size
} Fixup;

typedef struct { uint32_t addr; int line, col; const char* path; } LineRow;
typedef struct { uint32_t addr, size; const char* name; uint32_t nlen; } DebugFn;

typedef struct {
    Diag*    d;
    EmitMode mode;
    Sec      secs[NSECS_THREAD];
    SymTab   syms;
    Fixup*   fix;
    size_t   nfix, capfix;
    LineRow* lines;
    int      nlines, clines;
    DebugFn* debug_fns;
    int      ndebug_fns, cdebug_fns;
    // shared runtime routines a backend referenced, emitted once per object
    int bounds_fail_used;
    int arithmetic_fail_used;
    int contract_fail_used;
    int null_fail_used;
    int allocator_used;
    int mem_copy_used;    // __ace$mem_copy / __ace$mem_move
    int mem_fill_used;    // __ace$mem_fill
    int mem_compare_used; // __ace$mem_compare
    int str_n;            // string literals interned, for their __ace$str$N names
} Obj;

// The bytes of a string literal in the constant section, under a fresh name.
ESym* obj_string(Obj* o, const char* p, uint32_t len);
// The debug diagnostic "path:line:col: what\n" for site at, as a string
// literal of *len bytes, shared by both backends' located failures.
ESym* obj_located(Obj* o, Node* at, const char* what, uint32_t* len);

// Set up the sections and the __ace$text anchor.
void obj_init(Obj* o, Diag* d, EmitMode mode);
static inline Buf* obj_text(Obj* o) { return &o->secs[SEC_TEXT].data; }
void obj_fixup(Obj* o, uint8_t kind, uint32_t site, ESym* target);
// A line row at addr; a later row at the same address replaces it.
void obj_line(Obj* o, uint32_t addr, int line, int col, const char* path);
void obj_debug_fn(Obj* o, uint32_t addr, uint32_t size, const char* name, uint32_t nlen);
// Patch every recorded fixup once all text symbols are defined.
void obj_patch_fixups(Obj* o);
void obj_debug_info(Obj* o, ESym* text_anchor);
void obj_debug_line(Obj* o, ESym* text_anchor);
// Write the object file; returns its size, or 0 on error.
size_t obj_write(Obj* o, const char* path);
// elf.c: the same object as ELF64 x64 (linux-x64); Rel.type is R_X86_64_*.
size_t elf_write(Obj* o, const char* path);

// --- shared by the code generators -----------------------------------------------

// Function entries and loop heads start on a code boundary (nop padding;
// __text is 64-byte aligned) so identical code does not change speed with its
// offset in a fetch block. Loop-head padding runs once per loop entry.
enum { FUNC_ALIGN = 16, LOOP_ALIGN = 16 };

// The internal symbol of an Ace function (emit.c); the result lives in a
// static buffer until the next call.
const char* fn_mangle(const char* name, uint32_t len, SType* ft, Node* decl, SType** targs, int nt, int spec);
// The link symbol of conc's vtable for contract (emit.c), defined once per
// pair: by module-level signing emission, or, for a generic signing, by
// obj_vtable wherever a function constructs the pair.
const char* vtable_name(SType* conc, SType* contract);
// A fresh link symbol for a constant []T array literal's static data (emit.c).
const char* literal_name(void);
ESym*       obj_vtable(Obj* o, SType* conc, SType* contract, Node* conf);
// The internal symbol a direct call to Ace function s targets, a stamped
// generic's or a specialization's included (same buffer as fn_mangle).
const char* fn_call_mangle(Sym* s, Node* call);
// The internal symbol of generic function x (larger[int]) as a value, or NULL
// when no stamp matches its type.
const char* fn_value_mangle(Sym* s, Node* x);
// The N:immr:imms field (bits 22..10) of an AArch64 bitmask immediate, or 0
// when v does not encode (0 and all-ones never do). The values 1 and
// 0x100000001 encode as field 0 too, so a caller that tests for 0 treats them
// as not encoding.
uint32_t logical_imm_field(uint64_t v, int sf);
// imm8 of `fmov Vd, #imm` for an f32 (is64 = 0) or f64 bit pattern, or -1
int fmov_imm8(uint64_t bits, int is64);
// Division by a constant d >= 3, not a power of two, as a multiply-high by
// magic and shifts (Granlund-Montgomery); w is 32 or 64.
typedef struct { uint64_t magic; int shift, add, neg; } DivMagic;
DivMagic div_magic(uint64_t d, int w, int sign, int neg);

// Byte offset of struct t's field want, and the index of field name or -1 (emit.c).
uint32_t field_off(SType* t, int want);
int      find_field(SType* t, const char* name, uint32_t len);

// arm64.c: compile a lowered function into o's __text as symbol `name`,
// global (visible to the linker) for main and a C export.
void arm64_func(Obj* o, IrFunc* fn, const char* name, uint32_t len, int global);
// x64.c: the same for linux-x64, and its runtime failure routines.
void x64_func(Obj* o, IrFunc* fn, const char* name, uint32_t len, int global);
void x64_runtime(Obj* o);
// Whether System V returns a value of type t through memory, the caller's
// address taking rdi ahead of the arguments: an aggregate over 16 bytes.
int  x64_result_in_memory(SType* t);

#endif
