// emit.c — stage four: direct AArch64 codegen → Mach-O arm64 object (tcc-style:
// single-pass walk of the sema-checked AST, no optimization).
//
// Model: every local/param lives in an x29-relative stack slot; expressions
// evaluate into a virtual-register stack (int temps = x0-x7,x9-x15, fp temps =
// v0-v7,v16-v23) that is spilled to a per-function spill area around calls.
// x16/x17 are scratch OUTSIDE the temp pool (never live across a call), used
// by copy/addressing helpers. Frame: [x29,x30][locals][spill][out-args][sp];
// the `sub sp` placeholder is patched with the final frame size at fn end.
//
// ABI (pinned against clang on this machine):
//   - platform convention (Apple arm64 PCS): scalar int/ptr args in x0-x7,
//     float args in v0-v7; <=16B aggregates as 1-2 register chunks (per
//     8-byte word: FLOAT iff every member overlapping the word is a float
//     entirely inside it, else INT); >16B via sret x8.
//   - Darwin quirk: STACK ARGS ARE PACKED AT NATURAL SIZE (two i32 stack
//     args share 8 bytes) — verified against clang caller+callee dumps. The
//     outgoing area sits at [sp, #0..] at the bl; the callee reads incoming
//     stack args at [x29 + 24 + off].
//   - Apple arm64 variadic extras use 8-byte stack slots from [sp, #0], while
//     the fixed parameters retain ordinary register/stack classification.
//   - internal-only freedom (spec Functions & ABI #3): `mut` params and >16B
//     read-only params pass the caller's ADDRESS in an x reg.
//   - indirect calls jump through blr x16 (target reloaded from its spill
//     slot so arg marshaling cannot clobber it); extern calls are `bl` +
//     ARM64_RELOC_BRANCH26 to an undefined `_name` symbol.
//   - symbols: `_main` / `_export` at the C boundary; internals mangled
//     `__ace$fn$name[$T...]`, globals `__ace$g$name`, vtables `__ace$vt$name`,
//     string bytes `__ace$str$N` (mangling is compiler-private, C Interop #1).
//   - multi-return = anonymous struct (tuple) per platform rules: <=16B in
//     chunk registers, >16B sret.
//
// Object format facts (pinned against clang reference objects, otool dumps):
//   - one LC_SEGMENT_64 with an EMPTY segname holds all sections; each
//     section carries its real (segname, sectname) pair (__TEXT/__DATA/__DWARF)
//   - segment vmaddr=0, fileoff = first section's file offset; section addrs
//     are continuous from 0; maxprot=initprot=7 (RWX in objects — the linker
//     decides the final image)
//   - symbol order in LC_SYMTAB: locals (n_type 0x0e = N_SECT), then extdefs
//     (0x0f = N_SECT|N_EXT), then undefs (0x01 = N_EXT); strtab starts with
//     the empty string (index 0)
//   - MH flags = 0 — no SUBSECTIONS_VIA_SYMBOLS: internal bl's are patched
//     in-place, never relocated (spec.md Compiler #5)
//   - one LC_BUILD_VERSION (platform 1 = macOS, minos 11.0), LC_SYMTAB,
//     LC_DYSYMTAB
//
// Relocations (r_word1 = symbolnum(24)|pcrel<<24|length<<25|extern<<27|type<<28;
// BRANCH26=2, PAGE21=3, PAGEOFF12=4 (length 2), UNSIGNED=0 (length 3)):
//   - adrp/add address pairs against local symbols (PAGE21 pcrel=1 / PAGEOFF12
//     pcrel=0), 8-byte absolute words via UNSIGNED (vtable slots, string
//     pointers in data), extern branches via BRANCH26 (extern=1). Entries are
//     kept structured and serialized once symbol indices are known at write.
//
// Instruction encodings verified against `as` + objdump (scratch /tmp/ace_ref):
//   stp x29,x30,[sp,#-16]! = 0xA9BF7BFD   mov x29,sp = 0x910003FD
//   sub sp,sp,#imm12       = 0xD10003FF|imm<<10 (placeholder, patched at end)
//   sub sp,sp,#imm12,lsl#12 = previous encoding | 1<<22 (large frames round up)
//   mov sp,x29 = 0x910003BF   ldp x29,x30,[sp],#16 = 0xA8C17BFD
//   ret = 0xD65F03C0    bl = 0x94000000|imm26    b = 0x14000000|imm26
//   b.cond = 0x54000000|imm19<<5|cond (eq0 ne1 hs2 lo3 hi8 ls9 ge10 lt11
//   gt12 le13)   cbz/cbnz = (0x34/0x35)|sf<<31|imm19<<5|rt
//   cset wd,cond = 0x1A9F07E0|(cond^1)<<12|rd  (x: 0x9A9F07E0)
//   add/sub rr = (0x8B/0xCB)|sf<<31...|rm<<16|rn<<5|rd   neg = 0xCB0003E0-form
//   add rr,lsl#n = ADDrr|n<<10   add/sub imm = (0x91/0xD1)|imm<<10|rn<<5|rd
//   cmp rr = 0xEB00001F|rm<<16|rn<<5 (32: 0x6B...)  cmp imm = 0xF100001F-form
//   and/orr/eor rr = 0x8A/0xAA/0xCA (32: 0x0A/0x2A/0x4A)  mvn = 0xAA2003E0-form
//   mul = 0x9B007C00 (32: 0x1B...)  sdiv/udiv = 0x9AC00C00/0x9AC00800 (32 0x1A..)
//   msub = 0x9B008000|rm<<16|ra<<10|rn<<5|rd
//   lslv/lsrv/asrv = 0x9AC02000/2400/2800 (32 0x1AC0...)
//   sxtb w/x = 0x13001C00/0x93401C00  sxth = ..03C00  sxtw x = 0x93407C00
//   uxtb w/x = 0x53001C00/0x53401C00  uxth = ..03C00  uxtw x = 0xD3407C00
//   mov w/x reg = 0x2A/0xAA0003E0|rm<<16|rd
//   ldr/str uoff: x=F940/9000 w=B94/B90 h=794/790 b=394/390 |imm<<10|rn<<5|rt
//   ldur/stur x = 0xF840/0xF8000000 w = 0xB84/B80 |imm9<<12|rn<<5|rt
//   ldrsh w/x = 79C00000/79800000  ldrsb = 39C00000/39800000
//   ldr/str reg-off = 0xF8606800/0xF8206800|rm<<16|rn<<5|rt
//   ldp/stp x = 0xA9400000/0xA9000000|imm7<<15|rt2<<10|rn<<5|rt
//   ldr/str d = 0xFD400000/0xFD000000 s = 0xBD4/0xBD0
//   fmov s/d reg = 0x1E204000/0x1E604000  fmov d,x = 0x9E670000  s,w = 0x1E270000
//   fmov x,d = 0x9E660000  w,s = 0x1E260000
//   fadd/fsub/fmul/fdiv s = 0x1E2028/38/08/18000 d = 0x1E60..  fneg =
//   0x1E214000/0x1E614000  fcmp s/d = 0x1E202000/0x1E612000
//   scvtf = 1E22/9E22/1E62/9E620000 (+0x10000 = ucvtf)
//   fcvtzs = 1E38/9E38/1E78/9E780000 (+0x10000 = fcvtzu)
//   fcvt s<-d = 0x1E624000  d<-s = 0x1E22C000
//   adrp = 0x90000000|rd (imm 0 + PAGE21 reloc)
//   svc = 0xD4000001|imm16<<5  nop = 0xD503201F  blr = 0xD63F0000|rn<<5
//   movz w/x = 0x528/0xD2800000  movn = 0x128/0x9280  movk = 0x728/0xF28..
#include "ace.h"
#include "sema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int emit_has_main = 0;

// --- growable byte buffer ---------------------------------------------------------

static void oom(void) {
    fprintf(stderr, "ace: out of memory\n");
    exit(1);
}

typedef struct {
    uint8_t* d;
    size_t   len, cap;
} Buf;

static void buf_reserve(Buf* b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + extra) cap *= 2;
    b->d = realloc(b->d, cap);
    if (!b->d) oom();
    b->cap = cap;
}

static void buf_put8(Buf* b, uint8_t v) {
    buf_reserve(b, 1);
    b->d[b->len++] = v;
}

static void buf_put16(Buf* b, uint16_t v) {
    buf_put8(b, (uint8_t)v);
    buf_put8(b, (uint8_t)(v >> 8));
}

static void buf_put32(Buf* b, uint32_t v) {
    for (int i = 0; i < 4; i++) buf_put8(b, (uint8_t)(v >> (8 * i)));
}

static void buf_put64(Buf* b, uint64_t v) {
    for (int i = 0; i < 8; i++) buf_put8(b, (uint8_t)(v >> (8 * i)));
}

static void buf_uleb(Buf* b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) byte |= 0x80;
        buf_put8(b, byte);
    } while (v);
}

static void buf_sleb(Buf* b, int64_t v) {
    int more;
    do {
        uint8_t byte = (uint8_t)(v & 0x7f);
        int sign = byte & 0x40;
        v >>= 7;
        more = !((v == 0 && !sign) || (v == -1 && sign));
        if (more) byte |= 0x80;
        buf_put8(b, byte);
    } while (more);
}

static void buf_write(Buf* b, const void* p, size_t n) {
    buf_reserve(b, n);
    memcpy(b->d + b->len, p, n);
    b->len += n;
}

static void buf_patch32(Buf* b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) b->d[off + (size_t)i] = (uint8_t)(v >> (8 * i));
}

static void buf_putname(Buf* b, const char* name) { // 16-byte padded field
    size_t n = strlen(name);
    if (n > 16) n = 16;
    buf_write(b, name, n);
    for (size_t i = n; i < 16; i++) buf_put8(b, 0);
}

// --- sections & symbols ------------------------------------------------------------

enum {
    SEC_TEXT, SEC_CONST, SEC_DATA, SEC_BSS,
    SEC_DEBUG_ABBREV, SEC_DEBUG_INFO, SEC_DEBUG_LINE, NSECS,
    // nlist_64 n_sect is 1-based: SEC_TEXT -> 1
};

enum { // section flags (mach-o/loader.h)
    S_ZEROFILL               = 0x00000001,
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
};

enum { // n_type bits
    N_EXT_  = 0x01,
    N_SECT_ = 0x0e,
};

typedef struct { char* name; uint32_t strx, ord; uint8_t type, sect; uint64_t value; } ESym;
typedef struct { ESym** v; size_t n, cap; } SymTab;

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

static void reloc_add(Sec* s, uint32_t off, ESym* sym, int pcrel, int len,
                      int ext, uint32_t type) {
    if (s->nrl == s->crl) {
        s->crl = s->crl ? s->crl * 2 : 8;
        s->rl = realloc(s->rl, (size_t)s->crl * sizeof(Rel));
        if (!s->rl) oom();
    }
    s->rl[s->nrl++] = (Rel){ off, sym, (uint8_t)!!pcrel, (uint8_t)len,
                             (uint8_t)!!ext, (uint8_t)type };
}

static ESym* sym_intern(SymTab* t, const char* p, uint32_t len) {
    for (size_t i = 0; i < t->n; i++)
        if (strlen(t->v[i]->name) == len && memcmp(t->v[i]->name, p, len) == 0)
            return t->v[i];
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->v = realloc(t->v, t->cap * sizeof(ESym*));
        if (!t->v) oom();
    }
    ESym* s = malloc(sizeof(ESym));
    if (!s) oom();
    t->v[t->n++] = s;
    s->name = malloc((size_t)len + 1);
    if (!s->name) oom();
    memcpy(s->name, p, len);
    s->name[len] = 0;
    s->strx = s->ord = 0;
    s->type = 0;
    s->sect = 0;
    s->value = 0;
    return s;
}

// Define at the section cursor; bss grows its size cursor instead of data.
static ESym* sym_define(SymTab* t, Sec* secs, const char* p, uint32_t len,
                        int ext, Sec* in) {
    ESym* s = sym_intern(t, p, len);
    if (s->type) return s; // redefinition is a sema error; keep the first
    s->type = N_SECT_ | (ext ? N_EXT_ : 0);
    s->sect = (uint8_t)(1 + (int)(in - secs));
    s->value = (in->flags & S_ZEROFILL) ? in->size : in->data.len;
    return s;
}

// --- emitter state ----------------------------------------------------------------

enum { FK_SUBSP, FK_BRANCH };

typedef struct {
    uint8_t  kind;   // FK_*
    uint32_t site;   // offset of the instruction word in __text
    ESym*    target; // FK_BRANCH: call target (interned by name)
    uint32_t frame;  // FK_SUBSP: final frame size
} Fixup;

typedef struct { // per-function stack slot (keyed by binder/loop node)
    Node*    key;
    uint32_t off, size;
    uint8_t  byref; // slot holds a pointer to the value (mut/big params)
} Slot;

typedef struct { Sym* s; const char* name; uint32_t len; } GVar; // global storage
typedef struct { uint32_t addr; int line, col; const char* path; } LineRow;
typedef struct { uint32_t addr, size; const char* name; uint32_t nlen; } DebugFn;

enum { CLS_INT, CLS_FP, CLS_ADDR };
typedef struct { uint8_t cls; int idx; } EV; // int temp / fp temp / address in int temp

typedef struct { // loop / labeled-block context stack
    int         brk, cont; // labels; cont = -1 for non-loops
    const char* label;
    uint32_t    llen;
    int         is_loop;
} Ctx;

enum { TU_XREG, TU_VREG, TU_STACK, TU_STACK_AGGR };
typedef struct {
    uint8_t kind;    // TU_*; reg < 0 marks an unassigned (absent) unit
    int8_t  reg;     // x/v register number
    uint32_t soff;   // stack offset (TU_STACK*)
    uint32_t size;   // bytes to move
    uint32_t off;    // byte offset within the value (register units)
} TU;

typedef struct {
    Diag* d;
    Sec   secs[NSECS];
    SymTab syms;
    Fixup* fix;
    size_t nfix, capfix;
    GVar*  globs;
    int    nglobs, cglobs;
    int    str_n;
    // current function
    size_t subsp_fix;
    uint32_t frame;
    uint32_t spill_base;
    int     spill_on;
    uint32_t out_max;
    Slot*   slots;
    int     nslots, cslots;
    int     vti, vtf;
    uint32_t* lab;
    int      nlab, clab;
    struct { uint32_t site; int lab; uint8_t kind; } *bp; // 0=b 1=bcond 2=cbz/cbnz
    int      nbp, cbp;
    Ctx      ctx[64];
    int      nctx;
    int      ended_return;
    SType*   fnret;
    uint32_t sret_slot;
    Node*    fnbody;   // current function's top-level body block (defer check)
    struct { Node* call; Node* at; } *defers; // captured defers, source order
    int      ndefers, cdefers;
    uint32_t retsave_off; // result-register save area across deferred calls
    LineRow* lines;
    int      nlines, clines;
    DebugFn* debug_fns;
    int      ndebug_fns, cdebug_fns;
} Em;

#define MAX_ITEMPS 14  // x0-x7, x9-x15
#define MAX_FPTEMPS 16 // v0-v7, v16-v23
#define SPILL_SIZE (MAX_ITEMPS * 8 + MAX_FPTEMPS * 8)

static void em_err(Em* e, Node* n, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ",
            n && n->path ? n->path : e->d->path,
            n ? n->line : 0, n ? n->col : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    e->d->errors++;
}

static void fix_add(Em* e, uint8_t kind, uint32_t site, ESym* target) {
    if (e->nfix == e->capfix) {
        e->capfix = e->capfix ? e->capfix * 2 : 16;
        e->fix = realloc(e->fix, e->capfix * sizeof(Fixup));
        if (!e->fix) oom();
    }
    e->fix[e->nfix++] = (Fixup){ kind, site, target, 0 };
}

static Buf* text(Em* e) { return &e->secs[SEC_TEXT].data; }
static void ins(Em* e, uint32_t w) { buf_put32(text(e), w); }
static int xreg(int t) { return t < 8 ? t : t + 1; }
static int vreg(int t) { return t < 8 ? t : t + 8; }

static const char* arena_copy(const char* s, size_t n) {
    char* p = arena_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void line_mark(Em* e, Node* n) {
    if (!n || n->line <= 0) return;
    uint32_t addr = (uint32_t)text(e)->len;
    const char* path = n->path ? n->path : e->d->path;
    if (e->nlines && e->lines[e->nlines - 1].addr == addr) {
        e->lines[e->nlines - 1] = (LineRow){ addr, n->line, n->col, path };
        return;
    }
    if (e->nlines == e->clines) {
        e->clines = e->clines ? e->clines * 2 : 64;
        e->lines = realloc(e->lines, (size_t)e->clines * sizeof(LineRow));
        if (!e->lines) oom();
    }
    e->lines[e->nlines++] = (LineRow){ addr, n->line, n->col, path };
}

static void debug_fn_add(Em* e, Node* f, uint32_t addr, uint32_t size) {
    if (e->ndebug_fns == e->cdebug_fns) {
        e->cdebug_fns = e->cdebug_fns ? e->cdebug_fns * 2 : 16;
        e->debug_fns = realloc(e->debug_fns,
                               (size_t)e->cdebug_fns * sizeof(DebugFn));
        if (!e->debug_fns) oom();
    }
    e->debug_fns[e->ndebug_fns++] =
        (DebugFn){ addr, size, f->str, f->len };
}

static EV gen_expr(Em* e, Node* x);
static void emit_stmt(Em* e, Node* s);
static int gen_addr(Em* e, Node* x);
static int is_lvalue_node(Node* x);
static SType* lvalue_type(Em* e, Node* x);
static void emit_asm(Em* e, Node* s);
static void emit_epilogue(Em* e);
static ESym* emit_vtable(Em* e, SType* conc, SType* contract, Node* conf);

// --- type helpers -----------------------------------------------------------------

static SType* unwrap(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    return t;
}

static int is_byte_sequence_type(SType* t) {
    if (!t) return 0;
    if (t->kind == SK_PRIM) return t->prim == P_STRING;
    return (t->kind == SK_ARRAY || t->kind == SK_SLICE) && t->base &&
           t->base->kind == SK_PRIM && t->base->prim == P_U8;
}

static uint32_t ty_size(SType* t) { return sema_size(t); }
static uint32_t ty_align(SType* t) { return sema_align(t); }

static int is_floatt(SType* t) {
    t = unwrap(t);
    return t && t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64);
}

static int is_boolt(SType* t) {
    t = unwrap(t);
    return t && t->kind == SK_PRIM && t->prim == P_BOOL;
}

static int is_intt(SType* t) {
    t = unwrap(t);
    if (!t) return 0;
    return (t->kind == SK_PRIM && t->prim != P_BOOL && t->prim != P_F32 &&
            t->prim != P_F64 && t->prim != P_STRING) ||
           t->kind == SK_PTR || t->kind == SK_FUNC || t->kind == SK_ENUM ||
           t->kind == SK_ERROR;
}

static int ty_is_signed(SType* t) {
    t = unwrap(t);
    if (!t) return 1;
    if (t->kind == SK_ENUM) return 1;
    if (t->kind != SK_PRIM) return 0;
    switch (t->prim) {
    case P_I8: case P_I16: case P_I32: case P_I64: return 1;
    default: return 0;
    }
}

// single-register value (<= 8 bytes)
static int is_scalar(SType* t) {
    t = unwrap(t);
    if (!t) return 0;
    switch (t->kind) {
    case SK_PRIM: return t->prim != P_STRING;
    case SK_PTR: case SK_FUNC: case SK_ENUM: case SK_ERROR: return 1;
    default: return 0;
    }
}

static int scalar_bits(SType* t) {
    t = unwrap(t);
    if (!t || t->kind == SK_UT) return 64;
    if (t->kind == SK_ENUM) return 32;
    if (t->kind == SK_PTR || t->kind == SK_FUNC || t->kind == SK_ERROR) return 64;
    if (t->kind == SK_PRIM && t->prim != P_STRING) return (int)(t->size * 8);
    return 64;
}

static uint32_t field_off(SType* t, int want) {
    uint32_t off = 0;
    for (int i = 0; i <= want && i < t->nfields; i++) {
        uint32_t a = ty_align(t->fields[i].t);
        off = (off + a - 1) / a * a;
        if (i < want) off += ty_size(t->fields[i].t);
    }
    return off;
}

static int find_field(SType* t, const char* name, uint32_t len) {
    for (int i = 0; i < t->nfields; i++)
        if ((uint32_t)strlen(t->fields[i].name) == len &&
            memcmp(t->fields[i].name, name, len) == 0)
            return i;
    return -1;
}

static uint32_t align_up(uint32_t v, uint32_t a);

/*
 * AAPCS64 aggregate classification: flatten an aggregate into leaf members.
 * A homogeneous floating aggregate (HFA) is 1..4 leaves, every leaf f32/f64;
 * it rides FP registers one-per-leaf (up to v0..v3, even past 16 bytes).
 * Any other composite is INTEGER-class chunks: every 8-byte word goes to an
 * x register, float bits included. Returns the leaf count (offs/dbl filled),
 * or -1 when any leaf is not a float. `max` caps the flattening (callers
 * pass 5: five or more leaves is simply not an HFA).
 */
static int hfa_leaves(SType* t, uint32_t off, uint32_t* offs, uint8_t* dbl,
                      int n, int max) {
    t = unwrap(t);
    if (!t) return -1;
    if (t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64)) {
        if (n >= max) return -1;
        offs[n] = off;
        dbl[n] = t->prim == P_F64;
        return n + 1;
    }
    switch (t->kind) {
    case SK_ARRAY: {
        uint32_t es = ty_size(t->base);
        for (uint64_t i = 0; i < t->count; i++) {
            n = hfa_leaves(t->base, off + (uint32_t)i * es, offs, dbl, n, max);
            if (n < 0) return -1;
        }
        return n;
    }
    case SK_STRUCT: case SK_TUPLE: {
        uint32_t o = 0;
        for (int i = 0; i < t->nfields; i++) {
            uint32_t a = ty_align(t->fields[i].t);
            o = align_up(o, a);
            n = hfa_leaves(t->fields[i].t, off + o, offs, dbl, n, max);
            if (n < 0) return -1;
            o += ty_size(t->fields[i].t);
        }
        return n;
    }
    default:
        return -1; // ptr/slice/contract/enum/Error member breaks homogeneity
    }
}

static int classify_hfa(SType* t, uint32_t* offs, uint8_t* dbl) {
    int n = hfa_leaves(t, 0, offs, dbl, 0, 5);
    if (n < 1 || n > 4) return -1;
    for (int i = 1; i < n; i++)
        if (dbl[i] != dbl[0]) return -1; // homogeneous means one FP base type
    return n;
}

// --- constant loaders ----------------------------------------------------------------

static void load_imm(Em* e, int sf, int reg, uint64_t v) {
    if (!sf) v = (uint32_t)v;
    uint32_t movz = sf ? 0xD2800000u : 0x52800000u;
    uint64_t inv = v ^ (sf ? ~(uint64_t)0 : 0xFFFFFFFFu);
    if ((v >> 16) == 0) {
        ins(e, movz | ((uint32_t)v << 5) | (uint32_t)reg);
        return;
    }
    if ((inv >> 16) == 0) { // movn: register = ~imm16
        ins(e, (movz & ~0x40000000u) | ((uint32_t)inv << 5) | (uint32_t)reg);
        return;
    }
    ins(e, movz | ((uint32_t)(v & 0xFFFF) << 5) | (uint32_t)reg);
    for (int hw = 1; hw < (sf ? 4 : 2); hw++) {
        uint32_t chunk = (uint32_t)((v >> (16 * hw)) & 0xFFFF);
        if (!chunk) continue;
        uint32_t movk = sf ? 0xF2800000u : 0x72800000u;
        ins(e, movk | ((uint32_t)hw << 21) | (chunk << 5) | (uint32_t)reg);
    }
}

// integer constant value of a sema-checked node, if it is one
static int const_int(Node* v, uint64_t* out) {
    Node* n = v;
    int neg = 0;
    if (n->kind == N_UNARY && n->op == T_MINUS) {
        neg = 1;
        n = n->a;
    }
    if (n->kind == N_CALL && !n->sym && n->nkids == 1 &&
        n->a && n->a->kind == N_IDENT && node_ty(n->a)) {
        uint64_t inner;
        if (!const_int(n->kids[0], &inner)) return 0;
        *out = neg ? (uint64_t)0 - inner : inner;
        return 1;
    }
    if (n->kind == N_IDENT) {
        Sym* s = node_sym(n);
        if (s && s->is_global && s->is_const && s->init) {
            uint64_t inner;
            if (!const_int(s->init, &inner)) return 0;
            *out = neg ? (uint64_t)0 - inner : inner;
            return 1;
        }
    }
    if (n->kind != N_INT && n->kind != N_BOOL && n->kind != N_CHAR) return 0;
    SType* t = unwrap(node_ty(v));
    if (!t) return 0;
    if (t->kind == SK_UT) {
        *out = neg ? (uint64_t)0 - n->ival : n->ival;
        return 1;
    }
    if (t->kind != SK_PRIM) return 0;
    switch (t->prim) {
    case P_BOOL: case P_I8: case P_I16: case P_I32: case P_I64:
    case P_U8: case P_U16: case P_U32: case P_U64: case P_USIZE:
        break;
    default:
        return 0;
    }
    *out = neg ? (uint64_t)0 - n->ival : n->ival;
    return 1;
}

static void load_f64(Em* e, int is64, int vr, double d) {
    uint64_t bits = 0;
    if (is64) {
        memcpy(&bits, &d, 8);
    } else {
        float f = (float)d;
        uint32_t b = 0;
        memcpy(&b, &f, 4);
        bits = b;
    }
    load_imm(e, 1, 16, bits);
    ins(e, (is64 ? 0x9E670000u : 0x1E270000u) | (16u << 5) | (uint32_t)vr);
}

// --- labels & branches ---------------------------------------------------------------

enum {
    C_EQ = 0, C_NE = 1, C_HS = 2, C_LO = 3, C_HI = 8, C_LS = 9,
    C_GE = 10, C_LT = 11, C_GT = 12, C_LE = 13,
};

static int new_label(Em* e) {
    if (e->nlab == e->clab) {
        e->clab = e->clab ? e->clab * 2 : 32;
        e->lab = realloc(e->lab, (size_t)e->clab * sizeof(uint32_t));
        if (!e->lab) oom();
    }
    e->lab[e->nlab] = 0xFFFFFFFFu;
    return e->nlab++;
}

static void bind_label(Em* e, int l) {
    e->lab[l] = (uint32_t)text(e)->len;
    // Every bound label is a join point with an inbound branch, so linear
    // control reaches here again and the fall-off epilogue must come back.
    e->ended_return = 0;
}

static void bp_add(Em* e, int kind, int lab) {
    if (e->nbp == e->cbp) {
        e->cbp = e->cbp ? e->cbp * 2 : 32;
        e->bp = realloc(e->bp, (size_t)e->cbp * sizeof(*e->bp));
        if (!e->bp) oom();
    }
    e->bp[e->nbp].site = (uint32_t)text(e)->len - 4;
    e->bp[e->nbp].lab = lab;
    e->bp[e->nbp].kind = (uint8_t)kind;
    e->nbp++;
}

static void br_(Em* e, int lab) {
    ins(e, 0x14000000u);
    bp_add(e, 0, lab);
}

static void bcond_(Em* e, int cond, int lab) {
    ins(e, 0x54000000u | ((uint32_t)cond & 15u));
    bp_add(e, 1, lab);
}

static void cbz_(Em* e, int sf, int rt, int nz, int lab) {
    ins(e, ((nz ? 0x35000000u : 0x34000000u) | ((uint32_t)!!sf << 31)) |
           ((uint32_t)rt & 31u));
    bp_add(e, 2, lab);
}

static void cset_(Em* e, int sf, int cond, int rd) {
    ins(e, (sf ? 0x9A9F07E0u : 0x1A9F07E0u) | (((uint32_t)cond ^ 1u) << 12) |
           (uint32_t)rd);
}

static void patch_branches(Em* e) {
    Buf* t = text(e);
    for (int i = 0; i < e->nbp; i++) {
        uint32_t site = e->bp[i].site;
        uint32_t target = e->lab[e->bp[i].lab];
        if (target == 0xFFFFFFFFu) continue;
        int64_t delta = (int64_t)target - (int64_t)site;
        uint32_t cur;
        memcpy(&cur, t->d + site, 4);
        if (e->bp[i].kind == 0)
            cur |= (uint32_t)(delta >> 2) & 0x03FFFFFFu;
        else
            cur |= ((uint32_t)(delta >> 2) & 0x7FFFFu) << 5;
        buf_patch32(t, site, cur);
    }
    e->nbp = 0;
}

// --- frame, slots, temps --------------------------------------------------------------

static uint32_t align_up(uint32_t v, uint32_t a) {
    return a ? (v + a - 1) / a * a : v;
}

static Slot* slot_find(Em* e, Node* key) {
    for (int i = 0; i < e->nslots; i++)
        if (e->slots[i].key == key) return &e->slots[i];
    return NULL;
}

