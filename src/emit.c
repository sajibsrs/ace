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
//     in-place, never relocated (SPEC.md Compiler #5)
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

static uint32_t sym_hash(const char* p, uint32_t len) {
    uint32_t h = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < len; i++) h = (h ^ (uint8_t)p[i]) * 16777619u;
    return h;
}

static void sym_slot_put(SymTab* t, ESym* s) {
    size_t i = s->hash & (t->slot_cap - 1);
    while (t->slots[i]) i = (i + 1) & (t->slot_cap - 1);
    t->slots[i] = s;
}

static ESym* sym_intern(SymTab* t, const char* p, uint32_t len) {
    uint32_t h = sym_hash(p, len);
    if (t->slot_cap) {
        for (size_t i = h & (t->slot_cap - 1); t->slots[i];
             i = (i + 1) & (t->slot_cap - 1)) {
            ESym* s = t->slots[i];
            if (s->hash == h && s->len == len && memcmp(s->name, p, len) == 0)
                return s;
        }
    }
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
    s->hash = h;
    s->len = len;
    s->strx = s->ord = 0;
    s->type = 0;
    s->sect = 0;
    s->value = 0;
    if (t->n * 2 > t->slot_cap) { // load factor <= 1/2
        size_t cap = t->slot_cap ? t->slot_cap * 2 : 64;
        free(t->slots);
        t->slots = calloc(cap, sizeof(ESym*));
        if (!t->slots) oom();
        t->slot_cap = cap;
        for (size_t i = 0; i < t->n; i++) sym_slot_put(t, t->v[i]);
    } else {
        sym_slot_put(t, s);
    }
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
typedef struct {
    Sym* s;
    ESym* keys;
    ESym* values;
    ESym* slots;
    uint64_t seed;
    uint32_t mask;
    uint32_t count;
    uint8_t string_keys;
} StaticMapData;
typedef struct { uint32_t addr; int line, col; const char* path; } LineRow;
typedef struct { uint32_t addr, size; const char* name; uint32_t nlen; } DebugFn;

enum { CLS_INT, CLS_FP, CLS_ADDR };
typedef struct { uint8_t cls; int idx; } EV; // int temp / fp temp / address in int temp

typedef struct { // register-promotion candidate: a scalar local or parameter
    Node*    decl;    // binder node (the Sym's decl)
    uint32_t weight;  // use_weight summed over its uses
    uint8_t  escaped; // its address is taken somewhere; stays in its slot
} PromoCand;

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

#define MAX_ITEMPS 14  // x0-x7, x9-x14
#define LEAF_REGS 6    // x14 down to x9: promoted locals of a call-free function
/* temp_need counts operands, not the internal temps some lowerings add (a
 * static-map probe, a byte-sequence compare); the largest shortfall across
 * the std, test, bench, and package corpus is 2, so leaf promotion leaves 3
 * more. tmp_int's cap still rejects, never miscompiles, an expression beyond. */
#define TEMP_MARGIN 3
#define MAX_FPTEMPS 16 // v0-v7, v16-v23
#define LOOP_REGS 10   // x19-x28, callee-saved loop state and promoted locals
#define FP_PROMO_REGS 8 // d8-d15, callee-saved promoted float locals

typedef struct {
    Diag* d;
    Node* file;
    EmitMode mode;
    int bounds_fail_used;
    int arithmetic_fail_used;
    int contract_fail_used;
    int null_fail_used;
    int allocator_used;
    int mem_copy_used; // __ace$mem_copy / __ace$mem_move
    int mem_fill_used; // __ace$mem_fill
    int mem_compare_used; // __ace$mem_compare
    int current_is_main;
    Sec   secs[NSECS];
    SymTab syms;
    Fixup* fix;
    size_t nfix, capfix;
    GVar*  globs;
    int    nglobs, cglobs;
    StaticMapData* maps;
    int    nmaps, cmaps;
    int    str_n;
    // current function
    size_t subsp_fix;
    uint32_t frame;
    uint32_t int_spill[MAX_ITEMPS]; // per-temp spill slots, 0 until first use
    uint32_t fp_spill[MAX_FPTEMPS];
    Node**   lreg_loops; // loops whose state lives in x19-x28 (plan_loops)
    int      nlreg_loops, clreg_loops;
    int      lreg_top;   // next free loop register, as an offset from x19
    int      lreg_max;   // loop registers x19..x19+lreg_max-1
    Node*    preg_decl[LOOP_REGS]; // promoted locals, from x19+lreg_max up
    int      npreg;
    Node*    xpreg_decl[LEAF_REGS]; // leaf-function promoted locals, x14 down
    int      nxpreg;
    int      vti_cap;    // int temps available: MAX_ITEMPS less nxpreg
    int      scan_call;  // promo_scan met a call that may clobber x9-x14
    int      nsaved;     // callee-saved x registers the function saves and restores
    uint32_t lreg_save;  // depth of the callee-saved x save area
    Node*    fpreg_decl[FP_PROMO_REGS]; // promoted float locals, d8 up
    int      nfpreg;
    uint32_t fpreg_save; // depth of the d8-d15 save area
    PromoCand* pcands;   // plan_promotion scratch
    int      npcands, cpcands;
    int      fn_has_asm;
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

// Pad with nops to a code boundary; __text is 64-byte aligned, so the
// boundary holds after linking. Function entries and loop heads start on one
// so identical code does not change speed with its offset in a fetch block.
// Entry padding never executes; loop-head padding runs once per loop entry.
enum { FUNC_ALIGN = 16, LOOP_ALIGN = 16 };
static void align_code(Em* e, uint32_t bytes) {
    while (text(e)->len % bytes) ins(e, 0xD503201Fu);
}

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

// Relocations collected while folding a constant initializer.
typedef struct {
    uint32_t off;
    ESym* sym;
} ConstRel;

typedef struct {
    ConstRel* v;
    int n, cap;
} ConstRels;

static EV gen_expr(Em* e, Node* x);
static void mov_int(Em* e, int sf, int dst, int src);
static void emit_bl_internal(Em* e, const char* p, uint32_t len);
static int eval_const(Em* e, Node* v, SType* t, Buf* out, ConstRels* rels);
static void emit_stmt(Em* e, Node* s);
static int loop_regs_take(Em* e, Node* s);
static int loop_own_regs(Node* s);
static void plan_promotion(Em* e, Node* f);
static int promo_reg(Em* e, Sym* s);
static int gen_addr(Em* e, Node* x);
static int is_lvalue_node(Node* x);
static SType* lvalue_type(Em* e, Node* x);
static void emit_asm(Em* e, Node* s);
static void emit_epilogue(Em* e);
static ESym* emit_vtable(Em* e, SType* conc, SType* contract, Node* conf);
static StaticMapData* map_data(Em* e, Sym* s);

// --- type helpers -----------------------------------------------------------------

static SType* unwrap(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    return t;
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
           t->kind == SK_BITS ||
           t->kind == SK_ERROR;
}

static int ty_is_signed(SType* t) {
    t = unwrap(t);
    if (!t) return 1;
    if (t->kind == SK_ENUM || t->kind == SK_BITS) return ty_is_signed(t->base);
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
    case SK_PTR: case SK_FUNC: case SK_ENUM: case SK_BITS: case SK_ERROR: return 1;
    default: return 0;
    }
}

static int scalar_bits(SType* t) {
    t = unwrap(t);
    if (!t || t->kind == SK_UT) return 64;
    if (t->kind == SK_ENUM || t->kind == SK_BITS) return (int)(ty_size(t) * 8);
    if (t->kind == SK_PTR || t->kind == SK_FUNC || t->kind == SK_ERROR) return 64;
    if (t->kind == SK_PRIM && t->prim != P_STRING) return (int)(t->size * 8);
    return 64;
}

static uint32_t field_off(SType* t, int want) {
    int special = t->kind == SK_STRUCT && (t->is_packed || t->requested_align);
    if (t->kind == SK_STRUCT && !special)
        for (int i = 0; i < t->nfields; i++)
            if (t->fields[i].requested_align) {
                special = 1;
                break;
            }
    if (special) {
        (void)ty_size(t); // populate special-layout field offsets on demand
        return want >= 0 && want < t->nfields ? t->fields[want].offset : 0;
    }
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
        for (int i = 0; i < t->nfields; i++) {
            n = hfa_leaves(t->fields[i].t, off + field_off(t, i), offs, dbl, n, max);
            if (n < 0) return -1;
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
    int complement = 0;
    if (n->kind == N_UNARY && n->op == T_MINUS) {
        neg = 1;
        n = n->a;
    } else if (n->kind == N_UNARY && n->op == T_TILDE) {
        complement = 1;
        n = n->a;
    }
    if (n->kind == N_CALL && !n->sym && n->nkids == 1 &&
        n->a && n->a->kind == N_IDENT && node_ty(n->a) &&
        node_ty(n->a)->kind != SK_ERROR_DOMAIN) { // a domain packs its id above the code
        uint64_t inner;
        if (!const_int(n->kids[0], &inner)) return 0;
        *out = neg ? (uint64_t)0 - inner : complement ? ~inner : inner;
        return 1;
    }
    if (n->kind == N_IDENT) {
        Sym* s = node_sym(n);
        if (s && s->is_global && s->is_const && s->init) {
            uint64_t inner;
            if (!const_int(s->init, &inner)) return 0;
            *out = neg ? (uint64_t)0 - inner : complement ? ~inner : inner;
            return 1;
        }
    }
    if (n->kind != N_INT && n->kind != N_BOOL && n->kind != N_CHAR) return 0;
    SType* t = unwrap(node_ty(v));
    if (!t) return 0;
    if (t->kind == SK_UT) {
        *out = neg ? (uint64_t)0 - n->ival : complement ? ~n->ival : n->ival;
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
    *out = neg ? (uint64_t)0 - n->ival : complement ? ~n->ival : n->ival;
    return 1;
}

/*
 * A read of a global const integer with a compile-time initializer is its
 * value, not a load: yields the bits a slot load would produce. Narrow signed
 * negatives are left to the load, whose extension differs from a plain move.
 */
static int const_global_imm(Sym* s, SType* t, Node* x, uint64_t* out) {
    if (!s->is_global || !s->is_const || !s->init || !is_scalar(t) ||
        is_floatt(t))
        return 0;
    SType* u = unwrap(t);
    uint64_t c;
    if (!u || u->kind != SK_PRIM || !const_int(x, &c)) return 0;
    uint32_t size = ty_size(t);
    if (size < 8) {
        c &= (UINT64_C(1) << (size * 8)) - 1;
        if (ty_is_signed(t) && (c >> (size * 8 - 1)) & 1) return 0;
    }
    *out = c;
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
    if (align > 16) align = 16;
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
    if (e->vti >= e->vti_cap) {
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

/*
 * Spill slots are allocated per temp index on first use rather than as one
 * fixed area, so the frame stays shallow and later locals remain within the
 * single-instruction ldur/stur range (x29 - 256).
 */
static uint32_t spill_slot_int(Em* e, int i) {
    if (!e->int_spill[i]) e->int_spill[i] = temp_slot(e, 8, 8);
    return e->int_spill[i];
}
static uint32_t spill_slot_fp(Em* e, int j) {
    if (!e->fp_spill[j]) e->fp_spill[j] = temp_slot(e, 8, 8);
    return e->fp_spill[j];
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
    if (d <= 256) { // ldur rt, [x29, #-d]; ldursb/ldursh wt sign-extend narrow
        uint32_t b = !signext || w >= 4 ? ldur_base(w)
                   : w == 1 ? 0x38C00000u : 0x78C00000u;
        ins(e, b | ((((uint32_t)(0u - d)) & 0x1FFu) << 12) |
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
    if (off <= 256) { // stur s|d, [x29, #-off]
        ins(e, (is64 ? 0xFC000000u : 0xBC000000u) |
               ((((uint32_t)(0u - off)) & 0x1FFu) << 12) | (29u << 5) |
               (uint32_t)vr);
        return;
    }
    frame_addr(e, 16, off);
    ins(e, (is64 ? 0xFD000000u : 0xBD000000u) | (16u << 5) | (uint32_t)vr);
}

static void load_frame_fp(Em* e, int vr, uint32_t off, int is64) {
    if (off <= 256) { // ldur s|d, [x29, #-off]
        ins(e, (is64 ? 0xFC400000u : 0xBC400000u) |
               ((((uint32_t)(0u - off)) & 0x1FFu) << 12) | (29u << 5) |
               (uint32_t)vr);
        return;
    }
    frame_addr(e, 16, off);
    ins(e, (is64 ? 0xFD400000u : 0xBD400000u) | (16u << 5) | (uint32_t)vr);
}

static void spill_temps(Em* e) {
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
    if (off % w == 0 && off / w < 4096)
        ins(e, b | ((off / w) << 10) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    else {
        load_imm(e, 1, 16, off);
        uint32_t rb;
        if (!signext)
            rb = w == 1 ? 0x38606800u : w == 2 ? 0x78606800u :
                 w == 4 ? 0xB8606800u : 0xF8606800u;
        else
            rb = w == 1 ? 0x38E06800u : w == 2 ? 0x78E06800u :
                 w == 4 ? 0xB8606800u : 0xF8606800u;
        ins(e, rb | (16u << 16) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    }
}

static void st_reg(Em* e, int rt, int base, uint32_t off, uint32_t w) {
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u :
                 w == 4 ? 0xB9000000u : 0xF9000000u;
    if (off % w == 0 && off / w < 4096)
        ins(e, b | ((off / w) << 10) | ((uint32_t)base << 5) |
               ((uint32_t)rt & 31u));
    else {
        load_imm(e, 1, 16, off);
        uint32_t rb = w == 1 ? 0x38206800u : w == 2 ? 0x78206800u :
                      w == 4 ? 0xB8206800u : 0xF8206800u;
        ins(e, rb | (16u << 16) | ((uint32_t)base << 5) |
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

// rd = rn + off; rn may be sp (immediate adds read register 31 as sp)
static void addr_plus(Em* e, int rd, int rn, uint32_t off) {
    uint32_t lo = off & 0xFFFu, hi = off >> 12;
    if (hi) {
        ins(e, 0x91400000u | ((hi & 0xFFFu) << 10) | ((uint32_t)rn << 5) |
               (uint32_t)rd); // add rd, rn, #hi, lsl #12
        rn = rd;
    }
    if (lo || !hi)
        ins(e, 0x91000000u | (lo << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
}

/*
 * byte copy [dst+doff, +size) <- [src+soff, +size). Up to 32 bytes copies
 * inline through x16; larger sizes call __ace$mem_copy, clobbering x8,
 * x15-x17, x30, v24-v31, and flags. Ranges must not partially overlap.
 */
static void copy_mem(Em* e, int dst, uint32_t doff, int src, uint32_t soff, uint32_t size) {
    if (!size) return;
    if (size <= 32) {
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
    // x15 = dst + doff, x16 = src + soff, ordered so neither source register
    // is overwritten before it is read.
    if (dst != 16) {
        addr_plus(e, 16, src, soff);
        addr_plus(e, 15, dst, doff);
    } else if (src != 15) {
        addr_plus(e, 15, dst, doff);
        addr_plus(e, 16, src, soff);
    } else {
        addr_plus(e, 17, src, soff);
        addr_plus(e, 15, dst, doff);
        mov_int(e, 1, 16, 17);
    }
    load_imm(e, 1, 17, size);
    e->mem_copy_used = 1;
    emit_bl_internal(e, "__ace$mem_copy", 14);
}

// --- scalar moves ---------------------------------------------------------------------

static void mov_int(Em* e, int sf, int dst, int src) {
    if (dst == src) return;
    ins(e, (sf ? 0xAA0003E0u : 0x2A0003E0u) | ((uint32_t)src << 16) |
           ((uint32_t)dst & 31u));
}

static void mov_fp(Em* e, int is64, int dst, int src) {
    if (dst == src) return;
    ins(e, (is64 ? 0x1E604000u : 0x1E204000u) | ((uint32_t)vreg(src) << 5) |
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

/*
 * Load a scalar of type t from [a+off] into address temp a itself (a load
 * without writeback may name its base as destination); floats land in a fresh
 * fp temp and leave a allocated.
 */
static EV load_in_place(Em* e, int a, uint32_t off, SType* t) {
    if (is_floatt(t)) return load_scalar(e, xreg(a), off, t);
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    ld_reg(e, xreg(a), xreg(a), off, w, ty_is_signed(t) && w < 8);
    return (EV){ CLS_INT, a };
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
    // Float orderings are false on an unordered fcmp (NZCV = 0011): LO/LS test
    // C and GT/GE test N == V, all of which fail there.
    if (is_floatt(t)) {
        switch (op) {
        case T_LT: return C_LO;
        case T_LE: return C_LS;
        case T_GT: return C_GT;
        case T_GE: return C_GE;
        default: break;
        }
    }
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

static void emit_arithmetic_fail_call(Em* e);

// rd = ra op rb on physical registers; ra and rb are only read
static void emit_arith(Em* e, TokKind op, int rd, int ra, int rb, SType* t) {
    int sf = scalar_bits(t) == 64;
    uint32_t s = sf ? 0x80000000u : 0;
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

/*
 * Division by a constant (Granlund-Montgomery, with libdivide's generator):
 * a divisor known at compile time becomes a multiply-high by a reciprocal
 * "magic" number plus shifts, and powers of two become shifts and masks.
 * Values narrower than 32 bits live extended in 32-bit registers, so every
 * integer type divides at width 32 or 64.
 */
typedef struct { uint64_t magic; int shift, add, neg; } DivMagic;

// floor(2^(w + p) / d) and its remainder, for p < w and 2^p < d.
static uint64_t pow2_quotient(int w, int p, uint64_t d, uint64_t* rem) {
    if (w == 32) {
        uint64_t n = UINT64_C(1) << (32 + p);
        *rem = n % d;
        return n / d;
    }
    uint64_t hi = UINT64_C(1) << p, q = 0;
    for (int i = 0; i < 64; i++) { // 128-by-64 long division of hi:0
        uint64_t top = hi >> 63;
        hi <<= 1;
        q <<= 1;
        if (top || hi >= d) {
            hi -= d;
            q |= 1;
        }
    }
    *rem = hi;
    return q;
}

static int floor_log2(uint64_t v) {
    int l = 0;
    while (v >>= 1) l++;
    return l;
}

// Magic for d >= 3, not a power of two; sign selects the signed variant.
static DivMagic div_magic(uint64_t d, int w, int sign, int neg) {
    uint64_t mask = w == 64 ? UINT64_MAX : UINT32_MAX;
    int l = floor_log2(d);
    uint64_t rem, q = pow2_quotient(w, sign ? l - 1 : l, d, &rem);
    DivMagic m = { 0, sign ? l - 1 : l, 0, neg };
    if (d - rem >= (UINT64_C(1) << l)) { // needs the extra precision bit
        uint64_t twice = (rem * 2) & mask;
        q = (q * 2) & mask;
        if (twice >= d || twice < rem) q++;
        m.shift = l;
        m.add = 1;
    }
    q = (q + 1) & mask;
    m.magic = (neg ? (uint64_t)0 - q : q) & mask;
    return m;
}

/*
 * dst = a / c or a % c for a constant divisor. Returns 0, emitting nothing,
 * when the hardware divide must stay: a zero divisor, or signed -1, whose
 * checked-build overflow guard lives with the generic path. Uses x16/x17.
 */
static int emit_const_divide(Em* e, TokKind op, int dst, int a, uint64_t c,
                             SType* t) { // physical registers; a is only read
    SType* u = unwrap(t);
    if (!u || u->kind != SK_PRIM || !is_intt(u)) return 0;
    int w = scalar_bits(u) == 64 ? 64 : 32, sf = w == 64;
    uint32_t s = sf ? 0x80000000u : 0;
    int sig = ty_is_signed(u);
    uint64_t mask = sf ? UINT64_MAX : UINT32_MAX;
    int64_t sd = sf ? (int64_t)c : (int64_t)(int32_t)(uint32_t)c;
    uint64_t d = sig ? (sd < 0 ? (uint64_t)0 - (uint64_t)sd : (uint64_t)sd) & mask
                     : c & mask;
    if (d == 0 || (sig && sd == -1)) return 0;
    uint32_t ra = (uint32_t)a, rd = (uint32_t)dst;
    uint32_t qd = op == T_SLASH ? rd : 16u; // the remainder keeps q in x16
    uint32_t lsr = sf ? 0xD340FC00u : 0x53007C00u; // UBFM shift-right form
    uint32_t asr = sf ? 0x9340FC00u : 0x13007C00u; // SBFM shift-right form

    if (d == 1) {
        if (op == T_SLASH) mov_int(e, sf, (int)rd, (int)ra);
        else load_imm(e, sf, (int)rd, 0);
        return 1;
    }
    if ((d & (d - 1)) == 0) {
        uint32_t k = (uint32_t)floor_log2(d);
        if (!sig) {
            if (op == T_SLASH) ins(e, lsr | (k << 16) | (ra << 5) | rd);
            else ins(e, (sf ? 0x92400000u : 0x12000000u) | ((k - 1) << 10) |
                        (ra << 5) | rd); // and rd, ra, #(d - 1)
            return 1;
        }
        // Signed rounds toward zero: bias negative dividends by d - 1.
        ins(e, asr | ((uint32_t)(w - 1) << 16) | (ra << 5) | 16u);
        ins(e, s | 0x0B400000u | (16u << 16) | ((uint32_t)(w - (int)k) << 10) |
               (ra << 5) | 16u); // add x16, ra, x16, lsr #(w - k)
        ins(e, asr | (k << 16) | (16u << 5) | (sd < 0 ? 16u : qd));
        if (sd < 0) ins(e, s | 0x4B0003E0u | (16u << 16) | qd); // neg
    } else if (!sig) {
        DivMagic m = div_magic(d, w, 0, 0);
        load_imm(e, sf, 17, m.magic);
        if (sf) ins(e, 0x9BC07C00u | (17u << 16) | (ra << 5) | 16u); // umulh
        else ins(e, 0x9BA07C00u | (17u << 16) | (ra << 5) | 16u);    // umull
        if (!m.add) {
            ins(e, 0xD340FC00u | ((uint32_t)((sf ? 0 : 32) + m.shift) << 16) |
                   (16u << 5) | qd);
        } else {
            if (!sf) ins(e, 0xD340FC00u | (32u << 16) | (16u << 5) | 16u);
            ins(e, s | 0x4B000000u | (16u << 16) | (ra << 5) | 17u); // a - t
            ins(e, s | 0x0B400000u | (17u << 16) | (1u << 10) | (16u << 5) |
                   16u); // t + ((a - t) >> 1)
            ins(e, lsr | ((uint32_t)m.shift << 16) | (16u << 5) | qd);
        }
    } else {
        DivMagic m = div_magic(d, w, 1, sd < 0);
        load_imm(e, sf, 17, m.magic);
        if (sf) ins(e, 0x9B407C00u | (17u << 16) | (ra << 5) | 16u); // smulh
        else ins(e, 0x9B207C00u | (17u << 16) | (ra << 5) | 16u);    // smull
        if (!m.add) {
            ins(e, 0x9340FC00u | ((uint32_t)((sf ? 0 : 32) + m.shift) << 16) |
                   (16u << 5) | 16u);
        } else {
            if (!sf) ins(e, 0x9340FC00u | (32u << 16) | (16u << 5) | 16u);
            ins(e, s | (m.neg ? 0x4B000000u : 0x0B000000u) | (ra << 16) |
                   (16u << 5) | 16u); // t -= a or t += a
            ins(e, asr | ((uint32_t)m.shift << 16) | (16u << 5) | 16u);
        }
        ins(e, s | 0x0B400000u | (16u << 16) | ((uint32_t)(w - 1) << 10) |
               (16u << 5) | qd); // round toward zero: q += q < 0
    }
    if (op == T_PERCENT) { // r = a - q * c
        load_imm(e, sf, 17, c & mask);
        ins(e, s | 0x1B008000u | (17u << 16) | (ra << 10) | (16u << 5) | rd);
    }
    return 1;
}

/* Checked integer arithmetic is kept at each operation site.  The operands
 * are already extended according to their declared type by load_scalar; for
 * 8/16-bit arithmetic a 32-bit result therefore has enough room to range-check
 * after the operation. */
// physical registers, as emit_arith: ra and rb are only read
static void emit_checked_arith(Em* e, TokKind op, int rd, int ra, int rb,
                               SType* t) {
    int bits = scalar_bits(t);
    int sf = bits == 64;
    int sig = ty_is_signed(t);
    if (e->mode != EMIT_CHECKED || !is_intt(t)) {
        emit_arith(e, op, rd, ra, rb, t);
        return;
    }

    if (op == T_SLASH || op == T_PERCENT) {
        int ok = new_label(e);
        cbz_(e, sf, rb, 1, ok);
        emit_arithmetic_fail_call(e);
        bind_label(e, ok);
        if (sig) {
            int not_min = new_label(e), safe = new_label(e);
            uint64_t min = bits == 64 ? UINT64_C(0x8000000000000000)
                                      : (uint32_t)(-(INT64_C(1) << (bits - 1)));
            load_imm(e, sf, 16, min);
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                   (16u << 16) | ((uint32_t)ra << 5));
            bcond_(e, C_NE, not_min);
            load_imm(e, sf, 16, sf ? UINT64_MAX : UINT32_MAX);
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                   (16u << 16) | ((uint32_t)rb << 5));
            bcond_(e, C_NE, safe);
            emit_arithmetic_fail_call(e);
            bind_label(e, safe);
            bind_label(e, not_min);
        }
        emit_arith(e, op, rd, ra, rb, t);
        return;
    }

    if (op == T_SHL || op == T_SHR) {
        if (sig) {
            int nonnegative = new_label(e);
            ins(e, (sf ? 0xF100001Fu : 0x7100001Fu) | ((uint32_t)rb << 5));
            bcond_(e, C_GE, nonnegative);
            emit_arithmetic_fail_call(e);
            bind_label(e, nonnegative);
        }
        int in_range = new_label(e);
        load_imm(e, sf, 16, (uint64_t)bits);
        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
               (16u << 16) | ((uint32_t)rb << 5));
        bcond_(e, C_LO, in_range);
        emit_arithmetic_fail_call(e);
        bind_label(e, in_range);
        emit_arith(e, op, rd, ra, rb, t);
        return;
    }

    if (op == T_PLUS || op == T_MINUS) {
        uint32_t base;
        if (op == T_PLUS) base = sf ? 0xAB000000u : 0x2B000000u; // adds
        else              base = sf ? 0xEB000000u : 0x6B000000u; // subs
        ins(e, base | ((uint32_t)rb << 16) | ((uint32_t)ra << 5) |
               (uint32_t)rd);
        if (bits >= 32) {
            int ok = new_label(e);
            bcond_(e, sig ? 7 /* VC */ : (op == T_PLUS ? C_LO : C_HS), ok);
            emit_arithmetic_fail_call(e);
            bind_label(e, ok);
            return;
        }
    } else if (op == T_STAR) {
        if (bits < 32) {
            emit_arith(e, op, rd, ra, rb, t);
        } else if (bits == 32) {
            ins(e, (sig ? 0x9B207C00u : 0x9BA07C00u) |
                   ((uint32_t)rb << 16) | ((uint32_t)ra << 5) | 16u);
            mov_int(e, 0, rd, 16);
            if (sig) ins(e, 0x93407C00u | ((uint32_t)rd << 5) | 17u); // sxtw x17,wrd
            else     mov_int(e, 0, 17, rd);
            int ok = new_label(e);
            ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5));
            bcond_(e, C_EQ, ok);
            emit_arithmetic_fail_call(e);
            bind_label(e, ok);
            return;
        } else {
            ins(e, (sig ? 0x9B407C00u : 0x9BC07C00u) |
                   ((uint32_t)rb << 16) | ((uint32_t)ra << 5) | 16u);
            emit_arith(e, op, rd, ra, rb, t);
            if (sig) ins(e, 0x937FFC00u | ((uint32_t)rd << 5) | 17u); // asr x17,xrd,#63
            else load_imm(e, 1, 17, 0);
            int ok = new_label(e);
            ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5));
            bcond_(e, C_EQ, ok);
            emit_arithmetic_fail_call(e);
            bind_label(e, ok);
            return;
        }
    } else {
        emit_arith(e, op, rd, ra, rb, t);
        return;
    }

    /* Narrow add/sub/mul: compare the extended 32-bit result with the exact
     * declared range before store_scalar truncates it. */
    uint64_t lo = sig ? (uint64_t)(-(INT64_C(1) << (bits - 1))) : 0;
    uint64_t hi = sig ? (UINT64_C(1) << (bits - 1)) - 1
                      : (UINT64_C(1) << bits) - 1;
    int above_lo = new_label(e), in_range = new_label(e);
    load_imm(e, 0, 16, lo);
    ins(e, 0x6B00001Fu | (16u << 16) | ((uint32_t)rd << 5));
    bcond_(e, sig ? C_GE : C_HS, above_lo);
    emit_arithmetic_fail_call(e);
    bind_label(e, above_lo);
    load_imm(e, 0, 16, hi);
    ins(e, 0x6B00001Fu | (16u << 16) | ((uint32_t)rd << 5));
    bcond_(e, sig ? C_LE : C_LS, in_range);
    emit_arithmetic_fail_call(e);
    bind_label(e, in_range);
}

// integer constant usable as an add/sub/cmp 12-bit unsigned immediate
static int imm12_const(Node* x, uint64_t* out) {
    return const_int(x, out) && *out <= 4095;
}

/* rd = ra +/- imm with the same checks as emit_checked_arith; returns 0 (and
 * emits nothing) when only the register form can check the result. */
static int emit_arith_imm(Em* e, TokKind op, int rd, int ra, uint64_t imm,
                          SType* t) {
    if (op != T_PLUS && op != T_MINUS) return 0;
    int bits = scalar_bits(t), sf = bits == 64;
    int checked = e->mode == EMIT_CHECKED && is_intt(t);
    if (checked && bits < 32) return 0;
    uint32_t base = op == T_PLUS ? (checked ? 0x31000000u : 0x11000000u)
                                 : (checked ? 0x71000000u : 0x51000000u);
    ins(e, (sf ? 0x80000000u : 0) | base | ((uint32_t)imm << 10) |
           ((uint32_t)ra << 5) | (uint32_t)rd);
    if (checked) {
        int ok = new_label(e);
        bcond_(e, ty_is_signed(t) ? 7 /* VC */ : (op == T_PLUS ? C_LO : C_HS), ok);
        emit_arithmetic_fail_call(e);
        bind_label(e, ok);
    }
    return 1;
}

/*
 * AArch64 bitmask immediate: a run of ones rotated within a power-of-two
 * element that is replicated across the register. Returns the N:immr:imms
 * field (bits 22..10) or 0 when v is not encodable (0 and all-ones are not).
 */
static uint32_t logical_imm_field(uint64_t v, int sf) {
    int width = sf ? 64 : 32;
    uint64_t all = sf ? ~UINT64_C(0) : UINT64_C(0xFFFFFFFF);
    v &= all;
    if (v == 0 || v == all) return 0;
    int size = width;
    while (size > 2) {
        int half = size / 2;
        uint64_t mask = (UINT64_C(1) << half) - 1;
        if (((v >> half) & mask) != (v & mask)) break;
        size = half;
    }
    uint64_t mask = size == 64 ? ~UINT64_C(0) : (UINT64_C(1) << size) - 1;
    uint64_t elem = v & mask;
    int ones = __builtin_popcountll(elem);
    uint64_t run = (UINT64_C(1) << ones) - 1; // ones < size: not all-ones
    for (int r = 0; r < size; r++) {
        uint64_t rol = r ? ((elem << r) | (elem >> (size - r))) & mask : elem;
        if (rol != run) continue;
        uint32_t n = size == 64 ? 1u : 0u;
        uint32_t imms = (uint32_t)(((~(size - 1)) << 1) & 0x3F) |
                        (uint32_t)(ones - 1);
        return (n << 12) | ((uint32_t)r << 6) | imms;
    }
    return 0;
}

// rd = ra and/or/xor constant as one instruction; 0 when it does not encode
static int emit_logical_imm(Em* e, TokKind op, int rd, int ra, uint64_t c,
                            SType* t) {
    int sf = scalar_bits(t) == 64;
    uint32_t base;
    switch (op) {
    case T_AMP:   base = 0x12000000u; break;
    case T_PIPE:  base = 0x32000000u; break;
    case T_CARET: base = 0x52000000u; break;
    default: return 0;
    }
    uint32_t field = logical_imm_field(c, sf);
    if (!field) return 0;
    ins(e, (sf ? 0x80000000u : 0) | base | (field << 10) |
           ((uint32_t)ra << 5) | (uint32_t)rd);
    return 1;
}

static void emit_cmp_imm(Em* e, int sf, int ra, uint64_t imm) {
    ins(e, (sf ? 0xF100001Fu : 0x7100001Fu) | ((uint32_t)imm << 10) |
           ((uint32_t)ra << 5));
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

/* Typed pointer arithmetic scales a usize element offset and then performs an
 * unsigned machine-address add/subtract. Checked mode keeps all three local
 * preconditions at the operation site; release is only multiply plus add/sub. */
static void emit_pointer_arith(Em* e, TokKind op, int pointer, int offset,
                               uint32_t elem_size) {
    int rp = xreg(pointer), ro = xreg(offset);
    if (e->mode == EMIT_CHECKED) {
        int nonnull = new_label(e);
        cbz_(e, 1, rp, 1, nonnull);
        emit_arithmetic_fail_call(e);
        bind_label(e, nonnull);
    }
    if (elem_size != 1) {
        load_imm(e, 1, 16, elem_size);
        if (e->mode == EMIT_CHECKED) {
            ins(e, 0x9BC07C00u | (16u << 16) | ((uint32_t)ro << 5) | 17u); // umulh
            int scaled = new_label(e);
            cbz_(e, 1, 17, 0, scaled);
            emit_arithmetic_fail_call(e);
            bind_label(e, scaled);
        }
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)ro << 5) |
               (uint32_t)ro); // mul
    }
    uint32_t base = op == T_PLUS ? 0xAB000000u : 0xEB000000u; // adds/subs x
    ins(e, base | ((uint32_t)ro << 16) | ((uint32_t)rp << 5) | (uint32_t)rp);
    if (e->mode == EMIT_CHECKED) {
        int valid = new_label(e);
        bcond_(e, op == T_PLUS ? C_LO : C_HS, valid);
        emit_arithmetic_fail_call(e);
        bind_label(e, valid);
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
    ins(e, (is64 ? 0x1E602000u : 0x1E202000u) |
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

/*
 * Register a read-only operand is taken from: an integer-class promoted local
 * is read in its own register (v->idx = -1: no temp, no copy), anything else
 * evaluates into a temp. Reading the register at the consumer rather than
 * copying it at evaluation is exact: a promoted local's address never
 * escapes, so no operand evaluated in between can change it. Only consumers
 * that never write their operand registers may call this.
 */
static int src_reg(Em* e, Node* x, EV* v) {
    Sym* s = x->kind == N_IDENT ? node_sym(x) : NULL;
    if (s && s->kind == SYM_VAR && s->type && is_scalar(s->type) &&
        !is_floatt(s->type)) {
        int pr = promo_reg(e, s);
        if (pr >= 0) {
            *v = (EV){ CLS_INT, -1 };
            return pr;
        }
    }
    *v = gen_expr(e, x);
    return v->cls == CLS_FP ? vreg(v->idx) : xreg(v->idx);
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

    // Integer operands are read in place; the result takes the lowest operand
    // temp, or a fresh one when both operands are promoted locals.
    if (is_scalar(t) && !is_floatt(t) &&
        !(t->kind == SK_PTR && (op == T_PLUS || op == T_MINUS))) {
        EV a, b;
        int ra = src_reg(e, n->a, &a);
        if (a.cls != CLS_INT) {
            em_err(e, n, "integer operand did not produce a scalar");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        uint64_t divisor;
        if ((op == T_SLASH || op == T_PERCENT) && const_int(n->b, &divisor)) {
            int d = a.idx >= 0 ? a.idx : tmp_int(e);
            if (emit_const_divide(e, op, xreg(d), ra, divisor, t)) {
                e->vti = d + 1;
                return (EV){ CLS_INT, d };
            }
            e->vti = a.idx >= 0 ? a.idx + 1 : d;
        }
        uint64_t imm;
        int cond = tok_cond(op, t);
        if (imm12_const(n->b, &imm)) {
            int d = a.idx >= 0 ? a.idx : tmp_int(e);
            if (cond >= 0) {
                int sf = scalar_bits(t) == 64;
                emit_cmp_imm(e, sf, ra, imm);
                cset_(e, sf, cond, xreg(d));
                e->vti = d + 1;
                return (EV){ CLS_INT, d };
            }
            if (emit_arith_imm(e, op, xreg(d), ra, imm, t)) {
                e->vti = d + 1;
                return (EV){ CLS_INT, d };
            }
            if (a.idx < 0) e->vti = d; // release the unused result temp
        }
        if ((op == T_AMP || op == T_PIPE || op == T_CARET) &&
            const_int(n->b, &imm)) {
            int d = a.idx >= 0 ? a.idx : tmp_int(e);
            if (emit_logical_imm(e, op, xreg(d), ra, imm, t)) {
                e->vti = d + 1;
                return (EV){ CLS_INT, d };
            }
            if (a.idx < 0) e->vti = d; // release the unused result temp
        }
        int rb = src_reg(e, n->b, &b);
        if (b.cls != CLS_INT) {
            em_err(e, n, "integer operand did not produce a scalar");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        int d = a.idx >= 0 ? a.idx : b.idx >= 0 ? b.idx : tmp_int(e);
        if (cond >= 0) { // comparison -> bool
            int sf = scalar_bits(t) == 64;
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                   ((uint32_t)rb << 16) | ((uint32_t)ra << 5));
            cset_(e, sf, cond, xreg(d));
        } else {
            emit_checked_arith(e, op, xreg(d), ra, rb, t);
        }
        e->vti = d + 1;
        return (EV){ CLS_INT, d };
    }

    EV a = gen_expr(e, n->a);
    EV b = gen_expr(e, n->b);

    if (t->kind == SK_PTR && (op == T_PLUS || op == T_MINUS)) {
        if (a.cls != CLS_INT || b.cls != CLS_INT) {
            em_err(e, n, "pointer arithmetic operands must be scalar");
            return a;
        }
        emit_pointer_arith(e, op, a.idx, b.idx, ty_size(t->base));
        e->vti = a.idx + 1;
        return a;
    }

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
    emit_checked_arith(e, op, xreg(a.idx), xreg(a.idx), xreg(b.idx), t);
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
    if (!isf && nops <= 16) { // integer operands are read in place
        EV v[16];
        int r[16];
        int base = e->vti;
        uint64_t imm; // only the last operand is never a left operand
        int use_imm = imm12_const(n->kids[nops - 1], &imm);
        for (int i = 0; i < nops; i++)
            if (!use_imm || i < nops - 1) r[i] = src_reg(e, n->kids[i], &v[i]);
        int d = tmp_int(e), sf = scalar_bits(t) == 64;
        for (int i = 1; i < nops; i++) {
            int cond = tok_cond((TokKind)n->a->kids[i - 1]->op, t);
            if (cond < 0) cond = C_EQ;
            if (use_imm && i == nops - 1)
                emit_cmp_imm(e, sf, r[i - 1], imm);
            else
                ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                       ((uint32_t)r[i] << 16) | ((uint32_t)r[i - 1] << 5));
            cset_(e, 0, cond, i == 1 ? xreg(d) : 16);
            if (i > 1) // AND-fold the bools
                ins(e, 0x0A000000u | (16u << 16) | ((uint32_t)xreg(d) << 5) |
                       (uint32_t)xreg(d));
        }
        mov_int(e, 0, xreg(base), xreg(d));
        e->vti = base + 1;
        return (EV){ CLS_INT, base };
    }
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

/*
 * Branch to lab when condition x evaluates to jump_if (1 or 0); fall through
 * otherwise. Scalar comparisons — ==/!= and ordering chains — branch on the
 * flags directly (cmp/fcmp + b.cond; inverting the condition code is exact,
 * NaN included), `!` swaps the
 * sense, and && / || chain branches with no bool materialized. Anything else
 * evaluates to a bool and tests it with cbz/cbnz. Leaves no temps live.
 */
static void gen_branch(Em* e, Node* x, int jump_if, int lab) {
    int svi = e->vti, svf = e->vtf;
    if (x->kind == N_UNARY && x->op == T_BANG) {
        gen_branch(e, x->a, !jump_if, lab);
        return;
    }
    if (x->kind == N_BINARY && (x->op == T_ANDAND || x->op == T_OROR)) {
        int is_and = x->op == T_ANDAND;
        if (is_and != jump_if) { // && jumping on false, || jumping on true
            gen_branch(e, x->a, jump_if, lab);
            gen_branch(e, x->b, jump_if, lab);
        } else { // the left operand alone can settle it the other way
            int skip = new_label(e);
            gen_branch(e, x->a, !jump_if, skip);
            gen_branch(e, x->b, jump_if, lab);
            bind_label(e, skip);
        }
        return;
    }
    if (x->kind == N_CMPCHAIN && x->nkids <= 16) { // a < b <= c: every pair holds
        SType* t = node_ty(x->kids[0]);
        if (t && t->kind != SK_UT && is_scalar(t)) {
            int isf = is_floatt(t), sf = scalar_bits(t) == 64;
            EV v[16];
            int r[16];
            uint64_t imm; // only the last operand is never a left operand
            int last = x->nkids - 1;
            int use_imm = !isf && imm12_const(x->kids[last], &imm);
            for (int i = 0; i < x->nkids; i++)
                if (!use_imm || i < last) r[i] = src_reg(e, x->kids[i], &v[i]);
            int skip = jump_if && x->nkids > 2 ? new_label(e) : -1;
            for (int i = 1; i < x->nkids; i++) {
                int cond = tok_cond((TokKind)x->a->kids[i - 1]->op, t);
                if (cond < 0) cond = C_EQ;
                if (isf)
                    emit_fcmp(e, v[i - 1].idx, v[i].idx, unwrap(t)->prim == P_F64);
                else if (use_imm && i == last)
                    emit_cmp_imm(e, sf, r[i - 1], imm);
                else
                    ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                           ((uint32_t)r[i] << 16) | ((uint32_t)r[i - 1] << 5));
                if (!jump_if) bcond_(e, cond ^ 1, lab);       // any pair fails
                else if (i == x->nkids - 1) bcond_(e, cond, lab); // all held
                else bcond_(e, cond ^ 1, skip);
            }
            if (skip >= 0) bind_label(e, skip);
            e->vti = svi; e->vtf = svf;
            return;
        }
    }
    if (x->kind == N_BINARY && x->a->kind != N_NULL && x->b->kind != N_NULL) {
        SType* t = node_ty(x->a);
        int cond = t && is_scalar(t) ? tok_cond((TokKind)x->op, t) : -1;
        uint64_t imm;
        if (cond >= 0 && !is_floatt(t) && imm12_const(x->b, &imm)) {
            EV a;
            int ra = src_reg(e, x->a, &a);
            if (a.cls != CLS_INT) em_err(e, x, "comparison operands must be scalar");
            emit_cmp_imm(e, scalar_bits(t) == 64, ra, imm);
            bcond_(e, jump_if ? cond : cond ^ 1, lab);
            e->vti = svi; e->vtf = svf;
            return;
        }
        if (cond >= 0) {
            EV a, b;
            int ra = src_reg(e, x->a, &a);
            int rb = src_reg(e, x->b, &b);
            if (a.cls == CLS_FP && b.cls == CLS_FP) {
                emit_fcmp(e, a.idx, b.idx, unwrap(t)->prim == P_F64);
            } else if (a.cls == CLS_INT && b.cls == CLS_INT) {
                int sf = scalar_bits(t) == 64;
                ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                       ((uint32_t)rb << 16) | ((uint32_t)ra << 5));
            } else {
                em_err(e, x, "comparison operands must be scalar");
            }
            bcond_(e, jump_if ? cond : cond ^ 1, lab);
            e->vti = svi; e->vtf = svf;
            return;
        }
    }
    EV c;
    int rc = src_reg(e, x, &c);
    if (c.cls != CLS_INT) em_err(e, x, "condition must be bool");
    cbz_(e, 0, rc, jump_if, lab);
    e->vti = svi; e->vtf = svf;
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

static ESym* intern_string(Em* e, Node* s);
static void addr_of_sym(Em* e, int rd, ESym* sym);

static void emit_bounds_fail_call(Em* e, Node* at) {
    e->bounds_fail_used = 1;
    const char* path = at && at->path ? at->path : "<source>";
    size_t need = strlen(path) + 80;
    char* diagnostic = (char*)arena_alloc(need);
    int n = snprintf(diagnostic, need, "%s:%d:%d: bounds violation\n",
                     path, at ? at->line : 0, at ? at->col : 0);
    Node message;
    memset(&message, 0, sizeof message);
    message.kind = N_STR;
    message.str = diagnostic;
    message.len = (uint32_t)n;
    ESym* msg = intern_string(e, &message);
    load_imm(e, 1, 0, 2);
    addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, (uint64_t)n);
    emit_bl_internal(e, "__ace$bounds_fail", 17);
}

static void emit_arithmetic_fail_call(Em* e) {
    e->arithmetic_fail_used = 1;
    emit_bl_internal(e, "__ace$arithmetic_fail", 21);
}

static void emit_contract_fail_call(Em* e) {
    e->contract_fail_used = 1;
    emit_bl_internal(e, "__ace$contract_fail", 19);
}

static void emit_null_fail_call(Em* e) {
    e->null_fail_used = 1;
    emit_bl_internal(e, "__ace$null_fail", 15);
}

static void emit_null_guard(Em* e, int reg) {
    if (e->mode != EMIT_CHECKED) return;
    int ok = new_label(e);
    cbz_(e, 1, reg, 1, ok);
    emit_null_fail_call(e);
    bind_label(e, ok);
}

static int signed_index(Node* index) {
    SType* t = node_ty(index);
    return t && t->kind != SK_UT && t->kind != SK_ERR && ty_is_signed(t);
}

/* The lower comparison deliberately precedes widening: a negative signed
 * value must remain a lower-bound failure rather than becoming a huge usize. */
static void emit_index_guard(Em* e, Node* access, int index_reg,
                             int length_reg, uint64_t array_length) {
    if (e->mode != EMIT_CHECKED || (access->flags & F_BOUNDS_PROVEN)) return;
    if (signed_index(access->b)) {
        int ok = new_label(e);
        int sf = scalar_bits(node_ty(access->b)) == 64;
        ins(e, (sf ? 0xF100001Fu : 0x7100001Fu) |
               ((uint32_t)index_reg << 5)); // cmp index, #0
        bcond_(e, C_GE, ok);
        emit_bounds_fail_call(e, access);
        bind_label(e, ok);
    }
    widen_idx(e, access->b, index_reg);
    if (length_reg < 0) {
        load_imm(e, 1, 16, array_length);
        length_reg = 16;
    }
    int ok = new_label(e);
    ins(e, 0xEB00001Fu | ((uint32_t)length_reg << 16) |
           ((uint32_t)index_reg << 5)); // cmp index, length
    bcond_(e, C_LO, ok);
    emit_bounds_fail_call(e, access);
    bind_label(e, ok);
}

static void emit_nonnegative_guard(Em* e, Node* slice, Node* bound, int reg) {
    enum { BP_LO_NONNEG = 1, BP_HI_NONNEG = 2 };
    uint8_t proof = slice->aux ? *(uint8_t*)slice->aux : 0;
    int bit = bound == slice->b ? BP_LO_NONNEG : BP_HI_NONNEG;
    if (e->mode != EMIT_CHECKED || (slice->flags & F_BOUNDS_PROVEN) ||
        !bound || (proof & bit) || !signed_index(bound)) return;
    int ok = new_label(e);
    int sf = scalar_bits(node_ty(bound)) == 64;
    ins(e, (sf ? 0xF100001Fu : 0x7100001Fu) |
           ((uint32_t)reg << 5));
    bcond_(e, C_GE, ok);
    emit_bounds_fail_call(e, slice);
    bind_label(e, ok);
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
    case SK_STRUCT: case SK_ENUM: case SK_BITS: case SK_CONTRACT: case SK_UNION:
        buf_put8(b, t->kind == SK_STRUCT ? 'S' : t->kind == SK_ENUM ? 'E' :
                   t->kind == SK_BITS ? 'B' : t->kind == SK_UNION ? 'U' : 'I');
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
    if (e->nxpreg) { // call_is_inline let a clobbering call into a leaf
        e->d->errors++;
        fprintf(stderr, "ace: call in a leaf-register function\n");
    }
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

    /* A noalias fact is checked only where the call carries the concrete Ace
     * declaration.  Slice descriptors make the complete reachable byte range
     * locally available; no pointee walk or provenance machinery is added. */
    Sym* called = at ? node_sym(at) : NULL;
    Node* decl = called && called->kind == SYM_FUNC ? called->decl : NULL;
    if (e->mode == EMIT_CHECKED && decl) {
        int guard_ni = e->vti, guard_nf = e->vtf;
        spill_temps(e);
        uint32_t ranges[40];
        memset(ranges, 0, sizeof ranges);
        for (int i = 0; i < n && i < decl->nkids; i++) {
            SType* pt = eff[i];
            if (!pt || pt->kind != SK_SLICE || args[i].val.cls != CLS_ADDR)
                continue;
            ranges[i] = temp_slot(e, 16, 8) + 1; // zero remains "not a range"
            uint32_t off = ranges[i] - 1;
            load_frame(e, 13, spill_slot_int(e, args[i].val.idx), 8, 0);
            ld_reg(e, 16, 13, 0, 8, 0);
            ld_reg(e, 17, 13, 8, 8, 0);
            uint64_t esz = ty_size(pt->base);
            if (esz != 1) {
                load_imm(e, 1, 15, esz);
                ins(e, 0x9BC07C00u | (15u << 16) | (17u << 5) | 14u); // umulh
                int product_ok = new_label(e);
                cbz_(e, 1, 14, 0, product_ok);
                emit_contract_fail_call(e);
                bind_label(e, product_ok);
                ins(e, 0x9B007C00u | (15u << 16) | (17u << 5) | 17u); // mul
            }
            ins(e, 0xAB000000u | (17u << 16) | (16u << 5) | 17u); // adds end
            int end_ok = new_label(e);
            bcond_(e, C_LO, end_ok); // no carry
            emit_contract_fail_call(e);
            bind_label(e, end_ok);
            mov_int(e, 1, 14, 16);
            mov_int(e, 1, 15, 17);
            store_frame(e, 14, off, 8);
            store_frame(e, 15, off + 8, 8);
        }
        for (int i = 0; i < n && i < decl->nkids; i++) {
            if (!ranges[i]) continue;
            for (int j = i + 1; j < n && j < decl->nkids; j++) {
                if (!ranges[j] || !((decl->kids[i]->flags | decl->kids[j]->flags) &
                                    F_NOALIAS))
                    continue;
                uint32_t ai = ranges[i] - 1, aj = ranges[j] - 1;
                load_frame(e, 14, ai, 8, 0);
                load_frame(e, 15, ai + 8, 8, 0);
                load_frame(e, 16, aj, 8, 0);
                load_frame(e, 17, aj + 8, 8, 0);
                int disjoint = new_label(e);
                ins(e, 0xEB00001Fu | (17u << 16) | (14u << 5)); // a.start cmp b.end
                bcond_(e, C_HS, disjoint);
                ins(e, 0xEB00001Fu | (15u << 16) | (16u << 5)); // b.start cmp a.end
                bcond_(e, C_HS, disjoint);
                emit_contract_fail_call(e);
                bind_label(e, disjoint);
            }
        }
        reload_temps(e, guard_ni, guard_nf);
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
    /*
     * A scalar or by-address argument whose temp already is its argument
     * register is placed: the marshal order never writes another argument's
     * register, so it needs neither a spill nor a reload. Temps below `si`
     * were spilled at entry and have not changed since.
     */
    uint8_t placed[40];
    uint8_t skip_spill[64];
    memset(placed, 0, sizeof placed);
    memset(skip_spill, 0, sizeof skip_spill);
    for (int i = 0; i < n; i++) {
        EV v = args[i].val;
        TU* u = &units[i * 4];
        if (v.cls == CLS_FP || (v.cls == CLS_ADDR && !args[i].ptrval)) continue;
        if (u->kind != TU_XREG || u->reg != xreg(v.idx) || v.idx >= 8) continue;
        int single = 1;
        for (int w = 1; w < 4; w++)
            if (units[i * 4 + w].kind != TU_XREG || units[i * 4 + w].reg >= 0)
                single = 0;
        for (int j = 0; j < n; j++)
            if (j != i && args[j].val.cls != CLS_FP && args[j].val.idx == v.idx)
                single = 0;
        if (tgt.kind == TK_REG && tgt.reg == v.idx) single = 0;
        if (!single) continue;
        placed[i] = 1;
        if (v.idx >= si) skip_spill[v.idx] = 1;
    }
    for (int t = si; t < e->vti; t++)
        if (!(t < 64 && skip_spill[t]))
            store_frame(e, xreg(t), spill_slot_int(e, t), 8);
    for (int j = 0; j < e->vtf; j++)
        store_frame_fp(e, vreg(j), spill_slot_fp(e, j), 1);
    for (int i = 0; i < n; i++) {
        if (placed[i]) continue;
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
        emit_null_guard(e, 16);
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
            // move v0 clear of the temps reload_temps rewrites
            if (sf) mov_fp(e, is64, sf, 0);
            reload_temps(e, si, sf);
            e->vti = si; e->vtf = sf;
            int t = tmp_fp(e);
            out = (EV){ CLS_FP, t };
        } else {
            if (si) mov_int(e, 1, xreg(si), 0); // x0 is temp 0's register
            reload_temps(e, si, sf);
            e->vti = si; e->vtf = sf;
            int t = tmp_int(e);
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
 * length mismatch touches no element storage; equal lengths are compared by
 * the shared ordering routine with no converted view.
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
    e->mem_compare_used = 1;
    mov_int(e, 1, 15, xreg(aptr));
    mov_int(e, 1, 16, xreg(bptr));
    mov_int(e, 1, 17, xreg(alen));
    emit_bl_internal(e, "__ace$mem_compare", 17);
    ins(e, 0xF100001Fu | (17u << 5));                 // cmp x17, #0
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

static void addr_of_sym(Em* e, int rd, ESym* sym);

static EV gen_error_description(Em* e, EV value, int message) {
    uint32_t off = temp_slot(e, 16, 8);
    frame_addr(e, 16, off);
    st_reg(e, 31, 16, 0, 8);
    st_reg(e, 31, 16, 8, 8);
    int done = new_label(e);
    if (!message) {
        mov_int(e, 1, 16, xreg(value.idx));
        ins(e, 0xD360FC10u); // lsr x16, x16, #32
    }
    for (int i = 0; i < e->file->nkids; i++) {
        Node* domain = e->file->kids[i];
        if (domain->kind != N_ERROR_DOMAIN) continue;
        SType* dt = node_ty(domain);
        if (!dt) {
            Sym* ds = node_sym(domain);
            dt = ds ? ds->type : NULL;
        }
        if (!dt || !dt->type_id) continue;
        if (!message) {
            load_imm(e, 1, 17, dt->type_id);
            ins(e, 0xEB11021Fu); // cmp x16, x17
            int next = new_label(e);
            bcond_(e, C_NE, next);
            const char* name = domain->cname ? domain->cname : domain->str;
            uint32_t name_len = domain->cname ? (uint32_t)strlen(domain->cname)
                                               : domain->len;
            Node text = { .kind = N_STR, .str = name, .len = name_len };
            ESym* str = intern_string(e, &text);
            addr_of_sym(e, 17, str);
            frame_addr(e, 16, off);
            st_reg(e, 17, 16, 0, 8);
            load_imm(e, 1, 17, name_len);
            st_reg(e, 17, 16, 8, 8);
            br_(e, done);
            bind_label(e, next);
        } else {
            for (int j = 0; j < domain->nkids; j++) {
                Node* entry = domain->kids[j];
                uint64_t packed = ((uint64_t)dt->type_id << 32) |
                                  (uint32_t)entry->ival;
                load_imm(e, 1, 17, packed);
                ins(e, 0xEB11001Fu | ((uint32_t)xreg(value.idx) << 5));
                int next = new_label(e);
                bcond_(e, C_NE, next);
                ESym* str = intern_string(e, entry->b);
                addr_of_sym(e, 17, str);
                frame_addr(e, 16, off);
                st_reg(e, 17, 16, 0, 8);
                load_imm(e, 1, 17, entry->b->len);
                st_reg(e, 17, 16, 8, 8);
                br_(e, done);
                bind_label(e, next);
            }
        }
    }
    bind_label(e, done);
    frame_addr(e, xreg(value.idx), off);
    return (EV){ CLS_ADDR, value.idx };
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

/*
 * Register holding a promoted local, or -1 for slot storage: an x register
 * number for integer-class locals (x19 up, or x14 down in a call-free
 * function), a v register number (8-15) for float locals.
 */
static int promo_reg(Em* e, Sym* s) {
    if (!s || !s->decl) return -1;
    for (int i = 0; i < e->npreg; i++)
        if (e->preg_decl[i] == s->decl) return 19 + e->lreg_max + i;
    for (int i = 0; i < e->nxpreg; i++)
        if (e->xpreg_decl[i] == s->decl) return 14 - i;
    for (int i = 0; i < e->nfpreg; i++)
        if (e->fpreg_decl[i] == s->decl) return 8 + i;
    return -1;
}

// fmov between raw v registers (mov_fp takes temp indices)
static void fmov_raw(Em* e, int is64, int dst, int src) {
    if (dst == src) return;
    ins(e, (is64 ? 0x1E604000u : 0x1E204000u) | ((uint32_t)src << 5) |
           (uint32_t)dst);
}

/*
 * Move an integer-class value into its promoted register at the type's width,
 * leaving exactly what a slot store and reload would: 32-bit values
 * zero-extended, narrow signed values sign-extended into w (ldursb/ldursh),
 * narrow unsigned values zero-extended.
 */
static void set_preg(Em* e, int r, int src, SType* t) {
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    if (w >= 8) {
        mov_int(e, 1, r, src);
    } else if (w == 4) {
        ins(e, 0x2A0003E0u | ((uint32_t)src << 16) | (uint32_t)r); // mov wr, ws
    } else { // sxtb/sxth/uxtb/uxth wr, ws
        uint32_t imms = w == 1 ? 7u : 15u;
        ins(e, (ty_is_signed(t) ? 0x13000000u : 0x53000000u) | (imms << 10) |
               ((uint32_t)src << 5) | (uint32_t)r);
    }
}

static void store_preg(Em* e, int r, EV v, SType* t) {
    if (v.cls == CLS_ADDR) v = load_scalar(e, xreg(v.idx), 0, t);
    if (!is_floatt(t)) {
        set_preg(e, r, xreg(v.idx), t);
        return;
    }
    int is64 = unwrap(t)->prim == P_F64;
    if (v.cls == CLS_FP) // fmov s keeps the low 32 bits, as a slot would
        fmov_raw(e, is64, r, vreg(v.idx));
    else // raw bits in an int temp: fmov d, x / s, w
        ins(e, (is64 ? 0x9E670000u : 0x1E270000u) |
               ((uint32_t)xreg(v.idx) << 5) | (uint32_t)r);
}

// a promoted sequence-loop value binder takes the element at [p] directly
static void load_binder(Em* e, int r, int p, SType* t) {
    if (is_floatt(t)) {
        ldrf_(e, r, p, 0, unwrap(t)->prim == P_F64);
        return;
    }
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    ld_reg(e, r, p, 0, w, ty_is_signed(t) && w < 4); // ldrsb/ldrsh w, as a slot reload
}

// address of a variable's storage in a fresh int temp
static int gen_var_addr(Em* e, Sym* s, Node* at) {
    int t = tmp_int(e);
    if (promo_reg(e, s) >= 0) { // plan_promotion missed an escape
        em_err(e, at, "internal compiler error: address of register local '%.*s'",
               (int)s->len, s->name);
        return t;
    }
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

// frame slot of a local variable; NULL for globals and unallocated names
static Slot* local_slot(Em* e, Sym* s) {
    if (!s->decl || global_sym(e, s)) return NULL;
    return slot_find(e, s->decl);
}

// scalar of type t at x29 - depth into a fresh temp, as a slot reload
static EV load_frame_value(Em* e, uint32_t depth, SType* t) {
    if (is_floatt(t)) {
        int vt = tmp_fp(e);
        load_frame_fp(e, vreg(vt), depth, unwrap(t)->prim == P_F64);
        return (EV){ CLS_FP, vt };
    }
    int it = tmp_int(e);
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    load_frame(e, xreg(it), depth, w, ty_is_signed(t) && w < 8);
    return (EV){ CLS_INT, it };
}

/*
 * A place that is a plain local slot, or a struct-field chain over one, sits
 * at x29 - depth: its fields and descriptor words load directly instead of
 * through a materialized address.
 */
static int frame_place(Em* e, Node* x, uint32_t* depth) {
    if (x->kind == N_IDENT) {
        Sym* s = node_sym(x);
        if (!s || s->kind != SYM_VAR || promo_reg(e, s) >= 0) return 0;
        Slot* sl = local_slot(e, s);
        if (!sl || sl->byref) return 0;
        *depth = sl->off;
        return 1;
    }
    if (x->kind != N_FIELD_E || !is_lvalue_node(x)) return 0;
    SType* bt = lvalue_type(e, x->a);
    if (!bt) bt = node_ty(x->a);
    bt = bt ? unwrap(bt) : NULL;
    if (!bt || bt->kind != SK_STRUCT || !frame_place(e, x->a, depth)) return 0;
    int fi = find_field(bt, x->str, x->len);
    if (fi < 0) return 0;
    *depth -= field_off(bt, fi);
    return 1;
}

/*
 * Scalar variable read without a separate address temp: a plain slot loads
 * directly (one ldur within x29 - 256); a by-reference slot loads its pointer
 * and then the value into the same register.
 */
static EV load_var(Em* e, Sym* s, SType* t, Node* at) {
    int pr = promo_reg(e, s);
    if (pr >= 0 && is_floatt(t)) {
        int vt = tmp_fp(e);
        fmov_raw(e, unwrap(t)->prim == P_F64, vreg(vt), pr);
        return (EV){ CLS_FP, vt };
    }
    if (pr >= 0) {
        int it = tmp_int(e);
        mov_int(e, 1, xreg(it), pr);
        return (EV){ CLS_INT, it };
    }
    Slot* sl = local_slot(e, s);
    if (!sl) {
        int a = gen_var_addr(e, s, at);
        return load_in_place(e, a, 0, t);
    }
    if (!sl->byref) return load_frame_value(e, sl->off, t);
    int is64 = unwrap(t)->prim == P_F64;
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    int signext = ty_is_signed(t) && w < 8;
    int it = tmp_int(e);
    load_frame(e, xreg(it), sl->off, 8, 0); // slot holds the pointer
    if (is_floatt(t)) {
        int vt = tmp_fp(e);
        ldrf_(e, vreg(vt), xreg(it), 0, is64);
        e->vti--;
        return (EV){ CLS_FP, vt };
    }
    ld_reg(e, xreg(it), xreg(it), 0, w, signext);
    return (EV){ CLS_INT, it };
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
        if (x->len == 3 && memcmp(x->str, "len", 3) == 0) {
            SType* b = unwrap(node_ty(x->a));
            if (b && (b->kind == SK_ARRAY || b->kind == SK_SLICE ||
                      (b->kind == SK_PRIM && b->prim == P_STRING)))
                return 0;
        }
        return 1;
    case N_UNARY:
        return x->op == T_STAR || x->op == T_AMP;
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
        if (b->kind != SK_STRUCT && b->kind != SK_BITS) return NULL;
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
        if (x->op == T_STAR) {
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

static SType* bits_field_owner(Node* x, int* fi) {
    if (!x || x->kind != N_FIELD_E) return NULL;
    SType* b = node_ty(x->a);
    b = unwrap(b);
    while (b && b->kind == SK_PTR) b = b->base;
    if (!b || b->kind != SK_BITS) return NULL;
    int i = (int)x->ival;
    if (i < 0 || i >= b->nfields || b->fields[i].bit_anonymous) return NULL;
    if (fi) *fi = i;
    return b;
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
        if (x->op == T_STAR) { // *p: the pointer value IS the address
            EV pointer = gen_expr(e, x->a);
            if (pointer.cls != CLS_INT) {
                em_err(e, x, "dereference operand did not produce a pointer");
                return tmp_int(e);
            }
            int ta = pointer.idx;
            emit_null_guard(e, xreg(ta));
            return ta;
        }
        break;
    case N_FIELD_E: {
        SType* bt = lvalue_type(e, x->a);
        if (!bt) bt = node_ty(x->a);
        bt = unwrap(bt);
        int error_handle = bt && bt->kind == SK_ERROR;
        int direct_pointer = 0;
        int ta;
        if (is_lvalue_node(x->a)) ta = gen_addr(e, x->a);
        else {
            EV v = gen_expr(e, x->a);
            if (error_handle && v.cls == CLS_INT) {
                ta = v.idx;
            } else if (bt && bt->kind == SK_PTR && v.cls == CLS_INT) {
                ta = v.idx;
                direct_pointer = 1;
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
            if (direct_pointer) direct_pointer = 0;
            else ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            emit_null_guard(e, xreg(ta));
            bt = bt->base;
        }
        if (bt->kind == SK_BITS) return ta; // field projection shares backing address
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
        SType* seq = bt;
        int descriptor = bt && (bt->kind == SK_SLICE ||
                                (bt->kind == SK_PRIM && bt->prim == P_STRING));
        uint32_t depth = 0;
        int framed = descriptor && frame_place(e, x->a, &depth);
        int ta;
        if (framed) ta = tmp_int(e); // descriptor words load from the frame
        else if (is_lvalue_node(x->a)) ta = gen_addr(e, x->a);
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
            bt = &byte;
        } else if (bt->kind == SK_SLICE) {
            bt = bt->base;
        } else if (bt->kind == SK_ARRAY) {
            bt = bt->base;
        } else if (bt->kind == SK_PTR) {
            ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
            emit_null_guard(e, xreg(ta));
            bt = bt->base;
        } else {
            em_err(e, x, "cannot index this type");
            return ta;
        }
        int need_guard = e->mode == EMIT_CHECKED &&
                         !(x->flags & F_BOUNDS_PROVEN);
        uint32_t len_slot = 0;
        if (descriptor) {
            if (need_guard) {
                len_slot = temp_slot(e, 8, 8);
                if (framed) load_frame(e, 16, depth - 8, 8, 0);
                else ld_reg(e, 16, xreg(ta), 8, 8, 0);
                store_frame(e, 16, len_slot, 8);
            }
            if (framed) load_frame(e, xreg(ta), depth, 8, 0);
            else ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
        }
        uint32_t esz = ty_size(bt);
        uint64_t cidx;
        if (!need_guard && const_int(x->b, &cidx) && cidx <= 4095 &&
            cidx * esz <= 4095) { // constant unguarded index: one add or none
            if (cidx)
                ins(e, 0x91000000u | ((uint32_t)(cidx * esz) << 10) |
                       ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
            return ta;
        }
        int svi = e->vti, svf = e->vtf;
        // A 64-bit index is only read, so a promoted one is used in place; a
        // narrower one is widened in its register and needs its own temp.
        SType* it = node_ty(x->b);
        int narrow = it && it->kind != SK_UT && it->kind != SK_ERR &&
                     scalar_bits(it) != 64;
        EV ix;
        int ri;
        if (narrow) {
            ix = gen_expr(e, x->b);
            ri = xreg(ix.idx);
        } else ri = src_reg(e, x->b, &ix);
        int lenreg = -1;
        if (descriptor && need_guard) {
            lenreg = 16;
            load_frame(e, lenreg, len_slot, 8, 0);
        }
        emit_index_guard(e, x, ri, lenreg,
                         seq && seq->kind == SK_ARRAY ? seq->count : 0);
        if (e->mode != EMIT_CHECKED || (x->flags & F_BOUNDS_PROVEN))
            widen_idx(e, x->b, ri);
        if (esz == 1) {
            ins(e, 0x8B000000u | ((uint32_t)ri << 16) |
                   ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
        } else if ((esz & (esz - 1)) == 0) {
            int sh = 0;
            while ((1u << sh) < esz) sh++;
            ins(e, 0x8B000000u | ((uint32_t)sh << 10) |
                   ((uint32_t)ri << 16) |
                   ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
        } else {
            load_imm(e, 1, 16, esz);
            ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)ri << 5) | 16u);
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

static void registry_add(Em* e, Node* at, int ptr, int size, uint64_t align,
                         int align_reg) {
    if (e->mode != EMIT_CHECKED) return;
    e->allocator_used = 1;
    int ar = align_reg;
    if (ar < 0) {
        ar = tmp_int(e);
        load_imm(e, 1, xreg(ar), align);
    }
    CallArg args[3] = {
        { NULL, { CLS_INT, ptr }, 1, 0 },
        { NULL, { CLS_INT, size }, 1, 0 },
        { NULL, { CLS_INT, ar }, 1, 0 },
    };
    SType* params[3] = { raw_pointer_type(), default_i64(), default_i64() };
    uint8_t muts[3] = { 0, 0, 0 };
    CallTarget tgt = { TK_INTERNAL, "__ace$registry_add", 18, 0 };
    (void)emit_call(e, at, args, 3, params, muts, 3, 0, -1, tgt, NULL);
}

static EV registry_find(Em* e, Node* at, int ptr, int size) {
    e->allocator_used = 1;
    CallArg args[2] = {
        { NULL, { CLS_INT, ptr }, 1, 0 },
        { NULL, { CLS_INT, size }, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_INTERNAL, "__ace$registry_find", 19, 0 };
    return emit_call(e, at, args, 2, params, muts, 2, 0, -1, tgt,
                     raw_pointer_type());
}

static void registry_remove(Em* e, Node* at, int ptr, int size) {
    if (e->mode != EMIT_CHECKED) return;
    e->allocator_used = 1;
    CallArg args[2] = {
        { NULL, { CLS_INT, ptr }, 1, 0 },
        { NULL, { CLS_INT, size }, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_INTERNAL, "__ace$registry_remove", 21, 0 };
    (void)emit_call(e, at, args, 2, params, muts, 2, 0, -1, tgt, NULL);
}

static EV gen_process_array(Em* e, Node* call, const char* getter,
                            uint32_t getter_len) {
    CallTarget tgt = { TK_EXTERN, getter, getter_len, 0 };
    EV outer = emit_call(e, call, NULL, 0, NULL, NULL, 0, 0, -1, tgt,
                         raw_pointer_type());
    ld_reg(e, xreg(outer.idx), xreg(outer.idx), 0, 8, 0);
    return outer; // char ** returned through Darwin's char *** accessor
}

static EV gen_process_count_builtin(Em* e, Node* call, int environment) {
    if (!environment) {
        CallTarget tgt = { TK_EXTERN, "__NSGetArgc", 11, 0 };
        EV p = emit_call(e, call, NULL, 0, NULL, NULL, 0, 0, -1, tgt,
                         raw_pointer_type());
        ld_reg(e, xreg(p.idx), xreg(p.idx), 0, 4, 0);
        return p;
    }

    EV base = gen_process_array(e, call, "__NSGetEnviron", 14);
    int cursor = tmp_int(e);
    int count = tmp_int(e);
    mov_int(e, 1, xreg(cursor), xreg(base.idx));
    load_imm(e, 1, xreg(count), 0);
    int loop = new_label(e), done = new_label(e);
    bind_label(e, loop);
    ld_reg(e, 16, xreg(cursor), 0, 8, 0);
    cbz_(e, 1, 16, 0, done);
    ins(e, 0x91002000u | ((uint32_t)xreg(cursor) << 5) |
           (uint32_t)xreg(cursor)); // add cursor, cursor, #8
    ins(e, 0x91000400u | ((uint32_t)xreg(count) << 5) |
           (uint32_t)xreg(count)); // add count, count, #1
    br_(e, loop);
    bind_label(e, done);
    if (count != base.idx) mov_int(e, 1, xreg(base.idx), xreg(count));
    e->vti = base.idx + 1;
    return (EV){ CLS_INT, base.idx };
}

static EV gen_process_entry_builtin(Em* e, Node* call, int environment) {
    int si = e->vti, sf = e->vtf;
    uint32_t off = temp_slot(e, 16, 8);
    EV index = gen_expr(e, call->kids[0]);
    EV base = gen_process_array(e, call,
                                environment ? "__NSGetEnviron" : "__NSGetArgv",
                                environment ? 14 : 11);
    int entry_addr = tmp_int(e);
    ins(e, 0x8B000000u | (3u << 10) |
           ((uint32_t)xreg(index.idx) << 16) |
           ((uint32_t)xreg(base.idx) << 5) | (uint32_t)xreg(entry_addr));
    int ptr = tmp_int(e);
    ld_reg(e, xreg(ptr), xreg(entry_addr), 0, 8, 0);

    int cursor = tmp_int(e);
    int len = tmp_int(e);
    mov_int(e, 1, xreg(cursor), xreg(ptr));
    load_imm(e, 1, xreg(len), 0);
    int loop = new_label(e), scanned = new_label(e);
    bind_label(e, loop);
    ld_reg(e, 16, xreg(cursor), 0, 1, 0);
    cbz_(e, 0, 16, 0, scanned);
    ins(e, 0x91000400u | ((uint32_t)xreg(cursor) << 5) |
           (uint32_t)xreg(cursor)); // add cursor, cursor, #1
    ins(e, 0x91000400u | ((uint32_t)xreg(len) << 5) |
           (uint32_t)xreg(len)); // add len, len, #1
    br_(e, loop);
    bind_label(e, scanned);
    frame_addr(e, 16, off);
    st_reg(e, xreg(ptr), 16, 0, 8);
    st_reg(e, xreg(len), 16, 8, 8);
    return finish_slice_builtin(e, si, sf, off);
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
    registry_add(e, call, ptr.idx, size.idx, 0, align.idx);
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
    int old_size = tmp_int(e);
    ld_reg(e, xreg(old_size), xreg(data.idx), 8, 8, 0);
    EV size = gen_expr(e, call->kids[1]);
    int done = new_label(e);
    ins(e, 0xF100001Fu | ((uint32_t)xreg(size.idx) << 5));
    bcond_(e, C_LE, done);

    EV record = { CLS_INT, 0 };
    if (e->mode == EMIT_CHECKED)
        record = registry_find(e, call, ptemp, old_size);
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
    if (e->mode == EMIT_CHECKED) {
        st_reg(e, xreg(ptr.idx), xreg(record.idx), 0, 8);
        st_reg(e, xreg(size.idx), xreg(record.idx), 8, 8);
    }
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
    registry_add(e, call, ptr.idx, bytes, eal, -1);
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
    int old_count = tmp_int(e);
    ld_reg(e, xreg(old_count), xreg(data.idx), 8, 8, 0);
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

    int old_bytes = tmp_int(e);
    int old_scale = tmp_int(e);
    load_imm(e, 1, xreg(old_scale), esz);
    ins(e, 0x9B007C00u | ((uint32_t)xreg(old_scale) << 16) |
           ((uint32_t)xreg(old_count) << 5) | (uint32_t)xreg(old_bytes));
    EV record = { CLS_INT, 0 };
    if (e->mode == EMIT_CHECKED)
        record = registry_find(e, call, ptemp, old_bytes);
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
    if (e->mode == EMIT_CHECKED) {
        st_reg(e, xreg(ptr.idx), xreg(record.idx), 0, 8);
        st_reg(e, xreg(bytes), xreg(record.idx), 8, 8);
    }
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

    int length = tmp_int(e);
    ld_reg(e, xreg(length), xreg(addr), 8, 8, 0);
    SType* st = node_ty(raw);
    uint64_t esz = st && st->kind == SK_SLICE ? ty_size(st->base) : 1;
    int bytes = length;
    if (esz != 1) {
        bytes = tmp_int(e);
        load_imm(e, 1, 16, esz);
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(length) << 5) |
               (uint32_t)xreg(bytes));
    }
    registry_remove(e, call, ptr, bytes);

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

/* Usable size of a live allocation: the platform allocator's own report for
 * the slice's base pointer. Null and non-allocation pointers report 0. */
static EV gen_alloc_size_builtin(Em* e, Node* call) {
    EV data = gen_expr(e, call->kids[0]);
    if (data.cls != CLS_ADDR) {
        em_err(e, call->kids[0], "mem.alloc_size data must materialize as a slice");
        int zero = tmp_int(e);
        load_imm(e, 1, xreg(zero), 0);
        return (EV){ CLS_INT, zero };
    }
    int ptr = tmp_int(e);
    ld_reg(e, xreg(ptr), xreg(data.idx), 0, 8, 0);
    CallArg arg = { NULL, { CLS_INT, ptr }, 1, 0 };
    SType* param = raw_pointer_type();
    uint8_t mut = 0;
    CallTarget tgt = { TK_EXTERN, "_malloc_size", 12, 0 };
    return emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt,
                     default_i64());
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
    // The destination is a writable `mut []u8` view expression; tolerate the
    // historical N_MUTARG spelling defensively.
    Node* dst_node = call->kids[0]->kind == N_MUTARG ? call->kids[0]->a
                                                     : call->kids[0];
    int dst, dlen;
    gen_slice_parts(e, dst_node, &dst, &dlen);

    if (intrinsic == BI_MEM_COPY || intrinsic == BI_MEM_MOVE) {
        int src, slen;
        gen_slice_parts(e, call->kids[1], &src, &slen);
        if (e->mode == EMIT_CHECKED) {
            int lengths_ok = new_label(e);
            ins(e, 0xEB00001Fu | ((uint32_t)xreg(slen) << 16) |
                   ((uint32_t)xreg(dlen) << 5));
            bcond_(e, C_EQ, lengths_ok);
            emit_contract_fail_call(e);
            bind_label(e, lengths_ok);
            if (intrinsic == BI_MEM_COPY) {
                int empty = new_label(e), dst_end_ok = new_label(e);
                int src_end_ok = new_label(e), disjoint = new_label(e);
                cbz_(e, 1, xreg(slen), 0, empty);
                ins(e, 0xAB000000u | ((uint32_t)xreg(slen) << 16) |
                       ((uint32_t)xreg(dst) << 5) | 16u); // adds dst end
                bcond_(e, C_LO, dst_end_ok);
                emit_contract_fail_call(e);
                bind_label(e, dst_end_ok);
                ins(e, 0xAB000000u | ((uint32_t)xreg(slen) << 16) |
                       ((uint32_t)xreg(src) << 5) | 17u); // adds src end
                bcond_(e, C_LO, src_end_ok);
                emit_contract_fail_call(e);
                bind_label(e, src_end_ok);
                ins(e, 0xEB00001Fu | (17u << 16) |
                       ((uint32_t)xreg(dst) << 5));
                bcond_(e, C_HS, disjoint);
                ins(e, 0xEB00001Fu | (16u << 16) |
                       ((uint32_t)xreg(src) << 5));
                bcond_(e, C_HS, disjoint);
                emit_contract_fail_call(e);
                bind_label(e, disjoint);
                bind_label(e, empty);
            }
        }
        // Empty slices may carry null pointers; the routines touch no bytes
        // for a zero count.
        e->mem_copy_used = 1;
        mov_int(e, 1, 15, xreg(dst));
        mov_int(e, 1, 16, xreg(src));
        mov_int(e, 1, 17, xreg(slen));
        if (intrinsic == BI_MEM_MOVE) emit_bl_internal(e, "__ace$mem_move", 14);
        else emit_bl_internal(e, "__ace$mem_copy", 14);
    } else {
        e->mem_fill_used = 1;
        if (intrinsic == BI_MEM_FILL) {
            EV value = gen_expr(e, call->kids[1]);
            mov_int(e, 1, 16, xreg(value.idx));
        } else {
            load_imm(e, 1, 16, 0);
        }
        mov_int(e, 1, 15, xreg(dst));
        mov_int(e, 1, 17, xreg(dlen));
        emit_bl_internal(e, "__ace$mem_fill", 14);
    }
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

    // The shared routine orders the common prefix; zero-length prefixes
    // touch no bytes, so empty slices may carry null pointers.
    e->mem_compare_used = 1;
    mov_int(e, 1, 15, xreg(aptr));
    mov_int(e, 1, 16, xreg(bptr));
    mov_int(e, 1, 17, xreg(count));
    emit_bl_internal(e, "__ace$mem_compare", 17);
    int out = tmp_int(e);
    mov_int(e, 1, xreg(out), 17);
    int negative = new_label(e), done = new_label(e);
    cbz_(e, 1, xreg(out), 1, done);
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
    return load_in_place(e, a, 0, t);
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
            if (part->kind == PF_BINARY || part->kind == PF_OCTAL ||
                part->kind == PF_HEX_LOWER || part->kind == PF_HEX_UPPER) {
                int radix = tmp_int(e);
                uint64_t base = part->kind == PF_BINARY ? 2 :
                                part->kind == PF_OCTAL ? 8 : 16;
                load_imm(e, 0, xreg(radix), base);
                values[nv++] = (EV){ CLS_INT, radix };
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

/* Emit the exact Apple-arm64 scalar operation used by both direct intrinsic
 * calls and their ordinary callable symbols.  Narrow rotations duplicate the
 * source pattern through a 32-bit word, so rorv's hardware count reduction is
 * also the required modulo-8/modulo-16 reduction. */
static void emit_bits_integer(Em* e, int intrinsic, int bits,
                              int value, int count, int vector) {
    int sf = bits == 64;
    switch (intrinsic) {
    case BI_BITS_POP_COUNT:
        ins(e, 0x9E670000u | ((uint32_t)value << 5) | (uint32_t)vector);
        ins(e, 0x0E205800u | ((uint32_t)vector << 5) | (uint32_t)vector);
        ins(e, 0x0E31B800u | ((uint32_t)vector << 5) | (uint32_t)vector);
        ins(e, 0x0E013C00u | ((uint32_t)vector << 5) | (uint32_t)value);
        return;
    case BI_BITS_LEAD_ZEROS:
        ins(e, (sf ? 0xDAC01000u : 0x5AC01000u) |
               ((uint32_t)value << 5) | (uint32_t)value);
        if (bits < 32)
            ins(e, 0x51000000u | ((uint32_t)(32 - bits) << 10) |
                   ((uint32_t)value << 5) | (uint32_t)value);
        return;
    case BI_BITS_TRAIL_ZEROS:
        if (bits < 32) {
            load_imm(e, 0, 16, UINT64_C(1) << bits);
            ins(e, 0x2A000000u | (16u << 16) |
                   ((uint32_t)value << 5) | (uint32_t)value);
        }
        ins(e, (sf ? 0xDAC00000u : 0x5AC00000u) |
               ((uint32_t)value << 5) | (uint32_t)value);
        ins(e, (sf ? 0xDAC01000u : 0x5AC01000u) |
               ((uint32_t)value << 5) | (uint32_t)value);
        return;
    case BI_BITS_BYTE_SWAP:
        if (bits == 16)
            ins(e, 0x5AC00400u | ((uint32_t)value << 5) | (uint32_t)value);
        else if (bits == 32)
            ins(e, 0x5AC00800u | ((uint32_t)value << 5) | (uint32_t)value);
        else if (bits == 64)
            ins(e, 0xDAC00C00u | ((uint32_t)value << 5) | (uint32_t)value);
        return;
    case BI_BITS_ROTATE_LEFT:
    case BI_BITS_ROTATE_RIGHT:
        if (bits == 8) {
            ins(e, 0x2A002000u | ((uint32_t)value << 16) |
                   ((uint32_t)value << 5) | (uint32_t)value);
            ins(e, 0x2A004000u | ((uint32_t)value << 16) |
                   ((uint32_t)value << 5) | (uint32_t)value);
        } else if (bits == 16) {
            ins(e, 0x2A004000u | ((uint32_t)value << 16) |
                   ((uint32_t)value << 5) | (uint32_t)value);
        }
        if (intrinsic == BI_BITS_ROTATE_LEFT)
            ins(e, (sf ? 0xCB0003E0u : 0x4B0003E0u) |
                   ((uint32_t)count << 16) | (uint32_t)count);
        ins(e, (sf ? 0x9AC02C00u : 0x1AC02C00u) |
               ((uint32_t)count << 16) | ((uint32_t)value << 5) |
               (uint32_t)value);
        if (bits < 32)
            ins(e, uxtw_of(bits) | ((uint32_t)value << 5) |
                   (uint32_t)value);
        return;
    default:
        return;
    }
}

static EV gen_call(Em* e, Node* call) {
    Node* callee = call->a;

    // usize -> data pointer: identical bits, plus the local checked alignment
    // diagnostic promised by the explicit trust boundary.
    if (callee->kind == N_PTR && !call->sym) {
        EV address = gen_expr(e, call->kids[0]);
        SType* pt = node_ty(call);
        uint32_t align = pt && pt->base ? ty_align(pt->base) : 1;
        if (e->mode == EMIT_CHECKED && align > 1 &&
            !(call->flags & F_BOUNDS_PROVEN)) {
            load_imm(e, 1, 16, align - 1);
            ins(e, 0xEA00001Fu | (16u << 16) |
                   ((uint32_t)xreg(address.idx) << 5)); // tst address, align-1
            int aligned = new_label(e);
            bcond_(e, C_EQ, aligned);
            emit_arithmetic_fail_call(e);
            bind_label(e, aligned);
        }
        return address;
    }

    // Explicit pointer + element count -> two-word slice descriptor.
    if (callee->kind == N_SLICE_T && call->nkids == 2 && !call->sym) {
        int si = e->vti, sf = e->vtf;
        uint32_t off = temp_slot(e, 16, 8);
        EV pointer = gen_expr(e, call->kids[0]);
        EV count = gen_expr(e, call->kids[1]);
        if (e->mode == EMIT_CHECKED && !(call->flags & F_BOUNDS_PROVEN)) {
            int extent_ok = new_label(e);
            cbz_(e, 1, xreg(count.idx), 0, extent_ok);
            int nonnull = new_label(e);
            cbz_(e, 1, xreg(pointer.idx), 1, nonnull);
            emit_null_fail_call(e);
            bind_label(e, nonnull);
            uint32_t size = ty_size(node_ty(call)->base);
            if (size != 1) {
                load_imm(e, 1, 16, size);
                ins(e, 0x9BC07C00u | (16u << 16) |
                       ((uint32_t)xreg(count.idx) << 5) | 17u); // umulh
                int scaled = new_label(e);
                cbz_(e, 1, 17, 0, scaled);
                emit_arithmetic_fail_call(e);
                bind_label(e, scaled);
                ins(e, 0x9B007C00u | (16u << 16) |
                       ((uint32_t)xreg(count.idx) << 5) | 17u); // scaled bytes
            } else {
                ins(e, 0xAA0003F1u | ((uint32_t)xreg(count.idx) << 16)); // mov x17,count
            }
            ins(e, 0xAB00001Fu | (17u << 16) |
                   ((uint32_t)xreg(pointer.idx) << 5)); // adds xzr,pointer,bytes
            int end_ok = new_label(e);
            bcond_(e, C_LO, end_ok);
            emit_arithmetic_fail_call(e);
            bind_label(e, end_ok);
            bind_label(e, extent_ok);
        }
        frame_addr(e, 16, off);
        st_reg(e, xreg(pointer.idx), 16, 0, 8);
        st_reg(e, xreg(count.idx), 16, 8, 8);
        e->vti = si; e->vtf = sf;
        int result = tmp_int(e);
        frame_addr(e, xreg(result), off);
        return (EV){ CLS_ADDR, result };
    }
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

    // A nominal error domain packs its compiler-assigned id above raw i32 bits.
    if (callee->kind == N_IDENT && !call->sym && node_ty(callee) &&
        node_ty(callee)->kind == SK_ERROR_DOMAIN) {
        int out = tmp_int(e);
        load_imm(e, 1, xreg(out), call->ival << 32);
        if (call->nkids == 1) {
            EV code = gen_expr(e, call->kids[0]);
            mov_int(e, 0, xreg(code.idx), xreg(code.idx)); // clear upper 32 bits
            ins(e, 0xAA000000u | ((uint32_t)xreg(code.idx) << 16) |
                   ((uint32_t)xreg(out) << 5) | (uint32_t)xreg(out));
        }
        return (EV){ CLS_INT, out };
    }

    // cast: i32(x), Meters(x), Color(1) — callee resolves to a type symbol;
    // A slice-shaped identity cast arrives as an inline slice-type callee.
    if ((callee->kind == N_IDENT || callee->kind == N_SLICE_T) && !call->sym &&
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
    // alignof builtin (constant folded by sema)
    if (callee->kind == N_IDENT && !call->sym && callee->len == 7 &&
        memcmp(callee->str, "alignof", 7) == 0) {
        int t = tmp_int(e);
        load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }
    if (callee->kind == N_IDENT && !call->sym && callee->len == 8 &&
        memcmp(callee->str, "offsetof", 8) == 0) {
        int t = tmp_int(e); load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }
    if (callee->kind == N_IDENT && !call->sym && callee->len == 13 &&
        memcmp(callee->str, "static_assert", 13) == 0)
        return (EV){ CLS_INT, 0 };
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
        intrinsic->decl->ival == BI_MEM_ALLOC_SIZE &&
        call->ival == BI_MEM_ALLOC_SIZE)
        return gen_alloc_size_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_COMPARE && call->ival == BI_MEM_COMPARE)
        return gen_compare_builtin(e, call);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_EQUAL && call->ival == BI_MEM_EQUAL) {
        EV a = gen_expr(e, call->kids[0]);
        EV b = gen_expr(e, call->kids[1]);
        return gen_byte_sequence_eq(e, call, a, node_ty(call->kids[0]), b,
                                    node_ty(call->kids[1]), 1);
    }
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_BITS_FROM_FLOAT) {
        EV value = gen_expr(e, call->kids[0]);
        int bits = tmp_int(e);
        int is64 = unwrap(node_ty(call->kids[0]))->prim == P_F64;
        uint32_t op = is64 ? 0x9E660000u : 0x1E260000u;
        ins(e, op | ((uint32_t)vreg(value.idx) << 5) |
               (uint32_t)xreg(bits)); // fmov integer bits from fp register
        e->vtf = value.idx;
        return (EV){ CLS_INT, bits };
    }
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_BITS_TO_FLOAT) {
        EV bits = gen_expr(e, call->kids[0]);
        int value = tmp_fp(e);
        int is64 = unwrap(node_ty(call))->prim == P_F64;
        uint32_t op = is64 ? 0x9E670000u : 0x1E270000u;
        ins(e, op | ((uint32_t)xreg(bits.idx) << 5) |
               (uint32_t)vreg(value)); // fmov fp value from integer bits
        e->vti = bits.idx;
        return (EV){ CLS_FP, value };
    }
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival >= BI_BITS_POP_COUNT &&
        intrinsic->decl->ival <= BI_BITS_ROTATE_RIGHT) {
        int operation = intrinsic->decl->ival;
        EV value = gen_expr(e, call->kids[0]);
        EV count = { CLS_INT, 0 };
        if (call->nkids == 2) count = gen_expr(e, call->kids[1]);
        int vector = 0;
        int saved_vtf = e->vtf;
        if (operation == BI_BITS_POP_COUNT) vector = vreg(tmp_fp(e));
        emit_bits_integer(e, operation, scalar_bits(node_ty(call->kids[0])),
                          xreg(value.idx), xreg(count.idx), vector);
        e->vtf = saved_vtf;
        e->vti = value.idx + 1;
        return value;
    }
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_INTERNAL_LOAD_U64) {
        EV bytes = gen_expr(e, call->kids[0]);
        int ptr, len;
        byte_sequence_parts(e, call, bytes, node_ty(call->kids[0]), &ptr, &len);
        EV index = gen_expr(e, call->kids[1]);
        int out = ptr;
        ins(e, 0x8B000000u | ((uint32_t)xreg(index.idx) << 16) |
               ((uint32_t)xreg(ptr) << 5) | (uint32_t)xreg(out)); // add out,ptr,index
        ld_reg(e, xreg(out), xreg(out), 0, 8, 0); // native-endian unaligned load
        e->vti = out + 1;
        return (EV){ CLS_INT, out };
    }
    if (intrinsic && intrinsic->decl) {
        switch (intrinsic->decl->ival) {
        case BI_SYS_ARGC: return gen_process_count_builtin(e, call, 0);
        case BI_SYS_ARG: return gen_process_entry_builtin(e, call, 0);
        case BI_SYS_ENVC: return gen_process_count_builtin(e, call, 1);
        case BI_SYS_ENV: return gen_process_entry_builtin(e, call, 1);
        default: break;
        }
    }
    // assert(condition): checked evaluates once; release removes the complete
    // condition expression, including side effects.
    if (callee->kind == N_IDENT && !call->sym &&
        (call->ival == 2 || call->ival == 3) &&
        callee->len == 6 && memcmp(callee->str, "assert", 6) == 0) {
        if (e->mode != EMIT_CHECKED || call->ival == 3)
            return (EV){ CLS_INT, 0 };
        int ok = new_label(e);
        gen_branch(e, call->kids[0], 1, ok);
        const char* path = call->path ? build_path(call->path) : "<source>";
        size_t need = strlen(path) + 80;
        char* diagnostic = (char*)arena_alloc(need);
        int n = snprintf(diagnostic, need, "%s:%d:%d: assertion failed\n",
                         path, call->line, call->col);
        Node message;
        memset(&message, 0, sizeof message);
        message.kind = N_STR; message.str = diagnostic; message.len = (uint32_t)n;
        ESym* msg = intern_string(e, &message);
        load_imm(e, 1, 0, 2); addr_of_sym(e, 1, msg); load_imm(e, 1, 2, (uint64_t)n);
        emit_bl_extern(e, "_write", 6); emit_bl_extern(e, "_abort", 6);
        ins(e, 0xD4200000u);
        bind_label(e, ok);
        return (EV){ CLS_INT, 0 }; // statement-only, like the release form
    }
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
            emit_null_guard(e, xreg(vt));
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
                             ft->nparams, !ft->is_c_abi, -1, tgt, ft->ret);
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

static EV gen_staticmap_lookup(Em* e, Node* x, SType* map) {
    int svi = e->vti, svf = e->vtf;
    StaticMapData* md = map_data(e, node_sym(x->a));
    if (!md) {
        em_err(e, x, "StaticMap storage was not emitted before its lookup");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    uint32_t value_size = ty_size(map->ret);
    uint32_t outoff = temp_slot(e, value_size, ty_align(map->ret));
    int out = tmp_int(e);
    frame_addr(e, xreg(out), outoff);
    for (uint32_t o = 0; o < value_size; ) {
        uint32_t w = value_size - o >= 8 ? 8 : value_size - o >= 4 ? 4 :
                     value_size - o >= 2 ? 2 : 1;
        st_reg(e, 31, xreg(out), o, w);
        o += w;
    }
    int miss = new_label(e), matched = new_label(e);
    int entry = tmp_int(e);

    if (!md->string_keys) {
        EV key = gen_expr(e, x->b);
        uint32_t keyoff = temp_slot(e, 8, 8);
        store_frame(e, xreg(key.idx), keyoff, 8);
        int h = tmp_int(e), q = tmp_int(e);
        mov_int(e, 1, xreg(h), xreg(key.idx));
        load_imm(e, 1, 16, md->seed + UINT64_C(0x9e3779b97f4a7c15));
        ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        ins(e, 0xD340FC00u | (30u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(q));
        ins(e, 0xCA000000u | ((uint32_t)xreg(q) << 16) |
               ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 16, UINT64_C(0xbf58476d1ce4e5b9));
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        ins(e, 0xD340FC00u | (27u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(q));
        ins(e, 0xCA000000u | ((uint32_t)xreg(q) << 16) |
               ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 16, UINT64_C(0x94d049bb133111eb));
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        ins(e, 0xD340FC00u | (31u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(q));
        ins(e, 0xCA000000u | ((uint32_t)xreg(q) << 16) |
               ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 16, md->mask);
        ins(e, 0x8A000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        addr_of_sym(e, 16, md->slots);
        ins(e, 0x8B000000u | ((uint32_t)xreg(h) << 16) | (2u << 10) |
               (16u << 5) | 16u);
        ld_reg(e, xreg(entry), 16, 0, 4, 0);
        cbz_(e, 0, xreg(entry), 0, miss);
        ins(e, 0x51000400u | ((uint32_t)xreg(entry) << 5) |
               (uint32_t)xreg(entry));
        addr_of_sym(e, 16, md->keys);
        uint32_t ksz = ty_size(map->base);
        if ((ksz & (ksz - 1)) == 0) {
            uint32_t sh = ksz == 1 ? 0 : ksz == 2 ? 1 : ksz == 4 ? 2 : 3;
            ins(e, 0x8B000000u | ((uint32_t)xreg(entry) << 16) | (sh << 10) |
                   (16u << 5) | 16u);
        } else {
            load_imm(e, 1, 17, ksz);
            ins(e, 0x9B007C00u | (17u << 16) | ((uint32_t)xreg(entry) << 5) | 17u);
            ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        }
        ld_reg(e, 17, 16, 0, ksz, ty_is_signed(map->base) && ksz < 8);
        load_frame(e, 16, keyoff, ksz, ty_is_signed(map->base) && ksz < 8);
        ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5));
        bcond_(e, C_NE, miss);
    } else {
        EV key = gen_expr(e, x->b);
        int ptr = tmp_int(e), len = tmp_int(e), h = tmp_int(e), at = tmp_int(e);
        ld_reg(e, xreg(ptr), xreg(key.idx), 0, 8, 0);
        ld_reg(e, xreg(len), xreg(key.idx), 8, 8, 0);
        load_imm(e, 1, xreg(h), UINT64_C(1469598103934665603) ^ md->seed);
        load_imm(e, 1, xreg(at), 0);
        int loop = new_label(e), hashed = new_label(e);
        bind_label(e, loop);
        ins(e, 0xEB00001Fu | ((uint32_t)xreg(len) << 16) |
               ((uint32_t)xreg(at) << 5));
        bcond_(e, C_HS, hashed);
        ins(e, 0x38606810u | ((uint32_t)xreg(at) << 16) |
               ((uint32_t)xreg(ptr) << 5));
        ins(e, 0xCA000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        load_imm(e, 1, 17, UINT64_C(1099511628211));
        ins(e, 0x9B007C00u | (17u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        ins(e, 0x91000400u | ((uint32_t)xreg(at) << 5) | (uint32_t)xreg(at));
        br_(e, loop);
        bind_label(e, hashed);
        load_imm(e, 1, 16, md->mask);
        ins(e, 0x8A000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) |
               (uint32_t)xreg(h));
        addr_of_sym(e, 16, md->slots);
        ins(e, 0x8B000000u | ((uint32_t)xreg(h) << 16) | (2u << 10) |
               (16u << 5) | 16u);
        ld_reg(e, xreg(entry), 16, 0, 4, 0);
        cbz_(e, 0, xreg(entry), 0, miss);
        ins(e, 0x51000400u | ((uint32_t)xreg(entry) << 5) |
               (uint32_t)xreg(entry));
        addr_of_sym(e, 16, md->keys);
        ins(e, 0x8B000000u | ((uint32_t)xreg(entry) << 16) | (4u << 10) |
               (16u << 5) | 16u);
        ld_reg(e, 17, 16, 8, 8, 0);
        ins(e, 0xEB00001Fu | (17u << 16) | ((uint32_t)xreg(len) << 5));
        bcond_(e, C_NE, miss);
        ld_reg(e, 16, 16, 0, 8, 0);
        e->mem_compare_used = 1;
        mov_int(e, 1, 15, xreg(ptr));
        mov_int(e, 1, 17, xreg(len));
        emit_bl_internal(e, "__ace$mem_compare", 17);
        cbz_(e, 1, 17, 1, miss);
    }
    bind_label(e, matched);
    addr_of_sym(e, 16, md->values);
    load_imm(e, 1, 17, value_size);
    ins(e, 0x9B007C00u | (17u << 16) | ((uint32_t)xreg(entry) << 5) | 17u);
    ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
    mov_int(e, 1, 15, 16);
    copy_mem(e, xreg(out), 0, 15, 0, value_size);
    bind_label(e, miss);
    if (is_scalar(map->ret)) {
        if (is_floatt(map->ret)) {
            ldrf_(e, vreg(svf), xreg(out), 0,
                   unwrap(map->ret)->prim == P_F64);
            e->vti = svi;
            e->vtf = svf + 1;
            return (EV){ CLS_FP, svf };
        }
        uint32_t w = (uint32_t)((scalar_bits(map->ret) + 7) / 8);
        ld_reg(e, xreg(svi), xreg(out), 0, w,
               ty_is_signed(map->ret) && w < 8);
        e->vti = svi + 1;
        e->vtf = svf;
        return (EV){ CLS_INT, svi };
    }
    e->vti = svi + 1;
    e->vtf = svf;
    return (EV){ CLS_ADDR, svi };
}

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
        uint64_t folded;
        if (const_global_imm(s, t, x, &folded)) {
            int it = tmp_int(e);
            load_imm(e, ty_size(t) == 8, xreg(it), folded);
            return (EV){ CLS_INT, it };
        }
        if (is_scalar(t)) return load_var(e, s, t, x);
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
            if (v.cls != CLS_ADDR) {
                store_to_slot(e, v, off - member, ft);
            } else {
                int dst = xreg(v.idx) >= 14 ? xreg(v.idx) - 1 : 15;
                frame_addr(e, dst, off);
                store_value(e, v, dst, member, ft);
            }
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
        if (x->kind == N_STRUCTLIT && t->kind == SK_BITS) {
            int sf = scalar_bits(t) == 64;
            int out = tmp_int(e);
            load_imm(e, sf, xreg(out), 0);
            for (int i = 0; i < x->nkids; i++) {
                Node* fi = x->kids[i]; int fidx = find_field(t, fi->str, fi->len);
                if (fidx < 0) continue;
                SField* f = &t->fields[fidx];
                EV v = gen_expr(e, fi->a);
                if (e->mode == EMIT_CHECKED && f->bit_width < 64) {
                    int fail = new_label(e), ok = new_label(e); uint64_t hi;
                    if (f->bit_signed) {
                        uint64_t lo = (uint64_t)(-(INT64_C(1) << (f->bit_width - 1)));
                        hi = (UINT64_C(1) << (f->bit_width - 1)) - 1;
                        load_imm(e, sf, 16, lo);
                        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) |
                               ((uint32_t)xreg(v.idx) << 5));
                        bcond_(e, C_LT, fail);
                    } else hi = (UINT64_C(1) << f->bit_width) - 1;
                    load_imm(e, sf, 16, hi);
                    ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) |
                           ((uint32_t)xreg(v.idx) << 5));
                    bcond_(e, f->bit_signed ? C_LE : C_LS, ok);
                    bind_label(e, fail); emit_arithmetic_fail_call(e); bind_label(e, ok);
                }
                uint64_t low = f->bit_width == 64 ? UINT64_MAX :
                               (UINT64_C(1) << f->bit_width) - 1;
                load_imm(e, sf, 16, low);
                ins(e, (sf ? 0x8A000000u : 0x0A000000u) | (16u << 16) |
                       ((uint32_t)xreg(v.idx) << 5) | (uint32_t)xreg(v.idx));
                if (f->bit_offset) {
                    uint32_t sh = f->bit_offset, regw = sf ? 64u : 32u;
                    ins(e, (sf ? 0xD3400000u : 0x53000000u) | ((regw - sh) << 16) |
                           ((regw - 1 - sh) << 10) | ((uint32_t)xreg(v.idx) << 5) |
                           (uint32_t)xreg(v.idx));
                }
                ins(e, (sf ? 0xAA000000u : 0x2A000000u) | ((uint32_t)xreg(v.idx) << 16) |
                       ((uint32_t)xreg(out) << 5) | (uint32_t)xreg(out));
                e->vti = out + 1; // each initializer is consumed; preserve only accumulator
            }
            return (EV){ CLS_INT, out };
        }
        uint32_t off = temp_slot(e, ty_size(t), ty_align(t));
        if (x->kind == N_ARRAYLIT && x->a) {
            /*
             * [value : count]: a constant whose encoding repeats one byte, or
             * a runtime byte, fills the whole array; otherwise element 0 is
             * stored and the filled prefix doubles through disjoint copies,
             * so emitted code grows with log2(count), not count.
             */
            int svi = e->vti, svf = e->vtf;
            uint32_t esz = ty_size(t->base);
            uint64_t total = t->count * (uint64_t)esz;
            Buf cb = {0};
            ConstRels cr = {0};
            int uniform = total > 0 &&
                eval_const(e, x->a, t->base, &cb, &cr) && cr.n == 0 &&
                cb.len == esz;
            for (uint32_t i = 1; uniform && i < esz; i++)
                uniform = cb.d[i] == cb.d[0];
            uint8_t byte = uniform ? cb.d[0] : 0;
            free(cb.d);
            free(cr.v);
            if (uniform) {
                e->mem_fill_used = 1;
                frame_addr(e, 15, off);
                load_imm(e, 1, 16, byte);
                load_imm(e, 1, 17, total);
                emit_bl_internal(e, "__ace$mem_fill", 14);
            } else if (t->count > 0) {
                EV v = gen_expr(e, x->a);
                if (esz == 1 && v.cls == CLS_INT) {
                    e->mem_fill_used = 1;
                    mov_int(e, 1, 16, xreg(v.idx));
                    frame_addr(e, 15, off);
                    load_imm(e, 1, 17, total);
                    emit_bl_internal(e, "__ace$mem_fill", 14);
                } else {
                    if (v.cls != CLS_ADDR) {
                        store_to_slot(e, v, off, t->base);
                    } else {
                        int dst = xreg(v.idx) == 15 ? 14 : 15;
                        frame_addr(e, dst, off);
                        store_value(e, v, dst, 0, t->base);
                    }
                    for (uint64_t done = 1; esz && done < t->count;) {
                        uint64_t n = t->count - done < done
                            ? t->count - done : done;
                        e->mem_copy_used = 1;
                        frame_addr(e, 15, off - (uint32_t)(done * esz));
                        frame_addr(e, 16, off);
                        load_imm(e, 1, 17, n * esz);
                        emit_bl_internal(e, "__ace$mem_copy", 14);
                        done += n;
                    }
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
            SType* ft = x->kind == N_ARRAYLIT ? t->base : t->fields[fidx].t;
            if (v.cls != CLS_ADDR) {
                store_to_slot(e, v, off - fo, ft);
            } else {
                int dst = xreg(v.idx) >= 14 ? xreg(v.idx) - 1 : 15;
                frame_addr(e, dst, off);
                store_value(e, v, dst, fo, ft);
            }
            e->vti = svi; e->vtf = svf;
        }
        int r = tmp_int(e);
        frame_addr(e, xreg(r), off);
        return (EV){ CLS_ADDR, r };
    }
    case N_UNARY:
        if (x->op == T_AMP)
            return (EV){ CLS_INT, gen_addr(e, x) };
        if (x->op == T_VOLATILE) {
            SType* t = lvalue_type(e, x->a);
            int a = gen_addr(e, x->a);
            if (!t || !is_scalar(t)) {
                em_err(e, x, "volatile access needs a scalar place");
                return (EV){ CLS_INT, a };
            }
            /* The direct emitter has no optimization pass: this exact-width
               typed load is emitted once at the access site and cannot be
               cached, widened, merged, or moved. */
            return load_in_place(e, a, 0, t);
        }
        if (x->op == T_STAR) {
            SType* t = lvalue_type(e, x);
            int a = gen_addr(e, x);
            if (t && is_scalar(t)) {
                return load_in_place(e, a, 0, t);
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
                       ((uint32_t)vreg(v.idx) << 5) | (uint32_t)vreg(v.idx));
                return v;
            }
            int sf = !t || scalar_bits(t) == 64;
            if (e->mode == EMIT_CHECKED && t && is_intt(t)) {
                int bits = scalar_bits(t);
                uint64_t min = bits == 64 ? UINT64_C(0x8000000000000000)
                                          : (uint32_t)(-(INT64_C(1) << (bits - 1)));
                int ok = new_label(e);
                load_imm(e, sf, 16, min);
                ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) |
                       (16u << 16) | ((uint32_t)xreg(v.idx) << 5));
                bcond_(e, C_NE, ok);
                emit_arithmetic_fail_call(e);
                bind_label(e, ok);
            }
            ins(e, (sf ? 0xCB0003E0u : 0x4B0003E0u) |
                   ((uint32_t)xreg(v.idx) << 16) | (uint32_t)xreg(v.idx));
            return v;
        }
        if (x->op == T_BANG || x->op == T_TILDE) {
            SType* t = node_ty(x->a);
            EV v = gen_expr(e, x->a);
            if (v.cls == CLS_FP) {
                em_err(e, x, "invalid operand for '%s'",
                       tok_disp((TokKind)x->op));
                return v;
            }
            if (x->op == T_BANG) { // logical not
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
        if (ts && ts->kind == SYM_TYPE && ts->type &&
            ts->type->kind == SK_ERROR_DOMAIN) {
            int it = tmp_int(e);
            load_imm(e, 1, xreg(it), x->ival);
            return (EV){ CLS_INT, it };
        }
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ENUM) {
            int it = tmp_int(e);
            load_imm(e, scalar_bits(ts->type) == 64, xreg(it), x->ival);
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
        SType* eb = unwrap(node_ty(x->a));
        // sequence.len: arrays are compile-time constants; slices and strings
        // load the stored count from the second descriptor word.
        if (eb && x->len == 3 && memcmp(x->str, "len", 3) == 0 &&
            (eb->kind == SK_ARRAY || eb->kind == SK_SLICE ||
             (eb->kind == SK_PRIM && eb->prim == P_STRING))) {
            if (eb->kind == SK_ARRAY || x->a->kind == N_STR) {
                int t = tmp_int(e);
                load_imm(e, 1, xreg(t), x->ival);
                return (EV){ CLS_INT, t };
            }
            uint32_t depth;
            if (frame_place(e, x->a, &depth))
                return load_frame_value(e, depth - 8, node_ty(x));
            EV value = gen_expr(e, x->a);
            if (value.cls != CLS_ADDR) {
                em_err(e, x, "sequence .len has no descriptor");
                return (EV){ CLS_INT, tmp_int(e) };
            }
            return load_in_place(e, value.idx, 8, node_ty(x));
        }
        if (eb && eb->kind == SK_ERROR) {
            EV value = gen_expr(e, x->a);
            if (x->len == 6 && memcmp(x->str, "domain", 6) == 0)
                ins(e, 0xD360FC00u | ((uint32_t)xreg(value.idx) << 5) |
                       (uint32_t)xreg(value.idx)); // lsr x, x, #32
            else if (x->len == 4 && memcmp(x->str, "code", 4) == 0)
                mov_int(e, 0, xreg(value.idx), xreg(value.idx)); // raw i32 bits
            else
                return gen_error_description(e, value, x->len == 7);
            return value;
        }
        SType* bt = node_ty(x->a);
        if (bt && unwrap(bt) && unwrap(bt)->kind == SK_CONTRACT) {
            em_err(e, x, "contract method values must be called");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        SType* bits = unwrap(bt);
        while (bits && bits->kind == SK_PTR) bits = bits->base;
        if (bits && bits->kind == SK_BITS) {
            int fi = (int)x->ival;
            if (fi < 0 || fi >= bits->nfields) {
                em_err(e, x, "invalid bit field");
                return (EV){ CLS_INT, tmp_int(e) };
            }
            int a = gen_addr(e, x);
            EV v = load_in_place(e, a, 0, bits->base);
            SField* f = &bits->fields[fi];
            int rb = scalar_bits(bits), sf = rb == 64;
            uint32_t base = f->bit_signed ? (sf ? 0x93400000u : 0x13000000u)
                                          : (sf ? 0xD3400000u : 0x53000000u);
            uint32_t imms = (uint32_t)f->bit_offset + f->bit_width - 1;
            ins(e, base | ((uint32_t)f->bit_offset << 16) | (imms << 10) |
                   ((uint32_t)xreg(v.idx) << 5) | (uint32_t)xreg(v.idx));
            return v;
        }
        SType* t = node_ty(x);
        if (!t) t = lvalue_type(e, x);
        uint32_t depth;
        if (t && is_scalar(t) && frame_place(e, x, &depth))
            return load_frame_value(e, depth, t);
        int a = gen_addr(e, x);
        if (t && is_scalar(t)) {
            return load_in_place(e, a, 0, t);
        }
        return (EV){ CLS_ADDR, a };
    }
    case N_INDEX: {
        SType* bt = node_ty(x->a);
        if (bt && bt->kind == SK_STATICMAP)
            return gen_staticmap_lookup(e, x, bt);
        SType* t = node_ty(x);
        if (!t) t = lvalue_type(e, x);
        int a = gen_addr(e, x);
        if (t && is_scalar(t)) {
            return load_in_place(e, a, 0, t);
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
        enum { BP_ORDER = 4, BP_SOURCE = 8 };
        uint8_t proof = x->aux ? *(uint8_t*)x->aux : 0;
        int guard = e->mode == EMIT_CHECKED && !(x->flags & F_BOUNDS_PROVEN);
        int guard_source = guard && !(proof & BP_SOURCE);
        uint32_t len_slot = guard_source ? temp_slot(e, 8, 8) : 0;
        int svi = e->vti, svf = e->vtf;
        int base;
        if (is_lvalue_node(x->a)) base = gen_addr(e, x->a);
        else {
            EV v = gen_expr(e, x->a);
            base = v.idx;
        }
        if (guard_source) {
            if (bt->kind == SK_ARRAY) {
                load_imm(e, 1, 16, bt->count);
            } else {
                ld_reg(e, 16, base, 8, 8, 0);
            }
            store_frame(e, 16, len_slot, 8);
        }
        // hi (evaluated once; open-ended reads the sequence length)
        if (x->c) {
            EV hi = gen_expr(e, x->c);
            emit_nonnegative_guard(e, x, x->c, xreg(hi.idx));
            widen_idx(e, x->c, xreg(hi.idx));
            store_frame(e, xreg(hi.idx), hi_slot, 8);
        } else if (bt->kind == SK_ARRAY) {
            load_imm(e, 1, 16, bt->count);
            store_frame(e, 16, hi_slot, 8);
        } else {
            if (guard_source) load_frame(e, 16, len_slot, 8, 0);
            else ld_reg(e, 16, base, 8, 8, 0);
            store_frame(e, 16, hi_slot, 8);
        }
        // lo (evaluated once; defaults to 0)
        if (x->b) {
            EV lo = gen_expr(e, x->b);
            emit_nonnegative_guard(e, x, x->b, xreg(lo.idx));
            widen_idx(e, x->b, xreg(lo.idx));
            store_frame(e, xreg(lo.idx), lo_slot, 8);
        } else {
            load_imm(e, 1, 16, 0);
            store_frame(e, 16, lo_slot, 8);
        }
        if (guard) {
            if (!(proof & BP_ORDER)) {
                int ok_order = new_label(e);
                load_frame(e, 16, lo_slot, 8, 0);
                load_frame(e, 17, hi_slot, 8, 0);
                ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5)); // cmp lo, hi
                bcond_(e, C_LS, ok_order);
                emit_bounds_fail_call(e, x);
                bind_label(e, ok_order);
            }
            if (guard_source) {
                int ok_len = new_label(e);
                load_frame(e, 16, hi_slot, 8, 0);
                load_frame(e, 17, len_slot, 8, 0);
                ins(e, 0xEB00001Fu | (17u << 16) | (16u << 5)); // cmp hi, len
                bcond_(e, C_LS, ok_len);
                emit_bounds_fail_call(e, x);
                bind_label(e, ok_len);
            }
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

// assignment store: into a promoted register (direct 2, doff = register), in
// place for a direct local slot (direct 1), else through ta
static void store_target(Em* e, EV v, int direct, uint32_t doff, int ta,
                         SType* t) {
    if (direct == 2)
        store_preg(e, (int)doff, v, t);
    else if (direct)
        store_to_slot(e, v, doff, t);
    else
        store_scalar(e, v, xreg(ta), 0, t);
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
 * arms copy their payload binder before running the body. An unmatched tag
 * always jumps explicitly, to the else arm if present or past the switch
 * otherwise; it never falls through into the first case.
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
    int has_default = 0, default_idx = -1;
    for (int i = 0; i < s->nkids; i++) {
        lcase[i] = new_label(e);
        if (s->kids[i]->flags & F_DEFAULT) {
            has_default = 1;
            default_idx = i;
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
    br_(e, has_default ? lcase[default_idx] : lend);
    for (int i = 0; i < s->nkids; i++) {
        Node* c = s->kids[i];
        bind_label(e, lcase[i]);
        if (!(c->flags & F_DEFAULT)) {
            for (int j = 0; j < c->nkids; j++) {
                Node* pat = c->kids[j];
                if (pat->kind != N_VARPAT || !pat->b) continue;
                int vi = (int)pat->ival;
                SType* pt = vi < ut->nfields ? ut->fields[vi].t : NULL;
                if (!pt) continue;
                Sym* bs = node_sym(pat->b);
                uint32_t bslot = slot_alloc(e, bs && bs->decl ? bs->decl : pat->b,
                                            ty_size(pt), ty_align(pt));
                int base = tmp_int(e); // scrutinee, outside copy_mem's x15-x17
                load_frame(e, xreg(base), sslot, 8, 0);
                frame_addr(e, 15, bslot);
                copy_mem(e, 15, 0, xreg(base), poff, ty_size(pt)); // binder = payload copy
                e->vti = base;
            }
        }
        emit_block(e, c->b);
        if (i != s->nkids - 1 && !case_falls_through(c)) br_(e, lend);
    }
    bind_label(e, lend);
}

static void emit_bits_field_assign(Em* e, Node* target, Node* rhs, TokKind op,
                                   SType* bits, int fi) {
    SField* f = &bits->fields[fi];
    int width = scalar_bits(bits), sf = width == 64, regw = sf ? 64 : 32;
    int ta = gen_addr(e, target);
    EV whole = load_scalar(e, xreg(ta), 0, bits->base);
    EV value;
    if (op != T_ASSIGN) {
        int vr = tmp_int(e);
        mov_int(e, sf, xreg(vr), xreg(whole.idx));
        uint32_t base = f->bit_signed ? (sf ? 0x93400000u : 0x13000000u)
                                      : (sf ? 0xD3400000u : 0x53000000u);
        uint32_t imms = (uint32_t)f->bit_offset + f->bit_width - 1;
        ins(e, base | ((uint32_t)f->bit_offset << 16) | (imms << 10) |
               ((uint32_t)xreg(vr) << 5) | (uint32_t)xreg(vr));
        EV rv = gen_expr(e, rhs);
        emit_checked_arith(e, compound_op(op), xreg(vr), xreg(vr), xreg(rv.idx),
                           f->t);
        value = (EV){ CLS_INT, vr };
    } else value = gen_expr(e, rhs);
    if (e->mode == EMIT_CHECKED && f->bit_width < 64) {
        int fail = new_label(e), ok = new_label(e);
        uint64_t hi;
        if (f->bit_signed) {
            uint64_t lo = (uint64_t)(-(INT64_C(1) << (f->bit_width - 1)));
            hi = (UINT64_C(1) << (f->bit_width - 1)) - 1;
            load_imm(e, sf, 16, lo);
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) |
                   ((uint32_t)xreg(value.idx) << 5));
            bcond_(e, C_LT, fail);
        } else hi = (UINT64_C(1) << f->bit_width) - 1;
        load_imm(e, sf, 16, hi);
        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) |
               ((uint32_t)xreg(value.idx) << 5));
        bcond_(e, f->bit_signed ? C_LE : C_LS, ok);
        bind_label(e, fail); emit_arithmetic_fail_call(e); bind_label(e, ok);
    }
    uint64_t low = f->bit_width == 64 ? UINT64_MAX : (UINT64_C(1) << f->bit_width) - 1;
    uint64_t mask = low << f->bit_offset;
    int mr = tmp_int(e);
    load_imm(e, sf, xreg(mr), low);
    ins(e, (sf ? 0x8A000000u : 0x0A000000u) | ((uint32_t)xreg(mr) << 16) |
           ((uint32_t)xreg(value.idx) << 5) | (uint32_t)xreg(value.idx));
    if (f->bit_offset) {
        uint32_t sh = f->bit_offset;
        ins(e, (sf ? 0xD3400000u : 0x53000000u) | ((uint32_t)(regw - sh) << 16) |
               ((uint32_t)(regw - 1 - sh) << 10) |
               ((uint32_t)xreg(value.idx) << 5) | (uint32_t)xreg(value.idx));
    }
    load_imm(e, sf, xreg(mr), ~mask);
    ins(e, (sf ? 0x8A000000u : 0x0A000000u) | ((uint32_t)xreg(mr) << 16) |
           ((uint32_t)xreg(whole.idx) << 5) | (uint32_t)xreg(whole.idx));
    ins(e, (sf ? 0xAA000000u : 0x2A000000u) | ((uint32_t)xreg(value.idx) << 16) |
           ((uint32_t)xreg(whole.idx) << 5) | (uint32_t)xreg(whole.idx));
    store_scalar(e, whole, xreg(ta), 0, bits->base);
}

static void emit_stmt(Em* e, Node* s) {
    line_mark(e, s->kind == N_EXPRSTMT ? s->a : s);
    e->vti = 0; e->vtf = 0; // statement boundary
    switch (s->kind) {
    case N_VAR: {
        if (s->flags & F_MIXED) {
            if (s->nkids > 16) {
                em_err(e, s, "too many binders (max 16)");
                return;
            }
            uint32_t offs[16] = {0};
            for (int i = 0; i < s->nkids; i++) {
                if (s->kids[i]->kind != N_PARAM) continue;
                SType* t = node_sym(s->kids[i])->type;
                offs[i] = slot_alloc(e, s->kids[i], ty_size(t), ty_align(t));
            }
            EV rhs = gen_expr(e, s->b);
            if (rhs.cls != CLS_ADDR) {
                em_err(e, s, "mixed assignment right side must be a tuple");
                return;
            }
            SType* tup = node_ty(s->b);
            for (int i = 0; i < s->nkids; i++) {
                Node* binder = s->kids[i];
                SType* t = node_sym(binder)->type;
                if (binder->kind == N_PARAM) {
                    store_member(e, xreg(rhs.idx), tup, i, offs[i]);
                    continue;
                }
                int svi = e->vti;
                int ta = gen_addr(e, binder);
                if (is_scalar(t)) {
                    SType* ft = tup && tup->kind == SK_TUPLE && i < tup->nfields
                        ? tup->fields[i].t : t;
                    EV v = load_scalar(e, xreg(rhs.idx), field_off(tup, i), ft);
                    store_scalar(e, v, xreg(ta), 0, ft);
                } else {
                    copy_mem(e, xreg(ta), 0, xreg(rhs.idx), field_off(tup, i),
                             ty_size(t));
                }
                e->vti = svi;
            }
            return;
        }
        SType* wants[16];
        uint32_t offs[16];
        int nb = s->nkids < 16 ? s->nkids : 16;
        if (s->nkids > 16) {
            em_err(e, s, "too many binders (max 16)");
            return;
        }
        int pr = nb == 1 ? promo_reg(e, node_sym(s->kids[0])) : -1;
        if (pr >= 0) { // promoted: no slot
            if (s->b) store_preg(e, pr, gen_expr(e, s->b), node_sym(s->kids[0])->type);
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
        int bfi = -1; SType* bft = bits_field_owner(s->kids[0], &bfi);
        if (bft) {
            emit_bits_field_assign(e, s->kids[0], s->a, (TokKind)s->op, bft, bfi);
            return;
        }
        SType* t = lvalue_type(e, s->kids[0]);
        if (!t) {
            em_err(e, s, "assignment target has no type");
            return;
        }
        /*
         * A scalar local with a plain slot is read and written in place
         * (ldur/stur) with no address temp; the slot offset, not the Slot*,
         * is kept because evaluating the right side may grow the slot table.
         */
        Node* lhs = s->kids[0];
        Sym* ls = lhs->kind == N_IDENT ? node_sym(lhs) : NULL;
        int local = ls && ls->kind == SYM_VAR && is_scalar(t);
        int pr = local ? promo_reg(e, ls) : -1;
        Slot* lsl = local && pr < 0 ? local_slot(e, ls) : NULL;
        int direct = pr >= 0 ? 2 : lsl && !lsl->byref;
        uint32_t doff = pr >= 0 ? (uint32_t)pr : direct ? lsl->off : 0;
        // A plain store to a scalar field of a frame-resident local struct
        // also skips the address temp.
        if (!direct && s->op == T_ASSIGN && !(s->flags & F_VOLATILE) &&
            lhs->kind == N_FIELD_E && is_scalar(t) &&
            frame_place(e, lhs, &doff))
            direct = 1;
        int ta = direct ? -1 : gen_addr(e, lhs);
        if (s->op != T_ASSIGN) { // compound: x = x op y, address evaluated once
            if (!is_scalar(t)) {
                em_err(e, s, "compound assignment needs a scalar target");
                return;
            }
            TokKind op = compound_op((TokKind)s->op);
            int pointer = t->kind == SK_PTR && (op == T_PLUS || op == T_MINUS);
            // A promoted integer target is read in place and the result
            // computed into a fresh temp before it is stored back.
            int in_place = pr >= 0 && !is_floatt(t) && !pointer;
            uint64_t imm;
            if (in_place && scalar_bits(t) >= 32 && op != T_SLASH &&
                op != T_PERCENT) { // compute straight into the register
                if (imm12_const(s->a, &imm) &&
                    emit_arith_imm(e, op, pr, pr, imm, t))
                    return;
                EV rhs;
                int rb = src_reg(e, s->a, &rhs);
                emit_checked_arith(e, op, pr, pr, rb, t);
                return;
            }
            EV old = in_place ? (EV){ CLS_INT, tmp_int(e) }
                   : direct ? load_var(e, ls, t, lhs)
                            : load_scalar(e, xreg(ta), 0, t);
            int ra = in_place ? pr : xreg(old.idx);
            uint64_t divisor;
            if ((op == T_SLASH || op == T_PERCENT) && old.cls == CLS_INT &&
                const_int(s->a, &divisor) &&
                emit_const_divide(e, op, xreg(old.idx), ra, divisor, t)) {
                store_target(e, old, direct, doff, ta, t);
                return;
            }
            if (pointer) {
                EV rhs = gen_expr(e, s->a);
                emit_pointer_arith(e, op, old.idx, rhs.idx, ty_size(t->base));
            } else if (old.cls == CLS_FP) {
                EV rhs = gen_expr(e, s->a);
                emit_farith(e, op, old.idx, old.idx, rhs.idx,
                            unwrap(t)->prim == P_F64);
            } else if (old.cls == CLS_INT && imm12_const(s->a, &imm) &&
                       emit_arith_imm(e, op, xreg(old.idx), ra, imm, t)) {
            } else {
                EV rhs;
                int rb = src_reg(e, s->a, &rhs);
                emit_checked_arith(e, op, xreg(old.idx), ra, rb, t);
            }
            store_target(e, old, direct, doff, ta, t);
            return;
        }
        if (pr >= 0 && !is_floatt(t) && t->kind != SK_PTR) {
            uint64_t k;
            int bits = scalar_bits(t);
            if (const_int(s->a, &k)) { // constant straight into the register
                if (bits < 64) {
                    k &= bits == 32 ? 0xFFFFFFFFu : (UINT64_C(1) << bits) - 1;
                    if (ty_is_signed(t) && bits < 32 && (k >> (bits - 1)) & 1)
                        k = (uint32_t)(k | ~((UINT64_C(1) << bits) - 1));
                }
                load_imm(e, bits == 64, pr, k);
                return;
            }
            Node* r = s->a;
            SType* rt = r->kind == N_BINARY ? node_ty(r->a) : NULL;
            TokKind rop = r->kind == N_BINARY ? (TokKind)r->op : T_EOF;
            if (bits >= 32 && rt && is_scalar(rt) && !is_floatt(rt) &&
                rt->kind != SK_PTR && tok_cond(rop, rt) < 0 &&
                (rop == T_PLUS || rop == T_MINUS || rop == T_STAR ||
                 rop == T_AMP || rop == T_PIPE || rop == T_CARET ||
                 rop == T_SHL || rop == T_SHR) &&
                scalar_bits(rt) == bits) { // x = a op b into x's register
                EV a;
                int ra = src_reg(e, r->a, &a);
                if (a.cls == CLS_INT) {
                    uint64_t imm;
                    if (imm12_const(r->b, &imm) &&
                        emit_arith_imm(e, rop, pr, ra, imm, rt))
                        return;
                    EV b;
                    int rb = src_reg(e, r->b, &b);
                    if (b.cls == CLS_INT) {
                        emit_checked_arith(e, rop, pr, ra, rb, rt);
                        return;
                    }
                }
                em_err(e, s, "integer operand did not produce a scalar");
                return;
            }
        }
        EV v = gen_expr(e, s->a);
        if (direct) {
            store_target(e, v, direct, doff, ta, t);
        } else if (s->flags & F_VOLATILE) {
            /* Semantic checking admits only exact-width scalar stores here. */
            store_scalar(e, v, xreg(ta), 0, t);
        } else {
            store_value(e, v, xreg(ta), 0, t);
        }
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
        int l_else = new_label(e), l_end = new_label(e);
        gen_branch(e, s->a, 0, l_else);
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
        if (s->flags & F_FORGEN) {
            // General head: init; lcond: [cond]; body; lcont: [post];
            // br lcond; lbreak:. An omitted cond falls through; continue
            // targets the post clause so it runs before the next test, and
            // every loop owns its own exit targets.
            if (s->a) emit_stmt(e, s->a);
            int lcond = new_label(e), lcont = new_label(e), lbreak = new_label(e);
            align_code(e, LOOP_ALIGN);
            bind_label(e, lcond);
            if (s->b) gen_branch(e, s->b, 0, lbreak);
            if (e->nctx >= 64) {
                em_err(e, s, "control flow nested too deeply");
                return;
            }
            e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
            emit_block(e, s->d);
            e->nctx--;
            bind_label(e, lcont);
            if (s->c) emit_stmt(e, s->c);
            br_(e, lcond);
            bind_label(e, lbreak);
            return;
        }
        Node* value_binder = s->kids[s->nkids - 1];
        SType* elem = sema_build_type(value_binder->a);
        Sym* lv = node_sym(value_binder);
        uint32_t esz = ty_size(elem);
        // `in mut`: the value binder's slot holds the element address (the
        // cursor at binding time) and is marked byref, so gen_var_addr loads
        // it for every read, write, and address-of on the binder — the alias
        // reaches backing storage, never the slot's own address.
        int mut_iter = (s->flags & F_MUT) != 0 && s->b->kind != N_RANGE;
        // a promoted value binder takes each iteration's value in its
        // register (vpr) and has no slot
        int vpr = promo_reg(e, lv);
        uint32_t vs = 0;
        if (vpr < 0) {
            vs = slot_alloc(e, lv && lv->decl ? lv->decl : value_binder,
                            mut_iter ? 8 : esz, mut_iter ? 8 : ty_align(elem));
            if (mut_iter) e->slots[e->nslots - 1].byref = 1;
        }
        int lbreak = new_label(e), lcont = new_label(e), lcond = new_label(e);
        int lr = loop_regs_take(e, s); // x19+: cursor/count/step or p/end/index
        if (s->b->kind == N_RANGE) { // index loop over a range
            SType* rt = node_ty(s->b);
            if (!rt || rt->kind == SK_UT) rt = default_i64();
            uint32_t rsz = ty_size(rt);
            int rbits = scalar_bits(rt);
            uint32_t islot = temp_slot(e, rsz, 8), hslot = temp_slot(e, rsz, 8);
            uint32_t cslot = lr < 0 ? temp_slot(e, 8, 8) : 0;
            uint32_t dslot = lr < 0 ? temp_slot(e, 8, 8) : 0;
            uint32_t sslot = temp_slot(e, 8, 8);
            // register loops hold the exact 64-bit cursor, so the binder
            // store below needs no reload or extension
            int rcur = lr, rcnt = lr + 1, rstep = lr + 2;
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
            if (lr >= 0) {
                mov_int(e, 1, rcur, 16);
                mov_int(e, 1, rcnt, 15);
                mov_int(e, 1, rstep, 17);
            } else {
                store_frame(e, 15, cslot, 8);
                store_frame(e, 17, dslot, 8);
            }
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
            if (lr < 0) store_frame(e, 16, islot, rsz);
            ins(e, 0x91000000u | (1u << 10) | (15u << 5) | 15u);
            if (lr < 0) store_frame(e, 15, cslot, 8);
            load_frame(e, 17, sslot, 8, 0);
            ins(e, 0xCB000000u | (17u << 16) | (31u << 5) | 17u);
            if (lr >= 0) {
                mov_int(e, 1, rcur, 16);
                mov_int(e, 1, rcnt, 15);
                mov_int(e, 1, rstep, 17);
                br_(e, lsetup);
                bind_label(e, lempty);
                mov_int(e, 1, rcnt, 31); // mov xN, xzr: cursor and step unused
            } else {
                store_frame(e, 17, dslot, 8);
                br_(e, lsetup);
                bind_label(e, lempty);
                load_imm(e, 1, 16, 0);
                store_frame(e, 16, cslot, 8);
                load_frame(e, 17, sslot, 8, 0);
                store_frame(e, 17, dslot, 8);
            }
            align_code(e, LOOP_ALIGN);
            bind_label(e, lsetup);
            bind_label(e, lcond);
            if (lr >= 0) {
                cbz_(e, 1, rcnt, 0, lbreak);
                if (vpr >= 0) set_preg(e, vpr, rcur, elem);
                else store_frame(e, rcur, vs, (uint32_t)((scalar_bits(elem) + 7) / 8));
                e->vti = 0; e->vtf = 0;
                if (e->nctx >= 64) {
                    em_err(e, s, "control flow nested too deeply");
                    return;
                }
                e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
                emit_block(e, s->c);
                e->nctx--;
                bind_label(e, lcont);
                ins(e, 0x8B000000u | ((uint32_t)rstep << 16) |
                       ((uint32_t)rcur << 5) | (uint32_t)rcur); // add cursor, step
                ins(e, 0xD1000000u | (1u << 10) |
                       ((uint32_t)rcnt << 5) | (uint32_t)rcnt); // sub count, #1
                e->lreg_top -= 3;
                br_(e, lcond);
                bind_label(e, lbreak);
                return;
            }
            int tc = tmp_int(e);
            load_frame(e, xreg(tc), cslot, 8, 0);
            cbz_(e, 1, xreg(tc), 0, lbreak);
            int ti = tmp_int(e);
            load_frame(e, xreg(ti), islot, rsz, ty_is_signed(rt) && rsz < 8);
            // loop variable = i (truncating/extending store)
            if (scalar_bits(elem) > rbits) {
                if (ty_is_signed(rt)) ins(e, 0x93407C00u | (xreg(ti) << 5) | xreg(ti));
                else ins(e, 0xD3407C00u | (xreg(ti) << 5) | xreg(ti));
            }
            if (vpr >= 0)
                set_preg(e, vpr, xreg(ti), elem);
            else
                store_frame(e, xreg(ti), vs, scalar_bits(elem) > rbits ? 8u
                            : (uint32_t)((scalar_bits(elem) + 7) / 8));
            e->vti = 0; e->vtf = 0;
            if (e->nctx >= 64) {
                em_err(e, s, "control flow nested too deeply");
                return;
            }
            e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
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
        Node* index_binder = s->nkids == 2 ? s->kids[0] : NULL;
        int rp = lr, rend = lr + 1, rix = lr + 2; // register loops
        uint32_t pslot = 0, eslot = 0;
        if (lr < 0) {
            pslot = temp_slot(e, 8, 8);
            eslot = temp_slot(e, 8, 8);
        }
        uint32_t ivs = 0, ixslot = 0;
        int ipr = index_binder ? promo_reg(e, node_sym(index_binder)) : -1;
        if (index_binder) {
            Sym* ixv = node_sym(index_binder);
            if (ipr < 0)
                ivs = slot_alloc(e, ixv && ixv->decl ? ixv->decl : index_binder, 8, 8);
            if (lr >= 0)
                mov_int(e, 1, rix, 31); // mov xN, xzr
            else {
                ixslot = temp_slot(e, 8, 8);
                load_imm(e, 1, 16, 0);
                store_frame(e, 16, ixslot, 8);
            }
        }
        int svi = e->vti;
        int base;
        if (is_lvalue_node(s->b)) base = gen_addr(e, s->b);
        else {
            EV v = gen_expr(e, s->b);
            base = v.idx;
        }
        if (lr >= 0) {
            // p = start; end = p + len*esz (array: fixed count)
            if (bt->kind != SK_ARRAY) {
                ld_reg(e, rp, base, 0, 8, 0);
                ld_reg(e, 16, base, 8, 8, 0); // len
                load_imm(e, 1, 17, esz);
                ins(e, 0x9B007C00u | (17u << 16) | (16u << 5) | 17u); // x17 = len*esz
            } else {
                mov_int(e, 1, rp, base);
                load_imm(e, 1, 17, (uint64_t)bt->count * esz);
            }
            ins(e, 0x8B000000u | (17u << 16) | ((uint32_t)rp << 5) |
                   (uint32_t)rend); // end = p + x17
            e->vti = svi;
            align_code(e, LOOP_ALIGN);
            bind_label(e, lcond);
            ins(e, 0xEB00001Fu | ((uint32_t)rend << 16) | ((uint32_t)rp << 5));
            bcond_(e, C_HS, lbreak);
            if (vpr >= 0)
                load_binder(e, vpr, rp, elem);
            else if (mut_iter)
                store_frame(e, rp, vs, 8);
            else {
                frame_addr(e, 15, vs);
                copy_mem(e, 15, 0, rp, 0, esz);
            }
            if (ipr >= 0) mov_int(e, 1, ipr, rix);
            else if (index_binder) store_frame(e, rix, ivs, 8);
            e->vti = 0; e->vtf = 0;
            if (e->nctx >= 64) {
                em_err(e, s, "control flow nested too deeply");
                return;
            }
            e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
            emit_block(e, s->c);
            e->nctx--;
            bind_label(e, lcont);
            if (esz <= 4095)
                ins(e, 0x91000000u | (esz << 10) | ((uint32_t)rp << 5) | (uint32_t)rp);
            else {
                load_imm(e, 1, 16, esz);
                ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)rp << 5) | (uint32_t)rp);
            }
            if (index_binder)
                ins(e, 0x91000000u | (1u << 10) | ((uint32_t)rix << 5) | (uint32_t)rix);
            e->lreg_top -= loop_own_regs(s);
            br_(e, lcond);
            bind_label(e, lbreak);
            return;
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
        align_code(e, LOOP_ALIGN);
        bind_label(e, lcond);
        int tp = tmp_int(e), te = tmp_int(e);
        load_frame(e, xreg(tp), pslot, 8, 0);
        load_frame(e, xreg(te), eslot, 8, 0);
        ins(e, 0xEB00001Fu | (xreg(te) << 16) | (xreg(tp) << 5)); // cmp p, end
        bcond_(e, C_HS, lbreak);
        // loop variable = element copy at [p]; under `in mut`, the alias slot
        // receives the cursor itself (the element address)
        if (vpr >= 0)
            load_binder(e, vpr, xreg(tp), elem);
        else if (mut_iter)
            store_frame(e, xreg(tp), vs, 8);
        else {
            frame_addr(e, 15, vs);
            copy_mem(e, 15, 0, xreg(tp), 0, esz);
        }
        if (ipr >= 0) {
            load_frame(e, ipr, ixslot, 8, 0);
        } else if (index_binder) {
            load_frame(e, 16, ixslot, 8, 0);
            store_frame(e, 16, ivs, 8);
        }
        e->vti = 0; e->vtf = 0;
        if (e->nctx >= 64) {
            em_err(e, s, "control flow nested too deeply");
            return;
        }
        e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
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
        int has_default = 0, idefault = -1;
        if (s->nkids > 64) {
            em_err(e, s, "too many switch cases (max 64)");
            return;
        }
        for (int i = 0; i < s->nkids; i++) {
            lcase[i] = new_label(e);
            if (s->kids[i]->flags & F_DEFAULT) {
                has_default = 1;
                idefault = i;
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
        br_(e, has_default ? lcase[idefault] : lend);
        for (int i = 0; i < s->nkids; i++) {
            bind_label(e, lcase[i]);
            emit_block(e, s->kids[i]->b);
            if (i != s->nkids - 1 && !case_falls_through(s->kids[i])) br_(e, lend);
        }
        bind_label(e, lend);
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

/*
 * Callee-saved registers first..first+n-1 (x19 up: loop state, then promoted
 * locals; d8 up: promoted float locals) live in a frame save area at depth
 * `depth`: register k sits at x29 - depth + 8k, pairs through stp/ldp and an
 * odd last register through stur/ldur.
 */
static void save_regs(Em* e, int first, int n, uint32_t depth, int fp, int load) {
    for (int k = 0; k + 1 < n; k += 2) {
        uint32_t imm = (uint32_t)(-(int32_t)(depth - 8u * (uint32_t)k) / 8);
        uint32_t op = fp ? (load ? 0x6D400000u : 0x6D000000u)
                         : (load ? 0xA9400000u : 0xA9000000u);
        ins(e, op | ((imm & 0x7Fu) << 15) | ((uint32_t)(first + k + 1) << 10) |
               (29u << 5) | (uint32_t)(first + k));
    }
    if (n % 2) {
        int r = first + n - 1;
        uint32_t d = depth - 8u * (uint32_t)(n - 1);
        if (fp && load) load_frame_fp(e, r, d, 1);
        else if (fp) store_frame_fp(e, r, d, 1);
        else if (load) load_frame(e, r, d, 8, 0);
        else store_frame(e, r, d, 8);
    }
}

static void callee_saved(Em* e, int load) {
    if (e->nsaved) save_regs(e, 19, e->nsaved, e->lreg_save, 0, load);
    if (e->nfpreg) save_regs(e, 8, e->nfpreg, e->fpreg_save, 1, load);
}

static void emit_epilogue(Em* e) {
    e->ended_return = 1;
    if (e->mode == EMIT_CHECKED && e->current_is_main) {
        emit_bl_internal(e, "__ace$registry_leaks", 20);
    }
    callee_saved(e, 1);
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

// --- loop registers --------------------------------------------------------------------

// registers a range or sequence loop keeps in x19-x28 (0: general loop)
static int loop_own_regs(Node* s) {
    if (s->kind != N_FOR || (s->flags & F_FORGEN)) return 0;
    if (s->b->kind == N_RANGE) return 3;    // cursor, remaining count, step
    return s->nkids == 2 ? 3 : 2;           // cursor, end [, index]
}

/*
 * One linear walk over a function's statements before it is emitted. Returns
 * the register need of the deepest loop chain in s. A loop takes registers
 * when its own chain fits the pool, so inner loops win and no nested chain of
 * register loops can exceed LOOP_REGS; loops beyond that keep frame slots.
 */
static int plan_loops(Em* e, Node* s) {
    if (!s) return 0;
    int inner = 0, n;
    switch (s->kind) {
    case N_BLOCK:
        for (int i = 0; i < s->nkids; i++)
            if ((n = plan_loops(e, s->kids[i])) > inner) inner = n;
        return inner;
    case N_IF:
        inner = plan_loops(e, s->b);
        if ((n = plan_loops(e, s->c)) > inner) inner = n;
        return inner;
    case N_SWITCH:
        for (int i = 0; i < s->nkids; i++)
            if ((n = plan_loops(e, s->kids[i]->b)) > inner) inner = n;
        return inner;
    case N_LABEL:
        return plan_loops(e, s->a);
    case N_ASM:
        e->fn_has_asm = 1;
        return 0;
    case N_FOR: {
        if (s->flags & F_FORGEN) return plan_loops(e, s->d);
        int below = loop_own_regs(s) + plan_loops(e, s->c);
        if (below <= LOOP_REGS) {
            if (e->nlreg_loops == e->clreg_loops) {
                e->clreg_loops = e->clreg_loops ? e->clreg_loops * 2 : 16;
                e->lreg_loops = realloc(e->lreg_loops,
                                        (size_t)e->clreg_loops * sizeof(Node*));
                if (!e->lreg_loops) oom();
            }
            e->lreg_loops[e->nlreg_loops++] = s;
            if (below > e->lreg_max) e->lreg_max = below;
        }
        return below;
    }
    default:
        return 0;
    }
}

// first loop register (x19 + offset) for s, or -1 when s uses frame slots
static int loop_regs_take(Em* e, Node* s) {
    for (int i = 0; i < e->nlreg_loops; i++)
        if (e->lreg_loops[i] == s) {
            int r = 19 + e->lreg_top;
            e->lreg_top += loop_own_regs(s);
            return r;
        }
    return -1;
}

// --- register promotion ----------------------------------------------------------------

/*
 * Rank one use at loop depth `depth` adds to its local's promotion weight: a
 * straight-line use counts 1 and each loop level 8x, capped at depth 5 so a
 * sum cannot overflow short of 131072 uses. A local below PROMO_MIN_WEIGHT
 * keeps its slot: one straight-line use costs a slot store and load, about
 * what saving and restoring a callee-saved register would, while every
 * further use is a round-trip through memory the register avoids. The x9-x14
 * class of a call-free function needs no save, so a single use qualifies there.
 */
#define PROMO_MIN_WEIGHT 2

static uint32_t use_weight(int depth) {
    if (depth <= 0) return 1;
    return 1u << (3 * (depth < 5 ? depth : 5));
}

static PromoCand* promo_find(Em* e, Node* x) {
    Sym* s = x && x->kind == N_IDENT ? node_sym(x) : NULL;
    if (!s || !s->decl) return NULL;
    for (int i = 0; i < e->npcands; i++)
        if (e->pcands[i].decl == s->decl) return &e->pcands[i];
    return NULL;
}

// binder of a scalar local, loop binder, or by-value parameter
static void promo_add(Em* e, Node* binder) {
    Sym* s = node_sym(binder);
    if (!s || s->kind != SYM_VAR || s->is_global || s->decl != binder ||
        !is_scalar(s->type))
        return;
    if (e->npcands == e->cpcands) {
        e->cpcands = e->cpcands ? e->cpcands * 2 : 16;
        e->pcands = realloc(e->pcands, (size_t)e->cpcands * sizeof(PromoCand));
        if (!e->pcands) oom();
    }
    e->pcands[e->npcands++] = (PromoCand){ binder, 0, 0 };
}

static void promo_escape(Em* e, Node* x) {
    PromoCand* c = promo_find(e, x);
    if (c) c->escaped = 1;
}

/*
 * Whether gen_call lowers `call` without a bl that may clobber x9-x14: casts,
 * constructors, compile-time builtins, assert/panic (which never return), and
 * intrinsics that emit inline code or the x8/x15-x17-only byte helpers. This
 * mirrors gen_call's dispatch conservatively; emit_call rejects any real call
 * made while leaf registers hold locals, so a missed case cannot miscompile.
 */
static int call_is_inline(Node* call) {
    Node* callee = call->a;
    if (call->d && call->d->kind == N_CONFORMANCE) return 1;
    if (!call->sym) {
        if (callee->kind == N_PTR || callee->kind == N_SLICE_T) return 1;
        if (callee->kind == N_FIELD_E) return union_type_of_base(callee->a) != NULL;
        if (callee->kind != N_IDENT) return 0;
        Sym* cs = node_sym(callee);
        SType* ct = node_ty(callee);
        if ((cs && cs->kind == SYM_TYPE) || (ct && ct->kind != SK_FUNC)) return 1;
        static const char* const names[] = {
            "typeid", "sizeof", "alignof", "offsetof", "static_assert",
            "assert", "panic",
        };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (callee->len == strlen(names[i]) &&
                memcmp(callee->str, names[i], callee->len) == 0)
                return 1;
        return 0;
    }
    Sym* s = node_sym(call);
    uint64_t bi = s && s->decl ? (uint64_t)s->decl->ival : 0;
    switch (bi) {
    case BI_MEM_COPY: case BI_MEM_MOVE: case BI_MEM_FILL: case BI_MEM_ZERO:
    case BI_MEM_COMPARE: case BI_MEM_EQUAL:
        return call->ival == bi;
    case BI_BITS_FROM_FLOAT: case BI_BITS_TO_FLOAT: case BI_INTERNAL_LOAD_U64:
        return 1;
    default:
        return bi >= BI_BITS_POP_COUNT && bi <= BI_BITS_ROTATE_RIGHT;
    }
}

/*
 * Operand-count estimate of the int temps evaluating `n` holds at once:
 * operands are evaluated in order with earlier results held, so an expression
 * needs the most of i + (its i-th operand's need), and at least one temp per
 * operand plus its result. Statements hold nothing across their parts.
 * Lowerings with internal temps can exceed it; see TEMP_MARGIN.
 */
static int temp_need(Node* n) {
    if (!n) return 0;
    Node* part[4] = { n->a, n->b, n->c, NULL };
    if (n->kind == N_FOR && (n->flags & F_FORGEN)) part[3] = n->d;
    int held = 0, need = 0, count = 0;
    int stmt = n->kind == N_BLOCK || n->kind == N_IF || n->kind == N_FOR ||
               n->kind == N_SWITCH || n->kind == N_CASE || n->kind == N_LABEL ||
               n->kind == N_DEFER;
    for (int i = 0; i < 4 + n->nkids; i++) {
        Node* k = i < 4 ? part[i] : n->kids[i - 4];
        if (!k) continue;
        int kn = temp_need(k) + (stmt ? 0 : held);
        if (kn > need) need = kn;
        held++;
        count++;
    }
    if (!stmt && count + 1 > need) need = count + 1;
    return need;
}

/*
 * One walk over a function body before it is emitted: collect single-binder
 * scalar declarations and loop binders, weight every use by its loop depth, and mark each
 * place the emitter takes a variable's address or needs its storage (&x, a
 * volatile read or store, a mut argument, a field/index/slice base, a tuple
 * assignment target, a sequence loop's iterable). Follows a/b/c/kids only; Node.d is read solely as a
 * general for's body, since sema's d links form cycles.
 */
static void promo_scan(Em* e, Node* n, int depth) {
    if (!n) return;
    switch (n->kind) {
    case N_IDENT: {
        PromoCand* c = promo_find(e, n);
        if (c) c->weight += use_weight(depth);
        return;
    }
    case N_VAR:
        if (n->flags & F_MIXED) {
            for (int i = 0; i < n->nkids; i++)
                if (n->kids[i]->kind != N_PARAM) promo_escape(e, n->kids[i]);
        } else if (n->nkids == 1) {
            promo_add(e, n->kids[0]);
        }
        break;
    case N_ASSIGN:
        if (n->nkids > 1 || (n->flags & F_VOLATILE))
            for (int i = 0; i < n->nkids; i++) promo_escape(e, n->kids[i]);
        break;
    case N_UNARY:
        if (n->op == T_AMP || n->op == T_VOLATILE) promo_escape(e, n->a);
        break;
    case N_MUTARG: case N_FIELD_E: case N_INDEX: case N_SLICE_E:
        promo_escape(e, n->a);
        break;
    case N_CALL:
        if (!call_is_inline(n)) e->scan_call = 1;
        break;
    case N_FOR:
        if (n->flags & F_FORGEN) {
            promo_scan(e, n->a, depth);
            promo_scan(e, n->b, depth + 1);
            promo_scan(e, n->c, depth + 1);
            promo_scan(e, n->d, depth + 1);
        } else {
            // binders are per-iteration copies, except an `in mut` value alias
            int alias = (n->flags & F_MUT) && n->b->kind != N_RANGE;
            for (int i = 0; i < n->nkids; i++)
                if (!(alias && i == n->nkids - 1)) promo_add(e, n->kids[i]);
            if (n->b->kind != N_RANGE) promo_escape(e, n->b);
            promo_scan(e, n->b, depth);
            promo_scan(e, n->c, depth + 1);
        }
        return;
    default:
        break;
    }
    promo_scan(e, n->a, depth);
    promo_scan(e, n->b, depth);
    promo_scan(e, n->c, depth);
    for (int i = 0; i < n->nkids; i++) promo_scan(e, n->kids[i], depth);
}

// heaviest remaining non-escaping candidate of one register class, taken
static Node* promo_take(Em* e, int fp, uint32_t min_weight) {
    PromoCand* best = NULL;
    for (int i = 0; i < e->npcands; i++) {
        PromoCand* c = &e->pcands[i];
        if (!c->escaped && c->weight >= min_weight &&
            is_floatt(node_sym(c->decl)->type) == fp &&
            (!best || c->weight > best->weight))
            best = c;
    }
    if (!best) return NULL;
    best->weight = 0;
    return best->decl;
}

/*
 * Choose the locals that live in registers: integer-class ones in the x19-x28
 * registers loop state leaves free, float ones in d8-d15. A function that
 * makes no call first gives its heaviest integer locals x14 down to x9, which
 * nothing else clobbers and which need no save: as many as its expressions'
 * temp bound leaves above the temps. Within each class, non-escaping
 * candidates go by descending weight, ties in declaration order. Parameters
 * passed by reference (mut, method receivers) never qualify.
 */
static void plan_promotion(Em* e, Node* f) {
    e->npcands = 0;
    for (int i = 0; i < f->nkids; i++)
        if (!(f->kids[i]->flags & F_MUT) && !((f->flags & F_METHOD) && i == 0))
            promo_add(e, f->kids[i]);
    e->scan_call = 0;
    promo_scan(e, f->b, 0);
    Node* d;
    if (!e->scan_call) {
        int room = MAX_ITEMPS - temp_need(f->b) - TEMP_MARGIN;
        while (e->nxpreg < LEAF_REGS && e->nxpreg < room &&
               (d = promo_take(e, 0, 1)))
            e->xpreg_decl[e->nxpreg++] = d;
        e->vti_cap = MAX_ITEMPS - e->nxpreg;
    }
    while (e->lreg_max + e->npreg < LOOP_REGS &&
           (d = promo_take(e, 0, PROMO_MIN_WEIGHT)))
        e->preg_decl[e->npreg++] = d;
    while (e->nfpreg < FP_PROMO_REGS &&
           (d = promo_take(e, 1, PROMO_MIN_WEIGHT)))
        e->fpreg_decl[e->nfpreg++] = d;
}

static void emit_func(Em* e, Node* f, const char* symname, uint32_t symlen,
                      SType* ret, int is_main, int export_c) {
    align_code(e, FUNC_ALIGN);
    uint32_t debug_addr = (uint32_t)text(e)->len;
    line_mark(e, f);
    e->frame = 0;
    memset(e->int_spill, 0, sizeof e->int_spill);
    memset(e->fp_spill, 0, sizeof e->fp_spill);
    e->out_max = 0;
    e->nslots = 0;
    e->vti = e->vtf = 0;
    e->nlab = 0;
    e->nbp = 0;
    e->nctx = 0;
    e->ended_return = 0;
    e->fnret = ret;
    e->current_is_main = is_main;
    e->fnbody = f->b;
    e->ndefers = 0;
    e->retsave_off = 0;
    e->nlreg_loops = 0;
    e->lreg_top = e->lreg_max = 0;
    e->fn_has_asm = 0;
    plan_loops(e, f->b);
    e->npreg = e->nfpreg = e->nxpreg = 0;
    e->vti_cap = MAX_ITEMPS;
    if (e->fn_has_asm) { // asm bindings may name any x register
        e->nlreg_loops = 0;
        e->lreg_max = 0;
    } else {
        plan_promotion(e, f);
    }
    e->nsaved = e->lreg_max + e->npreg;
    sym_define(&e->syms, e->secs, symname, symlen, is_main || export_c,
               &e->secs[SEC_TEXT]);
    if (is_main) emit_has_main = 1;
    emit_prologue(e);
    if (e->nsaved)
        e->lreg_save = slot_alloc(e, NULL, 8u * (uint32_t)e->nsaved, 8);
    if (e->nfpreg)
        e->fpreg_save = slot_alloc(e, NULL, 8u * (uint32_t)e->nfpreg, 8);
    callee_saved(e, 0);

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
        int pr = promo_reg(e, node_sym(f->kids[i]));
        if (pr >= 0) { // promoted scalar: one register or stack unit
            TU* u = &units[i * 4];
            int is64 = unwrap(t)->prim == P_F64;
            if (u->kind == TU_XREG && u->reg >= 0) {
                set_preg(e, pr, u->reg, t);
            } else if (u->kind == TU_VREG) {
                fmov_raw(e, is64, pr, u->reg);
            } else if (u->kind == TU_STACK && is_floatt(t)) {
                incoming_addr(e, 16, u->soff);
                ldrf_(e, pr, 16, 0, is64);
            } else if (u->kind == TU_STACK) {
                incoming_addr(e, 16, u->soff);
                ld_reg(e, 16, 16, 0, u->size < 8 ? u->size : 8, 0);
                set_preg(e, pr, 16, t);
            }
            continue;
        }
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
                // unit u->off bytes into the slot sits at depth slot - off
                store_frame(e, u->reg, byref ? slot : slot - u->off, bw);
            } else if (u->kind == TU_VREG) {
                store_frame_fp(e, u->reg, slot - u->off, u->size == 8);
            } else if (u->kind == TU_STACK) {
                incoming_addr(e, 16, u->soff);
                if (is_floatt(t)) {
                    ldrf_(e, 0, 16, 0, u->size == 8);
                    store_frame_fp(e, 0, slot, u->size == 8);
                } else {
                    // u->size is the ABI stack-slot width (Darwin pads to a
                    // 4-byte minimum); homing into the frame must only copy
                    // the scalar's actual width, or a narrow trailing scalar
                    // (e.g. a stack-passed bool) overwrites adjacent bytes.
                    uint32_t slotw = u->size < 8 ? u->size : 8;
                    uint32_t bw = byref ? 8 : is_scalar(t)
                        ? (uint32_t)((scalar_bits(t) + 7) / 8) : slotw;
                    ld_reg(e, 16, 16, 0, slotw, 0);
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

    if (f->flags & F_INTRINSIC) {
        if (f->ival == BI_BITS_FROM_FLOAT) {
            int is64 = unwrap(pt[0])->prim == P_F64;
            ins(e, is64 ? 0x9E660000u : 0x1E260000u); // fmov x0,d0 / w0,s0
        } else if (f->ival == BI_BITS_TO_FLOAT) {
            int is64 = unwrap(ret)->prim == P_F64;
            ins(e, is64 ? 0x9E670000u : 0x1E270000u); // fmov d0,x0 / s0,w0
        } else if (f->ival == BI_INTERNAL_LOAD_U64) {
            ins(e, 0x8B020000u); // add x0,x0,x2
            ins(e, 0xF9400000u); // ldr x0,[x0]
        } else if (f->ival >= BI_BITS_POP_COUNT &&
                   f->ival <= BI_BITS_ROTATE_RIGHT) {
            emit_bits_integer(e, f->ival, scalar_bits(pt[0]), 0, 1, 0);
        }
        emit_epilogue(e);
        e->ended_return = 1;
    }

    if (f->b)
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

static void const_reloc_add(ConstRels* rs, uint32_t off, ESym* sym) {
    if (rs->n == rs->cap) {
        rs->cap = rs->cap ? rs->cap * 2 : 8;
        rs->v = realloc(rs->v, (size_t)rs->cap * sizeof(ConstRel));
        if (!rs->v) oom();
    }
    rs->v[rs->n++] = (ConstRel){ off, sym };
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
    if (t->kind == SK_ENUM || t->kind == SK_BITS) return 1;
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

static int eval_const(Em* e, Node* v, SType* t, Buf* out, ConstRels* rels) {
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
            if (!eval_const(e, v->kids[0], ut->fields[vi].t, out, rels)) return 0;
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
                return eval_const(e, arg, want, out, rels);
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
    case N_STR: {
        if (t->kind != SK_PRIM || t->prim != P_STRING) return 0;
        uint32_t off = (uint32_t)out->len;
        ESym* str = intern_string(e, v);
        buf_put64(out, 0);
        buf_put64(out, v->len);
        const_reloc_add(rels, off, str);
        return 1;
    }
    case N_FIELD_E: { // enum member (base carries the type symbol)
        Sym* ts = node_sym(v->a);
        if (ts && ts->kind == SYM_TYPE && ts->type &&
            ts->type->kind == SK_ERROR_DOMAIN) {
            put_u(out, v->ival, 8);
            return 1;
        }
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ENUM) {
            put_u(out, v->ival, ty_size(ts->type));
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
    case N_IDENT: {
        Sym* s = node_sym(v);
        if (s && s->is_global && s->is_const && s->init)
            return eval_const(e, s->init, t, out, rels);
        return 0;
    }
    case N_UNARY:
        if (v->op == T_MINUS || v->op == T_TILDE) {
            uint64_t c;
            if (const_int(v, &c)) {
                put_u(out, c, ty_size(t) < 8 ? ty_size(t) : 8);
                return 1;
            }
        }
        return 0;
    case N_ARRAYLIT: case N_STRUCTLIT: {
        if (v->kind == N_STRUCTLIT && t->kind == SK_BITS) {
            uint64_t raw = 0;
            for (int i = 0; i < v->nkids; i++) {
                int fi = find_field(t, v->kids[i]->str, v->kids[i]->len);
                uint64_t value;
                if (fi < 0 || !const_int(v->kids[i]->a, &value)) return 0;
                SField* f = &t->fields[fi];
                uint64_t low = f->bit_width == 64 ? UINT64_MAX :
                               (UINT64_C(1) << f->bit_width) - 1;
                raw |= (value & low) << f->bit_offset;
            }
            put_u(out, raw, ty_size(t));
            return 1;
        }
        if (v->kind == N_STRUCTLIT && t->kind != SK_STRUCT) return 0;
        if (v->kind == N_ARRAYLIT && t->kind != SK_ARRAY) return 0;
        int positional = v->nkids > 0 && v->kids[0]->str == NULL;
        size_t start = out->len;
        if (v->kind == N_STRUCTLIT) {
            for (uint32_t i = 0; i < ty_size(t); i++) buf_put8(out, 0);
            for (int i = 0; i < v->nkids; i++) {
                Node* fi = v->kids[i];
                int fidx = positional ? i : find_field(t, fi->str, fi->len);
                if (fidx < 0 || fidx >= t->nfields) { out->len = start; return 0; }
                Buf field = {0};
                ConstRels field_rels = {0};
                if (!eval_const(e, fi->a, t->fields[fidx].t, &field,
                                &field_rels) ||
                    field.len != ty_size(t->fields[fidx].t)) {
                    free(field.d); free(field_rels.v);
                    out->len = start; return 0;
                }
                uint32_t field_start = (uint32_t)(start + field_off(t, fidx));
                memcpy(out->d + field_start, field.d, field.len);
                for (int j = 0; j < field_rels.n; j++)
                    const_reloc_add(rels, field_start + field_rels.v[j].off,
                                    field_rels.v[j].sym);
                free(field.d);
                free(field_rels.v);
            }
            return 1;
        }
        uint32_t cursor = 0;
        if (v->kind == N_ARRAYLIT && v->a) {
            for (uint64_t i = 0; i < t->count; i++) {
                if (!eval_const(e, v->a, t->base, out, rels)) return 0;
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
            if (!eval_const(e, value, ft, out, rels)) return 0;
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

static Node* const_string_node(Node* n) {
    if (!n) return NULL;
    if (n->kind == N_STR) return n;
    if (n->kind == N_IDENT) {
        Sym* s = node_sym(n);
        if (s && s->is_global && s->is_const && s->init)
            return const_string_node(s->init);
    }
    if (n->kind == N_CALL && !n->sym && n->nkids == 1)
        return const_string_node(n->kids[0]);
    return NULL;
}

static uint64_t map_hash_int(uint64_t x, uint64_t seed) {
    uint64_t z = x + seed + UINT64_C(0x9e3779b97f4a7c15);
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static uint64_t map_hash_string(const char* p, uint32_t n, uint64_t seed) {
    uint64_t h = UINT64_C(1469598103934665603) ^ seed;
    for (uint32_t i = 0; i < n; i++) {
        h ^= (uint8_t)p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static StaticMapData* map_data(Em* e, Sym* s) {
    for (int i = 0; i < e->nmaps; i++) if (e->maps[i].s == s) return &e->maps[i];
    return NULL;
}

static void emit_staticmap_global(Em* e, Node* n, Sym* vs, SType* t) {
    Node* lit = n->b;
    int count = lit ? lit->nkids : 0;
    SType* rawkey = unwrap(t->base);
    int strings = rawkey && rawkey->kind == SK_PRIM && rawkey->prim == P_STRING;
    uint32_t size = 1;
    while (size < (uint32_t)(count ? count * 2 : 1)) size <<= 1;
    uint64_t seed = 0;
    uint32_t* slots = NULL;
    for (;;) {
        slots = calloc(size, sizeof(uint32_t));
        if (!slots) oom();
        int found = 0;
        for (seed = 0; seed < UINT64_C(1000000); seed++) {
            memset(slots, 0, (size_t)size * sizeof(uint32_t));
            int collision = 0;
            for (int i = 0; i < count; i++) {
                Node* entry = lit->kids[i];
                uint64_t h;
                if (strings) {
                    Node* s = const_string_node(entry->a);
                    h = map_hash_string(s->str, s->len, seed);
                } else {
                    h = map_hash_int(entry->ival, seed);
                }
                uint32_t at = (uint32_t)h & (size - 1);
                if (slots[at]) { collision = 1; break; }
                slots[at] = (uint32_t)i + 1;
            }
            if (!collision) { found = 1; break; }
        }
        if (found) break;
        free(slots);
        if (size >= (1u << 30)) {
            em_err(e, n, "could not construct StaticMap perfect hash");
            return;
        }
        size <<= 1;
    }

    Sec* d = &e->secs[SEC_CONST];
    char name[640];
    uint32_t ka = ty_align(t->base), va = ty_align(t->ret);
    Buf key_bytes = {0}, value_bytes = {0};
    ConstRels key_rels = {0};
    for (int i = 0; i < count; i++) {
        if (strings) {
            Node* text = const_string_node(lit->kids[i]->a);
            if (!text || !eval_const(e, text, t->base, &key_bytes, &key_rels))
                em_err(e, lit->kids[i]->a, "StaticMap key did not emit as a constant");
        } else {
            put_u(&key_bytes, lit->kids[i]->ival, ty_size(t->base));
        }
    }
    ConstRels value_rels = {0};
    for (int i = 0; i < count; i++)
        if (!eval_const(e, lit->kids[i]->b, t->ret, &value_bytes, &value_rels))
            em_err(e, lit->kids[i]->b, "StaticMap value did not emit as a constant");

    while (d->data.len % ka) buf_put8(&d->data, 0);
    snprintf(name, sizeof name, "__ace$map$%.*s$keys", (int)vs->len, vs->name);
    ESym* keys = sym_define(&e->syms, e->secs, name, (uint32_t)strlen(name), 0, d);
    uint32_t key_start = (uint32_t)d->data.len;
    buf_write(&d->data, key_bytes.d, key_bytes.len);
    for (int i = 0; i < key_rels.n; i++)
        reloc_add(d, key_start + key_rels.v[i].off, key_rels.v[i].sym,
                  0, 3, 1, RM_UNSIGNED);

    while (d->data.len % va) buf_put8(&d->data, 0);
    snprintf(name, sizeof name, "__ace$map$%.*s$values", (int)vs->len, vs->name);
    ESym* values = sym_define(&e->syms, e->secs, name, (uint32_t)strlen(name), 0, d);
    uint32_t value_start = (uint32_t)d->data.len;
    buf_write(&d->data, value_bytes.d, value_bytes.len);
    for (int i = 0; i < value_rels.n; i++)
        reloc_add(d, value_start + value_rels.v[i].off, value_rels.v[i].sym,
                  0, 3, 1, RM_UNSIGNED);
    free(key_bytes.d); free(value_bytes.d);
    free(key_rels.v); free(value_rels.v);

    while (d->data.len % 4) buf_put8(&d->data, 0);
    snprintf(name, sizeof name, "__ace$map$%.*s$slots", (int)vs->len, vs->name);
    ESym* slot_sym = sym_define(&e->syms, e->secs, name, (uint32_t)strlen(name), 0, d);
    for (uint32_t i = 0; i < size; i++) buf_put32(&d->data, slots[i]);
    free(slots);

    if (e->nmaps == e->cmaps) {
        e->cmaps = e->cmaps ? e->cmaps * 2 : 8;
        e->maps = realloc(e->maps, (size_t)e->cmaps * sizeof(*e->maps));
        if (!e->maps) oom();
    }
    e->maps[e->nmaps++] = (StaticMapData){
        vs, keys, values, slot_sym, seed, size - 1, (uint32_t)count,
        (uint8_t)strings
    };
}

static void add_global(Em* e, Sym* s, ESym* sym);

static void add_global(Em* e, Sym* s, ESym* sym) {
    if (e->nglobs == e->cglobs) {
        e->cglobs = e->cglobs ? e->cglobs * 2 : 8;
        e->globs = realloc(e->globs, (size_t)e->cglobs * sizeof(GVar));
        if (!e->globs) oom();
    }
    e->globs[e->nglobs++] =
        (GVar){ s, sym->name, (uint32_t)strlen(sym->name) };
}

static int emit_error_handle_global(Em* e, Node* n, Sym* vs,
                                    const char* nm) {
    Node* call = n->b;
    uint64_t code = 0;
    if (call && call->kind == N_NULL) {
        Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
        while (d->data.len % 8) buf_put8(&d->data, 0);
        ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
        buf_put64(&d->data, 0);
        add_global(e, vs, g);
        return 1;
    }
    if (call && call->kind == N_FIELD_E && node_ty(call) &&
        node_ty(call)->kind == SK_ERROR) {
        Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
        while (d->data.len % 8) buf_put8(&d->data, 0);
        ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
        buf_put64(&d->data, call->ival);
        add_global(e, vs, g);
        return 1;
    }
    if (!call || call->kind != N_CALL || call->nkids > 1 ||
        (call->nkids == 1 && !const_int(call->kids[0], &code))) {
        em_err(e, n, "global Error initializer must use a domain with a constant i32 code");
        return 0;
    }
    Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
    while (d->data.len % 8) buf_put8(&d->data, 0);
    ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
    buf_put64(&d->data, (call->ival << 32) | (uint32_t)code);
    add_global(e, vs, g);
    return 1;
}

static void emit_global(Em* e, Node* n) {
    Node* binder = n->kids[0];
    Sym* vs = node_sym(binder);
    if (!vs) return;
    SType* t = sema_build_type(binder->a);
    if (t->kind == SK_STATICMAP) {
        emit_staticmap_global(e, n, vs, t);
        return;
    }
    uint32_t size = ty_size(t), align = ty_align(t);
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
    if (t->kind == SK_ERROR) {
        emit_error_handle_global(e, n, vs, nm);
        return;
    }
    Buf bytes = {0};
    ConstRels rels = {0};
    if (!eval_const(e, n->b, t, &bytes, &rels)) {
        if (t->kind == SK_UNION)
            em_err(e, n, "global union initializer must be a compile-time variant value");
        else
            em_err(e, n, "global initializer must be a compile-time constant");
        free(bytes.d);
        free(rels.v);
        return;
    }
    Sec* d = (n->flags & F_CONST) ? &e->secs[SEC_CONST] : &e->secs[SEC_DATA];
    while (d->data.len % align) buf_put8(&d->data, 0);
    ESym* g = sym_define(&e->syms, e->secs, nm, (uint32_t)strlen(nm), 0, d);
    uint32_t start = (uint32_t)d->data.len;
    buf_write(&d->data, bytes.d, bytes.len);
    for (int i = 0; i < rels.n; i++)
        reloc_add(d, start + rels.v[i].off, rels.v[i].sym,
                  0, 3, 1, RM_UNSIGNED);
    free(bytes.d);
    free(rels.v);
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
    uint32_t max_section_align = 1;
    for (int i = 0; i < NSECS; i++) {
        uint32_t a = 1u << e->secs[i].align;
        if (max_section_align < a) max_section_align = a;
    }
    uint32_t base = (cmdend + max_section_align - 1) & ~(max_section_align - 1);

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

static void emit_bounds_failure_helper(Em* e) {
    if (!e->bounds_fail_used) return;
    sym_define(&e->syms, e->secs, "__ace$bounds_fail", 17, 0,
               &e->secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    ins(e, 0x910003FD); // mov x29, sp
    emit_bl_extern(e, "_write", 6);
    emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u); // brk #0 if abort unexpectedly returns
}

static void emit_arithmetic_failure_helper(Em* e) {
    if (!e->arithmetic_fail_used) return;
    static const char message[] = "ace: arithmetic violation\n";
    Sec* c = &e->secs[SEC_CONST];
    ESym* msg = sym_define(&e->syms, e->secs, "__ace$arithmetic_message", 24, 0, c);
    buf_write(&c->data, message, sizeof message - 1);

    sym_define(&e->syms, e->secs, "__ace$arithmetic_fail", 21, 0,
               &e->secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD);
    ins(e, 0x910003FD);
    load_imm(e, 1, 0, 2);
    addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, sizeof message - 1);
    emit_bl_extern(e, "_write", 6);
    emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u);
}

static void emit_contract_failure_helper(Em* e) {
    if (!e->contract_fail_used) return;
    static const char message[] = "ace: call contract violation\n";
    Sec* c = &e->secs[SEC_CONST];
    ESym* msg = sym_define(&e->syms, e->secs, "__ace$contract_message", 22, 0, c);
    buf_write(&c->data, message, sizeof message - 1);
    sym_define(&e->syms, e->secs, "__ace$contract_fail", 19, 0,
               &e->secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD);
    ins(e, 0x910003FD);
    load_imm(e, 1, 0, 2);
    addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, sizeof message - 1);
    emit_bl_extern(e, "_write", 6);
    emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u);
}

static void emit_null_failure_helper(Em* e) {
    if (!e->null_fail_used) return;
    static const char message[] = "ace: null use violation\n";
    Sec* c = &e->secs[SEC_CONST];
    ESym* msg = sym_define(&e->syms, e->secs, "__ace$null_message", 18, 0, c);
    buf_write(&c->data, message, sizeof message - 1);
    sym_define(&e->syms, e->secs, "__ace$null_fail", 15, 0,
               &e->secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD);
    ins(e, 0x910003FD);
    load_imm(e, 1, 0, 2);
    addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, sizeof message - 1);
    emit_bl_extern(e, "_write", 6);
    emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u);
}

static void helper_prologue(Em* e) {
    ins(e, 0xA9BF7BFD); // stp x29,x30,[sp,#-16]!
    ins(e, 0x910003FD); // mov x29,sp
    ins(e, 0xD10083FF); // sub sp,sp,#32
    st_reg(e, 19, 31, 0, 8);
    st_reg(e, 20, 31, 8, 8);
    st_reg(e, 21, 31, 16, 8);
}

static void helper_epilogue(Em* e) {
    ld_reg(e, 19, 31, 0, 8, 0);
    ld_reg(e, 20, 31, 8, 8, 0);
    ld_reg(e, 21, 31, 16, 8, 0);
    ins(e, 0x910083FF); // add sp,sp,#32
    ins(e, 0xA8C17BFD);
    ins(e, 0xD65F03C0);
}

// SIMD&FP single-register LDUR/STUR (op selects b/s/d/q and direction).
static void vmem(Em* e, uint32_t op, int rt, int rn, int32_t off) {
    ins(e, op | (((uint32_t)off & 0x1FFu) << 12) | ((uint32_t)rn << 5) |
           (uint32_t)rt);
}


// Q-register LDP/STP (op selects offset, pre-, or post-index form).
static void vpair(Em* e, uint32_t op, int rt, int rt2, int rn, int32_t off) {
    ins(e, op | (((uint32_t)(off / 16) & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rt);
}

enum {
    LDUR_B = 0x3C400000u, STUR_B = 0x3C000000u,
    LDUR_S = 0xBC400000u, STUR_S = 0xBC000000u,
    LDUR_D = 0xFC400000u, STUR_D = 0xFC000000u,
    LDUR_Q = 0x3CC00000u, STUR_Q = 0x3C800000u,
    LDP_Q = 0xAD400000u, STP_Q = 0xAD000000u,
    LDP_Q_POST = 0xACC00000u, STP_Q_POST = 0xAC800000u,
    LDP_Q_PRE = 0xADC00000u, STP_Q_PRE = 0xAD800000u,
};

// Branch to mismatch when the 32 bytes at [x15] and [x16] differ.
static void compare_block32(Em* e, int mismatch) {
    vpair(e, LDP_Q, 24, 25, 15, 0);
    vpair(e, LDP_Q, 26, 27, 16, 0);
    ins(e, 0x6E201C00u | (26u << 16) | (24u << 5) | 24u); // eor v24, v24, v26
    ins(e, 0x6E201C00u | (27u << 16) | (25u << 5) | 25u); // eor v25, v25, v27
    ins(e, 0x4EA01C00u | (25u << 16) | (24u << 5) | 24u); // orr v24, v24, v25
    ins(e, 0x6EA0A400u | (24u << 16) | (24u << 5) | 24u); // umaxp v24.4s
    ins(e, 0x9E660000u | (24u << 5) | 8u);                // fmov x8, d24
    cbz_(e, 1, 8, 1, mismatch);
}

/*
 * Shared byte-operation routines, emitted once per object when used. A private
 * convention keeps call sites spill-free: x15 = destination, x16 = source or
 * fill byte, x17 = byte count. The routines clobber only x8, x15-x17, x30,
 * v24-v31, and flags, none of which hold temps. Each size class up to 64 bytes
 * loads every byte before storing, so it also serves overlapping moves; tails
 * reuse overlapping chunks instead of byte loops. Larger runs move 32 bytes per
 * iteration with the far-end chunk loaded before the loop. NEON use here is
 * internal instruction selection, not a language vector facility.
 */
static void emit_byte_helpers(Em* e) {
    if (e->mem_copy_used) {
        int copy = new_label(e), backward = new_label(e), lt16 = new_label(e);
        int lt8 = new_label(e), lt4 = new_label(e), done = new_label(e);
        int gt32 = new_label(e), big = new_label(e), loop = new_label(e);
        int bloop = new_label(e);

        // move: at most 64 bytes is overlap-safe; above that, go backward only
        // when the destination starts inside the source.
        sym_define(&e->syms, e->secs, "__ace$mem_move", 14, 0,
                   &e->secs[SEC_TEXT]);
        ins(e, 0xF100001Fu | (64u << 10) | (17u << 5));   // cmp x17, #64
        bcond_(e, C_LS, copy);
        ins(e, 0xCB000000u | (16u << 16) | (15u << 5) | 8u); // x8 = dst - src
        ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));    // cmp x8, x17
        bcond_(e, C_LO, backward);

        sym_define(&e->syms, e->secs, "__ace$mem_copy", 14, 0,
                   &e->secs[SEC_TEXT]);
        bind_label(e, copy);
        ins(e, 0xF100001Fu | (32u << 10) | (17u << 5));   // cmp x17, #32
        bcond_(e, C_HI, gt32);
        ins(e, 0xF100001Fu | (16u << 10) | (17u << 5));   // cmp x17, #16
        bcond_(e, C_LO, lt16);
        vmem(e, LDUR_Q, 24, 16, 0);                        // 16..32
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        vmem(e, LDUR_Q, 25, 16, -16);
        vmem(e, STUR_Q, 24, 15, 0);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_Q, 25, 15, -16);
        ins(e, 0xD65F03C0u);
        bind_label(e, lt16);
        ins(e, 0xF100001Fu | (8u << 10) | (17u << 5));    // cmp x17, #8
        bcond_(e, C_LO, lt8);
        vmem(e, LDUR_D, 24, 16, 0);                        // 8..15
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        vmem(e, LDUR_D, 25, 16, -8);
        vmem(e, STUR_D, 24, 15, 0);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_D, 25, 15, -8);
        ins(e, 0xD65F03C0u);
        bind_label(e, lt8);
        ins(e, 0xF100001Fu | (4u << 10) | (17u << 5));    // cmp x17, #4
        bcond_(e, C_LO, lt4);
        vmem(e, LDUR_S, 24, 16, 0);                        // 4..7
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        vmem(e, LDUR_S, 25, 16, -4);
        vmem(e, STUR_S, 24, 15, 0);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_S, 25, 15, -4);
        ins(e, 0xD65F03C0u);
        bind_label(e, lt4);                                // 0..3: first, middle, last
        cbz_(e, 1, 17, 0, done);
        ins(e, 0xD341FE28u);                               // lsr x8, x17, #1
        vmem(e, LDUR_B, 24, 16, 0);
        ins(e, 0x3C606800u | (8u << 16) | (16u << 5) | 25u); // ldr b25, [x16, x8]
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        vmem(e, LDUR_B, 26, 16, -1);
        vmem(e, STUR_B, 24, 15, 0);
        ins(e, 0x3C206800u | (8u << 16) | (15u << 5) | 25u); // str b25, [x15, x8]
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_B, 26, 15, -1);
        bind_label(e, done);
        ins(e, 0xD65F03C0u);
        bind_label(e, gt32);
        ins(e, 0xF100001Fu | (64u << 10) | (17u << 5));   // cmp x17, #64
        bcond_(e, C_HI, big);
        vpair(e, LDP_Q, 24, 25, 16, 0);                    // 33..64
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        vpair(e, LDP_Q, 26, 27, 16, -32);
        vpair(e, STP_Q, 24, 25, 15, 0);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vpair(e, STP_Q, 26, 27, 15, -32);
        ins(e, 0xD65F03C0u);
        // Forward: preload the tail and head, then continue from a
        // 32-byte-aligned destination so no loop store splits a cache line.
        // The first aligned chunk loads before the head store, which may
        // overwrite it in a leftward overlapping move.
        int tail = new_label(e);
        bind_label(e, big);
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 8u);
        vpair(e, LDP_Q, 30, 31, 8, -32);
        vpair(e, LDP_Q, 26, 27, 16, 0);
        ins(e, 0x91000000u | (32u << 10) | (15u << 5) | 8u);  // add x8, x15, #32
        ins(e, 0x927BE908u);                                  // and x8, x8, #~31
        ins(e, 0xCB000000u | (15u << 16) | (8u << 5) | 8u);   // x8 = step, 1..32
        ins(e, 0x8B000000u | (8u << 16) | (16u << 5) | 16u);
        vpair(e, LDP_Q_POST, 24, 25, 16, 32);
        vpair(e, STP_Q, 26, 27, 15, 0);
        ins(e, 0x8B000000u | (8u << 16) | (15u << 5) | 15u);
        ins(e, 0xCB000000u | (8u << 16) | (17u << 5) | 17u);
        vpair(e, STP_Q_POST, 24, 25, 15, 32);
        ins(e, 0xF1000000u | (64u << 10) | (17u << 5) | 17u); // subs x17, x17, #64
        bcond_(e, C_LS, tail);
        align_code(e, 16);
        bind_label(e, loop);
        vpair(e, LDP_Q_POST, 24, 25, 16, 32);
        vpair(e, STP_Q_POST, 24, 25, 15, 32);
        ins(e, 0xF1000000u | (32u << 10) | (17u << 5) | 17u); // subs x17, x17, #32
        bcond_(e, C_HI, loop);
        bind_label(e, tail);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 8u);  // x8 = end - 32
        vpair(e, STP_Q, 30, 31, 8, 0);
        ins(e, 0xD65F03C0u);
        bind_label(e, backward);                           // backward, head preloaded
        vpair(e, LDP_Q, 30, 31, 16, 0);
        mov_int(e, 1, 8, 15);
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        ins(e, 0xD1000000u | (32u << 10) | (17u << 5) | 17u);
        align_code(e, 16);
        bind_label(e, bloop);
        vpair(e, LDP_Q_PRE, 24, 25, 16, -32);
        vpair(e, STP_Q_PRE, 24, 25, 15, -32);
        ins(e, 0xF1000000u | (32u << 10) | (17u << 5) | 17u);
        bcond_(e, C_HI, bloop);
        vpair(e, STP_Q, 30, 31, 8, 0);
        ins(e, 0xD65F03C0u);
    }
    if (e->mem_fill_used) {
        int lt16 = new_label(e), lt8 = new_label(e), lt4 = new_label(e);
        int done = new_label(e), gt32 = new_label(e), loop = new_label(e);

        sym_define(&e->syms, e->secs, "__ace$mem_fill", 14, 0,
                   &e->secs[SEC_TEXT]);
        ins(e, 0x4E010C00u | (16u << 5) | 24u);            // dup v24.16b, w16
        ins(e, 0xF100001Fu | (32u << 10) | (17u << 5));   // cmp x17, #32
        bcond_(e, C_HI, gt32);
        ins(e, 0xF100001Fu | (16u << 10) | (17u << 5));   // cmp x17, #16
        bcond_(e, C_LO, lt16);
        vmem(e, STUR_Q, 24, 15, 0);                        // 16..32
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_Q, 24, 15, -16);
        ins(e, 0xD65F03C0u);
        bind_label(e, lt16);
        ins(e, 0xF100001Fu | (8u << 10) | (17u << 5));    // cmp x17, #8
        bcond_(e, C_LO, lt8);
        vmem(e, STUR_D, 24, 15, 0);                        // 8..15
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_D, 24, 15, -8);
        ins(e, 0xD65F03C0u);
        bind_label(e, lt8);
        ins(e, 0xF100001Fu | (4u << 10) | (17u << 5));    // cmp x17, #4
        bcond_(e, C_LO, lt4);
        vmem(e, STUR_S, 24, 15, 0);                        // 4..7
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_S, 24, 15, -4);
        ins(e, 0xD65F03C0u);
        bind_label(e, lt4);                                // 0..3: first, middle, last
        cbz_(e, 1, 17, 0, done);
        ins(e, 0xD341FE28u);                               // lsr x8, x17, #1
        vmem(e, STUR_B, 24, 15, 0);
        ins(e, 0x3C206800u | (8u << 16) | (15u << 5) | 24u); // str b24, [x15, x8]
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        vmem(e, STUR_B, 24, 15, -1);
        bind_label(e, done);
        ins(e, 0xD65F03C0u);
        bind_label(e, gt32);                               // 33+: tail, then 32-byte runs
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 8u);
        vpair(e, STP_Q, 24, 24, 8, -32);
        ins(e, 0xD1000000u | (32u << 10) | (17u << 5) | 17u); // sub x17, x17, #32
        align_code(e, 16);
        bind_label(e, loop);
        vpair(e, STP_Q_POST, 24, 24, 15, 32);
        ins(e, 0xF1000000u | (32u << 10) | (17u << 5) | 17u); // subs x17, x17, #32
        bcond_(e, C_HI, loop);
        ins(e, 0xD65F03C0u);
    }
    if (e->mem_compare_used) {
        int lt16 = new_label(e), lt8 = new_label(e), lt4 = new_label(e);
        int gt32 = new_label(e), loop = new_label(e), words = new_label(e);
        int same = new_label(e), diff = new_label(e), res16 = new_label(e);
        int res32 = new_label(e), last = new_label(e);

        /*
         * compare: x15 = a, x16 = b, x17 = common length; returns -1, 0, or 1
         * in x17 by the first differing unsigned byte. NEON locates a
         * differing 16- or 32-byte chunk; byte-reversed 64-bit words then
         * order it, since reversal puts the first byte in the high bits.
         * Overlapping tail chunks rescan only bytes already known equal.
         */
        sym_define(&e->syms, e->secs, "__ace$mem_compare", 17, 0,
                   &e->secs[SEC_TEXT]);
        ins(e, 0xF100001Fu | (16u << 10) | (17u << 5));   // cmp x17, #16
        bcond_(e, C_LO, lt16);
        ins(e, 0xF100001Fu | (32u << 10) | (17u << 5));   // cmp x17, #32
        bcond_(e, C_HI, gt32);
        vmem(e, LDUR_Q, 24, 15, 0);                        // 16..32: head
        vmem(e, LDUR_Q, 25, 16, 0);
        ins(e, 0x6E201C00u | (25u << 16) | (24u << 5) | 24u); // eor v24, v24, v25
        ins(e, 0x6EA0A400u | (24u << 16) | (24u << 5) | 24u); // umaxp v24.4s
        ins(e, 0x9E660000u | (24u << 5) | 8u);                // fmov x8, d24
        cbz_(e, 1, 8, 1, res16);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u); // then the tail
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        ins(e, 0xD1000000u | (16u << 10) | (15u << 5) | 15u);
        ins(e, 0xD1000000u | (16u << 10) | (16u << 5) | 16u);
        vmem(e, LDUR_Q, 24, 15, 0);
        vmem(e, LDUR_Q, 25, 16, 0);
        ins(e, 0x6E201C00u | (25u << 16) | (24u << 5) | 24u);
        ins(e, 0x6EA0A400u | (24u << 16) | (24u << 5) | 24u);
        ins(e, 0x9E660000u | (24u << 5) | 8u);
        cbz_(e, 1, 8, 0, same);
        bind_label(e, res16);                              // [x15], [x16] differ in 16
        ld_reg(e, 8, 15, 0, 8, 0);
        ld_reg(e, 17, 16, 0, 8, 0);
        ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));    // cmp x8, x17
        bcond_(e, C_NE, diff);
        ld_reg(e, 8, 15, 8, 8, 0);
        ld_reg(e, 17, 16, 8, 8, 0);
        br_(e, diff);

        bind_label(e, gt32);                               // 33+: 32-byte blocks
        ins(e, 0xD1000000u | (32u << 10) | (17u << 5) | 17u); // sub x17, x17, #32
        align_code(e, 16);
        bind_label(e, loop);
        compare_block32(e, res32);
        ins(e, 0x91000000u | (32u << 10) | (15u << 5) | 15u); // add x15, x15, #32
        ins(e, 0x91000000u | (32u << 10) | (16u << 5) | 16u);
        ins(e, 0xF1000000u | (32u << 10) | (17u << 5) | 17u); // subs x17, x17, #32
        bcond_(e, C_HI, loop);
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u); // final block: end - 32
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        compare_block32(e, res32);
        br_(e, same);
        bind_label(e, res32);                              // [x15], [x16] differ in 32
        for (int w = 0; w < 3; w++) {
            ld_reg(e, 8, 15, (uint32_t)w * 8, 8, 0);
            ld_reg(e, 17, 16, (uint32_t)w * 8, 8, 0);
            ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));
            bcond_(e, C_NE, diff);
        }
        ld_reg(e, 8, 15, 24, 8, 0);
        ld_reg(e, 17, 16, 24, 8, 0);
        br_(e, diff);

        bind_label(e, lt16);                               // 8..15: two overlapping words
        ins(e, 0xF100001Fu | (8u << 10) | (17u << 5));    // cmp x17, #8
        bcond_(e, C_LO, lt8);
        ins(e, 0xD1000000u | (8u << 10) | (17u << 5) | 17u); // x17 = tail offset
        vmem(e, LDUR_D, 24, 15, 0);
        vmem(e, LDUR_D, 25, 16, 0);
        ins(e, 0xFC606800u | (17u << 16) | (15u << 5) | 26u); // ldr d26, [x15, x17]
        ins(e, 0xFC606800u | (17u << 16) | (16u << 5) | 27u); // ldr d27, [x16, x17]
        br_(e, words);
        bind_label(e, lt8);                                // 4..7: two overlapping halves
        ins(e, 0xF100001Fu | (4u << 10) | (17u << 5));    // cmp x17, #4
        bcond_(e, C_LO, lt4);
        ins(e, 0xD1000000u | (4u << 10) | (17u << 5) | 17u);
        vmem(e, LDUR_S, 24, 15, 0);
        vmem(e, LDUR_S, 25, 16, 0);
        ins(e, 0xBC606800u | (17u << 16) | (15u << 5) | 26u); // ldr s26, [x15, x17]
        ins(e, 0xBC606800u | (17u << 16) | (16u << 5) | 27u); // ldr s27, [x16, x17]
        br_(e, words);
        bind_label(e, lt4);                                // 1..3: first, middle, last
        cbz_(e, 1, 17, 0, same);
        ins(e, 0xD341FE28u);                               // lsr x8, x17, #1
        vmem(e, LDUR_B, 24, 15, 0);
        vmem(e, LDUR_B, 25, 16, 0);
        ins(e, 0x3C606800u | (8u << 16) | (15u << 5) | 28u); // ldr b28, [x15, x8]
        ins(e, 0x3C606800u | (8u << 16) | (16u << 5) | 29u); // ldr b29, [x16, x8]
        ins(e, 0x8B000000u | (17u << 16) | (15u << 5) | 15u);
        ins(e, 0x8B000000u | (17u << 16) | (16u << 5) | 16u);
        vmem(e, LDUR_B, 30, 15, -1);
        vmem(e, LDUR_B, 31, 16, -1);
        ins(e, 0x6E000400u | (3u << 16) | (28u << 5) | 24u); // mov v24.b[1], v28.b[0]
        ins(e, 0x6E000400u | (5u << 16) | (30u << 5) | 24u); // mov v24.b[2], v30.b[0]
        ins(e, 0x6E000400u | (3u << 16) | (29u << 5) | 25u); // mov v25.b[1], v29.b[0]
        ins(e, 0x6E000400u | (5u << 16) | (31u << 5) | 25u); // mov v25.b[2], v31.b[0]
        ins(e, 0x9E660000u | (24u << 5) | 8u);             // fmov x8, d24
        ins(e, 0x9E660000u | (25u << 5) | 17u);            // fmov x17, d25
        br_(e, last);
        bind_label(e, words);                              // head words, then tail words
        ins(e, 0x9E660000u | (24u << 5) | 8u);
        ins(e, 0x9E660000u | (25u << 5) | 17u);
        ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));
        bcond_(e, C_NE, diff);
        ins(e, 0x9E660000u | (26u << 5) | 8u);
        ins(e, 0x9E660000u | (27u << 5) | 17u);
        bind_label(e, last);
        ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));
        bcond_(e, C_NE, diff);
        bind_label(e, same);
        load_imm(e, 1, 17, 0);
        ins(e, 0xD65F03C0u);
        bind_label(e, diff);                               // x8 != x17 in memory order
        ins(e, 0xDAC00C00u | (8u << 5) | 8u);              // rev x8, x8
        ins(e, 0xDAC00C00u | (17u << 5) | 17u);            // rev x17, x17
        ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));
        load_imm(e, 1, 17, 1);
        ins(e, 0xDA800400u | (17u << 16) | ((uint32_t)C_HS << 12) |
               (17u << 5) | 17u);                          // cneg x17, x17, lo
        ins(e, 0xD65F03C0u);
    }
    patch_branches(e);
}

static void emit_allocator_helpers(Em* e) {
    if (!e->allocator_used) {
        if (e->mode == EMIT_CHECKED) {
            sym_define(&e->syms, e->secs, "__ace$registry_leaks", 20, 0,
                       &e->secs[SEC_TEXT]);
            ins(e, 0xD65F03C0); // no allocation intrinsic exists in this unit
        }
        return;
    }
    Sec* b = &e->secs[SEC_BSS];
    b->size = align_up(b->size, 8);
    ESym* head = sym_define(&e->syms, e->secs, "__ace$registry_head", 19, 0, b);
    b->size += 8;

    static const char message[] = "ace: allocator violation\n";
    Sec* c = &e->secs[SEC_CONST];
    ESym* msg = sym_define(&e->syms, e->secs, "__ace$allocator_message", 23, 0, c);
    buf_write(&c->data, message, sizeof message - 1);
    sym_define(&e->syms, e->secs, "__ace$allocator_fail", 20, 0,
               &e->secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD); ins(e, 0x910003FD);
    load_imm(e, 1, 0, 2); addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, sizeof message - 1);
    emit_bl_extern(e, "_write", 6); emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u);

    /* find(ptr,size) -> node; node layout is ptr,size,align,next. */
    sym_define(&e->syms, e->secs, "__ace$registry_find", 19, 0,
               &e->secs[SEC_TEXT]);
    mov_int(e, 1, 2, 0); mov_int(e, 1, 3, 1);
    addr_of_sym(e, 16, head); ld_reg(e, 0, 16, 0, 8, 0);
    int find_loop = new_label(e), find_next = new_label(e), find_ok = new_label(e);
    bind_label(e, find_loop);
    cbz_(e, 1, 0, 0, find_next);
    ld_reg(e, 16, 0, 0, 8, 0);
    ins(e, 0xEB00001Fu | (2u << 16) | (16u << 5));
    bcond_(e, C_NE, find_next);
    ld_reg(e, 16, 0, 8, 8, 0);
    ins(e, 0xEB00001Fu | (3u << 16) | (16u << 5));
    bcond_(e, C_EQ, find_ok);
    bind_label(e, find_next);
    cbz_(e, 1, 0, 0, find_ok); // null reaches failure below
    ld_reg(e, 0, 0, 24, 8, 0); br_(e, find_loop);
    bind_label(e, find_ok);
    int find_ret = new_label(e);
    cbz_(e, 1, 0, 1, find_ret);
    emit_bl_internal(e, "__ace$allocator_fail", 20);
    bind_label(e, find_ret); ins(e, 0xD65F03C0);

    sym_define(&e->syms, e->secs, "__ace$registry_add", 18, 0,
               &e->secs[SEC_TEXT]);
    helper_prologue(e);
    mov_int(e, 1, 19, 0); mov_int(e, 1, 20, 1); mov_int(e, 1, 21, 2);
    load_imm(e, 1, 0, 32); emit_bl_extern(e, "_malloc", 7);
    int add_ok = new_label(e); cbz_(e, 1, 0, 1, add_ok);
    emit_bl_internal(e, "__ace$allocator_fail", 20);
    bind_label(e, add_ok);
    st_reg(e, 19, 0, 0, 8); st_reg(e, 20, 0, 8, 8); st_reg(e, 21, 0, 16, 8);
    addr_of_sym(e, 16, head); ld_reg(e, 17, 16, 0, 8, 0);
    st_reg(e, 17, 0, 24, 8); st_reg(e, 0, 16, 0, 8);
    helper_epilogue(e);

    sym_define(&e->syms, e->secs, "__ace$registry_remove", 21, 0,
               &e->secs[SEC_TEXT]);
    helper_prologue(e);
    mov_int(e, 1, 19, 0); mov_int(e, 1, 20, 1); load_imm(e, 1, 21, 0);
    addr_of_sym(e, 16, head); ld_reg(e, 0, 16, 0, 8, 0);
    int rem_loop = new_label(e), rem_next = new_label(e), rem_found = new_label(e);
    bind_label(e, rem_loop);
    cbz_(e, 1, 0, 0, rem_next);
    ld_reg(e, 16, 0, 0, 8, 0);
    ins(e, 0xEB00001Fu | (19u << 16) | (16u << 5)); bcond_(e, C_NE, rem_next);
    ld_reg(e, 16, 0, 8, 8, 0);
    ins(e, 0xEB00001Fu | (20u << 16) | (16u << 5)); bcond_(e, C_EQ, rem_found);
    bind_label(e, rem_next);
    cbz_(e, 1, 0, 0, rem_found);
    mov_int(e, 1, 21, 0); ld_reg(e, 0, 0, 24, 8, 0); br_(e, rem_loop);
    bind_label(e, rem_found);
    int rem_present = new_label(e); cbz_(e, 1, 0, 1, rem_present);
    emit_bl_internal(e, "__ace$allocator_fail", 20);
    bind_label(e, rem_present);
    ld_reg(e, 17, 0, 24, 8, 0);
    int rem_nonhead = new_label(e), rem_unlinked = new_label(e);
    cbz_(e, 1, 21, 1, rem_nonhead);
    addr_of_sym(e, 16, head); st_reg(e, 17, 16, 0, 8); br_(e, rem_unlinked);
    bind_label(e, rem_nonhead); st_reg(e, 17, 21, 24, 8);
    bind_label(e, rem_unlinked); emit_bl_extern(e, "_free", 5);
    helper_epilogue(e);

    sym_define(&e->syms, e->secs, "__ace$registry_leaks", 20, 0,
               &e->secs[SEC_TEXT]);
    addr_of_sym(e, 16, head); ld_reg(e, 16, 16, 0, 8, 0);
    int no_leaks = new_label(e); cbz_(e, 1, 16, 0, no_leaks);
    emit_bl_internal(e, "__ace$allocator_fail", 20);
    bind_label(e, no_leaks); ins(e, 0xD65F03C0);
    patch_branches(e);
}

int emit_object(Node* file, Diag* d, const char* out_path,
                CompileReport* report, EmitMode mode) {
    static const Sec PROTO[NSECS] = {
        [SEC_TEXT]       = { .sect = "__text",       .seg = "__TEXT",
                             .flags = S_ATTR_PURE_INSTRUCTIONS |
                                      S_ATTR_SOME_INSTRUCTIONS, .align = 6 },
        [SEC_CONST]      = { .sect = "__const",      .seg = "__DATA_CONST",
                             .align = 4 },
        [SEC_DATA]       = { .sect = "__data",       .seg = "__DATA",
                             .align = 4 },
        [SEC_BSS]        = { .sect = "__bss",        .seg = "__DATA",
                             .flags = S_ZEROFILL, .align = 4 },
        [SEC_DEBUG_ABBREV] = { .sect = "__debug_abbrev", .seg = "__DWARF",
                               .flags = S_ATTR_DEBUG },
        [SEC_DEBUG_INFO] = { .sect = "__debug_info", .seg = "__DWARF",
                             .flags = S_ATTR_DEBUG },
        [SEC_DEBUG_LINE] = { .sect = "__debug_line", .seg = "__DWARF",
                             .flags = S_ATTR_DEBUG },
    };

    Em e;
    memset(&e, 0, sizeof e);
    e.vti_cap = MAX_ITEMPS;
    e.d = d;
    e.file = file;
    e.mode = mode;
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
                n->ival == BI_MEM_EQUAL || n->ival == BI_MEM_ALLOC_SIZE ||
                n->ival == BI_MEM_ALLOC_T || n->ival == BI_MEM_REALLOC_T ||
                n->ival == BI_MEM_RELEASE_T)
                break; // compiler-backed std/mem declarations have no body symbol
            if (n->ival == BI_FMT_PRINTF ||
                (n->ival >= BI_SYS_ARGC && n->ival <= BI_SYS_ENV))
                break; // compiler-backed calls lower directly; no fallback symbol
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
    emit_bounds_failure_helper(&e);
    emit_arithmetic_failure_helper(&e);
    emit_contract_failure_helper(&e);
    emit_null_failure_helper(&e);
    emit_allocator_helpers(&e);
    emit_byte_helpers(&e);
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