static uint32_t slot_alloc(Em* e, Node* key, uint32_t size, uint32_t align) {
    if (align > 8) align = 8;
    size = size ? size : 1;
    e->frame = align_up(e->frame + size, align);
    uint32_t off = e->frame;
    if (e->nslots == e->cslots) {
        e->cslots = e->cslots ? e->cslots * 2 : 16;
        e->slots = realloc(e->slots, (size_t)e->cslots * sizeof(Slot));
        if (!e->slots) oom();
    }
    e->slots[e->nslots++] = (Slot){ key, off, size, 0 };
    return off;
}

static uint32_t temp_slot(Em* e, uint32_t size, uint32_t align) {
    return slot_alloc(e, NULL, size, align);
}

// rd = x29 - off; slot offsets are aligned depths below the frame record.
static void frame_addr(Em* e, int rd, uint32_t off) {
    uint32_t d = off;
    if (d <= 4095)
        ins(e, 0xD1000000u | (d << 10) | (29u << 5) | ((uint32_t)rd & 31u));
    else {
        load_imm(e, 1, rd, d);
        ins(e, 0xCB000000u | ((uint32_t)rd << 16) | (29u << 5) |
               ((uint32_t)rd & 31u));
    }
}

static int tmp_int(Em* e) {
    if (e->vti >= MAX_ITEMPS) {
        e->d->errors++;
        fprintf(stderr, "ace: expression too deeply nested (int temps)\n");
        return 0;
    }
    return e->vti++;
}

static int tmp_fp(Em* e) {
    if (e->vtf >= MAX_FPTEMPS) {
        e->d->errors++;
        fprintf(stderr, "ace: expression too deeply nested (fp temps)\n");
        return 0;
    }
    return e->vtf++;
}

static uint32_t spill_slot_int(Em* e, int i) {
    return e->spill_base + (uint32_t)i * 8;
}
static uint32_t spill_slot_fp(Em* e, int j) {
    return e->spill_base + MAX_ITEMPS * 8 + (uint32_t)j * 8;
}

// --- frame loads/stores (x29-relative) ---------------------------------------------

static uint32_t stur_base(uint32_t w) {
    return w == 1 ? 0x38000000u : w == 2 ? 0x78000000u :
           w == 4 ? 0xB8000000u : 0xF8000000u;
}

static uint32_t ldur_base(uint32_t w) {
    return w == 1 ? 0x38400000u : w == 2 ? 0x78400000u :
           w == 4 ? 0xB8400000u : 0xF8400000u;
}

static void store_frame(Em* e, int rt, uint32_t off, uint32_t w) {
    uint32_t d = off;
    if (d <= 256) { // stur rt, [x29, #-d]
        ins(e, stur_base(w) | ((((uint32_t)(0u - d)) & 0x1FFu) << 12) |
               (29u << 5) | ((uint32_t)rt & 31u));
        return;
    }
    int base = rt == 16 ? 17 : 16;
    frame_addr(e, base, off);
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u :
                 w == 4 ? 0xB9000000u : 0xF9000000u;
    ins(e, b | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
}

static void load_frame(Em* e, int rt, uint32_t off, uint32_t w, int signext) {
    uint32_t d = off;
    if (d <= 256 && !signext) { // ldur rt, [x29, #-d]
        ins(e, ldur_base(w) | ((((uint32_t)(0u - d)) & 0x1FFu) << 12) |
               (29u << 5) | ((uint32_t)rt & 31u));
        return;
    }
    // The destination can also hold the computed address: `ldr rt, [rt]`.
    // This avoids clobbering the other reserved scratch register when callers
    // deliberately load a pair into x16/x17 from deep frame slots.
    int base = rt;
    frame_addr(e, base, off);
    uint32_t b;
    if (!signext)
        b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u :
            w == 4 ? 0xB9400000u : 0xF9400000u;
    else
        b = w == 1 ? 0x39C00000u : w == 2 ? 0x79C00000u :
            w == 4 ? 0xB9400000u : 0xF8400000u;
    ins(e, b | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
}

static void store_frame_fp(Em* e, int vr, uint32_t off, int is64) {
    frame_addr(e, 16, off);
    ins(e, (is64 ? 0xFD000000u : 0xBD000000u) | (16u << 5) | (uint32_t)vr);
}

static void load_frame_fp(Em* e, int vr, uint32_t off, int is64) {
    frame_addr(e, 16, off);
    ins(e, (is64 ? 0xFD400000u : 0xBD400000u) | (16u << 5) | (uint32_t)vr);
}

static void spill_temps(Em* e) {
    if (!e->spill_on) {
        e->spill_base = align_up(e->frame + 8, 8);
        e->frame = e->spill_base + SPILL_SIZE - 8;
        e->spill_on = 1;
    }
    for (int i = 0; i < e->vti; i++)
        store_frame(e, xreg(i), spill_slot_int(e, i), 8);
    for (int j = 0; j < e->vtf; j++)
        store_frame_fp(e, vreg(j), spill_slot_fp(e, j), 1);
}

static void reload_temps(Em* e, int ni, int nf) {
    for (int i = 0; i < ni; i++)
        load_frame(e, xreg(i), spill_slot_int(e, i), 8, 0);
    for (int j = 0; j < nf; j++)
        load_frame_fp(e, vreg(j), spill_slot_fp(e, j), 1);
}

// --- memory access via address register ---------------------------------------------

static void ld_reg(Em* e, int rt, int base, uint32_t off, uint32_t w, int signext) {
    uint32_t b;
    if (!signext)
        b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u :
            w == 4 ? 0xB9400000u : 0xF9400000u;
    else
        b = w == 1 ? 0x39C00000u : w == 2 ? 0x79C00000u :
            w == 4 ? 0xB9400000u : 0xF8400000u;
    if (w == 8 || off / w < 4096)
        ins(e, b | ((off / w) << 10) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    else {
        load_imm(e, 1, 16, off);
        ins(e, 0xF8606800u | (16u << 16) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    }
}

static void st_reg(Em* e, int rt, int base, uint32_t off, uint32_t w) {
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u :
                 w == 4 ? 0xB9000000u : 0xF9000000u;
    if (w == 8 || off / w < 4096)
        ins(e, b | ((off / w) << 10) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    else {
        load_imm(e, 1, 16, off);
        ins(e, 0xF8206800u | (16u << 16) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    }
}

static void ldrf_(Em* e, int vr, int base, uint32_t off, int is64) {
    uint32_t scale = is64 ? 8u : 4u; // unsigned-offset imm12 unit
    uint32_t b = is64 ? 0xFD400000u : 0xBD400000u;
    if (off % scale == 0 && off / scale < 4096)
        ins(e, b | ((off / scale) << 10) | ((uint32_t)base << 5) | (uint32_t)vr);
    else {
        load_imm(e, 1, 16, off);
        ins(e, (is64 ? 0xFD600000u : 0xBD600000u) | (16u << 16) |
               ((uint32_t)base << 5) | (uint32_t)vr);
    }
}

static void strf_(Em* e, int vr, int base, uint32_t off, int is64) {
    uint32_t scale = is64 ? 8u : 4u;
    uint32_t b = is64 ? 0xFD000000u : 0xBD000000u;
    if (off % scale == 0 && off / scale < 4096)
        ins(e, b | ((off / scale) << 10) | ((uint32_t)base << 5) | (uint32_t)vr);
    else {
        load_imm(e, 1, 16, off);
        ins(e, (is64 ? 0xFD200000u : 0xBD200000u) | (16u << 16) |
               ((uint32_t)base << 5) | (uint32_t)vr);
    }
}

// byte copy [dst+doff, +size) <- [src+soff, +size); uses x8/x16/x17
static void copy_mem(Em* e, int dst, uint32_t doff, int src, uint32_t soff, uint32_t size) {
    if (!size) return;
    if (size <= 16) {
        /*
         * Chunk widths respect both offsets' alignment: scaled unsigned
         * offsets cannot express, say, an 8-byte word at +4 (union payloads
         * sit right behind the 4-byte tag).
         */
        uint32_t o = 0;
        while (o < size) {
            uint32_t align = 8;
            while (align > 1 && (((doff + o) | (soff + o)) & (align - 1)))
                align >>= 1;
            uint32_t chunk = size - o < align ? size - o : align;
            uint32_t w = chunk >= 8 ? 8 : chunk >= 4 ? 4 : chunk >= 2 ? 2 : 1;
            ld_reg(e, 16, src, soff + o, w, 0);
            st_reg(e, 16, dst, doff + o, w);
            o += w;
        }
        return;
    }
    int loop = new_label(e), done = new_label(e);
    load_imm(e, 1, 17, size & ~7u);
    bind_label(e, loop);
    cbz_(e, 1, 17, 0, done);
    ins(e, 0xD1002231u); // sub x17, x17, #8
    // downward offsets: [src + soff + x17] -> x16 -> [dst + doff + x17]
    if (soff) {
        load_imm(e, 1, 8, soff);
        ins(e, 0x8B000000u | (8u << 16) | ((uint32_t)src << 5) | 8u); // x8 = src + soff
        ins(e, 0x8B000000u | (17u << 16) | (8u << 5) | 8u);           // x8 += x17
        ins(e, 0xF9400000u | (8u << 5) | 16u);                        // ldr x16, [x8]
    } else {
        ins(e, 0xF8606800u | (17u << 16) | ((uint32_t)src << 5) | 16u);
    }
    if (doff) {
        load_imm(e, 1, 8, doff);
        ins(e, 0x8B000000u | (8u << 16) | ((uint32_t)dst << 5) | 8u); // x8 = dst + doff
        ins(e, 0x8B000000u | (17u << 16) | (8u << 5) | 8u);           // x8 += x17
        ins(e, 0xF9000000u | (8u << 5) | 16u);                        // str x16, [x8]
    } else {
        ins(e, 0xF8206800u | (17u << 16) | ((uint32_t)dst << 5) | 16u);
    }
    br_(e, loop);
    bind_label(e, done);
    uint32_t rem = size & 7u;
    if (rem) {
        uint32_t w = rem == 1 ? 1 : rem == 2 ? 2 : rem <= 4 ? 4 : 8;
        ld_reg(e, 16, src, soff + size - w, w, 0);
        st_reg(e, 16, dst, doff + size - w, w);
    }
}

// --- scalar moves ---------------------------------------------------------------------

static void mov_int(Em* e, int sf, int dst, int src) {
    if (dst == src) return;
    ins(e, (sf ? 0xAA0003E0u : 0x2A0003E0u) | ((uint32_t)src << 16) |
           ((uint32_t)dst & 31u));
}

static void mov_fp(Em* e, int is64, int dst, int src) {
    if (dst == src) return;
    ins(e, (is64 ? 0x1E604000u : 0x1E204000u) | ((uint32_t)vreg(src) << 16) |
           (uint32_t)vreg(dst));
}

// load a scalar of type t from [base+off] into a fresh temp
static EV load_scalar(Em* e, int base, uint32_t off, SType* t) {
    if (is_floatt(t)) {
        int vt = tmp_fp(e);
        ldrf_(e, vreg(vt), base, off, unwrap(t)->prim == P_F64);
        return (EV){ CLS_FP, vt };
    }
    int it = tmp_int(e);
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    ld_reg(e, xreg(it), base, off, w, ty_is_signed(t) && w < 8);
    return (EV){ CLS_INT, it };
}

static void store_scalar(Em* e, EV v, int base, uint32_t off, SType* t) {
    if (v.cls == CLS_FP)
        strf_(e, vreg(v.idx), base, off, unwrap(t)->prim == P_F64);
    else
        st_reg(e, xreg(v.idx), base, off, (uint32_t)((scalar_bits(t) + 7) / 8));
}

// store an expression value (scalar temp or aggregate address) into [base+off]
static void store_value(Em* e, EV v, int base, uint32_t off, SType* t) {
    if (v.cls == CLS_ADDR) {
        if (base == 16 || base == 17) {
            mov_int(e, 1, 15, base);
            base = 15;
        }
        copy_mem(e, base, off, xreg(v.idx), 0, ty_size(t));
    } else
        store_scalar(e, v, base, off, t);
}

// fold (address temp n, int result temp n+1) into a single temp at n
static EV collapse(Em* e, int addr_temp, EV v) {
    if (v.cls == CLS_INT && v.idx == addr_temp + 1) {
        mov_int(e, 1, xreg(addr_temp), xreg(v.idx));
        e->vti--;
        return (EV){ CLS_INT, addr_temp };
    }
    return v; // fp result (different pool) or already folded
}

// --- casts ---------------------------------------------------------------------------

static uint32_t sxtw_of(int bits) {
    switch (bits) {
    case 8: return 0x93401C00u;
    case 16: return 0x93403C00u;
    default: return 0x93407C00u;
    }
}

static uint32_t uxtw_of(int bits) {
    switch (bits) {
    case 8: return 0xD3401C00u;
    case 16: return 0xD3403C00u;
    default: return 0xD3407C00u;
    }
}

static EV gen_cast(Em* e, EV v, SType* from, SType* to) {
    if (!from || !to || from->kind == SK_ERR || to->kind == SK_ERR) return v;
    if (is_floatt(from) && is_floatt(to)) { // f32 <-> f64
        int a64 = unwrap(from)->prim == P_F64, b64 = unwrap(to)->prim == P_F64;
        if (a64 == b64) return v;
        ins(e, (b64 ? 0x1E22C000u : 0x1E624000u) | ((uint32_t)vreg(v.idx) << 5) |
               (uint32_t)vreg(v.idx));
        return v;
    }
    if ((is_intt(from) || is_boolt(from)) && is_floatt(to)) { // scvtf/ucvtf
        int src64 = scalar_bits(from) == 64;
        int dst64 = unwrap(to)->prim == P_F64;
        uint32_t base = src64
            ? (dst64 ? 0x9E620000u : 0x9E220000u)
            : (dst64 ? 0x1E620000u : 0x1E220000u);
        if (!ty_is_signed(from)) base |= 0x10000u;
        /*
         * The result lands in a fresh fp temp: reusing the int temp's index
         * would alias a live value in the other pool (the two pools number
         * their registers independently).
         */
        int t = tmp_fp(e);
        ins(e, base | ((uint32_t)xreg(v.idx) << 5) | (uint32_t)vreg(t));
        e->vti = v.idx; // the int temp is consumed
        return (EV){ CLS_FP, t };
    }
    if (is_floatt(from) && (is_intt(to) || is_boolt(to))) { // fcvtzs/fcvtzu
        int src64 = unwrap(from)->prim == P_F64;
        int dst64 = scalar_bits(to) == 64;
        uint32_t base = src64
            ? (dst64 ? 0x9E780000u : 0x1E780000u)
            : (dst64 ? 0x9E380000u : 0x1E380000u);
        if (!ty_is_signed(to)) base |= 0x10000u;
        int t = tmp_int(e);
        ins(e, base | ((uint32_t)vreg(v.idx) << 5) | (uint32_t)xreg(t));
        e->vtf = v.idx; // the fp temp is consumed
        return (EV){ CLS_INT, t };
    }
    // int <-> int widths, enum, distinct, ptr <-> ptr
    int fa = scalar_bits(from), ta = scalar_bits(to);
    if (ta == 64 && fa < 64) {
        uint32_t op = ty_is_signed(from) ? sxtw_of(fa) : uxtw_of(fa);
        if (op) ins(e, op | ((uint32_t)xreg(v.idx) << 5) | (uint32_t)xreg(v.idx));
    } else if (ta < 64 && fa == 64) {
        mov_int(e, 0, xreg(v.idx), xreg(v.idx));
    }
    return v;
}

// widen an index temp to 64-bit lanes per its type's signedness
static void widen_idx(Em* e, Node* idx, int r) {
    SType* it = node_ty(idx);
    if (!it || it->kind == SK_UT || it->kind == SK_ERR) return;
    if (scalar_bits(it) == 64) return;
    uint32_t op = ty_is_signed(it) ? sxtw_of(scalar_bits(it)) : uxtw_of(scalar_bits(it));
    if (op) ins(e, op | ((uint32_t)r << 5) | (uint32_t)r);
}

// --- binary operators -------------------------------------------------------------------

static int tok_cond(TokKind op, SType* t) {
    int unsig = !ty_is_signed(t);
    switch (op) {
    case T_EQEQ: return C_EQ;
    case T_BANGEQ: return C_NE;
    case T_LT: return unsig ? C_LO : C_LT;
    case T_LE: return unsig ? C_LS : C_LE;
    case T_GT: return unsig ? C_HI : C_GT;
    case T_GE: return unsig ? C_HS : C_GE;
    default: return -1;
    }
}

static void emit_arith(Em* e, TokKind op, int dst, int a, int b, SType* t) {
    int sf = scalar_bits(t) == 64;
    uint32_t s = sf ? 0x80000000u : 0;
    int ra = xreg(a), rb = xreg(b), rd = xreg(dst);
    switch (op) {
    case T_PLUS:  ins(e, s | 0x0B000000u | (rb << 16) | (ra << 5) | rd); return;
    case T_MINUS: ins(e, s | 0x4B000000u | (rb << 16) | (ra << 5) | rd); return;
    case T_STAR:  ins(e, s | 0x1B007C00u | (rb << 16) | (ra << 5) | rd); return;
    case T_AMP:   ins(e, s | 0x0A000000u | (rb << 16) | (ra << 5) | rd); return;
    case T_PIPE:  ins(e, s | 0x2A000000u | (rb << 16) | (ra << 5) | rd); return;
    case T_CARET: ins(e, s | 0x4A000000u | (rb << 16) | (ra << 5) | rd); return;
    case T_SLASH: {
        uint32_t base = ty_is_signed(t) ? (s | 0x1AC00C00u) : (s | 0x1AC00800u);
        ins(e, base | (rb << 16) | (ra << 5) | rd);
        return;
    }
    case T_PERCENT: {
        uint32_t div = ty_is_signed(t) ? (s | 0x1AC00C00u) : (s | 0x1AC00800u);
        ins(e, div | (rb << 16) | (ra << 5) | 16u); // x16 = a / b
        ins(e, s | 0x1B008000u | (rb << 16) | (ra << 10) | (16u << 5) | rd);
        return; // rd = a - x16*b
    }
    case T_SHL: ins(e, s | 0x1AC02000u | (rb << 16) | (ra << 5) | rd); return;
    case T_SHR:
        ins(e, s | (ty_is_signed(t) ? 0x1AC02800u : 0x1AC02400u) |
               (rb << 16) | (ra << 5) | rd);
        return;
    default: return;
    }
}

static TokKind compound_op(TokKind op) {
    switch (op) {
    case T_ADDEQ: return T_PLUS;
    case T_SUBEQ: return T_MINUS;
    case T_MULEQ: return T_STAR;
    case T_DIVEQ: return T_SLASH;
    case T_MODEQ: return T_PERCENT;
    case T_ANDEQ: return T_AMP;
    case T_OREQ: return T_PIPE;
    case T_XOREQ: return T_CARET;
    case T_SHLEQ: return T_SHL;
    case T_SHREQ: return T_SHR;
    default: return op;
    }
}

static void emit_farith(Em* e, TokKind op, int dst, int a, int b, int is64) {
    uint32_t s = is64 ? 0x00400000u : 0;
    uint32_t ra = vreg(a), rb = vreg(b), rd = vreg(dst);
    switch (op) {
    case T_PLUS:  ins(e, 0x1E202800u | s | (rb << 16) | (ra << 5) | rd); return;
    case T_MINUS: ins(e, 0x1E203800u | s | (rb << 16) | (ra << 5) | rd); return;
    case T_STAR:  ins(e, 0x1E200800u | s | (rb << 16) | (ra << 5) | rd); return;
    case T_SLASH: ins(e, 0x1E201800u | s | (rb << 16) | (ra << 5) | rd); return;
    default: return;
    }
}

static void emit_fcmp(Em* e, int a, int b, int is64) {
    ins(e, (is64 ? 0x1E612000u : 0x1E202000u) |
           (vreg(b) << 16) | (vreg(a) << 5));
}

// --- tagged unions -----------------------------------------------------------------

// The union type a construction/pattern base names, or NULL (sema annotated).
static SType* union_type_of_base(Node* base) {
    if (!base) return NULL;
    if (base->kind == N_IDENT) {
        Sym* ts = node_sym(base);
        return ts && ts->kind == SYM_TYPE && ts->type &&
               ts->type->kind == SK_UNION ? ts->type : NULL;
    }
    if (base->kind == N_INDEX) {
        SType* t = node_ty(base);
        return t && t->kind == SK_UNION ? t : NULL;
    }
    return NULL;
}

/*
 * Materialize a union value: i32 tag, then the payload (when the variant
 * carries one) in the aligned inline storage. Inactive bytes are untouched —
 * they are unreachable through the language, so nothing initializes them.
 */
static EV gen_union_construct(Em* e, Node* at, SType* ut, int variant, EV payload) {
    (void)at;
    SType* pt = variant < ut->nfields ? ut->fields[variant].t : NULL;
    uint32_t off = temp_slot(e, ty_size(ut), ty_align(ut));
    load_imm(e, 0, 16, (uint64_t)variant);
    frame_addr(e, 17, off);
    st_reg(e, 16, 17, 0, 4);
    if (pt) {
        int svi = e->vti, svf = e->vtf;
        store_value(e, payload, 17, sema_union_payload_off(ut), pt);
        e->vti = svi;
        e->vtf = svf;
    }
    int t = tmp_int(e);
    frame_addr(e, xreg(t), off);
    return (EV){ CLS_ADDR, t };
}

static EV gen_byte_sequence_eq(Em* e, Node* n, EV a, SType* at,
                               EV b, SType* bt, int eq);

/*
 * Compare union tags and, when they match, the active payload. Sema guarantees
 * both sides are the same union type and that every payload supports equality.
 */
static EV gen_union_eq(Em* e, Node* n, EV a, EV b, SType* ut, int eq) {
    (void)n;
    int result = tmp_int(e);
    int ldiff = new_label(e), ldone = new_label(e);
    ld_reg(e, 16, xreg(a.idx), 0, 4, 0);
    ld_reg(e, 17, xreg(b.idx), 0, 4, 0);
    ins(e, 0x6B00001Fu | (17u << 16) | (16u << 5)); // cmp w16, w17
    bcond_(e, C_NE, ldiff);
    load_imm(e, 0, xreg(result), 1); // tags equal: payloadless variants are equal
    int* variants = (int*)arena_alloc((size_t)(ut->nfields ? ut->nfields : 1) *
                                      sizeof(int));
    int* labs = (int*)arena_alloc((size_t)(ut->nfields ? ut->nfields : 1) *
                                  sizeof(int));
    int nvar = 0;
    for (int i = 0; i < ut->nfields; i++) {
        if (!ut->fields[i].t) continue;
        ins(e, 0x7100001Fu | ((uint32_t)i << 10) | (16u << 5)); // cmp w16, #i
        labs[nvar] = new_label(e);
        variants[nvar++] = i;
        bcond_(e, C_EQ, labs[nvar - 1]);
    }
    br_(e, ldone); // no payload variant matched the tag
    uint32_t poff = sema_union_payload_off(ut);
    int branch_i = e->vti, branch_f = e->vtf;
    for (int k = 0; k < nvar; k++) {
        e->vti = branch_i;
        e->vtf = branch_f;
        bind_label(e, labs[k]);
        SType* pt = ut->fields[variants[k]].t;
        uint32_t psz = ty_size(pt);
        int t2;
        if (unwrap(pt)->kind == SK_UNION) {
            int pa = tmp_int(e), pb = tmp_int(e);
            ins(e, 0x91000000u | (poff << 10) |
                   ((uint32_t)xreg(a.idx) << 5) | (uint32_t)xreg(pa));
            ins(e, 0x91000000u | (poff << 10) |
                   ((uint32_t)xreg(b.idx) << 5) | (uint32_t)xreg(pb));
            EV nested = gen_union_eq(e, n, (EV){ CLS_ADDR, pa },
                                     (EV){ CLS_ADDR, pb }, unwrap(pt), 1);
            t2 = nested.idx;
        } else if (is_byte_sequence_type(pt)) {
            int pa = tmp_int(e), pb = tmp_int(e);
            ins(e, 0x91000000u | (poff << 10) |
                   ((uint32_t)xreg(a.idx) << 5) | (uint32_t)xreg(pa));
            ins(e, 0x91000000u | (poff << 10) |
                   ((uint32_t)xreg(b.idx) << 5) | (uint32_t)xreg(pb));
            EV bytes = gen_byte_sequence_eq(e, n, (EV){ CLS_ADDR, pa }, pt,
                                            (EV){ CLS_ADDR, pb }, pt, 1);
            t2 = bytes.idx;
        } else if (is_floatt(pt)) {
            t2 = tmp_int(e);
            int is64 = unwrap(pt)->prim == P_F64;
            int v0 = tmp_fp(e), v1 = tmp_fp(e);
            ldrf_(e, vreg(v0), xreg(a.idx), poff, is64);
            ldrf_(e, vreg(v1), xreg(b.idx), poff, is64);
            emit_fcmp(e, v0, v1, is64);
            cset_(e, 0, C_EQ, xreg(t2));
        } else {
            t2 = tmp_int(e);
            load_imm(e, 0, xreg(t2), 1);
            for (uint32_t w = 0; w * 8 < psz; w++) {
                /*
                 * Tail words compare at their exact width: the bytes past a
                 * payload's size are unspecified storage, never data.
                 */
                uint32_t rem = psz - w * 8;
                uint32_t bw = rem >= 8 ? 8 : rem >= 4 ? 4 : rem;
                ld_reg(e, 16, xreg(a.idx), poff + w * 8, bw, 0);
                ld_reg(e, 17, xreg(b.idx), poff + w * 8, bw, 0);
                ins(e, (bw == 8 ? 0xEB00001Fu : 0x6B00001Fu) |
                       (17u << 16) | (16u << 5));
                int t3 = tmp_int(e);
                cset_(e, 0, C_EQ, xreg(t3));
                ins(e, 0x0A000000u | (xreg(t3) << 16) | (xreg(t2) << 5) |
                       xreg(t2)); // t2 &= word equal
            }
        }
        ins(e, 0x0A000000u | (xreg(t2) << 16) | (xreg(result) << 5) |
               xreg(result)); // result &= payload equal
        br_(e, ldone);
    }
    e->vti = branch_i;
    e->vtf = branch_f;
    bind_label(e, ldiff);
    load_imm(e, 0, xreg(result), 0); // differing tags are never equal
    bind_label(e, ldone);
    if (!eq) // the arms computed plain equality; != inverts it once here
        ins(e, 0x52000000u | (xreg(result) << 5) | xreg(result)); // eor w, w, #1
    if (result != a.idx) mov_int(e, 0, xreg(a.idx), xreg(result));
    e->vti = a.idx + 1;
    return (EV){ CLS_INT, a.idx };
}

static EV gen_binop(Em* e, Node* n) {
    TokKind op = (TokKind)n->op;
    SType* t = node_ty(n->a);
    if (!t || t->kind == SK_UT || t->kind == SK_ERR) {
        em_err(e, n, "untyped operand reached codegen");
        return (EV){ CLS_INT, tmp_int(e) };
    }

    // null comparison: x == null / x != null
    if ((op == T_EQEQ || op == T_BANGEQ) &&
        ((n->b->kind == N_NULL) != (n->a->kind == N_NULL))) {
        Node* other = n->b->kind == N_NULL ? n->a : n->b;
        int want = op == T_BANGEQ ? C_NE : C_EQ;
        EV v = gen_expr(e, other);
        int rt = xreg(v.idx);
        if (v.cls == CLS_ADDR) { // 16-byte niche: first word vs 0
            ld_reg(e, 16, rt, 0, 8, 0);
            rt = 16;
        }
        ins(e, 0xF100001Fu | ((uint32_t)rt << 5)); // cmp rt, #0
        cset_(e, 0, want, xreg(v.idx));
        e->vti = v.idx + 1; // replace scalar/address temporary with the bool
        return (EV){ CLS_INT, v.idx };
    }

    // union equality: tag, then the active payload when the tags match
    {
        SType* ut = unwrap(node_ty(n->a));
        if ((op == T_EQEQ || op == T_BANGEQ) && ut && ut->kind == SK_UNION) {
            EV a = gen_expr(e, n->a);
            EV b = gen_expr(e, n->b);
            if (a.cls != CLS_ADDR || b.cls != CLS_ADDR) {
                em_err(e, n, "union values must materialize in memory");
                return a;
            }
            return gen_union_eq(e, n, a, b, ut, op == T_EQEQ);
        }
    }

    if ((op == T_EQEQ || op == T_BANGEQ) &&
        is_byte_sequence_type(node_ty(n->a)) &&
        is_byte_sequence_type(node_ty(n->b))) {
        EV a = gen_expr(e, n->a);
        EV b = gen_expr(e, n->b);
        return gen_byte_sequence_eq(e, n, a, node_ty(n->a), b,
                                    node_ty(n->b), op == T_EQEQ);
    }

    EV a = gen_expr(e, n->a);
    EV b = gen_expr(e, n->b);

    // two-word identity compare (slices, strings, contracts): == and !=
    if (a.cls == CLS_ADDR && b.cls == CLS_ADDR) {
        if (op == T_EQEQ || op == T_BANGEQ) {
            int eq = op == T_EQEQ;
            ld_reg(e, 16, xreg(a.idx), 0, 8, 0);
            ld_reg(e, 17, xreg(b.idx), 0, 8, 0);
            ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5)); // cmp x16, x17
            ld_reg(e, 16, xreg(a.idx), 8, 8, 0);
            ld_reg(e, 17, xreg(b.idx), 8, 8, 0);
            cset_(e, 0, eq ? C_EQ : C_NE, xreg(a.idx));
            ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5));
            cset_(e, 0, eq ? C_EQ : C_NE, xreg(b.idx));
            ins(e, (eq ? 0x0A000000u : 0x2A000000u) | (xreg(b.idx) << 16) |
                   (xreg(a.idx) << 5) | (uint32_t)xreg(a.idx));
            e->vti = a.idx + 1;
            return (EV){ CLS_INT, a.idx };
        }
        em_err(e, n, "ordering comparisons on two-word values come with str (chunk 7)");
        return a;
    }

    int cond = tok_cond(op, t);
    if (cond >= 0) { // comparison -> bool
        if (a.cls == CLS_FP) {
            emit_fcmp(e, a.idx, b.idx, unwrap(t)->prim == P_F64);
            e->vtf = a.idx; // free b; result is an int temp reusing a's index
            int r = tmp_int(e);
            cset_(e, 0, cond, xreg(r));
            return (EV){ CLS_INT, r };
        }
        int sf = scalar_bits(t) == 64;
        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
               (xreg(b.idx) << 16) | (xreg(a.idx) << 5));
        e->vti = a.idx + 1; // free b
        cset_(e, sf, cond, xreg(a.idx));
        return (EV){ CLS_INT, a.idx };
    }
    if (a.cls == CLS_FP) {
        emit_farith(e, op, a.idx, a.idx, b.idx, unwrap(t)->prim == P_F64);
        e->vtf = a.idx + 1;
        return a;
    }
    emit_arith(e, op, a.idx, a.idx, b.idx, t);
    e->vti = a.idx + 1;
    return a;
}

static EV gen_shortcircuit(Em* e, Node* n) {
    int is_and = n->op == T_ANDAND;
    EV a = gen_expr(e, n->a);
    if (a.cls != CLS_INT) {
        em_err(e, n, "short-circuit operands must be bool");
        return a;
    }
    int si = a.idx;
    int lf = new_label(e), lend = new_label(e);
    cbz_(e, 0, xreg(si), !is_and, lf); // AND: zero short-circuits false; OR: nonzero short-circuits true
    e->vti = si; // reuse the slot for b
    EV b = gen_expr(e, n->b);
    if (b.cls != CLS_INT) {
        em_err(e, n->b, "unexpected operand shape in short-circuit");
    } else if (b.idx != si) {
        mov_int(e, 0, xreg(si), xreg(b.idx));
    }
    e->vti = si + 1;
    cbz_(e, 0, xreg(si), !is_and, lf);
    load_imm(e, 0, xreg(si), is_and);
    br_(e, lend);
    bind_label(e, lf);
    load_imm(e, 0, xreg(si), !is_and);
    bind_label(e, lend);
    e->vti = si + 1;
    return (EV){ CLS_INT, si };
}

static EV gen_cmpchain(Em* e, Node* n) {
    SType* t = node_ty(n->kids[0]);
    if (!t || t->kind == SK_UT) {
        em_err(e, n, "untyped comparison chain reached codegen");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    int isf = is_floatt(t);
    int nops = n->nkids;
    int obase = isf ? e->vtf : e->vti;
    for (int i = 0; i < nops; i++) gen_expr(e, n->kids[i]);
    int nb = e->vti;
    for (int i = 1; i < nops; i++) {
        int cond = tok_cond((TokKind)n->a->kids[i - 1]->op, t);
        if (cond < 0) cond = C_EQ;
        if (isf) {
            emit_fcmp(e, obase + i - 1, obase + i, unwrap(t)->prim == P_F64);
            int r = tmp_int(e);
            cset_(e, 0, cond, xreg(r));
        } else {
            int sf = scalar_bits(t) == 64;
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                   (xreg(obase + i) << 16) | (xreg(obase + i - 1) << 5));
            int r = tmp_int(e);
            cset_(e, 0, cond, xreg(r));
        }
    }
    for (int i = 1; i < nops - 1; i++) // AND-fold the bools
        ins(e, 0x0A000000u | (xreg(nb + i) << 16) | (xreg(nb) << 5) |
               (uint32_t)xreg(nb));
    if (isf) {
        e->vtf = obase;
        e->vti = nb + 1;
        return (EV){ CLS_INT, nb };
    }
    if (nb != obase) mov_int(e, 0, xreg(obase), xreg(nb));
    e->vti = obase + 1;
    return (EV){ CLS_INT, obase };
}

// --- calls ------------------------------------------------------------------------------

typedef struct {
    Node* node;    // unevaluated argument
    EV    val;     // value once evaluated
    int   pre;     // 1 = val already filled (dispatch receiver)
    int   ptrval;  // pre: val already IS the address to pass (mut receiver)
} CallArg;

// transfer-unit assignment shared by caller and callee. internal=1 allows the
// Ace-only byref forms (mut params, >16B read-only by address). Units are four
// per parameter (an HFA uses up to four FP registers, one per leaf).
static uint32_t assign_cc(SType** params, uint8_t* muts, int nparams, int internal,
                          int varargs_from, TU* units /* 4 per param */) {
    int xn = 0, vn = 0;
    uint32_t soff = 0;
    for (int i = 0; i < nparams; i++) {
        SType* t = params[i];
        uint32_t size = ty_size(t);
        TU* u = &units[i * 4];
        for (int w = 0; w < 4; w++) {
            u[w].kind = TU_XREG; u[w].reg = -1;
            u[w].soff = 0; u[w].size = 0; u[w].off = 0;
        }
        if (varargs_from >= 0 && i >= varargs_from) {
            // Darwin's arm64 PCS places every unnamed variadic argument on
            // the stack, independently of the remaining argument registers.
            // Default promotions have already materialized it as i64/f64.
            soff = align_up(soff, 8);
            u[0].kind = TU_STACK;
            u[0].soff = soff;
            u[0].size = 8;
            soff += 8;
            continue;
        }
        if (internal && muts && muts[i]) { // Ace mut parameter: original address
            if (xn < 8) { u[0].kind = TU_XREG; u[0].reg = (int8_t)(xn++); u[0].size = 8; }
            else {
                soff = align_up(soff, 8);
                u[0].kind = TU_STACK; u[0].soff = soff; u[0].size = 8; soff += 8;
            }
            continue;
        }
        if (is_scalar(t)) {
            uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
            if (w < 4) w = 4; // Darwin PCS: stack slots are at least 4 bytes
            if (is_floatt(t)) {
                if (vn < 8) { u[0].kind = TU_VREG; u[0].reg = (int8_t)(vn++); u[0].size = w; }
                else {
                    soff = align_up(soff, w > 8 ? 8 : w);
                    u[0].kind = TU_STACK; u[0].soff = soff; u[0].size = w; soff += w;
                }
            } else {
                if (xn < 8) { u[0].kind = TU_XREG; u[0].reg = (int8_t)(xn++); u[0].size = 8; }
                else {
                    soff = align_up(soff, w > 8 ? 8 : w);
                    u[0].kind = TU_STACK; u[0].soff = soff; u[0].size = w; soff += w;
                }
            }
            continue;
        }
        uint32_t offs[5];
        uint8_t dbl[5];
        int nl = classify_hfa(t, offs, dbl);
        if (nl >= 1 && nl <= 4) {
            if (vn + nl <= 8) { // HFA: one FP register per leaf
                for (int w = 0; w < nl; w++) {
                    u[w].kind = TU_VREG;
                    u[w].reg = (int8_t)(vn + w);
                    u[w].off = offs[w];
                    u[w].size = dbl[w] ? 8 : 4;
                }
                vn += nl;
                continue;
            }
            // FP registers cannot hold the whole HFA: it goes on the stack
            uint32_t a = ty_align(t);
            if (a > 8) a = 8;
            soff = align_up(soff, a);
            u[0].kind = TU_STACK_AGGR;
            u[0].soff = soff;
            u[0].size = size;
            soff += align_up(size, 8);
            vn = 8;
            continue;
        }
        if (size > 16) { // non-HFA composite: pointer to a caller-owned copy
            if (xn < 8) {
                u[0].kind = TU_XREG;
                u[0].reg = (int8_t)(xn++);
                u[0].size = 8;
            } else {
                soff = align_up(soff, 8);
                u[0].kind = TU_STACK;
                u[0].soff = soff;
                u[0].size = 8;
                soff += 8;
            }
            continue;
        }
        if (size <= 16) { // non-HFA chunks are INTEGER-class, floats included
            int nw = (int)((size + 7) / 8);
            if (xn + nw > 8) {
                uint32_t a = ty_align(t);
                if (a > 8) a = 8;
                soff = align_up(soff, a);
                u[0].kind = TU_STACK_AGGR;
                u[0].soff = soff;
                u[0].size = size;
                soff += align_up(size, 8);
                xn = 8;
                continue;
            }
            for (int w = 0; w < 2; w++) {
                uint32_t off = (uint32_t)w * 8;
                if (off >= size) break;
                u[w].kind = TU_XREG;
                u[w].reg = (int8_t)(xn++);
                u[w].size = 8;
                u[w].off = off;
            }
            continue;
        }
    }
    return soff;
}

typedef struct {
    int sret;
    int nunits;
    TU units[4];
} RetCls;

static void classify_ret(SType* t, RetCls* r) {
    memset(r, 0, sizeof *r);
    if (!t) return;
    uint32_t size = ty_size(t);
    if (is_scalar(t)) {
        r->units[0].kind = is_floatt(t) ? TU_VREG : TU_XREG;
        r->units[0].reg = 0;
        r->units[0].size = is_floatt(t) && unwrap(t)->prim == P_F32 ? 4 : 8;
        r->nunits = 1;
        return;
    }
    uint32_t offs[5];
    uint8_t dbl[5];
    int nl = classify_hfa(t, offs, dbl);
    if (nl >= 1 && nl <= 4) { // HFA returns in v0..v3, one register per leaf
        for (int w = 0; w < nl; w++) {
            r->units[w].kind = TU_VREG;
            r->units[w].reg = (int8_t)w;
            r->units[w].off = offs[w];
            r->units[w].size = dbl[w] ? 8 : 4;
        }
        r->nunits = nl;
        return;
    }
    if (size <= 16) { // non-HFA: INTEGER chunks, float bits included
        for (int w = 0; w < 2; w++) {
            if ((uint32_t)w * 8 >= size) break;
            r->units[w].kind = TU_XREG;
            r->units[w].reg = (int8_t)w;
            r->units[w].size = 8;
            r->units[w].off = (uint32_t)w * 8;
            r->nunits++;
        }
        return;
    }
    r->sret = 1;
}

static void emit_bl_internal(Em* e, const char* p, uint32_t len) {
    ins(e, 0x94000000u); // bl .+0
    ESym* t = sym_intern(&e->syms, p, len);
    fix_add(e, FK_BRANCH, (uint32_t)(text(e)->len - 4), t);
}

static void emit_bl_extern(Em* e, const char* p, uint32_t len) {
    ins(e, 0x94000000u);
    ESym* t = sym_intern(&e->syms, p, len);
    reloc_add(&e->secs[SEC_TEXT], (uint32_t)(text(e)->len - 4), t, 1, 2, 1,
              RM_BRANCH26);
}

// --- mangling ---------------------------------------------------------------------------

static void mangle_name(Buf* b, const char* name) {
    char tmp[32];
    size_t len = strlen(name);
    snprintf(tmp, sizeof tmp, "%zu_", len);
    buf_write(b, tmp, strlen(tmp));
    buf_write(b, name, len);
}

static void mangle_type(Buf* b, SType* t) {
    if (!t) { buf_put8(b, '?'); return; }
    switch (t->kind) {
    case SK_PRIM: {
        const char* n = t->prim == P_BOOL ? "bool" :
                        t->prim == P_I8 ? "i8" : t->prim == P_I16 ? "i16" :
                        t->prim == P_I32 ? "i32" : t->prim == P_I64 ? "i64" :
                        t->prim == P_U8 ? "u8" : t->prim == P_U16 ? "u16" :
                        t->prim == P_U32 ? "u32" : t->prim == P_U64 ? "u64" :
                        t->prim == P_F32 ? "f32" : t->prim == P_F64 ? "f64" :
                        t->prim == P_STRING ? "string" :
                        t->prim == P_USIZE ? "usize" : "typeid";
        buf_write(b, n, strlen(n));
        return;
    }
    case SK_PTR: buf_put8(b, 'p'); mangle_type(b, t->base); return;
    case SK_SLICE: buf_put8(b, 's'); mangle_type(b, t->base); return;
    case SK_ARRAY: {
        char tmp[32];
        snprintf(tmp, sizeof tmp, "a%llu_", (unsigned long long)t->count);
        buf_write(b, tmp, strlen(tmp));
        mangle_type(b, t->base);
        return;
    }
    case SK_ERROR: buf_write(b, "Error", 5); return;
    case SK_STRUCT: case SK_ENUM: case SK_CONTRACT: case SK_UNION:
        buf_put8(b, t->kind == SK_STRUCT ? 'S' : t->kind == SK_ENUM ? 'E' :
                   t->kind == SK_UNION ? 'U' : 'I');
        mangle_name(b, t->name);
        if (t->nargs) {
            buf_put8(b, 'g');
            for (int i = 0; i < t->nargs; i++) {
                buf_put8(b, '$');
                mangle_type(b, t->args[i]);
            }
        }
        return;
    case SK_DISTINCT:
        buf_put8(b, 'D');
        mangle_name(b, t->name);
        return;
    case SK_TUPLE:
        buf_put8(b, 't');
        for (int i = 0; i < t->nfields; i++) mangle_type(b, t->fields[i].t);
        buf_put8(b, '_');
        return;
    case SK_FUNC:
        buf_put8(b, 'f');
        for (int i = 0; i < t->nparams; i++) mangle_type(b, t->params[i]);
        buf_put8(b, 'r');
        mangle_type(b, t->ret);
        buf_put8(b, '_');
        return;
    default:
        buf_put8(b, '?');
        return;
    }
}

static char fnname_buf[512];

// `spec` marks a generic specialization: its exact type-argument tuple rides
// an 's' tag so the symbol can never collide with a template stamp ('g' tag)
// or an ordinary same-params overload (no tag).
static const char* fn_mangle(const char* name, uint32_t len, SType* ft, Node* decl,
                             SType** targs, int nt, int spec) {
    Buf b = {0};
    buf_write(&b, "__ace$fn$", strlen("__ace$fn$"));
    buf_write(&b, name, len);
    buf_write(&b, "$p", 2);
    if (ft && ft->kind == SK_FUNC) {
        for (int i = 0; i < ft->nparams; i++) {
            buf_put8(&b, decl && i < decl->nkids && (decl->kids[i]->flags & F_MUT)
                            ? 'm' : 'r');
            mangle_type(&b, ft->params[i]);
            buf_put8(&b, '$');
        }
    }
    if (nt) buf_put8(&b, spec ? 's' : 'g');
    for (int i = 0; i < nt; i++) {
        buf_put8(&b, '$');
        mangle_type(&b, targs[i]);
    }
    buf_put8(&b, 0);
    snprintf(fnname_buf, sizeof fnname_buf, "%s", b.d);
    free(b.d);
    return fnname_buf;
}

// The C-boundary spelling of an extern function: module-prefixed extern
// declarations carry their original name in decl->cname (module.c).
static const char* extern_c_name(Sym* s) {
    if (s->decl && s->decl->cname) return s->decl->cname;
    return s->name;
}

// --- the shared call sequence ----------------------------------------------------------

enum { TK_INTERNAL, TK_EXTERN, TK_REG };
typedef struct {
    int kind;
    const char* name; // arena storage (TK_INTERNAL/TK_EXTERN)
    uint32_t nlen;
    int reg;          // TK_REG: int temp holding the target (spill slot used)
} CallTarget;

static SType* default_i64(void) {
    static SType* t;
    if (!t) {
        t = (SType*)arena_alloc(sizeof(SType));
        t->kind = SK_PRIM;
        t->prim = P_I64;
        t->size = t->align = 8;
    }
    return t;
}

static SType* default_f64(void) {
    static SType* t;
    if (!t) {
        t = (SType*)arena_alloc(sizeof(SType));
        t->kind = SK_PRIM;
        t->prim = P_F64;
        t->size = 8;
        t->align = 8;
    }
    return t;
}

static SType* raw_pointer_type(void) {
    static SType* t;
    if (!t) {
        t = (SType*)arena_alloc(sizeof(SType));
        memset(t, 0, sizeof *t);
        t->kind = SK_PTR;
        t->size = t->align = 8;
    }
    return t;
}

static EV emit_call(Em* e, Node* at, CallArg* args, int nargs,
                    SType** params, uint8_t* muts, int nparams, int internal,
                    int varargs_from, CallTarget tgt, SType* ret) {
    int si = e->vti, sf = e->vtf;
    spill_temps(e);

    // effective parameter types (variadic extras materialize to i64/f64)
    SType* eff[40];
    int n = nargs < 40 ? nargs : 40;
    if (nargs > 40) em_err(e, at, "too many arguments (max 40)");
    for (int i = 0; i < n; i++)
        eff[i] = i < nparams ? params[i]
              : is_floatt(node_ty(args[i].node)) ? default_f64()
                                                 : default_i64();

    // evaluate arguments into temps
    for (int i = 0; i < n; i++) {
        if (args[i].pre) continue;
        Node* a = args[i].node;
        uint32_t arg_size = ty_size(eff[i]);
        uint32_t hfa_offs[5];
        uint8_t hfa_dbl[5];
        int is_hfa = classify_hfa(eff[i], hfa_offs, hfa_dbl) >= 0;
        int byref = (internal && muts && i < nparams && muts[i]) ||
                    (arg_size > 16 && !is_hfa);
        if (byref) { // the argument's address travels in an int temp
            if (!internal && arg_size > 16) {
                // AAPCS64 passes a >16-byte composite indirectly, but C value
                // semantics require a caller-owned copy rather than exposing
                // the Ace argument's original storage to the foreign callee.
                EV v = gen_expr(e, a);
                if (v.cls != CLS_ADDR) {
                    em_err(e, a, "large C aggregate argument must materialize in memory");
                    args[i].val = v;
                    continue;
                }
                uint32_t copy = temp_slot(e, arg_size, ty_align(eff[i]));
                frame_addr(e, 15, copy);
                copy_mem(e, 15, 0, xreg(v.idx), 0, arg_size);
                int p = tmp_int(e);
                frame_addr(e, xreg(p), copy);
                args[i].val = (EV){ CLS_INT, p };
                continue;
            }
            if (is_lvalue_node(a)) {
                args[i].val.cls = CLS_INT;
                args[i].val.idx = gen_addr(e, a);
            } else {
                EV v = gen_expr(e, a);
                if (v.cls != CLS_ADDR)
                    em_err(e, a, "aggregate argument must materialize in memory");
                args[i].val = v;
                args[i].ptrval = 1; // the materialized address is what travels
            }
            continue;
        }
        args[i].val = gen_expr(e, a);
        if (varargs_from >= 0 && i >= varargs_from &&
            is_floatt(node_ty(a)) && unwrap(node_ty(a))->prim == P_F32)
            args[i].val = gen_cast(e, args[i].val, node_ty(a), default_f64());
    }

    TU units[160];
    uint32_t sbytes = assign_cc(eff, muts, n, internal, varargs_from, units);
    if (sbytes > e->out_max) e->out_max = sbytes;

    RetCls rc;
    classify_ret(ret, &rc);

    // marshal. Argument values are read exclusively from their spill slots:
    // argument registers overlap the temp pool, so placing one argument (a
    // two-word aggregate load into x1, say) must not read a later argument's
    // still-unplaced temp register directly.
    spill_temps(e);
    for (int i = 0; i < n; i++) {
        EV v = args[i].val;
        int pass_addr = args[i].ptrval || args[i].val.cls == CLS_INT;
        int is_agg = v.cls == CLS_ADDR && !pass_addr;
        uint32_t aslot = spill_slot_int(e, v.idx);
        if (is_agg) load_frame(e, 17, aslot, 8, 0); // aggregate base
        for (int w = 0; w < 4; w++) {
            TU* u = &units[i * 4 + w];
            if (u->kind == TU_XREG && u->reg < 0) continue;
            if (u->kind == TU_XREG) {
                if (is_agg) // aggregate chunk
                    ld_reg(e, u->reg, 17, u->off, 8, 0);
                else // scalar or by-address value from its spill slot
                    load_frame(e, u->reg, aslot, 8, 0);
            } else if (u->kind == TU_VREG) {
                if (v.cls == CLS_FP)
                    load_frame_fp(e, u->reg, spill_slot_fp(e, v.idx),
                                  u->size == 8);
                else // HFA leaf
                    ldrf_(e, u->reg, 17, u->off, u->size == 8);
            } else if (u->kind == TU_STACK) {
                if (v.cls == CLS_FP) {
                    // v0-v7 may already hold earlier arguments; use a
                    // non-argument scratch register for a spilled FP value.
                    load_frame_fp(e, 16, spill_slot_fp(e, v.idx), 1);
                    strf_(e, 16, 31, u->soff, u->size == 8);
                    continue;
                } else if (is_agg)
                    ld_reg(e, 16, 17, u->off, 8, 0);
                else
                    load_frame(e, 16, aslot, 8, 0);
                uint32_t scale = u->size >= 4 ? u->size : 4;
                ins(e, (u->size == 8 ? 0xF9000000u : u->size == 4 ? 0xB9000000u :
                       0x39000000u) |
                       ((u->soff / scale) << 10) | (31u << 5) | 16u);
            } else { // TU_STACK_AGGR: small composite/HFA passed on stack
                load_frame(e, 14, aslot, 8, 0); // base outside copy_mem's x15-x17
                if (u->soff <= 4095)
                    ins(e, 0x91000000u | (u->soff << 10) | (31u << 5) | 15u);
                else {
                    load_imm(e, 1, 15, u->soff);
                    ins(e, 0x8B000000u | (15u << 16) | (31u << 5) | 15u);
                }
                copy_mem(e, 15, 0, 14, 0, u->size);
            }
        }
    }

    // sret buffer
    uint32_t sret_off = 0;
    if (rc.sret) {
        sret_off = temp_slot(e, ty_size(ret), ty_align(ret));
        frame_addr(e, 8, sret_off);
    }

    // the call itself
    if (tgt.kind == TK_INTERNAL) emit_bl_internal(e, tgt.name, tgt.nlen);
    else if (tgt.kind == TK_EXTERN) emit_bl_extern(e, tgt.name, tgt.nlen);
    else { // target reloaded from its spill slot (marshaling cannot clobber it)
        load_frame(e, 16, spill_slot_int(e, tgt.reg), 8, 0);
        ins(e, 0xD63F0000u | (16u << 5)); // blr x16
    }

    // stage results, reload spilled temps, produce the result EV
    EV out = { CLS_INT, 0 };
    if (rc.sret) {
        reload_temps(e, si, sf);
        e->vti = si; e->vtf = sf;
        int t = tmp_int(e);
        frame_addr(e, xreg(t), sret_off);
        out = (EV){ CLS_ADDR, t };
    } else if (rc.nunits == 1 && is_scalar(ret)) {
        // single scalar: stage through the result temp's spill slot
        if (is_floatt(ret)) {
            int is64 = rc.units[0].size == 8;
            store_frame_fp(e, 0, spill_slot_fp(e, sf), is64); // v0/s0
            reload_temps(e, si, sf);
            e->vti = si; e->vtf = sf;
            int t = tmp_fp(e);
            load_frame_fp(e, vreg(t), spill_slot_fp(e, sf), is64);
            out = (EV){ CLS_FP, t };
        } else {
            store_frame(e, 0, spill_slot_int(e, si), 8); // x0
            reload_temps(e, si, sf);
            e->vti = si; e->vtf = sf;
            int t = tmp_int(e);
            load_frame(e, xreg(t), spill_slot_int(e, si), 8, 0);
            out = (EV){ CLS_INT, t };
        }
    } else if (rc.nunits >= 1) {
        // <=16B aggregate or multi-chunk return: materialize into a frame slot
        uint32_t off = temp_slot(e, align_up(ty_size(ret), 8), ty_align(ret));
        for (int w = 0; w < rc.nunits; w++) {
            // Slots grow down from x29, while aggregate offsets grow upward.
            frame_addr(e, 16, off - rc.units[w].off);
            if (rc.units[w].kind == TU_XREG)
                st_reg(e, rc.units[w].reg, 16, 0, 8);
            else
                strf_(e, rc.units[w].reg, 16, 0, rc.units[w].size == 8);
        }
        reload_temps(e, si, sf);
        e->vti = si; e->vtf = sf;
        int t = tmp_int(e);
        frame_addr(e, xreg(t), off);
        out = (EV){ CLS_ADDR, t };
    } else {
        reload_temps(e, si, sf);
        e->vti = si; e->vtf = sf;
    }
    return out;
}

static int byte_sequence_parts(Em* e, Node* at, EV value, SType* t,
                               int* ptr, int* len) {
    *ptr = tmp_int(e);
    *len = tmp_int(e);
    if (value.cls != CLS_ADDR) {
        em_err(e, at, "byte sequence must materialize in memory");
        load_imm(e, 1, xreg(*ptr), 0);
        load_imm(e, 1, xreg(*len), 0);
        return 0;
    }
    if (t->kind == SK_ARRAY) {
        mov_int(e, 1, xreg(*ptr), xreg(value.idx));
        load_imm(e, 1, xreg(*len), t->count);
        return 1;
    }
    ld_reg(e, xreg(*ptr), xreg(value.idx), 0, 8, 0);
    ld_reg(e, xreg(*len), xreg(value.idx), 8, 8, 0);
    return 1;
}

/*
 * Compare the complete byte sequences represented by [N]u8, []u8, or
 * string.  Both values have already been evaluated in source order.  A
 * length mismatch touches no element storage; equal nonzero lengths use the
 * platform byte comparator directly and introduce no converted view.
 */
static EV gen_byte_sequence_eq(Em* e, Node* n, EV a, SType* at,
                               EV b, SType* bt, int eq) {
    int aptr, alen, bptr, blen;
    byte_sequence_parts(e, n->a ? n->a : n, a, at, &aptr, &alen);
    byte_sequence_parts(e, n->b ? n->b : n, b, bt, &bptr, &blen);

    int out = tmp_int(e);
    load_imm(e, 0, xreg(out), 0);
    int done = new_label(e);
    ins(e, 0xEB00001Fu | ((uint32_t)xreg(blen) << 16) |
           ((uint32_t)xreg(alen) << 5)); // cmp alen, blen
    bcond_(e, C_NE, done);
    load_imm(e, 0, xreg(out), 1);
    cbz_(e, 1, xreg(alen), 0, done);

    CallArg args[3] = {
        { NULL, { CLS_INT, aptr }, 1, 0 },
        { NULL, { CLS_INT, bptr }, 1, 0 },
        { NULL, { CLS_INT, alen }, 1, 0 },
    };
    SType* params[3] = { raw_pointer_type(), raw_pointer_type(), default_i64() };
    uint8_t muts[3] = { 0, 0, 0 };
    CallTarget tgt = { TK_EXTERN, "_memcmp", 7, 0 };
    EV cmp = emit_call(e, n, args, 3, params, muts, 3, 0, -1, tgt,
                       default_i64());
    ins(e, 0x7100001Fu | ((uint32_t)xreg(cmp.idx) << 5)); // cmp wresult, #0
    cset_(e, 0, C_EQ, xreg(out));

    bind_label(e, done);
    if (!eq)
        ins(e, 0x52000000u | (xreg(out) << 5) | xreg(out)); // eor w, w, #1
    if (out != a.idx) mov_int(e, 0, xreg(a.idx), xreg(out));
    e->vti = a.idx + 1;
    return (EV){ CLS_INT, a.idx };
}

static EV gen_call(Em* e, Node* call);
static void store_to_slot(Em* e, EV v, uint32_t off, SType* t);

// --- string literals --------------------------------------------------------------------

static ESym* intern_string(Em* e, Node* s) {
    Sec* c = &e->secs[SEC_CONST];
    while (c->data.len % 8) buf_put8(&c->data, 0);
    char nm[64];
    snprintf(nm, sizeof nm, "__ace$str$%d", e->str_n++);
    ESym* sym = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, c);
    buf_write(&c->data, s->str, s->len);
    return sym;
}

// --- variable addressing ------------------------------------------------------------------

static ESym* global_sym(Em* e, Sym* s) {
    for (int i = 0; i < e->nglobs; i++)
        if (e->globs[i].s == s)
            return sym_intern(&e->syms, e->globs[i].name, e->globs[i].len);
    return NULL;
}

// adrp rd, sym@PAGE ; add rd, rd, sym@PAGEOFF
static void addr_of_sym(Em* e, int rd, ESym* sym) {
    uint32_t site = (uint32_t)text(e)->len;
    ins(e, 0x90000000u | ((uint32_t)rd & 31u));
    reloc_add(&e->secs[SEC_TEXT], site, sym, 1, 2, 1, RM_PAGE21);
    site = (uint32_t)text(e)->len;
    ins(e, 0x91000000u | ((uint32_t)rd << 5) | ((uint32_t)rd & 31u));
    reloc_add(&e->secs[SEC_TEXT], site, sym, 0, 2, 1, RM_PAGEOFF12);
}

// address of a variable's storage in a fresh int temp
static int gen_var_addr(Em* e, Sym* s, Node* at) {
    int t = tmp_int(e);
    ESym* g = global_sym(e, s);
    if (g) {
        addr_of_sym(e, xreg(t), g);
        return t;
    }
    Slot* sl = s->decl ? slot_find(e, s->decl) : NULL;
    if (!sl) {
        em_err(e, at, "no storage for '%.*s'", (int)s->len, s->name);
        load_imm(e, 1, xreg(t), 0);
        return t;
    }
    if (sl->byref)
        load_frame(e, xreg(t), sl->off, 8, 0); // slot holds the pointer
    else
        frame_addr(e, xreg(t), sl->off);
    return t;
}

static int is_lvalue_node(Node* x) {
    switch (x->kind) {
    case N_IDENT: case N_INDEX:
        return 1;
    case N_FIELD_E:
        /*
         * `Type.variant` union constructions are rvalues that materialize in
         * memory; gen_expr lowers them, so they must not take the gen_addr
         * path (there is no storage behind a type name).
         */
        if (x->a && x->a->kind == N_IDENT && node_sym(x->a) &&
            node_sym(x->a)->kind == SYM_TYPE)
            return 0;
        if (x->a && x->a->kind == N_INDEX && node_ty(x->a) &&
            node_ty(x->a)->kind == SK_UNION)
            return 0;
        return 1;
    case N_UNARY:
        return x->op == T_TILDE || x->op == T_AMP;
    default:
        return 0;
    }
}

// emitter-side lvalue type walk (assignment targets may lack sema annotations)
static SType* lvalue_type(Em* e, Node* x) {
    switch (x->kind) {
    case N_IDENT: {
        Sym* s = node_sym(x);
        return s ? s->type : NULL;
    }
    case N_FIELD_E: {
        SType* b = lvalue_type(e, x->a);
        if (!b) return node_ty(x->a);
        b = unwrap(b);
        if (b->kind == SK_ERROR) b = b->base;
        while (b->kind == SK_PTR) b = b->base;
        if (x->len == 3 && memcmp(x->str, "len", 3) == 0 &&
            (b->kind == SK_SLICE ||
             (b->kind == SK_PRIM && b->prim == P_STRING)))
            return node_ty(x);
        if (b->kind != SK_STRUCT) return NULL;
        int i = find_field(b, x->str, x->len);
        return i >= 0 ? b->fields[i].t : NULL;
    }
    case N_INDEX: {
        SType* b = lvalue_type(e, x->a);
        if (!b) return node_ty(x->a);
        b = unwrap(b);
        while (b->kind == SK_PTR) b = b->base;
        if (b->kind == SK_PRIM && b->prim == P_STRING) {
            static SType byte = { .kind = SK_PRIM, .prim = P_U8,
                                  .size = 1, .align = 1 };
            return &byte;
        }
        if (b->kind == SK_SLICE || b->kind == SK_ARRAY) return b->base;
        return NULL;
    }
    case N_UNARY:
        if (x->op == T_TILDE) {
            SType* b = lvalue_type(e, x->a);
            return b && b->kind == SK_PTR ? b->base : NULL;
        }
        if (x->op == T_AMP) {
            SType* b = lvalue_type(e, x->a);
            if (!b) return NULL;
            SType* p = (SType*)arena_alloc(sizeof(SType));
            memset(p, 0, sizeof *p);
            p->kind = SK_PTR;
            p->base = b;
            p->size = p->align = 8;
            return p;
        }
        return NULL;
    default:
        return node_ty(x);
    }
}

static int gen_addr(Em* e, Node* x) {
    switch (x->kind) {
    case N_IDENT: {
        Sym* s = node_sym(x);
        if (!s) {
            em_err(e, x, "no storage for this name");
            return tmp_int(e);
        }
        if (s->kind == SYM_FUNC) { // function value: address of the function
            int t = tmp_int(e);
            ESym* fs;
            if (s->is_extern || (s->decl && (s->decl->flags & F_EXPO))) {
                // C-boundary symbol: externs and exports carry their unmangled
                // linker spelling (the export body is emitted under it)
                const char* cn = s->is_extern ? extern_c_name(s)
                                              : (s->decl->cname ? s->decl->cname
                                                                : s->name);
                char nm[512];
                snprintf(nm, sizeof nm, "_%s", cn);
                fs = sym_intern(&e->syms, nm, (uint32_t)strlen(nm));
            } else {
                fs = sym_intern(&e->syms,
                                fn_mangle(s->name, s->len, s->type, s->decl,
                                          s->spec_args,
                                          s->spec_args ? s->spec_nargs : 0,
                                          s->spec_args != NULL),
                                (uint32_t)strlen(fnname_buf));
            }
            addr_of_sym(e, xreg(t), fs);
            return t;
        }
        return gen_var_addr(e, s, x);
    }
    case N_UNARY:
        if (x->op == T_AMP) return gen_addr(e, x->a);
        if (x->op == T_TILDE) { // ~p: the pointer value IS the address
            int ta = gen_addr(e, x->a);
            ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            return ta;
        }
        break;
    case N_FIELD_E: {
        SType* bt = lvalue_type(e, x->a);
        if (!bt) bt = node_ty(x->a);
        bt = unwrap(bt);
        int error_handle = bt && bt->kind == SK_ERROR;
        int ta;
        if (is_lvalue_node(x->a)) ta = gen_addr(e, x->a);
        else {
            EV v = gen_expr(e, x->a);
            if (error_handle && v.cls == CLS_INT) {
                ta = v.idx;
            } else if (v.cls != CLS_ADDR) {
                em_err(e, x, "cannot address this value");
                return tmp_int(e);
            } else {
                ta = v.idx;
            }
        }
        if (!bt) {
            em_err(e, x, "no field base type");
            return ta;
        }
        if (error_handle) {
            if (is_lvalue_node(x->a))
                ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            bt = bt->base;
        }
        while (bt->kind == SK_PTR) { // auto-deref p.x
            ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            bt = bt->base;
        }
        if (x->len == 3 && memcmp(x->str, "len", 3) == 0 &&
            (bt->kind == SK_SLICE ||
             (bt->kind == SK_PRIM && bt->prim == P_STRING))) {
            uint32_t off = 8;
            ins(e, 0x91000000u | (off << 10) | ((uint32_t)xreg(ta) << 5) |
                   (uint32_t)xreg(ta));
            return ta;
        }
        if (bt->kind != SK_STRUCT) {
            em_err(e, x, "field access on a non-struct");
            return ta;
        }
        int fi = find_field(bt, x->str, x->len);
        if (fi < 0) {
            em_err(e, x, "no such field");
            return ta;
        }
        uint32_t off = field_off(bt, fi);
        if (off) {
            if (off <= 4095)
                ins(e, 0x91000000u | (off << 10) | ((uint32_t)xreg(ta) << 5) |
                       (uint32_t)xreg(ta));
            else {
                load_imm(e, 1, 16, off);
                ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)xreg(ta) << 5) |
                       (uint32_t)xreg(ta));
            }
        }
        return ta;
    }
    case N_INDEX: {
        SType* bt = lvalue_type(e, x->a);
        bt = bt ? unwrap(bt) : NULL;
        int ta;
        if (is_lvalue_node(x->a)) ta = gen_addr(e, x->a);
        else {
            EV v = gen_expr(e, x->a);
            if (v.cls != CLS_ADDR) {
                em_err(e, x, "cannot index this value");
                return tmp_int(e);
            }
            ta = v.idx;
        }
        if (!bt) {
            em_err(e, x, "no index base type");
            return ta;
        }
        if (bt->kind == SK_PRIM && bt->prim == P_STRING) {
            static SType byte = { .kind = SK_PRIM, .prim = P_U8,
                                  .size = 1, .align = 1 };
            ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            bt = &byte;
        } else if (bt->kind == SK_SLICE) {
            ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            bt = bt->base;
        } else if (bt->kind == SK_ARRAY) {
            bt = bt->base;
        } else if (bt->kind == SK_PTR) {
            ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            bt = bt->base;
        } else {
            em_err(e, x, "cannot index this type");
            return ta;
        }
        int svi = e->vti, svf = e->vtf;
        EV ix = gen_expr(e, x->b);
        widen_idx(e, x->b, xreg(ix.idx));
        uint32_t esz = ty_size(bt);
        if (esz == 1) {
            ins(e, 0x8B000000u | ((uint32_t)xreg(ix.idx) << 16) |
                   ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
        } else if ((esz & (esz - 1)) == 0) {
            int sh = 0;
            while ((1u << sh) < esz) sh++;
            ins(e, 0x8B000000u | ((uint32_t)sh << 10) |
                   ((uint32_t)xreg(ix.idx) << 16) |
                   ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
        } else {
            load_imm(e, 1, 16, esz);
            ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(ix.idx) << 5) | 16u);
            ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)xreg(ta) << 5) |
                   (uint32_t)xreg(ta));
        }
        e->vti = svi; e->vtf = svf; // index temp freed
        return ta;
    }
    default:
        break;
    }
    em_err(e, x, "not an addressable value");
    return tmp_int(e);
}

// --- call dispatch --------------------------------------------------------------------

// The v1 allocator is deliberately the platform fast path: malloc/realloc/free
// with the language-visible requested length carried by the returned []u8.
// Apple arm64 malloc supplies the target's fundamental 16-byte alignment, so
// larger alignments are rejected without adding an Ace allocation header.
static void zero_slice_slot(Em* e, uint32_t off) {
    frame_addr(e, 16, off);
    st_reg(e, 31, 16, 0, 8);
    st_reg(e, 31, 16, 8, 8);
}

static EV finish_slice_builtin(Em* e, int si, int sf, uint32_t off) {
    e->vti = si;
    e->vtf = sf;
    int out = tmp_int(e);
    frame_addr(e, xreg(out), off);
    return (EV){ CLS_ADDR, out };
}

static EV gen_alloc_builtin(Em* e, Node* call) {
    int si = e->vti, sf = e->vtf;
    uint32_t off = temp_slot(e, 16, 8);
    zero_slice_slot(e, off); // invalid request and OOM both produce null

    EV size = gen_expr(e, call->kids[0]);
    EV align = gen_expr(e, call->kids[1]);
    int done = new_label(e);

    // size > 0
    ins(e, 0xF100001Fu | ((uint32_t)xreg(size.idx) << 5));
    bcond_(e, C_LE, done);
    // align is a positive power of two no greater than 16
    ins(e, 0xF100001Fu | ((uint32_t)xreg(align.idx) << 5));
    bcond_(e, C_LE, done);
    ins(e, 0xF100001Fu | (16u << 10) | ((uint32_t)xreg(align.idx) << 5));
    bcond_(e, C_GT, done);
    ins(e, 0xD1000000u | (1u << 10) |
           ((uint32_t)xreg(align.idx) << 5) | 16u); // x16 = align - 1
    ins(e, 0xEA00001Fu | (16u << 16) |
           ((uint32_t)xreg(align.idx) << 5));       // tst align, align - 1
    bcond_(e, C_NE, done);

    CallArg arg = { NULL, size, 1, 0 };
    SType* param = default_i64();
    uint8_t mut = 0;
    CallTarget tgt = { TK_EXTERN, "_malloc", 7, 0 };
    EV ptr = emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt,
                       raw_pointer_type());
    cbz_(e, 1, xreg(ptr.idx), 0, done);
    frame_addr(e, 16, off);
    st_reg(e, xreg(ptr.idx), 16, 0, 8);
    st_reg(e, xreg(size.idx), 16, 8, 8);

    bind_label(e, done);
    return finish_slice_builtin(e, si, sf, off);
}

static EV gen_realloc_builtin(Em* e, Node* call) {
    int si = e->vti, sf = e->vtf;
    uint32_t off = temp_slot(e, 16, 8);
    zero_slice_slot(e, off);

    EV data = gen_expr(e, call->kids[0]);
    if (data.cls != CLS_ADDR) {
        em_err(e, call->kids[0], "realloc data must materialize as a slice");
        return finish_slice_builtin(e, si, sf, off);
    }
    int ptemp = tmp_int(e);
    ld_reg(e, xreg(ptemp), xreg(data.idx), 0, 8, 0);
    EV size = gen_expr(e, call->kids[1]);
    int done = new_label(e);
    ins(e, 0xF100001Fu | ((uint32_t)xreg(size.idx) << 5));
    bcond_(e, C_LE, done);

    CallArg args[2] = {
        { NULL, { CLS_INT, ptemp }, 1, 0 },
        { NULL, size, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_EXTERN, "_realloc", 8, 0 };
    EV ptr = emit_call(e, call, args, 2, params, muts, 2, 0, -1, tgt,
                       raw_pointer_type());
    cbz_(e, 1, xreg(ptr.idx), 0, done);
    frame_addr(e, 16, off);
    st_reg(e, xreg(ptr.idx), 16, 0, 8);
    st_reg(e, xreg(size.idx), 16, 8, 8);

    bind_label(e, done);
    return finish_slice_builtin(e, si, sf, off);
}

/*
 * Typed allocation: the request is count * sizeof(T) bytes with alignment
 * taken from T (always within the platform heap's fundamental 16-byte
 * alignment for Ace layouts). The returned descriptor carries the ELEMENT
 * count. A multiplication overflow, zero byte size (sizeof(T) == 0 or a zero
 * count), unsupported alignment, or OOM produces the null slice without
 * aborting and without calling the allocator on the overflow paths.
 */
static EV gen_alloc_typed_builtin(Em* e, Node* call) {
    int si = e->vti, sf = e->vtf;
    uint32_t off = temp_slot(e, 16, 8);
    zero_slice_slot(e, off);

    SType* elem = node_ty(call)->base;
    uint64_t esz = ty_size(elem);
    uint64_t eal = ty_align(elem);
    EV count = gen_expr(e, call->kids[0]);
    int done = new_label(e);

    if (esz == 0 || eal > 16) { // zero byte size / unsupported alignment
        bind_label(e, done);
        return finish_slice_builtin(e, si, sf, off);
    }

    cbz_(e, 1, xreg(count.idx), 0, done); // zero count is a zero byte-size request
    int limit = tmp_int(e);
    load_imm(e, 1, xreg(limit), UINT64_MAX / esz);
    ins(e, 0xEB00001Fu | ((uint32_t)xreg(limit) << 16) |
            ((uint32_t)xreg(count.idx) << 5)); // cmp count, limit
    bcond_(e, C_HI, done); // count * sizeof(T) would overflow usize

    int bytes = tmp_int(e);
    int scale = tmp_int(e);
    load_imm(e, 1, xreg(scale), esz);
    ins(e, 0x9B007C00u | ((uint32_t)xreg(scale) << 16) |
            ((uint32_t)xreg(count.idx) << 5) | (uint32_t)xreg(bytes)); // mul

    CallArg arg = { NULL, { CLS_INT, bytes }, 1, 0 };
    SType* param = default_i64();
    uint8_t mut = 0;
    CallTarget tgt = { TK_EXTERN, "_malloc", 7, 0 };
    EV ptr = emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt,
                       raw_pointer_type());
    cbz_(e, 1, xreg(ptr.idx), 0, done);
    frame_addr(e, 16, off);
    st_reg(e, xreg(ptr.idx), 16, 0, 8);
    st_reg(e, xreg(count.idx), 16, 8, 8); // descriptor length: element count

    bind_label(e, done);
    return finish_slice_builtin(e, si, sf, off);
}

/* Typed reallocation over the same rules: failure leaves the original
 * allocation live and unchanged; success makes only the result authoritative
 * with the new element count. */
static EV gen_realloc_typed_builtin(Em* e, Node* call) {
    int si = e->vti, sf = e->vtf;
    uint32_t off = temp_slot(e, 16, 8);
    zero_slice_slot(e, off);

    SType* elem = node_ty(call)->base;
    uint64_t esz = ty_size(elem);
    uint64_t eal = ty_align(elem);
    EV data = gen_expr(e, call->kids[0]);
    if (data.cls != CLS_ADDR) {
        em_err(e, call->kids[0], "mem.realloc[T] data must materialize as a slice");
        return finish_slice_builtin(e, si, sf, off);
    }
    int ptemp = tmp_int(e);
    ld_reg(e, xreg(ptemp), xreg(data.idx), 0, 8, 0);
    EV count = gen_expr(e, call->kids[1]);
    int done = new_label(e);

    if (esz == 0 || eal > 16) { // zero byte size / unsupported alignment
        bind_label(e, done);
        return finish_slice_builtin(e, si, sf, off);
    }

    cbz_(e, 1, xreg(count.idx), 0, done); // zero count fails, original stays live
    int limit = tmp_int(e);
    load_imm(e, 1, xreg(limit), UINT64_MAX / esz);
    ins(e, 0xEB00001Fu | ((uint32_t)xreg(limit) << 16) |
            ((uint32_t)xreg(count.idx) << 5)); // cmp count, limit
    bcond_(e, C_HI, done); // overflow fails without calling the allocator

    int bytes = tmp_int(e);
    int scale = tmp_int(e);
    load_imm(e, 1, xreg(scale), esz);
    ins(e, 0x9B007C00u | ((uint32_t)xreg(scale) << 16) |
            ((uint32_t)xreg(count.idx) << 5) | (uint32_t)xreg(bytes)); // mul

    CallArg args[2] = {
        { NULL, { CLS_INT, ptemp }, 1, 0 },
        { NULL, { CLS_INT, bytes }, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_EXTERN, "_realloc", 8, 0 };
    EV ptr = emit_call(e, call, args, 2, params, muts, 2, 0, -1, tgt,
                       raw_pointer_type());
    cbz_(e, 1, xreg(ptr.idx), 0, done);
    frame_addr(e, 16, off);
    st_reg(e, xreg(ptr.idx), 16, 0, 8);
    st_reg(e, xreg(count.idx), 16, 8, 8); // descriptor length: element count

    bind_label(e, done);
    return finish_slice_builtin(e, si, sf, off);
}

static EV gen_release_builtin(Em* e, Node* call) {
    int si = e->vti, sf = e->vtf;
    Node* raw = call->kids[0]->a; // sema requires N_MUTARG + mutable lvalue
    int addr = gen_addr(e, raw);
    int ptr = tmp_int(e);
    ld_reg(e, xreg(ptr), xreg(addr), 0, 8, 0);
    int clear = new_label(e);
    cbz_(e, 1, xreg(ptr), 0, clear); // release(null) is a no-op

    CallArg arg = { NULL, { CLS_INT, ptr }, 1, 0 };
    SType* param = raw_pointer_type();
    uint8_t mut = 0;
    CallTarget tgt = { TK_EXTERN, "_free", 5, 0 };
    (void)emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt, NULL);

    bind_label(e, clear);
    st_reg(e, 31, xreg(addr), 0, 8);
    st_reg(e, 31, xreg(addr), 8, 8);
    e->vti = si;
    e->vtf = sf;
    return (EV){ CLS_INT, 0 }; // release has no value; expression statements discard it
}

static int gen_slice_parts(Em* e, Node* value, int* ptr, int* len) {
    EV slice = gen_expr(e, value);
    *ptr = tmp_int(e);
    *len = tmp_int(e);
    if (slice.cls != CLS_ADDR) {
        em_err(e, value, "byte operation argument must materialize as a slice");
        load_imm(e, 1, xreg(*ptr), 0);
        load_imm(e, 1, xreg(*len), 0);
        return 0;
    }
    ld_reg(e, xreg(*ptr), xreg(slice.idx), 0, 8, 0);
    ld_reg(e, xreg(*len), xreg(slice.idx), 8, 8, 0);
    return 1;
}

static EV gen_byte_write_builtin(Em* e, Node* call, int intrinsic) {
    int si = e->vti, sf = e->vtf;
    Node* dst_node = call->kids[0]->a; // sema requires a mut destination lvalue
    int dst, dlen;
    gen_slice_parts(e, dst_node, &dst, &dlen);

    int done = new_label(e);
    if (intrinsic == BI_MEM_COPY || intrinsic == BI_MEM_MOVE) {
        int src, slen;
        gen_slice_parts(e, call->kids[1], &src, &slen);
        cbz_(e, 1, xreg(slen), 0, done); // empty slices may carry null pointers
        CallArg args[3] = {
            { NULL, { CLS_INT, dst }, 1, 0 },
            { NULL, { CLS_INT, src }, 1, 0 },
            { NULL, { CLS_INT, slen }, 1, 0 },
        };
        SType* params[3] = { raw_pointer_type(), raw_pointer_type(), default_i64() };
        uint8_t muts[3] = { 0, 0, 0 };
        const char* name = intrinsic == BI_MEM_COPY ? "_memcpy" : "_memmove";
        uint32_t nlen = intrinsic == BI_MEM_COPY ? 7 : 8;
        CallTarget tgt = { TK_EXTERN, name, nlen, 0 };
        (void)emit_call(e, call, args, 3, params, muts, 3, 0, -1, tgt, NULL);
    } else {
        int byte = tmp_int(e);
        if (intrinsic == BI_MEM_FILL) {
            EV value = gen_expr(e, call->kids[1]);
            mov_int(e, 1, xreg(byte), xreg(value.idx));
        } else {
            load_imm(e, 1, xreg(byte), 0);
        }
        cbz_(e, 1, xreg(dlen), 0, done);
        CallArg args[3] = {
            { NULL, { CLS_INT, dst }, 1, 0 },
            { NULL, { CLS_INT, byte }, 1, 0 },
            { NULL, { CLS_INT, dlen }, 1, 0 },
        };
        SType* params[3] = { raw_pointer_type(), default_i64(), default_i64() };
        uint8_t muts[3] = { 0, 0, 0 };
        CallTarget tgt = { TK_EXTERN, "_memset", 7, 0 };
        (void)emit_call(e, call, args, 3, params, muts, 3, 0, -1, tgt, NULL);
    }
    bind_label(e, done);
    e->vti = si;
    e->vtf = sf;
    return (EV){ CLS_INT, 0 };
}

static EV gen_compare_builtin(Em* e, Node* call) {
    int si = e->vti, sf = e->vtf;
    int aptr, alen, bptr, blen;
    gen_slice_parts(e, call->kids[0], &aptr, &alen);
    gen_slice_parts(e, call->kids[1], &bptr, &blen);

    int count = tmp_int(e);
    mov_int(e, 1, xreg(count), xreg(alen));
    ins(e, 0xEB00001Fu | ((uint32_t)xreg(blen) << 16) |
           ((uint32_t)xreg(alen) << 5)); // cmp alen, blen
    int have_count = new_label(e);
    bcond_(e, C_LS, have_count);
    mov_int(e, 1, xreg(count), xreg(blen));
    bind_label(e, have_count);

    int out = tmp_int(e);
    load_imm(e, 1, xreg(out), 0);
    int by_length = new_label(e), negative = new_label(e), done = new_label(e);
    cbz_(e, 1, xreg(count), 0, by_length);

    CallArg args[3] = {
        { NULL, { CLS_INT, aptr }, 1, 0 },
        { NULL, { CLS_INT, bptr }, 1, 0 },
        { NULL, { CLS_INT, count }, 1, 0 },
    };
    SType* params[3] = { raw_pointer_type(), raw_pointer_type(), default_i64() };
    uint8_t muts[3] = { 0, 0, 0 };
    CallTarget tgt = { TK_EXTERN, "_memcmp", 7, 0 };
    EV cmp = emit_call(e, call, args, 3, params, muts, 3, 0, -1, tgt,
                       default_i64());
    ins(e, 0x7100001Fu | ((uint32_t)xreg(cmp.idx) << 5)); // cmp wresult, #0
    bcond_(e, C_EQ, by_length);
    bcond_(e, C_LT, negative);
    load_imm(e, 1, xreg(out), 1);
    br_(e, done);

    bind_label(e, by_length);
    ins(e, 0xEB00001Fu | ((uint32_t)xreg(blen) << 16) |
           ((uint32_t)xreg(alen) << 5)); // equal prefix: shorter slice sorts first
    bcond_(e, C_EQ, done);
    bcond_(e, C_LO, negative);
    load_imm(e, 1, xreg(out), 1);
    br_(e, done);

    bind_label(e, negative);
    load_imm(e, 1, xreg(out), UINT64_MAX);
    bind_label(e, done);

    if (out != si) mov_int(e, 1, xreg(si), xreg(out));
    e->vti = si + 1;
    e->vtf = sf;
    return (EV){ CLS_INT, si };
}

static EV load_printf_capture(Em* e, uint32_t off, SType* t) {
    if (!is_scalar(t)) {
        int a = tmp_int(e);
        frame_addr(e, xreg(a), off);
        return (EV){ CLS_ADDR, a };
    }
    int a = tmp_int(e);
    frame_addr(e, xreg(a), off);
    EV v = load_scalar(e, xreg(a), 0, t);
    return collapse(e, a, v);
}

static EV emit_printf_helper(Em* e, Node* at, Sym* helper, EV* values, int n) {
    if (!helper || !helper->type || helper->type->kind != SK_FUNC) {
        em_err(e, at, "missing checked fmt.printf helper");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    CallArg args[3];
    uint8_t muts[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        args[i].node = NULL;
        args[i].val = values[i];
        args[i].pre = 1;
        args[i].ptrval = 0;
    }
    SType* ft = helper->type;
    SType** targs = helper->spec_args;
    int ntargs = helper->spec_args ? helper->spec_nargs : 0;
    const char* mn = fn_mangle(helper->name, helper->len, ft,
                               helper->decl, targs, ntargs, targs != NULL);
    CallTarget tgt = { TK_INTERNAL, arena_copy(mn, strlen(mn)),
                       (uint32_t)strlen(mn), 0 };
    return emit_call(e, at, args, n, ft->params, muts, ft->nparams,
                     1, -1, tgt, ft->ret);
}

static EV gen_printf_builtin(Em* e, Node* call) {
    PrintfPlan* plan = (PrintfPlan*)call->aux;
    if (!plan) {
        em_err(e, call, "fmt.printf call has no checked format plan");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    int si = e->vti, sf = e->vtf;
    int nargs = call->nkids - 1;
    uint32_t* slots = (uint32_t*)arena_alloc(
        (size_t)(nargs ? nargs : 1) * sizeof(uint32_t));

    // A printf call observes ordinary call evaluation: all runtime arguments
    // are captured once, left-to-right, before the first output operation.
    for (int i = 0; i < nargs; i++) {
        Node* arg = call->kids[i + 1];
        if (arg->kind == N_MUTARG) arg = arg->a;
        SType* t = node_ty(arg);
        slots[i] = temp_slot(e, ty_size(t), ty_align(t));
        EV v = gen_expr(e, arg);
        store_to_slot(e, v, slots[i], t);
        e->vti = si;
        e->vtf = sf;
    }

    uint32_t total_slot = temp_slot(e, 8, 8);
    store_frame(e, 31, total_slot, 8);
    int done = new_label(e);

    for (int i = 0; i < plan->nparts; i++) {
        PrintfPart* part = &plan->parts[i];
        Sym* helper = (Sym*)part->helper;
        EV values[3];
        int nv = 0;
        int empty_success = -1;

        if (part->kind == PF_TEXT) {
            Node text_node;
            memset(&text_node, 0, sizeof text_node);
            text_node.kind = N_STR;
            text_node.path = call->path;
            text_node.line = call->line;
            text_node.col = call->col;
            text_node.str = plan->format->str + part->off;
            text_node.len = part->len;
            text_node.ty = node_ty(call->kids[0]);
            values[nv++] = gen_expr(e, &text_node);
        } else if (part->kind == PF_PERCENT) {
            int t = tmp_int(e);
            load_imm(e, 0, xreg(t), 37);
            values[nv++] = (EV){ CLS_INT, t };
        } else {
            Node* arg = call->kids[part->arg];
            if (arg->kind == N_MUTARG) arg = arg->a;
            SType* t = node_ty(arg);
            values[nv++] = load_printf_capture(e, slots[part->arg - 1], t);
            if (part->kind == PF_STRING) {
                empty_success = new_label(e);
                int len = tmp_int(e);
                ld_reg(e, xreg(len), xreg(values[0].idx), 8, 8, 0);
                cbz_(e, 1, xreg(len), 0, empty_success);
            }
            if (part->kind == PF_HEX_LOWER || part->kind == PF_HEX_UPPER) {
                int upper = tmp_int(e);
                load_imm(e, 0, xreg(upper), part->kind == PF_HEX_UPPER);
                values[nv++] = (EV){ CLS_INT, upper };
            } else if (part->kind == PF_FIXED) {
                int precision = tmp_int(e);
                load_imm(e, 1, xreg(precision), part->precision);
                values[nv++] = (EV){ CLS_INT, precision };
            }
        }

        EV written = emit_printf_helper(e, call, helper, values, nv);
        ins(e, 0xF100001Fu | ((uint32_t)xreg(written.idx) << 5)); // cmp n, #0
        int success = new_label(e);
        bcond_(e, C_GT, success);
        store_frame(e, xreg(written.idx), total_slot, 8);
        br_(e, done);
        bind_label(e, success);
        load_frame(e, 16, total_slot, 8, 0);
        ins(e, 0x8B000000u | ((uint32_t)xreg(written.idx) << 16) |
               (16u << 5) | 16u); // total += written
        store_frame(e, 16, total_slot, 8);
        if (empty_success >= 0) bind_label(e, empty_success);
        e->vti = si;
        e->vtf = sf;
    }

    bind_label(e, done);
    e->vti = si;
    e->vtf = sf;
    int out = tmp_int(e);
    load_frame(e, xreg(out), total_slot, 8, 0);
    return (EV){ CLS_INT, out };
}

static EV gen_call(Em* e, Node* call) {
    Node* callee = call->a;
    // Explicit contract construction: Shape(&value) / Shape[T](&value).
    if (call->d && call->d->kind == N_CONFORMANCE) {
        SType* pt = node_ty(call->kids[0]);
        SType* conc = pt && pt->kind == SK_PTR ? pt->base : NULL;
        SType* contract = node_ty(call);
        ESym* vs = emit_vtable(e, conc, contract, call->d);
        uint32_t off = temp_slot(e, 16, 8);
        EV d = gen_expr(e, call->kids[0]);
        int t = tmp_int(e);
        addr_of_sym(e, xreg(t), vs);
        frame_addr(e, 16, off);
        st_reg(e, xreg(t), 16, 0, 8);
        int dreg = xreg(d.idx);
        if (d.cls == CLS_ADDR) {
            ld_reg(e, 16, xreg(d.idx), 0, 8, 0);
            dreg = 16;
        }
        frame_addr(e, 17, off);
        st_reg(e, dreg, 17, 8, 8);
        frame_addr(e, xreg(t), off);
        return (EV){ CLS_ADDR, t };
    }

    // union variant construction: Event.key(42) / Pair[int].ok(v)
    if (callee->kind == N_FIELD_E && !call->sym) {
        SType* ut = union_type_of_base(callee->a);
        if (ut) {
            int variant = (int)callee->ival;
            if (variant >= ut->nfields || !ut->fields[variant].t) {
                em_err(e, call, "payloadless variant is a plain value, not a call");
                return (EV){ CLS_INT, tmp_int(e) };
            }
            if (call->nkids != 1) {
                em_err(e, call, "variant construction takes exactly one payload value");
                return (EV){ CLS_INT, tmp_int(e) };
            }
            Node* arg = call->kids[0]->kind == N_MUTARG ? call->kids[0]->a
                                                       : call->kids[0];
            EV pv = gen_expr(e, arg);
            return gen_union_construct(e, call, ut, variant, pv);
        }
    }

    // Compile-time type identity: a unique nonzero word, with no metadata.
    if (callee->kind == N_IDENT && !call->sym && callee->len == 6 &&
        memcmp(callee->str, "typeid", 6) == 0) {
        int t = tmp_int(e);
        load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }

    // Error(&const_global_descriptor): the descriptor address is the handle.
    if (callee->kind == N_IDENT && !call->sym && node_ty(callee) &&
        node_ty(callee)->kind == SK_ERROR) {
        return gen_expr(e, call->kids[0]);
    }

    // cast: i32(x), Meters(x), Color(1) — callee resolves to a type symbol
    if (callee->kind == N_IDENT && !call->sym &&
        ((node_sym(callee) && node_sym(callee)->kind == SYM_TYPE) ||
         (node_ty(callee) && node_ty(callee)->kind != SK_FUNC))) {
        Node* arg = call->kids[0];
        /* An untyped literal under a float cast materializes directly at the
           target width: its own untyped bit pattern is not a value. */
        if (node_ty(arg) && node_ty(arg)->kind == SK_UT && is_floatt(node_ty(call)) &&
            (arg->kind == N_INT || arg->kind == N_FLOAT)) {
            int vt = tmp_fp(e);
            if (arg->kind == N_FLOAT) {
                load_f64(e, unwrap(node_ty(call))->prim == P_F64, vreg(vt), arg->fval);
            } else {
                load_imm(e, 1, 16, (uint64_t)arg->ival);
                uint32_t scvtf = unwrap(node_ty(call))->prim == P_F64
                    ? 0x9E620000u : 0x9E220000u;
                ins(e, scvtf | (16u << 5) | (uint32_t)vreg(vt));
            }
            return (EV){ CLS_FP, vt };
        }
        EV v = gen_expr(e, call->kids[0]);
        return gen_cast(e, v, node_ty(call->kids[0]), node_ty(call));
    }
    // sizeof builtin (constant folded by sema)
    if (callee->kind == N_IDENT && !call->sym && callee->len == 6 &&
        memcmp(callee->str, "sizeof", 6) == 0) {
        int t = tmp_int(e);
        load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }
    Sym* intrinsic = call->sym ? node_sym(call) : NULL;
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_FMT_PRINTF && call->ival == BI_FMT_PRINTF)
        return gen_printf_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_ALLOC && call->ival == BI_MEM_ALLOC)
        return gen_alloc_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_REALLOC && call->ival == BI_MEM_REALLOC)
        return gen_realloc_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_RELEASE && call->ival == BI_MEM_RELEASE)
        return gen_release_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_ALLOC_T && call->ival == BI_MEM_ALLOC_T)
        return gen_alloc_typed_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_REALLOC_T && call->ival == BI_MEM_REALLOC_T)
        return gen_realloc_typed_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_RELEASE_T && call->ival == BI_MEM_RELEASE_T)
        return gen_release_builtin(e, call); // same lowering: free + null the lvalue
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_COPY && call->ival == BI_MEM_COPY)
        return gen_byte_write_builtin(e, call, BI_MEM_COPY);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_MOVE && call->ival == BI_MEM_MOVE)
        return gen_byte_write_builtin(e, call, BI_MEM_MOVE);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_FILL && call->ival == BI_MEM_FILL)
        return gen_byte_write_builtin(e, call, BI_MEM_FILL);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_ZERO && call->ival == BI_MEM_ZERO)
        return gen_byte_write_builtin(e, call, BI_MEM_ZERO);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_COMPARE && call->ival == BI_MEM_COMPARE)
        return gen_compare_builtin(e, call);
    // panic(message): write the bytes to stderr best-effort, then abort —
    // both undefined externals resolved by the system linker (spec Errors #15).
    // The write result is ignored: a failed or partial write never blocks the
    // abort, and nothing after it ever runs.
    if (callee->kind == N_IDENT && !call->sym && call->ival &&
        callee->len == 5 && memcmp(callee->str, "panic", 5) == 0) {
        EV v = gen_expr(e, call->kids[0]);
        if (v.cls != CLS_ADDR) {
            em_err(e, call, "panic message must be a string in memory");
            return v;
        }
        ld_reg(e, 16, xreg(v.idx), 0, 8, 0); // x16 = message bytes
        ld_reg(e, 17, xreg(v.idx), 8, 8, 0); // x17 = message length
        mov_int(e, 1, 1, 16);
        mov_int(e, 1, 2, 17);
        load_imm(e, 0, 0, 2); // w0 = 2 (stderr)
        emit_bl_extern(e, "_write", 6);
        emit_bl_extern(e, "_abort", 6); // noreturn
        return (EV){ CLS_INT, tmp_int(e) };
    }
    // generic instantiation call: bigger[int](3, 9) — find the stamp by signature
    if (callee->kind == N_INDEX && callee->sym) {
        Sym* gs = node_sym(callee);
        int nt = gs->decl->c ? gs->decl->c->nkids : 0;
        for (STypeInst* inst = sema_insts(gs->decl); inst; inst = inst->next) {
            if (inst->result != callee->ty) continue;
            const char* mn = fn_mangle(gs->name, gs->len, inst->result,
                                       gs->decl, inst->args, nt, 0);
            CallTarget tgt = { TK_INTERNAL, arena_copy(mn, strlen(mn)),
                               (uint32_t)strlen(mn), 0 };
            SType* ft = callee->ty;
            CallArg args[40];
            uint8_t muts[40];
            int n = call->nkids < 40 ? call->nkids : 40;
            if (call->nkids > 40) em_err(e, call, "too many arguments");
            for (int i = 0; i < n; i++) {
                args[i].node = call->kids[i];
                args[i].pre = 0;
                args[i].val = (EV){0, 0};
                args[i].ptrval = 0;
                muts[i] = 0;
                if (call->kids[i]->kind == N_MUTARG)
                    args[i].node = call->kids[i]->a;
            }
            for (int i = 0; i < n && i < gs->decl->nkids; i++)
                muts[i] = (gs->decl->kids[i]->flags & F_MUT) != 0;
            return emit_call(e, call, args, call->nkids, ft->params, muts,
                             ft->nparams, 1, -1, tgt, ft->ret);
        }
        em_err(e, call, "generic stamp not found");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    // contract dispatch: s.area() / mut s.scale(2.0)
    if (callee->kind == N_FIELD_E && !call->sym) {
        SType* it = node_ty(callee->a);
        if (it && it->kind == SK_CONTRACT) {
            int midx = -1;
            for (int i = 0; i < it->decl->nkids; i++) {
                Node* m = it->decl->kids[i];
                if (m->len == callee->len &&
                    memcmp(m->str, callee->str, callee->len) == 0) { midx = i; break; }
            }
            if (midx < 0) {
                em_err(e, call, "no such method");
                return (EV){ CLS_INT, tmp_int(e) };
            }
            // evaluate the contract value, load fn + data (spill-safe temps)
            spill_temps(e);
            EV iv = gen_expr(e, callee->a);
            if (iv.cls != CLS_ADDR) {
                em_err(e, call, "contract value must be in memory");
                return iv;
            }
            int vt = tmp_int(e), fn = tmp_int(e), data = tmp_int(e);
            ld_reg(e, xreg(vt), xreg(iv.idx), 0, 8, 0);              // vtable
            ld_reg(e, xreg(fn), xreg(vt), (uint32_t)midx * 8, 8, 0); // fn slot
            ld_reg(e, xreg(data), xreg(iv.idx), 8, 8, 0);            // data ptr
            static SType* trecv; // ABI-only receiver: always passed by address
            if (!trecv) {
                trecv = (SType*)arena_alloc(sizeof(SType));
                trecv->kind = SK_PRIM;
                trecv->prim = P_I64;
                trecv->size = trecv->align = 8;
            }
            CallArg args[40];
            SType* pty[40];
            uint8_t muts[40];
            int n = 1 + call->nkids;
            if (n > 40) { em_err(e, call, "too many arguments"); n = 40; }
            args[0].node = NULL;
            args[0].pre = 1;
            args[0].val = (EV){ CLS_INT, data };
            args[0].ptrval = 1;
            pty[0] = trecv;
            muts[0] = 1; // all method receivers use the uniform data-pointer ABI
            for (int i = 1; i < n; i++) {
                Node* a = call->kids[i - 1];
                args[i].node = a;
                args[i].pre = 0;
                args[i].val = (EV){0, 0};
                args[i].ptrval = 0;
                muts[i] = 0;
                if (a->kind == N_MUTARG) {
                    args[i].node = a->a;
                    muts[i] = 1;
                }
                pty[i] = node_ty(args[i].node);
            }
            SType* mret = node_ty(call);
            CallTarget tgt = { TK_REG, NULL, 0, fn };
            return emit_call(e, call, args, n, pty, muts, n, 1, -1, tgt, mret);
        }
    }
    // direct / concrete-method / indirect
    if (call->sym) {
        Sym* s = node_sym(call);

        if (s->kind == SYM_VAR) { // indirect call through a function value
            SType* ft = s->type;
            for (int i = 0; i < call->nkids; i++)
                if (call->kids[i]->kind == N_MUTARG)
                    em_err(e, call->kids[i],
                           "mut marks are not supported through function values");
            EV f = gen_expr(e, callee);
            CallArg args[40];
            uint8_t muts[40];
            int n = call->nkids < 40 ? call->nkids : 40;
            if (call->nkids > 40) em_err(e, call, "too many arguments");
            for (int i = 0; i < n; i++) {
                args[i].node = call->kids[i];
                args[i].pre = 0;
                args[i].val = (EV){0, 0};
                args[i].ptrval = 0;
                muts[i] = 0;
            }
            CallTarget tgt = { TK_REG, NULL, 0, f.idx };
            return emit_call(e, call, args, call->nkids, ft->params, muts,
                             ft->nparams, 1, -1, tgt, ft->ret);
        }

        if (s->kind == SYM_FUNC) {
            CallTarget tgt = { 0, NULL, 0, 0 };
            int c_boundary = s->is_extern;
            if (s->is_extern) {
                const char* cn = extern_c_name(s);
                char nm[512];
                snprintf(nm, sizeof nm, "_%s", cn);
                tgt.kind = TK_EXTERN;
                tgt.name = arena_copy(nm, strlen(nm));
                tgt.nlen = (uint32_t)strlen(nm);
            } else if (s->decl && (s->decl->flags & F_EXPO)) {
                // exported body: emitted under its unmangled C name with the C
                // convention — internal callers target the same symbol/ABI
                const char* cn = s->decl->cname ? s->decl->cname : s->name;
                char nm[512];
                snprintf(nm, sizeof nm, "_%s", cn);
                tgt.kind = TK_INTERNAL;
                tgt.name = arena_copy(nm, strlen(nm));
                tgt.nlen = (uint32_t)strlen(nm);
                c_boundary = 1;
            } else {
                SType* cft = node_ty(callee);
                if (!cft || cft->kind != SK_FUNC) cft = s->type;
                SType** targs = NULL;
                int ntargs = 0;
                if (s->is_generic && cft && cft->nparams > 0) {
                    SType* recv = cft->params[0];
                    targs = recv->args;
                    ntargs = recv->nargs;
                } else if (s->spec_args) {
                    targs = s->spec_args;
                    ntargs = s->spec_nargs;
                }
                const char* mn = fn_mangle(s->name, s->len, cft,
                                           s->decl, targs, ntargs,
                                           s->spec_args != NULL);
                tgt.kind = TK_INTERNAL;
                tgt.name = arena_copy(mn, strlen(mn));
                tgt.nlen = (uint32_t)strlen(mn);
            }
            SType* ft = node_ty(callee);
            if (!ft || ft->kind != SK_FUNC) ft = s->type;
            CallArg args[40];
            uint8_t muts[40];
            Node* argnodes[41];
            int n = 0;
            int recv_mut = 0;
            if (callee->kind == N_FIELD_E && !s->is_extern &&
                !(call->flags & F_METHOD)) { // instance method
                argnodes[0] = callee->a;
                recv_mut = (call->flags & F_MUT) != 0;
                n = 1;
            }
            for (int i = 0; i < call->nkids; i++) argnodes[n + i] = call->kids[i];
            n += call->nkids;
            if (n > 40) { em_err(e, call, "too many arguments"); n = 40; }
            for (int i = 0; i < n; i++) {
                args[i].node = argnodes[i];
                args[i].pre = 0;
                args[i].val = (EV){0, 0};
                args[i].ptrval = 0;
                muts[i] = 0;
                if (argnodes[i]->kind == N_MUTARG) {
                    args[i].node = argnodes[i]->a;
                    muts[i] = 1;
                }
            }
            if (callee->kind == N_FIELD_E && !s->is_extern && recv_mut)
                muts[0] = 1;
            for (int i = 0; i < n && s->decl && i < s->decl->nkids; i++)
                muts[i] = (s->decl->kids[i]->flags & F_MUT) != 0;
            if (s->decl && (s->decl->flags & F_METHOD) && n > 0)
                muts[0] = 1; // uniform method receiver ABI; semantic mut is separate
            int varargs_from = s->is_extern && s->decl &&
                               (s->decl->flags & F_VARARGS)
                ? ft->nparams : -1;
            return emit_call(e, call, args, n, ft->params, muts,
                             ft->nparams, !c_boundary, varargs_from, tgt,
                             ft->ret);
        }
    }
    em_err(e, call, "unsupported callee in codegen");
    return (EV){ CLS_INT, tmp_int(e) };
}

// --- expressions -----------------------------------------------------------------------

static EV gen_expr(Em* e, Node* x) {
    if (!x) {
        em_err(e, NULL, "missing expression in codegen");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    switch (x->kind) {
    case N_INT: case N_CHAR: {
        SType* t = node_ty(x);
        /*
         * An untyped integer literal that sema materialized as a float
         * operand (e.g. `r * 100` with r: f64) is a float value, not an
         * integer bit pattern.
         */
        if (t && t->kind != SK_UT && is_floatt(t)) {
            int vt = tmp_fp(e);
            load_f64(e, unwrap(t)->prim == P_F64, vreg(vt), (double)x->ival);
            return (EV){ CLS_FP, vt };
        }
        int sf = !t || t->kind == SK_UT || t->size == 8;
        int it = tmp_int(e);
        load_imm(e, sf, xreg(it), x->ival);
        return (EV){ CLS_INT, it };
    }
    case N_BOOL: {
        int it = tmp_int(e);
        load_imm(e, 0, xreg(it), x->ival);
        return (EV){ CLS_INT, it };
    }
    case N_FLOAT: {
        SType* t = node_ty(x);
        int is64 = !t || t->kind == SK_UT || unwrap(t)->prim == P_F64;
        int vt = tmp_fp(e);
        load_f64(e, is64, vreg(vt), x->fval);
        return (EV){ CLS_FP, vt };
    }
    case N_STR: {
        uint32_t off = temp_slot(e, 16, 8);
        ESym* s = intern_string(e, x);
        int t = tmp_int(e);
        addr_of_sym(e, xreg(t), s);
        frame_addr(e, 16, off);
        st_reg(e, xreg(t), 16, 0, 8);
        load_imm(e, 1, xreg(t), x->len);
        frame_addr(e, 16, off);
        st_reg(e, xreg(t), 16, 8, 8);
        frame_addr(e, xreg(t), off);
        return (EV){ CLS_ADDR, t };
    }
    case N_NULL: {
        SType* t = node_ty(x);
        SType* base = t;
        uint32_t size = t ? ty_size(t) : 8;
        if (base && (base->kind == SK_PTR || base->kind == SK_ERROR ||
                     (base->kind == SK_PRIM && base->prim == P_TYPEID))) {
            int it = tmp_int(e);
            load_imm(e, 1, xreg(it), 0);
            return (EV){ CLS_INT, it };
        }
        uint32_t off = temp_slot(e, size, 8);
        int it = tmp_int(e);
        frame_addr(e, 16, off);
        for (uint32_t o = 0; o < size; o += 8)
            st_reg(e, 31, 16, o, o + 8 <= size ? 8 : size - o); // str xzr
        frame_addr(e, xreg(it), off);
        return (EV){ CLS_ADDR, it };
    }
    case N_IDENT: {
        Sym* s = node_sym(x);
        if (!s) {
            em_err(e, x, "unresolved name");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        if (s->kind == SYM_FUNC)
            return (EV){ CLS_INT, gen_addr(e, x) };
        if (s->kind != SYM_VAR) {
            em_err(e, x, "not a value");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        SType* t = s->type;
        if (!t) {
            em_err(e, x, "variable has no type");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        if (is_scalar(t)) {
            int a = gen_var_addr(e, s, x);
            EV v = load_scalar(e, xreg(a), 0, t);
            return collapse(e, a, v);
        }
        return (EV){ CLS_ADDR, gen_var_addr(e, s, x) };
    }
    case N_TUPLE_E: {
        SType* t = node_ty(x);
        uint32_t size = t ? ty_size(t) : (uint32_t)(x->nkids * 8);
        uint32_t off = temp_slot(e, size, 8);
        for (int i = 0; i < x->nkids; i++) {
            SType* ft = t && t->kind == SK_TUPLE && i < t->nfields
                ? t->fields[i].t
                : node_ty(x->kids[i]);
            int svi = e->vti, svf = e->vtf;
            EV v = gen_expr(e, x->kids[i]);
            uint32_t member = t && t->kind == SK_TUPLE
                ? field_off(t, i) : (uint32_t)i * 8;
            int dst = 15;
            if (v.cls != CLS_FP && xreg(v.idx) >= 14) dst = xreg(v.idx) - 1;
            frame_addr(e, dst, off);
            store_value(e, v, dst, member, ft);
            e->vti = svi; e->vtf = svf;
        }
        int r = tmp_int(e);
        frame_addr(e, xreg(r), off);
        return (EV){ CLS_ADDR, r };
    }
    case N_ARRAYLIT: case N_STRUCTLIT: {
        SType* t = node_ty(x);
        if (!t) {
            em_err(e, x, "literal type missing");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        uint32_t off = temp_slot(e, ty_size(t), ty_align(t));
        if (x->kind == N_ARRAYLIT && x->a) {
            int svi = e->vti, svf = e->vtf;
            EV v = gen_expr(e, x->a);
            if (t->count > 0) {
                int dst = 15;
                if (v.cls != CLS_FP && xreg(v.idx) == dst) dst = 14;
                frame_addr(e, dst, off);
                store_value(e, v, dst, 0, t->base);
                for (uint64_t i = 1; i < t->count; i++) {
                    frame_addr(e, 15, off);
                    uint32_t elem_off = (uint32_t)i * ty_size(t->base);
                    if (elem_off <= 4095) {
                        ins(e, 0x91000000u | (elem_off << 10) | (15u << 5) | 14u);
                    } else {
                        load_imm(e, 1, 14, elem_off);
                        ins(e, 0x8B000000u | (14u << 16) | (15u << 5) | 14u);
                    }
                    copy_mem(e, 14, 0, 15, 0, ty_size(t->base));
                }
            }
            e->vti = svi; e->vtf = svf;
            int r = tmp_int(e);
            frame_addr(e, xreg(r), off);
            return (EV){ CLS_ADDR, r };
        }
        int positional = x->nkids > 0 && x->kids[0]->str == NULL;
        for (int i = 0; i < x->nkids; i++) {
            Node* fi = x->kids[i];
            Node* value = x->kind == N_ARRAYLIT ? fi : fi->a;
            int fidx = i;
            if (x->kind == N_STRUCTLIT && !positional) {
                fidx = find_field(t, fi->str, fi->len);
                if (fidx < 0) {
                    em_err(e, fi, "no such field");
                    continue;
                }
            }
            uint32_t fo = x->kind == N_ARRAYLIT
                ? (uint32_t)fidx * ty_size(t->base)
                : field_off(t, fidx);
            int svi = e->vti, svf = e->vtf;
            EV v = gen_expr(e, value);
            int dst = 15;
            if (v.cls != CLS_FP && xreg(v.idx) >= 14) dst = xreg(v.idx) - 1;
            frame_addr(e, dst, off);
            store_value(e, v, dst, fo,
                        x->kind == N_ARRAYLIT ? t->base : t->fields[fidx].t);
            e->vti = svi; e->vtf = svf;
        }
        int r = tmp_int(e);
        frame_addr(e, xreg(r), off);
        return (EV){ CLS_ADDR, r };
    }
    case N_UNARY:
        if (x->op == T_AMP)
            return (EV){ CLS_INT, gen_addr(e, x) };
        if (x->op == T_TILDE) {
            SType* t = lvalue_type(e, x);
            int a = gen_addr(e, x);
            if (t && is_scalar(t)) {
                EV v = load_scalar(e, xreg(a), 0, t);
                return collapse(e, a, v);
            }
            return (EV){ CLS_ADDR, a };
        }
        if (x->op == T_MINUS) {
            uint64_t c;
            if (const_int(x, &c)) {
                SType* t = node_ty(x);
                int sf = !t || t->size != 4;
                int it = tmp_int(e);
                load_imm(e, sf, xreg(it), c);
                return (EV){ CLS_INT, it };
            }
            SType* t = node_ty(x->a);
            EV v = gen_expr(e, x->a);
            if (v.cls == CLS_FP) {
                int is64 = !t || unwrap(t)->prim == P_F64;
                ins(e, (is64 ? 0x1E614000u : 0x1E214000u) |
                       ((uint32_t)vreg(v.idx) << 16) | (uint32_t)vreg(v.idx));
                return v;
            }
            int sf = !t || scalar_bits(t) == 64;
            ins(e, (sf ? 0xCB0003E0u : 0x4B0003E0u) |
                   ((uint32_t)xreg(v.idx) << 16) | (uint32_t)xreg(v.idx));
            return v;
        }
        if (x->op == T_BANG) {
            SType* t = node_ty(x->a);
            EV v = gen_expr(e, x->a);
            if (v.cls == CLS_FP) {
                em_err(e, x, "'!' needs bool or integer operands");
                return v;
            }
            if (t && is_boolt(t)) { // logical not
                ins(e, 0x7100001Fu | ((uint32_t)xreg(v.idx) << 5)); // cmp w, #0
                cset_(e, 0, C_EQ, xreg(v.idx));
                return v;
            }
            int sf = !t || scalar_bits(t) == 64;
            ins(e, (sf ? 0xAA2003E0u : 0x2A2003E0u) |
                   ((uint32_t)xreg(v.idx) << 16) | (uint32_t)xreg(v.idx)); // mvn
            return v;
        }
        break;
    case N_BINARY:
        if (x->op == T_ANDAND || x->op == T_OROR) return gen_shortcircuit(e, x);
        return gen_binop(e, x);
    case N_CMPCHAIN:
        return gen_cmpchain(e, x);
    case N_FIELD_E: {
        Sym* ts = node_sym(x->a); // enum member: Color.red
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ENUM) {
            int it = tmp_int(e);
            load_imm(e, 0, xreg(it), x->ival);
            return (EV){ CLS_INT, it };
        }
        // union variant value (payloadless): Event.quit / Pair[int].none
        {
            SType* ut = union_type_of_base(x->a);
            if (ut) {
                int vi = (int)x->ival;
                if (vi >= ut->nfields || ut->fields[vi].t) {
                    em_err(e, x, "payload variant needs a constructor call");
                    return (EV){ CLS_INT, tmp_int(e) };
                }
                return gen_union_construct(e, x, ut, vi, (EV){ CLS_INT, 0 });
            }
        }
        SType* seq = unwrap(node_ty(x->a));
        while (seq && seq->kind == SK_PTR) seq = unwrap(seq->base);
        if (seq && seq->kind == SK_ARRAY && x->len == 3 &&
            memcmp(x->str, "len", 3) == 0) {
            int it = tmp_int(e);
            load_imm(e, 1, xreg(it), x->ival);
            return (EV){ CLS_INT, it };
        }
        SType* bt = node_ty(x->a);
        if (bt && unwrap(bt) && unwrap(bt)->kind == SK_CONTRACT) {
            em_err(e, x, "contract method values must be called");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        SType* t = node_ty(x);
        if (!t) t = lvalue_type(e, x);
        int a = gen_addr(e, x);
        if (t && is_scalar(t)) {
            EV v = load_scalar(e, xreg(a), 0, t);
            return collapse(e, a, v);
        }
        return (EV){ CLS_ADDR, a };
    }
    case N_INDEX: {
        SType* t = node_ty(x);
        if (!t) t = lvalue_type(e, x);
        int a = gen_addr(e, x);
        if (t && is_scalar(t)) {
            EV v = load_scalar(e, xreg(a), 0, t);
            return collapse(e, a, v);
        }
        return (EV){ CLS_ADDR, a };
    }
    case N_SLICE_E: {
        SType* bt = lvalue_type(e, x->a);
        bt = bt ? unwrap(bt) : NULL;
        int is_string = bt && bt->kind == SK_PRIM && bt->prim == P_STRING;
        if (!bt || (bt->kind != SK_SLICE && bt->kind != SK_ARRAY && !is_string)) {
            em_err(e, x, "slice of a non-sequence");
            return (EV){ CLS_ADDR, tmp_int(e) };
        }
        uint32_t esz = is_string ? 1 : ty_size(bt->base);
        uint32_t off = temp_slot(e, 16, 8);
        uint32_t lo_slot = temp_slot(e, 8, 8), hi_slot = temp_slot(e, 8, 8);
        int svi = e->vti, svf = e->vtf;
        int base;
        if (is_lvalue_node(x->a)) base = gen_addr(e, x->a);
        else {
            EV v = gen_expr(e, x->a);
            base = v.idx;
        }
        // hi (evaluated once; open-ended reads the sequence length)
        if (x->c) {
            EV hi = gen_expr(e, x->c);
            widen_idx(e, x->c, xreg(hi.idx));
            store_frame(e, xreg(hi.idx), hi_slot, 8);
        } else if (bt->kind == SK_ARRAY) {
            load_imm(e, 1, 16, bt->count);
            store_frame(e, 16, hi_slot, 8);
        } else {
            ld_reg(e, 16, base, 8, 8, 0); // slice/string length word
            store_frame(e, 16, hi_slot, 8);
        }
        // lo (evaluated once; defaults to 0)
        if (x->b) {
            EV lo = gen_expr(e, x->b);
            widen_idx(e, x->b, xreg(lo.idx));
            store_frame(e, xreg(lo.idx), lo_slot, 8);
        } else {
            load_imm(e, 1, 16, 0);
            store_frame(e, 16, lo_slot, 8);
        }
        // data pointer (slice/string: load; array: the base address)
        if (bt->kind != SK_ARRAY)
            ld_reg(e, base, base, 0, 8, 0);
        // ptr = data + lo*esz
        load_frame(e, 16, lo_slot, 8, 0);
        if (esz > 1) {
            load_imm(e, 1, 17, esz);
            ins(e, 0x9B007C00u | (17u << 16) | (16u << 5) | 16u); // mul x16
        }
        ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)base << 5) |
               (uint32_t)base);
        frame_addr(e, 17, off);
        st_reg(e, base, 17, 0, 8);
        // len = hi - lo
        load_frame(e, 16, hi_slot, 8, 0);
        load_frame(e, 17, lo_slot, 8, 0);
        ins(e, 0xCB000000u | (17u << 16) | (16u << 5) | 16u); // sub x16, x16, x17
        frame_addr(e, 17, off);
        st_reg(e, 16, 17, 8, 8);
        e->vti = svi; e->vtf = svf;
        int r = tmp_int(e);
        frame_addr(e, xreg(r), off);
        return (EV){ CLS_ADDR, r };
    }
    case N_CALL:
        return gen_call(e, x);
    default:
        break;
    }
    em_err(e, x, "codegen for '%s' expressions comes in the next chunk",
           node_kind_name((NKind)x->kind));
    return (EV){ CLS_INT, tmp_int(e) };
}

// --- statements -----------------------------------------------------------------------

static void store_to_slot(Em* e, EV v, uint32_t off, SType* t) {
    if (v.cls == CLS_ADDR) {
        frame_addr(e, 15, off);
        copy_mem(e, 15, 0, xreg(v.idx), 0, ty_size(t));
    } else if (v.cls == CLS_FP) {
        store_frame_fp(e, vreg(v.idx), off,
                       is_scalar(t) ? unwrap(t)->prim == P_F64 : 1);
    } else {
        store_frame(e, xreg(v.idx), off, (uint32_t)((scalar_bits(t) + 7) / 8));
    }
}

static void emit_block(Em* e, Node* b) {
    for (int i = 0; i < b->nkids; i++) emit_stmt(e, b->kids[i]);
}

// --- deferred calls ---------------------------------------------------------------------
/*
 * `defer` lowers by inline expansion (no runtime counter or epilogue loop):
 * the defer statement captures its call, and every exit point re-emits the
 * captured sequence in reverse immediately before the epilogue. Capture is
 * two-tier: read-only arguments are fully evaluated at the defer statement
 * and snapshotted into private frame slots, while `mut`-marked arguments and
 * `mut`-marked method receivers capture the lvalue address, so the exit-time
 * call reads the argument's final storage. A defer after a top-level return
 * captures into dead code and never fires. `panic` never returns, so it runs
 * no defers. Because a deferred call clobbers the temp pools and the result
 * registers, an already-marshaled return value parks in a frame save area
 * across the expansion.
 */

// fresh emitter-internal lvalue backed by a frame slot: gen_addr yields the
// slot's address (or, when byaddr, the pointer stored inside it), so the
// ordinary call path marshals the deferred argument without re-evaluating
// its expression at exit time.
static Node* defer_slot_ident(Em* e, Node* src, SType* t, int byaddr) {
    Node* id = (Node*)arena_alloc(sizeof(Node));
    memset(id, 0, sizeof *id);
    id->kind = N_IDENT;
    id->path = src ? src->path : NULL;
    id->line = src ? src->line : 0;
    id->col = src ? src->col : 0;
    Sym* s = (Sym*)arena_alloc(sizeof(Sym));
    memset(s, 0, sizeof *s);
    s->name = "defer";
    s->len = 5;
    s->kind = SYM_VAR;
    s->type = t;
    s->decl = id; // the ident doubles as the slot key
    id->sym = s;
    id->ty = t;
    slot_alloc(e, id, byaddr ? 8 : ty_size(t), byaddr ? 8 : ty_align(t));
    if (byaddr) e->slots[e->nslots - 1].byref = 1;
    return id;
}

// capture one deferred argument at the defer statement: byaddr stores the
// argument lvalue's address (exit sees final storage), otherwise the value
// is snapshotted (exit sees the defer-time state). Returns the synthetic
// lvalue the exit-time call reads.
static Node* defer_capture_arg(Em* e, Node* a, int byaddr) {
    Node* inner = a->kind == N_MUTARG ? a->a : a;
    SType* t = node_ty(inner);
    Node* id = defer_slot_ident(e, inner, t, byaddr);
    uint32_t off = slot_find(e, id)->off;
    if (byaddr) {
        int ta = gen_addr(e, inner);
        store_frame(e, xreg(ta), off, 8);
    } else {
        EV v = gen_expr(e, inner);
        store_to_slot(e, v, off, t);
    }
    return id;
}

// rewrap a mut-captured argument so every call path sees the original
// call-site mut mark
static Node* defer_mutarg(Node* a, Node* id) {
    Node* w = (Node*)arena_alloc(sizeof(Node));
    memset(w, 0, sizeof *w);
    w->kind = N_MUTARG;
    w->path = a->path;
    w->line = a->line;
    w->col = a->col;
    w->a = id;
    w->ty = id->ty;
    return w;
}

/*
 * Capture a top-level `defer` statement's call. The synthetic call copies the
 * original's symbol, builtin marker, flags, and printf plan, so the ordinary
 * gen_call dispatch lowers the exit-time call unchanged; only the argument
 * nodes are replaced by capture-slot lvalues. Instance-method and
 * contract-method receivers are captured with the call's `mut` mark deciding
 * address vs snapshot, exactly like `mut`-marked arguments. An indirect call
 * through a function value snapshots the callee value itself at the defer
 * statement, so a later reassignment of the variable cannot change which
 * function the exit-time call dispatches to.
 */
static void defer_capture(Em* e, Node* s) {
    Node* call = s->a; // N_CALL (sema-checked)
    Node* sc = (Node*)arena_alloc(sizeof(Node));
    memset(sc, 0, sizeof *sc);
    sc->kind = N_CALL;
    sc->flags = call->flags;
    sc->sym = call->sym;
    sc->ival = call->ival;
    sc->ty = call->ty;
    sc->aux = call->aux;
    sc->path = call->path;
    sc->line = call->line;
    sc->col = call->col;

    Node* callee = call->a;
    Sym* tgt = call->sym ? node_sym(call) : NULL;
    // instance mirrors the direct call path's receiver-prepend guard: only a
    // non-extern SYM_FUNC callee with an unqualified field callee receives
    // the implicit receiver (a SYM_VAR callee is a function-value field call
    // whose field expression is the callee itself, not a receiver)
    int instance = callee->kind == N_FIELD_E && tgt && tgt->kind == SYM_FUNC &&
        !tgt->is_extern && !(call->flags & F_METHOD);
    SType* baset = callee->kind == N_FIELD_E ? node_ty(callee->a) : NULL;
    int contract = callee->kind == N_FIELD_E && !call->sym && baset &&
        baset->kind == SK_CONTRACT;
    if (instance || contract) {
        Node* rid = defer_capture_arg(e, callee->a, (call->flags & F_MUT) != 0);
        Node* scallee = (Node*)arena_alloc(sizeof(Node));
        memset(scallee, 0, sizeof *scallee);
        scallee->kind = N_FIELD_E;
        scallee->str = callee->str;
        scallee->len = callee->len;
        scallee->sym = callee->sym;
        scallee->ty = callee->ty;
        scallee->path = callee->path;
        scallee->line = callee->line;
        scallee->col = callee->col;
        scallee->a = rid;
        sc->a = scallee;
    } else if (tgt && tgt->kind == SYM_VAR) {
        // Indirect call through a function value: the callee is part of what
        // gets called, so it snapshots at the defer statement like a
        // read-only argument and the exit-time call dispatches through the
        // snapshot instead of re-reading the variable.
        sc->a = defer_capture_arg(e, callee, 0);
    } else {
        sc->a = callee;
    }
    sc->nkids = call->nkids;
    if (call->nkids) {
        sc->kids = (Node**)arena_alloc((size_t)call->nkids * sizeof(Node*));
        for (int i = 0; i < call->nkids; i++) {
            Node* a = call->kids[i];
            if (a->kind == N_MUTARG)
                sc->kids[i] = defer_mutarg(a, defer_capture_arg(e, a, 1));
            else
                sc->kids[i] = defer_capture_arg(e, a, 0);
        }
    }
    if (e->ndefers == e->cdefers) {
        e->cdefers = e->cdefers ? e->cdefers * 2 : 4;
        e->defers = realloc(e->defers, (size_t)e->cdefers * sizeof *e->defers);
        if (!e->defers) oom();
    }
    e->defers[e->ndefers].call = sc;
    e->defers[e->ndefers].at = s;
    e->ndefers++;
}

// re-emit the captured calls, last defer first, before an epilogue. `ret` is
// the function's return type (NULL when no result is live, e.g. fall-off):
// result-register units park in a frame save area across the calls.
static void emit_defers(Em* e, SType* ret) {
    if (!e->ndefers) return;
    RetCls rc;
    classify_ret(ret, &rc);
    int save = !rc.sret && rc.nunits > 0;
    if (save) {
        if (!e->retsave_off) e->retsave_off = temp_slot(e, 4 * 8, 8);
        for (int w = 0; w < rc.nunits; w++) {
            TU* u = &rc.units[w];
            if (u->kind == TU_XREG)
                store_frame(e, u->reg, e->retsave_off + (uint32_t)w * 8, 8);
            else
                store_frame_fp(e, u->reg, e->retsave_off + (uint32_t)w * 8,
                               u->size == 8);
        }
    }
    for (int i = e->ndefers - 1; i >= 0; i--) {
        line_mark(e, e->defers[i].at); // exit-time marks cite the defer line
        e->vti = 0;
        e->vtf = 0;
        gen_expr(e, e->defers[i].call); // results discarded
    }
    if (save) {
        for (int w = 0; w < rc.nunits; w++) {
            TU* u = &rc.units[w];
            if (u->kind == TU_XREG)
                load_frame(e, u->reg, e->retsave_off + (uint32_t)w * 8, 8, 0);
            else
                load_frame_fp(e, u->reg, e->retsave_off + (uint32_t)w * 8,
                              u->size == 8);
        }
    }
}

// copy member i of a tuple result (at address `ra`) into frame offset `off`
static void store_member(Em* e, int ra, SType* tup, int i, uint32_t off) {
    SType* ft = tup && tup->kind == SK_TUPLE && i < tup->nfields
        ? tup->fields[i].t : NULL;
    uint32_t mo = tup && tup->kind == SK_TUPLE ? field_off(tup, i) : (uint32_t)i * 8;
    if (!ft) {
        em_err(e, NULL, "missing tuple member type");
        return;
    }
    if (is_scalar(ft)) {
        EV v = load_scalar(e, ra, mo, ft);
        if (v.cls == CLS_FP) store_frame_fp(e, vreg(v.idx), off,
                                            unwrap(ft)->prim == P_F64);
        else store_frame(e, xreg(v.idx), off, (uint32_t)((scalar_bits(ft) + 7) / 8));
    } else {
        frame_addr(e, 15, off);
        copy_mem(e, 15, 0, ra, mo, ty_size(ft));
    }
}

static int case_falls_through(Node* c) {
    Node* b = c ? c->b : NULL;
    return b && b->nkids > 0 && b->kids[b->nkids - 1]->kind == N_FALLTHROUGH;
}

/*
 * Union switch: the scrutinee's address is parked in a stack slot (arm bodies
 * reuse the temp registers), each pattern compares the i32 tag, and matching
 * arms copy their payload binder before running the body. No else means no
 * arm runs when nothing matches.
 */
static void emit_union_switch(Em* e, Node* s, SType* ut) {
    EV v = gen_expr(e, s->a);
    if (v.cls != CLS_ADDR) {
        em_err(e, s, "union scrutinee must materialize in memory");
        return;
    }
    uint32_t sslot = temp_slot(e, 8, 8);
    store_frame(e, xreg(v.idx), sslot, 8);
    uint32_t poff = sema_union_payload_off(ut);
    if (s->nkids > 64) {
        em_err(e, s, "too many switch cases (max 64)");
        return;
    }
    int lcase[64], lend = new_label(e);
    int has_else = 0;
    for (int i = 0; i < s->nkids; i++) {
        lcase[i] = new_label(e);
        if (s->kids[i]->flags & F_ELSE) {
            has_else = 1;
            continue;
        }
        for (int j = 0; j < s->kids[i]->nkids; j++) {
            Node* pat = s->kids[i]->kids[j];
            uint32_t vi = (uint32_t)pat->ival;
            load_frame(e, 16, sslot, 8, 0);
            ld_reg(e, 17, 16, 0, 4, 0); // tag
            ins(e, 0x7100001Fu | ((vi & 0xFFFu) << 10) | (17u << 5)); // cmp w17, #vi
            bcond_(e, C_EQ, lcase[i]);
        }
    }
    if (!has_else) br_(e, lend);
    for (int i = 0; i < s->nkids; i++) {
        Node* c = s->kids[i];
        bind_label(e, lcase[i]);
        if (!(c->flags & F_ELSE)) {
            for (int j = 0; j < c->nkids; j++) {
                Node* pat = c->kids[j];
                if (pat->kind != N_VARPAT || !pat->b) continue;
                int vi = (int)pat->ival;
                SType* pt = vi < ut->nfields ? ut->fields[vi].t : NULL;
                if (!pt) continue;
                Sym* bs = node_sym(pat->b);
                uint32_t bslot = slot_alloc(e, bs && bs->decl ? bs->decl : pat->b,
                                            ty_size(pt), ty_align(pt));
                load_frame(e, 14, sslot, 8, 0); // scrutinee (x14: copy_mem-safe)
                frame_addr(e, 15, bslot);
                copy_mem(e, 15, 0, 14, poff, ty_size(pt)); // binder = payload copy
            }
        }
        emit_block(e, c->b);
        if (i != s->nkids - 1 && !case_falls_through(c)) br_(e, lend);
    }
    bind_label(e, lend);
}

static void emit_stmt(Em* e, Node* s) {
    line_mark(e, s->kind == N_EXPRSTMT ? s->a : s);
    e->vti = 0; e->vtf = 0; // statement boundary
    switch (s->kind) {
    case N_VAR: {
        SType* wants[16];
        uint32_t offs[16];
        int nb = s->nkids < 16 ? s->nkids : 16;
        if (s->nkids > 16) {
            em_err(e, s, "too many binders (max 16)");
            return;
        }
        for (int i = 0; i < nb; i++) {
            Node* binder = s->kids[i];
            Sym* vs = node_sym(binder);
            wants[i] = vs && vs->type ? vs->type : sema_build_type(binder->a);
            offs[i] = slot_alloc(e, binder, ty_size(wants[i]), ty_align(wants[i]));
        }
        if (!s->b) return;
        if (nb == 1) {
            EV v = gen_expr(e, s->b);
            store_to_slot(e, v, offs[0], wants[0]);
            return;
        }
        // typed destructuring: rhs is a tuple in memory
        EV rhs = gen_expr(e, s->b);
        if (rhs.cls != CLS_ADDR) {
            em_err(e, s, "destructuring initializer must be a tuple");
            return;
        }
        SType* tup = node_ty(s->b);
        for (int i = 0; i < nb; i++)
            store_member(e, xreg(rhs.idx), tup, i, offs[i]);
        return;
    }
    case N_ASSIGN: {
        if (s->nkids > 1) { // multi-assignment from a tuple
            EV rhs = gen_expr(e, s->a);
            if (rhs.cls != CLS_ADDR) {
                em_err(e, s, "multi-assignment right side must be a tuple");
                return;
            }
            SType* tup = node_ty(s->a);
            for (int i = 0; i < s->nkids && i < 16; i++) {
                SType* tt = lvalue_type(e, s->kids[i]);
                int svi = e->vti;
                int ta = gen_addr(e, s->kids[i]);
                if (tt && is_scalar(tt)) {
                    // load member then store through the address
                    SType* ft = tup && tup->kind == SK_TUPLE && i < tup->nfields
                        ? tup->fields[i].t : tt;
                    EV v = load_scalar(e, xreg(rhs.idx),
                                       tup && tup->kind == SK_TUPLE
                                           ? field_off(tup, i) : (uint32_t)i * 8, ft);
                    store_scalar(e, v, xreg(ta), 0, ft);
                } else if (tt) {
                    uint32_t mo = tup && tup->kind == SK_TUPLE
                        ? field_off(tup, i) : (uint32_t)i * 8;
                    copy_mem(e, xreg(ta), 0, xreg(rhs.idx), mo, ty_size(tt));
                }
                e->vti = svi;
            }
            return;
        }
        SType* t = lvalue_type(e, s->kids[0]);
        if (!t) {
            em_err(e, s, "assignment target has no type");
            return;
        }
        int ta = gen_addr(e, s->kids[0]);
        if (s->op != T_ASSIGN) { // compound: x = x op y, address evaluated once
            if (!is_scalar(t)) {
                em_err(e, s, "compound assignment needs a scalar target");
                return;
            }
            EV old = load_scalar(e, xreg(ta), 0, t);
            EV rhs = gen_expr(e, s->a);
            if (old.cls == CLS_FP) {
                emit_farith(e, compound_op((TokKind)s->op), old.idx, old.idx, rhs.idx,
                            unwrap(t)->prim == P_F64);
                strf_(e, vreg(old.idx), xreg(ta), 0, unwrap(t)->prim == P_F64);
            } else {
                emit_arith(e, compound_op((TokKind)s->op), old.idx, old.idx, rhs.idx, t);
                store_scalar(e, old, xreg(ta), 0, t);
            }
            return;
        }
        EV v = gen_expr(e, s->a);
        store_value(e, v, xreg(ta), 0, t);
        return;
    }
    case N_EXPRSTMT:
        gen_expr(e, s->a);
        return;
    case N_DEFER: {
        // sema restricts defer to direct statements of the body block; the
        // emitter re-checks because a nested defer would capture at the wrong
        // point and expand on unrelated exits.
        int top = 0;
        for (int i = 0; e->fnbody && i < e->fnbody->nkids; i++)
            if (e->fnbody->kids[i] == s) {
                top = 1;
                break;
            }
        if (!top) {
            em_err(e, s, "defer must be a top-level statement of the function body");
            return;
        }
        defer_capture(e, s);
        return;
    }
    case N_IF: {
        EV c = gen_expr(e, s->a);
        if (c.cls != CLS_INT) {
            em_err(e, s, "if condition must be bool");
            return;
        }
        int l_else = new_label(e), l_end = new_label(e);
        cbz_(e, 0, xreg(c.idx), 0, l_else);
        emit_block(e, s->b);
        if (s->c) {
            br_(e, l_end);
            bind_label(e, l_else);
            if (s->c->kind == N_IF) emit_stmt(e, s->c);
            else emit_block(e, s->c);
            bind_label(e, l_end);
        } else {
            bind_label(e, l_else);
        }
        return;
    }
    case N_FOR: {
        Node* value_binder = s->kids[s->nkids - 1];
        SType* elem = sema_build_type(value_binder->a);
        Sym* lv = node_sym(value_binder);
        uint32_t esz = ty_size(elem);
        uint32_t vs = slot_alloc(e, lv && lv->decl ? lv->decl : value_binder,
                                 esz, ty_align(elem));
        int lbreak = new_label(e), lcont = new_label(e), lcond = new_label(e);
        if (s->b->kind == N_RANGE) { // index loop over a range
            SType* rt = node_ty(s->b);
            if (!rt || rt->kind == SK_UT) rt = default_i64();
            uint32_t rsz = ty_size(rt);
            int rbits = scalar_bits(rt);
            uint32_t islot = temp_slot(e, rsz, 8), hslot = temp_slot(e, rsz, 8);
            uint32_t cslot = temp_slot(e, 8, 8), dslot = temp_slot(e, 8, 8);
            uint32_t sslot = temp_slot(e, 8, 8);
            int svi = e->vti;
            EV lo = gen_expr(e, s->b->a);
            store_frame(e, xreg(lo.idx), islot, rsz);
            EV hi = gen_expr(e, s->b->b);
            store_frame(e, xreg(hi.idx), hslot, rsz);
            if (s->b->c) {
                EV step = gen_expr(e, s->b->c);
                store_frame(e, xreg(step.idx), sslot, 8);
            } else {
                load_imm(e, 1, 16, 1);
                store_frame(e, 16, sslot, 8);
            }
            e->vti = svi;
            int unsig = !ty_is_signed(rt);
            int ldesc = new_label(e), lempty = new_label(e), lsetup = new_label(e);
            load_frame(e, 16, islot, rsz, ty_is_signed(rt) && rsz < 8);
            load_frame(e, 17, hslot, rsz, ty_is_signed(rt) && rsz < 8);
            if (!unsig && rbits < 64) {
                uint32_t sx = sxtw_of(rbits);
                ins(e, sx | (16u << 5) | 16u);
                ins(e, sx | (17u << 5) | 17u);
            }
            ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5)); // cmp first, second
            bcond_(e, C_EQ, lempty);
            bcond_(e, unsig ? C_HI : C_GT, ldesc);
            // Ascending: cursor = lower; count = ceil((upper-lower)/step).
            ins(e, 0xCB000000u | (16u << 16) | (17u << 5) | 15u);
            load_frame(e, 17, sslot, 8, 0);
            ins(e, 0xD1000000u | (1u << 10) | (15u << 5) | 15u);
            ins(e, 0x9AC00800u | (17u << 16) | (15u << 5) | 15u);
            ins(e, 0x91000000u | (1u << 10) | (15u << 5) | 15u);
            store_frame(e, 15, cslot, 8);
            store_frame(e, 17, dslot, 8);
            br_(e, lsetup);
            bind_label(e, ldesc);
            // Descending selects the same lower-anchored values in reverse.
            load_frame(e, 16, islot, rsz, ty_is_signed(rt) && rsz < 8);
            load_frame(e, 17, hslot, rsz, ty_is_signed(rt) && rsz < 8);
            if (!unsig && rbits < 64) {
                uint32_t sx = sxtw_of(rbits);
                ins(e, sx | (16u << 5) | 16u);
                ins(e, sx | (17u << 5) | 17u);
            }
            ins(e, 0xCB000000u | (17u << 16) | (16u << 5) | 15u);
            load_frame(e, 17, sslot, 8, 0);
            ins(e, 0xD1000000u | (1u << 10) | (15u << 5) | 15u);
            ins(e, 0x9AC00800u | (17u << 16) | (15u << 5) | 15u);
            ins(e, 0x9B007C00u | (17u << 16) | (15u << 5) | 16u);
            load_frame(e, 17, hslot, rsz, ty_is_signed(rt) && rsz < 8);
            if (!unsig && rbits < 64) {
                uint32_t sx = sxtw_of(rbits);
                ins(e, sx | (17u << 5) | 17u);
            }
            ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
            store_frame(e, 16, islot, rsz);
            ins(e, 0x91000000u | (1u << 10) | (15u << 5) | 15u);
            store_frame(e, 15, cslot, 8);
            load_frame(e, 17, sslot, 8, 0);
            ins(e, 0xCB000000u | (17u << 16) | (31u << 5) | 17u);
            store_frame(e, 17, dslot, 8);
            br_(e, lsetup);
            bind_label(e, lempty);
            load_imm(e, 1, 16, 0);
            store_frame(e, 16, cslot, 8);
            load_frame(e, 17, sslot, 8, 0);
            store_frame(e, 17, dslot, 8);
            bind_label(e, lsetup);
            bind_label(e, lcond);
            int tc = tmp_int(e);
            load_frame(e, xreg(tc), cslot, 8, 0);
            cbz_(e, 0, xreg(tc), 0, lbreak);
            int ti = tmp_int(e);
            load_frame(e, xreg(ti), islot, rsz, ty_is_signed(rt) && rsz < 8);
            // loop variable = i (truncating/extending store)
            if (scalar_bits(elem) <= rbits)
                store_frame(e, xreg(ti), vs, (uint32_t)((scalar_bits(elem) + 7) / 8));
            else {
                if (ty_is_signed(rt)) ins(e, 0x93407C00u | (xreg(ti) << 5) | xreg(ti));
                else ins(e, 0xD3407C00u | (xreg(ti) << 5) | xreg(ti));
                store_frame(e, xreg(ti), vs, 8);
            }
            e->vti = 0; e->vtf = 0;
            if (e->nctx >= 64) {
                em_err(e, s, "control flow nested too deeply");
                return;
            }
            e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, NULL, 0, 1 };
            emit_block(e, s->c);
            e->nctx--;
            bind_label(e, lcont);
            int ti2 = tmp_int(e);
            load_frame(e, xreg(ti2), islot, rsz, ty_is_signed(rt) && rsz < 8);
            int td = tmp_int(e);
            load_frame(e, xreg(td), dslot, 8, 0);
            ins(e, 0x8B000000u | (xreg(td) << 16) |
                   (xreg(ti2) << 5) | xreg(ti2));
            store_frame(e, xreg(ti2), islot, rsz);
            int tc2 = tmp_int(e);
            load_frame(e, xreg(tc2), cslot, 8, 0);
            ins(e, 0xD1000000u | (1u << 10) |
                   (xreg(tc2) << 5) | xreg(tc2));
            store_frame(e, xreg(tc2), cslot, 8);
            e->vti = 0;
            br_(e, lcond);
            bind_label(e, lbreak);
            return;
        }
        // sequence loop: pointer-bump (spec Iterations #9-10)
        SType* bt = lvalue_type(e, s->b);
        bt = bt ? unwrap(bt) : NULL;
        int is_string = bt && bt->kind == SK_PRIM && bt->prim == P_STRING;
        if (!bt || (bt->kind != SK_SLICE && bt->kind != SK_ARRAY && !is_string)) {
            em_err(e, s, "for-in iterates slices, arrays, strings, or ranges");
            return;
        }
        uint32_t pslot = temp_slot(e, 8, 8), eslot = temp_slot(e, 8, 8);
        Node* index_binder = s->nkids == 2 ? s->kids[0] : NULL;
        uint32_t ivs = 0, ixslot = 0;
        if (index_binder) {
            Sym* ixv = node_sym(index_binder);
            ivs = slot_alloc(e, ixv && ixv->decl ? ixv->decl : index_binder, 8, 8);
            ixslot = temp_slot(e, 8, 8);
            load_imm(e, 1, 16, 0);
            store_frame(e, 16, ixslot, 8);
        }
        int svi = e->vti;
        int base;
        if (is_lvalue_node(s->b)) base = gen_addr(e, s->b);
        else {
            EV v = gen_expr(e, s->b);
            base = v.idx;
        }
        if (bt->kind != SK_ARRAY) {
            // slice/string descriptor: p = ptr, end = p + len*esz
            ld_reg(e, 16, base, 0, 8, 0);
            store_frame(e, 16, pslot, 8);
            ld_reg(e, 16, base, 8, 8, 0); // len
            load_imm(e, 1, 17, esz);
            ins(e, 0x9B007C00u | (17u << 16) | (16u << 5) | 17u); // x17 = len*esz
            load_frame(e, 16, pslot, 8, 0);
            ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u); // x16 = p + ...
            store_frame(e, 16, eslot, 8);
        } else {
            store_frame(e, base, pslot, 8);
            load_imm(e, 1, 16, (uint64_t)bt->count * esz);
            ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)base << 5) | 16u);
            store_frame(e, 16, eslot, 8);
        }
        e->vti = svi;
        bind_label(e, lcond);
        int tp = tmp_int(e), te = tmp_int(e);
        load_frame(e, xreg(tp), pslot, 8, 0);
        load_frame(e, xreg(te), eslot, 8, 0);
        ins(e, 0xEB00001Fu | (xreg(te) << 16) | (xreg(tp) << 5)); // cmp p, end
        bcond_(e, C_HS, lbreak);
        // loop variable = element copy at [p]
        frame_addr(e, 15, vs);
        copy_mem(e, 15, 0, xreg(tp), 0, esz);
        if (index_binder) {
            load_frame(e, 16, ixslot, 8, 0);
            store_frame(e, 16, ivs, 8);
        }
        e->vti = 0; e->vtf = 0;
        if (e->nctx >= 64) {
            em_err(e, s, "control flow nested too deeply");
            return;
        }
        e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, NULL, 0, 1 };
        emit_block(e, s->c);
        e->nctx--;
        bind_label(e, lcont);
        int tp2 = tmp_int(e);
        load_frame(e, xreg(tp2), pslot, 8, 0);
        if (esz <= 4095)
            ins(e, 0x91000000u | (esz << 10) | (xreg(tp2) << 5) | xreg(tp2));
        else {
            load_imm(e, 1, 16, esz);
            ins(e, 0x8B000000u | (16u << 16) | (xreg(tp2) << 5) | xreg(tp2));
        }
        store_frame(e, xreg(tp2), pslot, 8);
        if (index_binder) {
            load_frame(e, 16, ixslot, 8, 0);
            ins(e, 0x91000000u | (1u << 10) | (16u << 5) | 16u);
            store_frame(e, 16, ixslot, 8);
        }
        e->vti = 0;
        br_(e, lcond);
        bind_label(e, lbreak);
        return;
    }
    case N_SWITCH: {
        {
            SType* scrut = node_ty(s->a);
            SType* ut = scrut ? unwrap(scrut) : NULL;
            if (ut && ut->kind == SK_UNION) {
                emit_union_switch(e, s, ut);
                return;
            }
        }
        SType* t = node_ty(s->a);
        if (!t || !is_scalar(t)) {
            em_err(e, s, "switch scrutinee must be a scalar (strings: chunk 7)");
            return;
        }
        EV v = gen_expr(e, s->a);
        int vi = v.idx;
        int isf = v.cls == CLS_FP;
        int lcase[64], lend = new_label(e);
        int has_else = 0, ielse = -1;
        if (s->nkids > 64) {
            em_err(e, s, "too many switch cases (max 64)");
            return;
        }
        for (int i = 0; i < s->nkids; i++) {
            lcase[i] = new_label(e);
            if (s->kids[i]->flags & F_ELSE) {
                has_else = 1;
                ielse = i;
                continue;
            }
            for (int j = 0; j < s->kids[i]->nkids; j++) {
                int svi = e->vti, svf = e->vtf;
                EV p = gen_expr(e, s->kids[i]->kids[j]);
                if (isf) {
                    emit_fcmp(e, vi, p.idx, unwrap(t)->prim == P_F64);
                    bcond_(e, C_EQ, lcase[i]);
                } else {
                    int sf = scalar_bits(t) == 64;
                    ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                           (xreg(p.idx) << 16) | (xreg(vi) << 5));
                    bcond_(e, C_EQ, lcase[i]);
                }
                e->vti = svi; e->vtf = svf;
            }
        }
        if (!has_else) br_(e, lend);
        for (int i = 0; i < s->nkids; i++) {
            bind_label(e, lcase[i]);
            emit_block(e, s->kids[i]->b);
            if (i != s->nkids - 1 && !case_falls_through(s->kids[i])) br_(e, lend);
        }
        bind_label(e, lend);
        (void)ielse;
        return;
    }
    case N_RETURN: {
        SType* rt = e->fnret;
        RetCls rc;
        classify_ret(rt, &rc);
        if (!s->a) {
            emit_defers(e, rt);
            emit_epilogue(e);
            return;
        }
        if (rc.sret) {
            EV v = gen_expr(e, s->a);
            load_frame(e, 15, e->sret_slot, 8, 0); // x8 from the prologue
            copy_mem(e, 15, 0, xreg(v.idx), 0, ty_size(rt));
            emit_defers(e, rt); // the result already sits in the caller's buffer
            emit_epilogue(e);
            return;
        }
        if (rc.nunits == 1 && is_scalar(rt)) {
            EV v = gen_expr(e, s->a);
            if (v.cls == CLS_FP)
                mov_fp(e, unwrap(rt)->prim == P_F64, 0, v.idx);
            else
                mov_int(e, scalar_bits(rt) == 64, 0, xreg(v.idx));
            emit_defers(e, rt);
            emit_epilogue(e);
            return;
        }
        if (rc.nunits >= 1) { // <=16B aggregate/multi-chunk
            EV v = gen_expr(e, s->a);
            if (v.cls != CLS_ADDR) {
                em_err(e, s, "aggregate return value must be in memory");
                return;
            }
            int defer = -1; // unit loading into the base register must go last
            for (int w = 0; w < rc.nunits; w++) {
                if (rc.units[w].kind == TU_XREG) {
                    if (rc.units[w].reg == xreg(v.idx)) {
                        defer = w;
                        continue;
                    }
                    ld_reg(e, rc.units[w].reg, xreg(v.idx), rc.units[w].off, 8, 0);
                } else
                    ldrf_(e, rc.units[w].reg, xreg(v.idx), rc.units[w].off,
                          rc.units[w].size == 8);
            }
            if (defer >= 0)
                ld_reg(e, rc.units[defer].reg, xreg(v.idx),
                       rc.units[defer].off, 8, 0);
            emit_defers(e, rt);
            emit_epilogue(e);
            return;
        }
        gen_expr(e, s->a); // void return with an expression: discard
        emit_defers(e, rt);
        emit_epilogue(e);
        return;
    }
    case N_BREAK: {
        Ctx* c = NULL;
        for (int i = e->nctx - 1; i >= 0; i--) {
            if (s->str) {
                if (e->ctx[i].label &&
                    (uint32_t)strlen(e->ctx[i].label) == s->len &&
                    memcmp(e->ctx[i].label, s->str, s->len) == 0) {
                    c = &e->ctx[i];
                    break;
                }
            } else if (e->ctx[i].is_loop) {
                c = &e->ctx[i];
                break;
            }
        }
        if (!c) {
            em_err(e, s, "no target for break");
            return;
        }
        br_(e, c->brk);
        return;
    }
    case N_CONTINUE: {
        Ctx* c = NULL;
        for (int i = e->nctx - 1; i >= 0; i--) {
            if (s->str) {
                if (e->ctx[i].label &&
                    (uint32_t)strlen(e->ctx[i].label) == s->len &&
                    memcmp(e->ctx[i].label, s->str, s->len) == 0) {
                    c = &e->ctx[i];
                    break;
                }
            } else if (e->ctx[i].is_loop) {
                c = &e->ctx[i];
                break;
            }
        }
        if (!c) {
            em_err(e, s, "no target for continue");
            return;
        }
        br_(e, c->cont);
        return;
    }
    case N_FALLTHROUGH:
        return; // the enclosing switch omits its normal end branch
    case N_LABEL: {
        if (e->nctx >= 64) {
            em_err(e, s, "control flow nested too deeply");
            return;
        }
        int lstart = new_label(e), lend = new_label(e);
        bind_label(e, lstart);
        e->ctx[e->nctx++] = (Ctx){ lend, lstart, s->str, s->len, 0 };
        emit_block(e, s->a);
        bind_label(e, lend);
        e->nctx--;
        return;
    }
    case N_ASM:
        emit_asm(e, s);
        return;
    case N_BLOCK:
        emit_block(e, s);
        return;
    default:
        em_err(e, s, "codegen for '%s' statements comes in the next chunk",
               node_kind_name((NKind)s->kind));
    }
}

// --- inline assembly --------------------------------------------------------------------

static int parse_xreg_name(Node* r) {
    if (r->kind != N_IDENT || r->len < 2 || r->str[0] != 'x') return -1;
    int n = 0;
    for (uint32_t i = 1; i < r->len; i++) {
        if (r->str[i] < '0' || r->str[i] > '9') return -1;
        n = n * 10 + (r->str[i] - '0');
    }
    return n <= 30 ? n : -1;
}

// v1 mini-assembler: nop / svc #imm / brk #imm (spec Inline Assembly; the
// instruction text is emitted verbatim as machine code)
static int asm_insn(Em* e, Node* s) {
    const char* p = s->str;
    uint32_t n = s->len;
    uint32_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    uint32_t start = i;
    while (i < n && p[i] != ' ' && p[i] != '\t' && p[i] != '#') i++;
    uint32_t wlen = i - start;
    if (wlen == 3 && memcmp(p + start, "nop", 3) == 0) {
        ins(e, 0xD503201Fu);
        return 1;
    }
    if ((wlen == 3 && memcmp(p + start, "svc", 3) == 0) ||
        (wlen == 3 && memcmp(p + start, "brk", 3) == 0)) {
        int is_svc = p[start] == 's';
        while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '#')) i++;
        uint64_t imm = 0;
        int any = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') {
            imm = imm * 10 + (uint64_t)(p[i] - '0');
            i++;
            any = 1;
        }
        if (!any) {
            em_err(e, s, "svc/brk need an immediate");
            return 0;
        }
        ins(e, (is_svc ? 0xD4000001u : 0xD4200000u) | ((uint32_t)imm << 5));
        return 1;
    }
    em_err(e, s, "supported asm instructions: nop, svc #imm, brk #imm");
    return 0;
}

static void emit_asm(Em* e, Node* s) {
    for (int i = 0; i < s->nkids; i++) {
        Node* bnd = s->kids[i];
        int is_in = bnd->len == 2 && memcmp(bnd->str, "in", 2) == 0;
        int is_out = bnd->len == 3 && memcmp(bnd->str, "out", 3) == 0;
        if (!is_in && !is_out) continue; // clobbers: nothing to do
        for (int j = 0; j + 1 < bnd->nkids; j += 2) {
            int reg = parse_xreg_name(bnd->kids[j]);
            if (reg < 0) {
                em_err(e, bnd->kids[j], "asm bindings name x registers");
                continue;
            }
            Node* val = bnd->kids[j + 1];
            if (is_in) {
                if (val->kind == N_INT) {
                    load_imm(e, 1, reg, val->ival);
                    continue;
                }
                Sym* vs = node_sym(val);
                if (!vs || vs->kind != SYM_VAR || !vs->decl) {
                    em_err(e, val, "asm 'in' needs a local or an immediate");
                    continue;
                }
                Slot* sl = slot_find(e, vs->decl);
                if (!sl) {
                    em_err(e, val, "asm 'in' variable has no slot");
                    continue;
                }
                if (sl->byref) {
                    load_frame(e, 16, sl->off, 8, 0);
                    ld_reg(e, reg, 16, 0, 8, 0);
                } else {
                    ld_reg(e, reg, 29, 0, 8, 0);
                    text(e)->len -= 4; // replace with the real frame access
                    load_frame(e, reg, sl->off, 8, 0);
                }
            } else { // out: register -> local
                Sym* vs = node_sym(val);
                if (!vs || vs->kind != SYM_VAR || !vs->decl) {
                    em_err(e, val, "asm 'out' needs a local");
                    continue;
                }
                Slot* sl = slot_find(e, vs->decl);
                if (!sl) continue;
                store_frame(e, reg, sl->off, sl->size < 8 ? sl->size : 8);
            }
        }
    }
    asm_insn(e, s);
}

// --- function emission -----------------------------------------------------------------

static void emit_prologue(Em* e) {
    ins(e, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    ins(e, 0x910003FD); // mov x29, sp
    e->frame = 16; // saved x29/x30 occupy the first 16 bytes of depth
    e->ended_return = 0;
    e->subsp_fix = e->nfix;
    fix_add(e, FK_SUBSP, (uint32_t)text(e)->len, NULL);
    ins(e, 0xD10003FF); // sub sp, sp, #frame — patched at end
}

static void emit_epilogue(Em* e) {
    e->ended_return = 1;
    ins(e, 0x910003BF); // mov sp, x29
    ins(e, 0xA8C17BFD); // ldp x29, x30, [sp], #16
    ins(e, 0xD65F03C0); // ret
}

static void incoming_addr(Em* e, int rd, uint32_t soff) {
    // x29 is the stack pointer after saving x29/x30. Incoming stack
    // arguments begin at the caller's SP, exactly 16 bytes above it.
    uint32_t d = 16 + soff;
    if (d <= 4095)
        ins(e, 0x91000000u | (d << 10) | (29u << 5) | ((uint32_t)rd & 31u));
    else {
        load_imm(e, 1, rd, d);
        ins(e, 0x8B000000u | ((uint32_t)rd << 16) | (29u << 5) | ((uint32_t)rd & 31u));
    }
}

static void emit_func(Em* e, Node* f, const char* symname, uint32_t symlen,
                      SType* ret, int is_main, int export_c) {
    uint32_t debug_addr = (uint32_t)text(e)->len;
    line_mark(e, f);
    e->frame = 0;
    e->spill_on = 0;
    e->out_max = 0;
    e->nslots = 0;
    e->vti = e->vtf = 0;
    e->nlab = 0;
    e->nbp = 0;
    e->nctx = 0;
    e->ended_return = 0;
    e->fnret = ret;
    e->fnbody = f->b;
    e->ndefers = 0;
    e->retsave_off = 0;
    sym_define(&e->syms, e->secs, symname, symlen, is_main || export_c,
               &e->secs[SEC_TEXT]);
    if (is_main) emit_has_main = 1;
    emit_prologue(e);

    RetCls rc;
    classify_ret(ret, &rc);
    if (rc.sret) { // save incoming x8; the return statement reloads it
        e->sret_slot = slot_alloc(e, f, 8, 8);
        store_frame(e, 8, e->sret_slot, 8);
    }

    // parameters: stack slots + incoming copies (mirrors assign_cc exactly)
    int np = f->nkids;
    if (np > 40) {
        em_err(e, f, "too many parameters (max 40)");
        np = 40;
    }
    SType* pt[40];
    uint8_t muts[40];
    for (int i = 0; i < np; i++) {
        Sym* ps = node_sym(f->kids[i]);
        pt[i] = ps && ps->type ? ps->type : sema_build_type(f->kids[i]->a);
        muts[i] = (f->kids[i]->flags & F_MUT) != 0;
    }
    if ((f->flags & F_METHOD) && np > 0)
        muts[0] = 1; // every method receives self through the vtable data-pointer ABI
    TU units[160];
    assign_cc(pt, muts, np, !export_c, -1, units);
    for (int i = 0; i < np; i++) {
        SType* t = pt[i];
        uint32_t size = ty_size(t);
        uint32_t hfa_offs[5];
        uint8_t hfa_dbl[5];
        int is_hfa = classify_hfa(t, hfa_offs, hfa_dbl) >= 0;
        int byref = (!export_c && muts[i]) || (size > 16 && !is_hfa);
        uint32_t storage = byref ? 8 : is_scalar(t) ? size : align_up(size, 8);
        uint32_t slot = slot_alloc(e, f->kids[i], storage,
                                   byref ? 8 : ty_align(t));
        if (byref) e->slots[e->nslots - 1].byref = 1;
        for (int w = 0; w < 4; w++) {
            TU* u = &units[i * 4 + w];
            if (u->kind == TU_XREG) {
                if (u->reg < 0) continue;
                uint32_t bw = byref ? 8 : is_scalar(t)
                    ? (uint32_t)((scalar_bits(t) + 7) / 8) : 8;
                if (byref) {
                    store_frame(e, u->reg, slot, bw);
                } else {
                    frame_addr(e, 16, slot);
                    st_reg(e, u->reg, 16, u->off, bw);
                }
            } else if (u->kind == TU_VREG) {
                frame_addr(e, 16, slot);
                strf_(e, u->reg, 16, u->off, u->size == 8);
            } else if (u->kind == TU_STACK) {
                incoming_addr(e, 16, u->soff);
                if (is_floatt(t)) {
                    ldrf_(e, 0, 16, 0, u->size == 8);
                    store_frame_fp(e, 0, slot, u->size == 8);
                } else {
                    uint32_t bw = u->size < 8 ? u->size : 8;
                    ld_reg(e, 16, 16, 0, bw, 0);
                    frame_addr(e, 17, slot);
                    st_reg(e, 16, 17, byref ? 0u : u->off, bw);
                }
            } else { // TU_STACK_AGGR: by-value stack (extern/export C ABI)
                incoming_addr(e, 17, u->soff);
                frame_addr(e, 15, slot);
                copy_mem(e, 15, 0, 17, 0, size);
            }
        }
    }

    for (int i = 0; i < f->b->nkids; i++) emit_stmt(e, f->b->kids[i]);

    if (!e->ended_return) { // fall-off end: zero the result registers
        emit_defers(e, NULL); // no result register is live here
        if (rc.nunits == 1 && is_scalar(ret) && is_floatt(ret))
            ins(e, unwrap(ret)->prim == P_F64 ? 0x9E6703E0u : 0x1E2703E0u);
        else if (rc.nunits >= 1) {
            for (int w = 0; w < rc.nunits; w++) {
                if (rc.units[w].kind == TU_XREG)
                    load_imm(e, 1, rc.units[w].reg, 0);
                else
                    ins(e, (rc.units[w].size == 8 ? 0x9E6703E0u : 0x1E2703E0u) |
                           (uint32_t)rc.units[w].reg);
            }
        }
        emit_epilogue(e);
    }
    e->fix[e->subsp_fix].frame = align_up(e->frame + e->out_max, 16);
    patch_branches(e);
    debug_fn_add(e, f, debug_addr, (uint32_t)text(e)->len - debug_addr);
}

// --- globals ---------------------------------------------------------------------------

static void put_u(Buf* b, uint64_t v, uint32_t w) {
    for (uint32_t i = 0; i < w; i++) buf_put8(b, (uint8_t)(v >> (8 * i)));
}

// constant initializer folding (little-endian, declaration-order layout)
/*
 * Constructor-constant targets, family-checked: an int argument may fold into
 * an integer primitive, an enum, or a distinct over either; a float argument
 * only into a float primitive or a distinct over one. The checked cast target
 * is always terminal (aliases resolve to the same SType at declaration), so
 * unwrap's DISTINCT walk lands on the representation. Contracts, Error,
 * unions, strings, pointers, and aggregates match neither family and stay
 * unfolded.
 */
static int fold_int_target(SType* t) {
    t = unwrap(t);
    if (!t) return 0;
    if (t->kind == SK_ENUM) return 1;
    if (t->kind == SK_DISTINCT) return fold_int_target(t->base);
    return t->kind == SK_PRIM &&
        ((t->prim >= P_I8 && t->prim <= P_U64) || t->prim == P_USIZE);
}

static int fold_float_target(SType* t) {
    t = unwrap(t);
    if (!t) return 0;
    if (t->kind == SK_DISTINCT) return fold_float_target(t->base);
    return t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64);
}

static int eval_const(Em* e, Node* v, SType* t, Buf* out) {
    t = unwrap(t);
    if (!t) return 0;
    switch (v->kind) {
    case N_CALL: {
        // union variant construction with a compile-time payload
        Node* callee = v->a;
        SType* ut = callee && callee->kind == N_FIELD_E && !v->sym
            ? union_type_of_base(callee->a) : NULL;
        if (ut) {
            int vi = (int)callee->ival;
            if (vi >= ut->nfields || !ut->fields[vi].t) return 0;
            uint32_t poff = sema_union_payload_off(ut);
            size_t start = out->len;
            put_u(out, (uint64_t)vi, 4);
            while (out->len - start < poff) buf_put8(out, 0);
            if (!eval_const(e, v->kids[0], ut->fields[vi].t, out)) return 0;
            while (out->len - start < ty_size(t)) buf_put8(out, 0);
            return 1;
        }
        if (v->a && v->a->kind == N_IDENT && v->a->len == 6 &&
            memcmp(v->a->str, "typeid", 6) == 0 && v->ival) {
            put_u(out, v->ival, 8);
            return 1;
        }
        // Constructor-shaped cast sema resolved against a type (no callee
        // function symbol): the runtime cast emits only a width adjustment
        // (gen_cast), so folding the argument at the checked target is
        // byte-identical. The family gate keeps the fold honest — the folder
        // writes the argument's own representation, so int<->float crossings
        // would store raw bits of the wrong family.
        if (v->a && v->a->kind == N_IDENT && v->nkids == 1 && !v->sym) {
            SType* want = node_ty(v->a);
            Node* arg = v->kids[0];
            int arg_int = arg->kind == N_INT || arg->kind == N_CHAR ||
                arg->kind == N_BOOL ||
                (arg->kind == N_UNARY && arg->op == T_MINUS);
            if ((arg_int && fold_int_target(want)) ||
                (arg->kind == N_FLOAT && fold_float_target(want)))
                return eval_const(e, arg, want, out);
        }
        return 0;
    }
    case N_INT: case N_BOOL: case N_CHAR: {
        uint64_t c;
        if (const_int(v, &c)) {
            put_u(out, c, ty_size(t) < 8 ? ty_size(t) : 8);
            return 1;
        }
        return 0;
    }
    case N_FLOAT: {
        if (t->kind == SK_PRIM && t->prim == P_F64) {
            put_u(out, 0, 8);
            memcpy(out->d + out->len - 8, &v->fval, 8);
        } else {
            float f = (float)v->fval;
            uint32_t b = 0;
            memcpy(&b, &f, 4);
            put_u(out, b, 4);
        }
        return 1;
    }
    case N_FIELD_E: { // enum member (base carries the type symbol)
        Sym* ts = node_sym(v->a);
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ENUM) {
            put_u(out, v->ival, 4);
            return 1;
        }
        { // union variant value (payloadless): tag, then canonical zero payload
            SType* ut = union_type_of_base(v->a);
            if (ut) {
                size_t start = out->len;
                put_u(out, v->ival, 4);
                while (out->len - start < ty_size(t)) buf_put8(out, 0);
                return 1;
            }
        }
        return 0;
    }
    case N_NULL: {
        for (uint32_t i = 0; i < ty_size(t); i++) buf_put8(out, 0);
        return 1;
    }
    case N_UNARY:
        if (v->op == T_MINUS) {
            uint64_t c;
            if (const_int(v, &c)) {
                put_u(out, c, ty_size(t) < 8 ? ty_size(t) : 8);
                return 1;
            }
        }
        return 0;
    case N_ARRAYLIT: case N_STRUCTLIT: {
        if (v->kind == N_STRUCTLIT && t->kind != SK_STRUCT) return 0;
        if (v->kind == N_ARRAYLIT && t->kind != SK_ARRAY) return 0;
        int positional = v->nkids > 0 && v->kids[0]->str == NULL;
        size_t start = out->len;
        uint32_t cursor = 0;
        if (v->kind == N_ARRAYLIT && v->a) {
            for (uint64_t i = 0; i < t->count; i++) {
                if (!eval_const(e, v->a, t->base, out)) return 0;
            }
            return 1;
        }
        for (int i = 0; i < v->nkids; i++) {
            Node* fi = v->kids[i];
            Node* value = v->kind == N_ARRAYLIT ? fi : fi->a;
            int fidx = i;
            SType* ft;
            uint32_t fo;
            if (v->kind == N_ARRAYLIT) {
                ft = t->base;
                fo = (uint32_t)i * ty_size(t->base);
            } else {
                if (!positional) {
                    fidx = find_field(t, fi->str, fi->len);
                    if (fidx < 0) return 0;
                }
                fo = field_off(t, fidx);
                ft = t->fields[fidx].t;
            }
            while (cursor < fo) {
                buf_put8(out, 0);
                cursor++;
            }
            if (!eval_const(e, value, ft, out)) return 0;
            cursor = (uint32_t)(out->len - start);
        }
        while (cursor < ty_size(t)) { // tail padding
            buf_put8(out, 0);
            cursor++;
        }
        return 1;
    }
    default:
        return 0;
    }
}

static void add_global(Em* e, Sym* s, ESym* sym);
static Node* static_string_node(Node* n);

/*
 * Emit a compile-time union value. Most payloads use the ordinary constant
 * folder; a static string needs a section relocation for its pointer word.
 */
static int emit_union_global(Em* e, Node* n, Sym* vs, const char* nm, SType* t) {
    Node* ref = n->b;
    Node* payload = NULL;
    if (ref && ref->kind == N_CALL) {
        payload = ref->nkids == 1 ? ref->kids[0] : NULL;
        ref = ref->a;
    }
    if (!ref || ref->kind != N_FIELD_E) return 0;
    SType* ut = union_type_of_base(ref->a);
    int vi = (int)ref->ival;
    if (!ut || ut != t || vi < 0 || vi >= ut->nfields) return 0;
    SType* pt = ut->fields[vi].t;
    if ((pt != NULL) != (payload != NULL)) return 0;

    Node* text = pt && unwrap(pt)->kind == SK_PRIM &&
                 unwrap(pt)->prim == P_STRING ? static_string_node(payload) : NULL;
    Buf payload_bytes = {0};
    if (pt && !text && !eval_const(e, payload, pt, &payload_bytes)) {
        free(payload_bytes.d);
        return 0;
    }

    const char* str_name = NULL;
    uint32_t str_name_len = 0;
    if (text) {
        ESym* str = intern_string(e, text);
        str_name = str->name;
        str_name_len = (uint32_t)strlen(str_name);
    }

    uint32_t align = ty_align(t) > 8 ? 8 : ty_align(t);
    Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
    while (d->data.len % align) buf_put8(&d->data, 0);
    size_t start = d->data.len;
    ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
    put_u(&d->data, (uint64_t)vi, 4);
    while (d->data.len - start < sema_union_payload_off(t)) buf_put8(&d->data, 0);
    if (text) {
        ESym* str = sym_intern(&e->syms, str_name, str_name_len);
        uint32_t off = (uint32_t)d->data.len;
        buf_put64(&d->data, 0);
        reloc_add(d, off, str, 0, 3, 1, RM_UNSIGNED);
        buf_put64(&d->data, text->len);
    } else if (pt) {
        buf_write(&d->data, payload_bytes.d, payload_bytes.len);
    }
    while (d->data.len - start < ty_size(t)) buf_put8(&d->data, 0);
    free(payload_bytes.d);
    add_global(e, vs, g);
    return 1;
}

static void add_global(Em* e, Sym* s, ESym* sym) {
    if (e->nglobs == e->cglobs) {
        e->cglobs = e->cglobs ? e->cglobs * 2 : 8;
        e->globs = realloc(e->globs, (size_t)e->cglobs * sizeof(GVar));
        if (!e->globs) oom();
    }
    e->globs[e->nglobs++] =
        (GVar){ s, sym->name, (uint32_t)strlen(sym->name) };
}

static Node* init_field(Node* lit, const char* name) {
    size_t len = strlen(name);
    for (int i = 0; lit && i < lit->nkids; i++) {
        Node* f = lit->kids[i];
        if (f->str && f->len == len && memcmp(f->str, name, len) == 0)
            return f->a;
    }
    return NULL;
}

static Node* static_string_node(Node* n) {
    if (n && n->kind == N_STR) return n;
    if (n && n->kind == N_IDENT) {
        Sym* s = node_sym(n);
        if (s && s->is_global && s->is_const && s->init)
            return static_string_node(s->init);
    }
    return NULL;
}

static int is_error_descriptor_type(SType* t) {
    return t && t->kind == SK_STRUCT && !t->decl && t->name &&
           strcmp(t->name, "ErrorDescriptor") == 0;
}

static int emit_error_descriptor_global(Em* e, Node* n, Sym* vs,
                                        const char* nm, SType* t) {
    Node* code = init_field(n->b, "code");
    Node* domain = init_field(n->b, "domain");
    Node* message = static_string_node(init_field(n->b, "message"));
    uint64_t cv = 0;
    if (!code || !const_int(code, &cv) || !domain || !message) {
        em_err(e, n, "invalid static ErrorDescriptor initializer");
        return 0;
    }
    uint64_t dv = domain->kind == N_NULL ? 0 : domain->ival;
    ESym* str = intern_string(e, message);
    const char* str_name = str->name;
    uint32_t str_len = (uint32_t)strlen(str_name);
    Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
    while (d->data.len % 8) buf_put8(&d->data, 0);
    size_t start = d->data.len;
    ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
    str = sym_intern(&e->syms, str_name, str_len);
    buf_put32(&d->data, (uint32_t)cv); // code + natural padding
    buf_put32(&d->data, 0);
    buf_put64(&d->data, dv);
    uint32_t off = (uint32_t)d->data.len;
    buf_put64(&d->data, 0);
    reloc_add(d, off, str, 0, 3, 1, RM_UNSIGNED);
    buf_put64(&d->data, message->len);
    if ((uint32_t)(d->data.len - start) != ty_size(t))
        em_err(e, n, "ErrorDescriptor layout mismatch in codegen: wrote %u, expected %u",
               (unsigned)(d->data.len - start), ty_size(t));
    g = sym_intern(&e->syms, nm, (uint32_t)strlen(nm));
    add_global(e, vs, g);
    return 1;
}

static int emit_error_handle_global(Em* e, Node* n, Sym* vs,
                                    const char* nm) {
    Node* call = n->b;
    Node* arg = call && call->kind == N_CALL && call->nkids == 1
        ? call->kids[0] : NULL;
    Node* target = arg && arg->kind == N_UNARY && arg->op == T_AMP ? arg->a : NULL;
    Sym* ds = target && target->kind == N_IDENT ? node_sym(target) : NULL;
    ESym* descriptor = ds ? global_sym(e, ds) : NULL;
    if (!descriptor) {
        em_err(e, n, "Error descriptor must be emitted before its handle");
        return 0;
    }
    const char* descriptor_name = descriptor->name;
    uint32_t descriptor_len = (uint32_t)strlen(descriptor_name);
    Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
    while (d->data.len % 8) buf_put8(&d->data, 0);
    ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
    descriptor = sym_intern(&e->syms, descriptor_name, descriptor_len);
    uint32_t off = (uint32_t)d->data.len;
    buf_put64(&d->data, 0);
    reloc_add(d, off, descriptor, 0, 3, 1, RM_UNSIGNED);
    add_global(e, vs, g);
    return 1;
}

static void emit_global(Em* e, Node* n) {
    Node* binder = n->kids[0];
    Sym* vs = node_sym(binder);
    if (!vs) return;
    SType* t = sema_build_type(binder->a);
    uint32_t size = ty_size(t), align = ty_align(t) > 8 ? 8 : ty_align(t);
    char nm[512];
    snprintf(nm, sizeof nm, "__ace$g$%.*s", (int)binder->len, binder->str);
    if (!n->b) { // no initializer: zero-fill storage
        Sec* b = &e->secs[SEC_BSS];
        b->size = align_up(b->size, align);
        ESym* s = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, b);
        b->size += size;
        add_global(e, vs, s);
        return;
    }
    if (is_error_descriptor_type(t)) {
        emit_error_descriptor_global(e, n, vs, nm, t);
        return;
    }
    if (t->kind == SK_ERROR) {
        emit_error_handle_global(e, n, vs, nm);
        return;
    }
    if (t->kind == SK_UNION) {
        if (!emit_union_global(e, n, vs, nm, t))
            em_err(e, n, "global union initializer must be a compile-time variant value");
        return;
    }
    // string global: {ptr into __const, len} — the pointer needs a reloc
    if (n->b->kind == N_STR && t->kind == SK_PRIM && t->prim == P_STRING) {
        ESym* str = intern_string(e, n->b);
        Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
        while (d->data.len % 8) buf_put8(&d->data, 0);
        ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
        uint32_t off = (uint32_t)d->data.len;
        buf_put64(&d->data, 0);
        reloc_add(d, off, str, 0, 3, 1, RM_UNSIGNED);
        buf_put64(&d->data, n->b->len);
        add_global(e, vs, g);
        return;
    }
    Buf bytes = {0};
    if (!eval_const(e, n->b, t, &bytes)) {
        em_err(e, n, "global initializer must be a compile-time constant");
        free(bytes.d);
        return;
    }
    Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
    while (d->data.len % align) buf_put8(&d->data, 0);
    ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
    buf_write(&d->data, bytes.d, bytes.len);
    free(bytes.d);
    add_global(e, vs, g);
}

// --- vtables ---------------------------------------------------------------------------

static const char* vtable_name(SType* conc, SType* contract) {
    Buf b = {0};
    buf_write(&b, "__ace$vt$", 9);
    mangle_type(&b, conc);
    buf_put8(&b, '$');
    mangle_type(&b, contract);
    buf_put8(&b, 0);
    const char* out = arena_copy((const char*)b.d, b.len - 1);
    free(b.d);
    return out;
}

static ESym* emit_embed_thunk(Em* e, const char* vtname, int slot,
                              const char* target, uint32_t off) {
    char name[1024];
    snprintf(name, sizeof name, "%s$embed$%d", vtname, slot);
    ESym* prior = sym_intern(&e->syms, name, (uint32_t)strlen(name));
    if (prior->type) return prior;
    int skip = -1;
    if (e->frame) {
        skip = new_label(e);
        br_(e, skip); // a generic vtable may be materialized inside a function
    }
    ESym* out = sym_define(&e->syms, e->secs, name, (uint32_t)strlen(name), 0,
                           &e->secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    if (off <= 4095) {
        ins(e, 0x91000000u | (off << 10)); // add x0, x0, #field-offset
    } else {
        load_imm(e, 1, 16, off);
        ins(e, 0x8B000000u | (16u << 16)); // add x0, x0, x16
    }
    ins(e, 0x94000000u);
    ESym* fn = sym_intern(&e->syms, target, (uint32_t)strlen(target));
    fix_add(e, FK_BRANCH, (uint32_t)(text(e)->len - 4), fn);
    ins(e, 0xA8C17BFD); // ldp x29, x30, [sp], #16
    ins(e, 0xD65F03C0); // ret
    if (skip >= 0) bind_label(e, skip);
    return out;
}

static ESym* emit_vtable(Em* e, SType* conc, SType* contract, Node* conf) {
    if (!conc || !contract || !conf) {
        em_err(e, conf, "invalid contract conformance in codegen");
        return sym_intern(&e->syms, "__ace$vt$invalid", 16);
    }
    const char* nm = vtable_name(conc, contract);
    ESym* prior = sym_intern(&e->syms, nm, (uint32_t)strlen(nm));
    if (prior->type) return prior;
    Sec* c = &e->secs[SEC_CONST];
    while (c->data.len % 8) buf_put8(&c->data, 0);
    ESym* out = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, c);
    for (int i = 0; i < contract->decl->nkids; i++) { // contract method order
        uint32_t off = (uint32_t)c->data.len;
        Node* ref = i < conf->nkids ? conf->kids[i] : NULL;
        Sym* fsym = ref ? node_sym(ref) : NULL;
        if (!fsym) {
            Node* m = contract->decl->kids[i];
            em_err(e, conf, "signing misses method '%.*s' (sema gap)", (int)m->len, m->str);
            buf_put64(&c->data, 0);
            continue;
        }
        SType* ft = fsym->type;
        SType** args = NULL;
        int nargs = 0;
        SType* method_owner = conc;
        int embed_fi = -1;
        if (ref->a) {
            embed_fi = find_field(conc, ref->a->str, ref->a->len);
            if (embed_fi >= 0) method_owner = conc->fields[embed_fi].t;
        }
        if (fsym->is_generic && method_owner->nargs) {
            args = method_owner->args;
            nargs = method_owner->nargs;
            for (STypeInst* it = sema_insts(fsym->decl); it; it = it->next) {
                int same = it->result && it->result->nparams &&
                           it->result->params[0]->decl == method_owner->decl &&
                           it->result->params[0]->nargs == method_owner->nargs;
                for (int a = 0; a < method_owner->nargs && same; a++)
                    same = it->args[a] == method_owner->args[a];
                if (same) { ft = it->result; break; }
            }
        }
        const char* fnm = fn_mangle(fsym->name, fsym->len, ft,
                                    fsym->decl, args, nargs, 0);
        ESym* fs;
        if (ref->a) {
            fs = emit_embed_thunk(e, nm, i, fnm, field_off(conc, embed_fi));
        } else {
            fs = sym_intern(&e->syms, fnm, (uint32_t)strlen(fnm));
        }
        buf_put64(&c->data, 0);
        reloc_add(c, off, fs, 0, 3, 1, RM_UNSIGNED);
    }
    return out;
}

static void emit_signing(Em* e, Node* signing) {
    // Generic contracts are materialized per exact pair at construction sites.
    if (signing->c) return;
    SType* conc = sema_build_type(signing->a);
    for (int i = 0; i < signing->nkids; i++) {
        SType* contract = node_ty(signing->kids[i]->a);
        if (!contract) contract = sema_build_type(signing->kids[i]->a);
        emit_vtable(e, conc, contract, signing->kids[i]);
    }
}

// --- fixups ---------------------------------------------------------------------------

static void patch_fixups(Em* e) {
    Buf* t = text(e);
    for (size_t i = 0; i < e->nfix; i++) {
        Fixup* f = &e->fix[i];
        if (f->kind == FK_SUBSP) {
            uint32_t frame = (f->frame + 15u) & ~15u;
            if (frame <= 0xFFFu) {
                buf_patch32(t, f->site, 0xD10003FFu | (frame << 10));
                continue;
            }
            uint32_t pages = (frame + 0xFFFu) >> 12;
            if (pages > 0xFFFu) {
                fprintf(stderr, "ace: function stack frame exceeds AArch64 immediate range\n");
                e->d->errors++;
                continue;
            }
            buf_patch32(t, f->site, 0xD14003FFu | (pages << 10));
            continue;
        }
        if (!f->target->type || f->target->sect != 1 + SEC_TEXT) {
            fprintf(stderr, "ace: internal call to '%s' is not a text symbol\n",
                    f->target->name);
            e->d->errors++;
            continue;
        }
        int64_t delta = (int64_t)f->target->value - (int64_t)f->site;
        if (delta < -(1 << 27) || delta >= (1 << 27) || (delta & 3)) {
            fprintf(stderr, "ace: call to '%s' out of branch range\n",
                    f->target->name);
            e->d->errors++;
            continue;
        }
        buf_patch32(t, f->site,
                    0x94000000u | (uint32_t)((delta >> 2) & 0x03FFFFFFu));
    }
}

// --- DWARF line table -------------------------------------------------------------

static void source_path(const char* input, char* out, size_t cap) {
    if (input[0] == '/') {
        snprintf(out, cap, "%s", input);
        return;
    }
    char cwd[3072];
    if (getcwd(cwd, sizeof cwd))
        snprintf(out, cap, "%s/%s", cwd, input);
    else
        snprintf(out, cap, "%s", input);
}

static void emit_debug_info(Em* e, ESym* text_anchor) {
    if (!e->nlines) return;
    Buf* ab = &e->secs[SEC_DEBUG_ABBREV].data;
    // Abbrev 1: compile unit. Function children let dsymutil select the text
    // ranges while deliberately omitting types, variables, and frame data.
    buf_uleb(ab, 1);
    buf_uleb(ab, 0x11); // DW_TAG_compile_unit
    buf_put8(ab, 1);    // DW_CHILDREN_yes
    buf_uleb(ab, 0x25); buf_uleb(ab, 0x08); // DW_AT_producer, DW_FORM_string
    buf_uleb(ab, 0x13); buf_uleb(ab, 0x05); // DW_AT_language, DW_FORM_data2
    buf_uleb(ab, 0x03); buf_uleb(ab, 0x08); // DW_AT_name, DW_FORM_string
    buf_uleb(ab, 0x10); buf_uleb(ab, 0x17); // DW_AT_stmt_list, DW_FORM_sec_offset
    buf_uleb(ab, 0x1b); buf_uleb(ab, 0x08); // DW_AT_comp_dir, DW_FORM_string
    buf_uleb(ab, 0x11); buf_uleb(ab, 0x01); // DW_AT_low_pc, DW_FORM_addr
    buf_uleb(ab, 0x12); buf_uleb(ab, 0x06); // DW_AT_high_pc, DW_FORM_data4
    buf_put8(ab, 0); buf_put8(ab, 0);
    buf_uleb(ab, 2);
    buf_uleb(ab, 0x2e); // DW_TAG_subprogram
    buf_put8(ab, 0);    // DW_CHILDREN_no
    buf_uleb(ab, 0x11); buf_uleb(ab, 0x01); // DW_AT_low_pc, DW_FORM_addr
    buf_uleb(ab, 0x12); buf_uleb(ab, 0x06); // DW_AT_high_pc, DW_FORM_data4
    buf_uleb(ab, 0x03); buf_uleb(ab, 0x08); // DW_AT_name, DW_FORM_string
    buf_put8(ab, 0); buf_put8(ab, 0);
    buf_put8(ab, 0);    // abbreviation table terminator

    char path[4096], cwd[3072];
    source_path(e->d->path, path, sizeof path);
    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
    Buf* in = &e->secs[SEC_DEBUG_INFO].data;
    size_t unit = in->len;
    buf_put32(in, 0);   // unit_length
    buf_put16(in, 4);   // DWARF v4
    buf_put32(in, 0);   // abbrev offset
    buf_put8(in, 8);    // address size
    buf_uleb(in, 1);    // abbrev code
    buf_write(in, "Ace compiler", sizeof "Ace compiler");
    buf_put16(in, 0x8000); // DW_LANG_lo_user: Ace has no assigned DWARF code
    buf_write(in, path, strlen(path) + 1);
    buf_put32(in, 0);   // stmt_list offset
    buf_write(in, cwd, strlen(cwd) + 1);
    uint32_t low_pc = (uint32_t)in->len;
    buf_put64(in, 0);
    reloc_add(&e->secs[SEC_DEBUG_INFO], low_pc, text_anchor,
              0, 3, 0, RM_UNSIGNED);
    buf_put32(in, (uint32_t)text(e)->len); // high_pc is a size in DWARF v4
    for (int i = 0; i < e->ndebug_fns; i++) {
        DebugFn* f = &e->debug_fns[i];
        buf_uleb(in, 2);
        uint32_t fn_pc = (uint32_t)in->len;
        buf_put64(in, f->addr); // relocation addend within __text
        reloc_add(&e->secs[SEC_DEBUG_INFO], fn_pc, text_anchor,
                  0, 3, 0, RM_UNSIGNED);
        buf_put32(in, f->size);
        buf_write(in, f->name, f->nlen);
        buf_put8(in, 0);
    }
    buf_put8(in, 0);    // end compile-unit children
    buf_patch32(in, unit, (uint32_t)(in->len - unit - 4));
}

static void emit_debug_line(Em* e, ESym* text_anchor) {
    if (!e->nlines) return;
    Buf* b = &e->secs[SEC_DEBUG_LINE].data;

    const char** files = malloc((size_t)e->nlines * sizeof(char*));
    if (!files) oom();
    int nfiles = 0;
    for (int i = 0; i < e->nlines; i++) {
        int seen = 0;
        for (int j = 0; j < nfiles; j++)
            if (strcmp(files[j], e->lines[i].path) == 0) { seen = 1; break; }
        if (!seen) files[nfiles++] = e->lines[i].path;
    }

    size_t unit = b->len;
    buf_put32(b, 0);             // unit_length, patched below
    buf_put16(b, 4);             // DWARF v4
    size_t header_len = b->len;
    buf_put32(b, 0);             // header_length, patched below
    size_t header = b->len;
    buf_put8(b, 1);              // minimum_instruction_length
    buf_put8(b, 1);              // maximum_operations_per_instruction
    buf_put8(b, 1);              // default_is_stmt
    buf_put8(b, (uint8_t)-5);    // line_base
    buf_put8(b, 14);             // line_range
    buf_put8(b, 13);             // opcode_base
    static const uint8_t oplen[12] = { 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1 };
    buf_write(b, oplen, sizeof oplen);
    buf_put8(b, 0);              // include_directories terminator
    for (int i = 0; i < nfiles; i++) {
        char path[4096];
        source_path(files[i], path, sizeof path);
        buf_write(b, path, strlen(path) + 1);
        buf_uleb(b, 0);          // directory index (full filename)
        buf_uleb(b, 0);          // modification time
        buf_uleb(b, 0);          // file size
    }
    buf_put8(b, 0);              // file_names terminator
    buf_patch32(b, header_len, (uint32_t)(b->len - header));

    // DW_LNE_set_address. The object relocation supplies __text's final base.
    buf_put8(b, 0);
    buf_uleb(b, 9);
    buf_put8(b, 2);
    uint32_t address_off = (uint32_t)b->len;
    buf_put64(b, 0);
    reloc_add(&e->secs[SEC_DEBUG_LINE], address_off, text_anchor,
              0, 3, 0, RM_UNSIGNED);

    uint32_t addr = 0;
    int line = 1, col = 0, file = 1;
    for (int i = 0; i < e->nlines; i++) {
        LineRow* r = &e->lines[i];
        int want_file = 1;
        for (int j = 0; j < nfiles; j++)
            if (strcmp(files[j], r->path) == 0) { want_file = j + 1; break; }
        if (r->addr > addr) {
            buf_put8(b, 2);      // DW_LNS_advance_pc
            buf_uleb(b, r->addr - addr);
            addr = r->addr;
        }
        if (want_file != file) {
            buf_put8(b, 4);      // DW_LNS_set_file
            buf_uleb(b, (uint64_t)want_file);
            file = want_file;
        }
        if (r->line != line) {
            buf_put8(b, 3);      // DW_LNS_advance_line
            buf_sleb(b, (int64_t)r->line - line);
            line = r->line;
        }
        if (r->col != col) {
            buf_put8(b, 5);      // DW_LNS_set_column
            buf_uleb(b, (uint64_t)(r->col > 0 ? r->col : 0));
            col = r->col;
        }
        buf_put8(b, 1);          // DW_LNS_copy
    }
    uint32_t end = (uint32_t)text(e)->len;
    if (end > addr) {
        buf_put8(b, 2);
        buf_uleb(b, end - addr);
    }
    buf_put8(b, 0);
    buf_uleb(b, 1);
    buf_put8(b, 1);              // DW_LNE_end_sequence
    buf_patch32(b, unit, (uint32_t)(b->len - unit - 4));
    free(files);
}

// --- Mach-O object writing ----------------------------------------------------------

static size_t write_object(Em* e, const char* path) {
    // Section size = data length for file-backed sections (bss is reserved).
    for (int i = 0; i < NSECS; i++)
        if (!(e->secs[i].flags & S_ZEROFILL))
            e->secs[i].size = e->secs[i].data.len;

    uint32_t segcmd = 72 + 80 * NSECS;
    uint32_t cmdend = 32 + segcmd + 24 + 24 + 80; // header + 4 load commands
    uint32_t base = (cmdend + 7u) & ~7u;          // first section file offset

    // Layout: addrs continuous from 0; file offsets track (bss takes no bytes).
    uint64_t addr = 0;
    uint32_t off = base;
    for (int i = 0; i < NSECS; i++) {
        Sec* s = &e->secs[i];
        uint32_t a = 1u << s->align;
        addr = (addr + a - 1) & ~(uint64_t)(a - 1);
        off = (off + a - 1) & ~(a - 1);
        s->addr = addr;
        s->offset = off;
        addr += s->size;
        if (!(s->flags & S_ZEROFILL)) off += (uint32_t)s->size;
    }
    uint64_t vmsize = addr;
    uint32_t filesize = off - base;

    // Symbol order (pinned): locals, extdefs, undefs. Assign strtab indices
    // and table ordinals; relocation entries serialize against the ordinals.
    Buf str;
    memset(&str, 0, sizeof str);
    buf_put8(&str, 0); // strx 0: the empty string
    ESym** order = malloc((e->syms.n ? e->syms.n : 1) * sizeof(ESym*));
    if (!order) oom();
    size_t norder = 0, nloc = 0, next_ = 0, nund = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (size_t i = 0; i < e->syms.n; i++) {
            ESym* s = e->syms.v[i];
            int want = pass == 0 ? (s->type && !(s->type & N_EXT_))
                    : pass == 1 ? (s->type && (s->type & N_EXT_))
                                : !s->type;
            if (!want) continue;
            s->strx = (uint32_t)str.len;
            s->ord = (uint32_t)norder;
            buf_write(&str, s->name, strlen(s->name) + 1);
            order[norder++] = s;
            if (pass == 0) nloc++;
            else if (pass == 1) next_++;
            else nund++;
        }
    }

    // Serialize structured relocations now that symbol ordinals are known.
    Buf relocs[NSECS];
    memset(relocs, 0, sizeof relocs);
    for (int i = 0; i < NSECS; i++) {
        for (int j = 0; j < e->secs[i].nrl; j++) {
            Rel* r = &e->secs[i].rl[j];
            buf_put32(&relocs[i], r->off);
            uint32_t target = r->ext ? r->sym->ord : r->sym->sect;
            buf_put32(&relocs[i], (target & 0xFFFFFFu) |
                      ((uint32_t)r->pcrel << 24) | ((uint32_t)r->len << 25) |
                      ((uint32_t)r->ext << 27) | ((uint32_t)r->type << 28));
        }
    }

    // Relocation data follows all section data.
    uint32_t reloff = off;
    for (int i = 0; i < NSECS; i++) {
        e->secs[i].reloff = reloff;
        reloff += (uint32_t)relocs[i].len;
    }

    // Assemble the file image.
    Buf o;
    memset(&o, 0, sizeof o);

    // mach_header_64
    buf_put32(&o, 0xFEEDFACFu);             // MH_MAGIC_64
    buf_put32(&o, 0x0100000Cu);             // CPU_TYPE_ARM64
    buf_put32(&o, 0);                       // CPU_SUBTYPE_ARM64_ALL
    buf_put32(&o, 1);                       // MH_OBJECT
    buf_put32(&o, 4);                       // ncmds
    buf_put32(&o, segcmd + 24 + 24 + 80);   // sizeofcmds
    buf_put32(&o, 0);                       // flags (pinned: no bit set)
    buf_put32(&o, 0);                       // reserved

    // LC_SEGMENT_64 — one nameless segment holding all sections
    buf_put32(&o, 0x19);
    buf_put32(&o, segcmd);
    buf_putname(&o, "");                    // segname: empty (pinned)
    buf_put64(&o, 0);                       // vmaddr
    buf_put64(&o, vmsize);
    buf_put64(&o, base);                    // fileoff
    buf_put64(&o, filesize);
    buf_put32(&o, 7);                       // maxprot RWX
    buf_put32(&o, 7);                       // initprot RWX
    buf_put32(&o, NSECS);
    buf_put32(&o, 0);                       // flags
    for (int i = 0; i < NSECS; i++) {
        Sec* s = &e->secs[i];
        buf_putname(&o, s->sect);
        buf_putname(&o, s->seg);
        buf_put64(&o, s->addr);
        buf_put64(&o, s->size);
        buf_put32(&o, s->offset);
        buf_put32(&o, s->align);
        buf_put32(&o, relocs[i].len ? s->reloff : 0);
        buf_put32(&o, (uint32_t)(relocs[i].len / 8));
        buf_put32(&o, s->flags);
        buf_put32(&o, 0);                   // reserved1
        buf_put32(&o, 0);                   // reserved2
        buf_put32(&o, 0);                   // reserved3
    }

    // LC_BUILD_VERSION
    buf_put32(&o, 0x32);
    buf_put32(&o, 24);
    buf_put32(&o, 1);                       // PLATFORM_MACOS
    buf_put32(&o, 0x000B0000u);             // minos 11.0
    buf_put32(&o, 0x000B0000u);             // sdk 11.0
    buf_put32(&o, 0);                       // ntools

    // LC_SYMTAB — symoff/stroff patched once known
    size_t symcmd = o.len;
    buf_put32(&o, 0x02);
    buf_put32(&o, 24);
    buf_put32(&o, 0);                       // symoff (patch)
    buf_put32(&o, (uint32_t)e->syms.n);
    buf_put32(&o, 0);                       // stroff (patch)
    buf_put32(&o, (uint32_t)str.len);

    // LC_DYSYMTAB
    buf_put32(&o, 0x0B);
    buf_put32(&o, 80);
    buf_put32(&o, 0); buf_put32(&o, (uint32_t)nloc);
    buf_put32(&o, (uint32_t)nloc); buf_put32(&o, (uint32_t)next_);
    buf_put32(&o, (uint32_t)(nloc + next_)); buf_put32(&o, (uint32_t)nund);
    for (int i = 0; i < 12; i++) buf_put32(&o, 0); // remaining dysym fields

    // Section data (offsets pinned above), then relocations.
    while (o.len < base) buf_put8(&o, 0);
    for (int i = 0; i < NSECS; i++) {
        Sec* s = &e->secs[i];
        while (o.len < s->offset) buf_put8(&o, 0);
        if (s->flags & S_ZEROFILL) continue;
        buf_write(&o, s->data.d, s->data.len);
    }
    for (int i = 0; i < NSECS; i++)
        buf_write(&o, relocs[i].d, relocs[i].len);

    // Symbol table (in `order`), then the string table.
    uint32_t symoff = (uint32_t)((o.len + 7) & ~(size_t)7);
    while (o.len < symoff) buf_put8(&o, 0);
    for (size_t i = 0; i < norder; i++) {
        ESym* s = order[i];
        buf_put32(&o, s->strx);
        buf_put8(&o, s->type ? s->type : N_EXT_); // undefined: N_EXT only
        buf_put8(&o, s->sect);
        buf_put16(&o, 0);                         // n_desc
        uint64_t value = s->value;
        if (s->sect > 0 && s->sect <= NSECS)
            value += e->secs[s->sect - 1].addr;
        buf_put64(&o, value);
    }
    uint32_t stroff = (uint32_t)o.len;
    buf_write(&o, str.d, str.len);

    buf_patch32(&o, symcmd + 8, symoff);
    buf_patch32(&o, symcmd + 16, stroff);

    size_t object_size = o.len;
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "ace: cannot write '%s'\n", path);
        e->d->errors++;
    } else {
        fwrite(o.d, 1, o.len, fp);
        fclose(fp);
    }
    free(str.d);
    free(o.d);
    free(order);
    for (int i = 0; i < NSECS; i++) free(relocs[i].d);
    return e->d->errors ? 0 : object_size;
}

// --- entry ---------------------------------------------------------------------------

int emit_object(Node* file, Diag* d, const char* out_path,
                CompileReport* report) {
    static const Sec PROTO[NSECS] = {
        [SEC_TEXT]       = { .sect = "__text",       .seg = "__TEXT",
                             .flags = S_ATTR_PURE_INSTRUCTIONS |
                                      S_ATTR_SOME_INSTRUCTIONS, .align = 2 },
        [SEC_CONST]      = { .sect = "__const",      .seg = "__DATA_CONST",
                             .align = 3 },
        [SEC_DATA]       = { .sect = "__data",       .seg = "__DATA",
                             .align = 3 },
        [SEC_BSS]        = { .sect = "__bss",        .seg = "__DATA",
                             .flags = S_ZEROFILL, .align = 3 },
        [SEC_DEBUG_ABBREV] = { .sect = "__debug_abbrev", .seg = "__DWARF",
                               .flags = S_ATTR_DEBUG },
        [SEC_DEBUG_INFO] = { .sect = "__debug_info", .seg = "__DWARF",
                             .flags = S_ATTR_DEBUG },
        [SEC_DEBUG_LINE] = { .sect = "__debug_line", .seg = "__DWARF",
                             .flags = S_ATTR_DEBUG },
    };

    Em e;
    memset(&e, 0, sizeof e);
    e.d = d;
    for (int i = 0; i < NSECS; i++) {
        e.secs[i].sect = PROTO[i].sect;
        e.secs[i].seg = PROTO[i].seg;
        e.secs[i].flags = PROTO[i].flags;
        e.secs[i].align = PROTO[i].align;
    }
    sym_define(&e.syms, e.secs, "__ace$text", 10, 0, &e.secs[SEC_TEXT]);

    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        switch (n->kind) {
        case N_FUNC: {
            if (n->ival == BI_MEM_ALLOC || n->ival == BI_MEM_REALLOC ||
                n->ival == BI_MEM_RELEASE || n->ival == BI_MEM_COPY ||
                n->ival == BI_MEM_MOVE || n->ival == BI_MEM_FILL ||
                n->ival == BI_MEM_ZERO || n->ival == BI_MEM_COMPARE ||
                n->ival == BI_MEM_ALLOC_T || n->ival == BI_MEM_REALLOC_T ||
                n->ival == BI_MEM_RELEASE_T)
                break; // compiler-backed std/mem declarations have no body symbol
            if (n->ival == BI_FMT_PRINTF)
                break; // format-dependent calls lower directly; no runtime printf symbol
            Sym* decl_sym = node_sym(n);
            if (n->c && !(decl_sym && decl_sym->spec_args)) {
                // generic template: one function per instantiation memo entry
                int nt = n->c->nkids;
                for (STypeInst* inst = sema_insts(n); inst; inst = inst->next) {
                    sema_stamp_body(n, inst->args, nt);
                    Sym* gs = node_sym(n);
                    const char* mn = fn_mangle(gs ? gs->name : n->str,
                                               gs ? gs->len : n->len, inst->result,
                                               gs ? gs->decl : n, inst->args, nt, 0);
                    emit_func(&e, n, mn, (uint32_t)strlen(mn),
                              inst->result->ret, 0, 0);
                }
                break;
            }
            int is_main = !(n->flags & F_METHOD) && n->len == 4 &&
                          memcmp(n->str, "main", 4) == 0;
            int export_c = (n->flags & F_EXPO) != 0;
            SType* ret = n->a ? sema_build_type(n->a) : NULL;
            if (is_main) {
                emit_func(&e, n, "_main", 5, ret, 1, 0);
            } else if (export_c) {
                const char* cn = n->cname ? n->cname : n->str;
                char nm[512];
                snprintf(nm, sizeof nm, "_%s", cn);
                emit_func(&e, n, nm, (uint32_t)strlen(nm), ret, 0, 1);
            } else {
                Sym* fs = node_sym(n);
                const char* mn = fn_mangle(fs ? fs->name : n->str,
                                           fs ? fs->len : n->len,
                                           fs ? fs->type : NULL, n,
                                           fs ? fs->spec_args : NULL,
                                           fs && fs->spec_args ? fs->spec_nargs : 0,
                                           fs && fs->spec_args != NULL);
                emit_func(&e, n, mn, (uint32_t)strlen(mn), ret, 0, 0);
            }
            break;
        }
        case N_VAR:
            emit_global(&e, n);
            break;
        case N_SIGNING:
            emit_signing(&e, n);
            break;
        default:
            break; // types/imports/externs: no code
        }
    }
    patch_fixups(&e);
    if (d->errors) return 1;
    if (report) report->machine_code = text(&e)->len;
    ESym* text_anchor = sym_intern(&e.syms, "__ace$text", 10);
    emit_debug_info(&e, text_anchor);
    emit_debug_line(&e, text_anchor);
    size_t object_size = write_object(&e, out_path);
    if (report) report->object_file = object_size;
    return d->errors ? 1 : 0;
}
