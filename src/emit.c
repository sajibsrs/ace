// emit.c — stage four's object driver: compiles each function through the IR
// backend (lower.c, optimizer.c, arm64.c; SPEC.md Compiler #2) into one
// Mach-O arm64 object, and emits what no function body holds: global and
// constant data, static maps, vtables and their embed thunks, the runtime
// failure and allocator routines, and the shared byte routines. It also owns
// symbol mangling and the constant folding global initializers need.
//
// ABI (pinned against clang on this machine):
//   - platform convention (Apple arm64 PCS): scalar int/ptr args in x0-x7,
//     float args in v0-v7; <=16B aggregates as 1-2 register chunks (per
//     8-byte word: FLOAT iff every member overlapping the word is a float
//     entirely inside it, else INT); >16B via sret x8.
//   - Darwin quirk: STACK ARGS ARE PACKED AT NATURAL SIZE (two i32 stack
//     args share 8 bytes) — verified against clang caller+callee dumps. The
//     outgoing area sits at [sp, #0..] at the bl.
//   - Apple arm64 variadic extras use 8-byte stack slots from [sp, #0], while
//     the fixed parameters retain ordinary register/stack classification.
//   - internal-only freedom (spec Functions & ABI #3): `mut` params and >16B
//     read-only params pass the caller's ADDRESS in an x reg.
//   - extern calls are `bl` + ARM64_RELOC_BRANCH26 to an undefined `_name`
//     symbol.
//   - symbols: `_main` / `_export` at the C boundary; internals mangled
//     `__ace$fn$name[$T...]`, globals `__ace$g$name`, vtables `__ace$vt$name`,
//     string bytes `__ace$str$N` (mangling is compiler-private, C Interop #1).
//   - multi-return = anonymous struct (tuple) per platform rules: <=16B in
//     chunk registers, >16B sret.
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
//   fmov s/d #imm8 = 0x1E201000/0x1E601000 | imm8 << 13
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
#include "backend.h"
#include "sema.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int emit_has_main = 0;
const char* ace_target = "darwin-arm64";

// --- emitter state ----------------------------------------------------------------

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

typedef struct {
    Obj   o;
    Node* file;
    GVar*  globs;
    int    nglobs, cglobs;
    StaticMapData* maps;
    int    nmaps, cmaps;
    uint32_t* lab;
    int      nlab, clab;
    struct { uint32_t site; int lab; uint8_t kind; } *bp; // 0=b 1=bcond 2=cbz/cbnz
    int      nbp, cbp;
} Em;

static void em_err(Em* e, Node* n, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", n && n->path ? n->path : e->o.d->path, n ? n->line : 0, n ? n->col : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    e->o.d->errors++;
}

static Buf* text(Em* e) { return obj_text(&e->o); }

static void fix_add(Em* e, uint8_t kind, uint32_t site, ESym* target) {
    obj_fixup(&e->o, kind, site, target);
}

static void ins(Em* e, uint32_t w) { buf_put32(text(e), w); }

// Pad with nops to a code boundary (FUNC_ALIGN, LOOP_ALIGN in backend.h).
static void align_code(Em* e, uint32_t bytes) {
    while (text(e)->len % bytes) ins(e, 0xD503201Fu);
}

static const char* arena_copy(const char* s, size_t n) {
    char* p = arena_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
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

static void mov_int(Em* e, int sf, int dst, int src);
static void emit_bl_internal(Em* e, const char* p, uint32_t len);
static int eval_const(Em* e, Node* v, SType* t, Buf* out, ConstRels* rels);

// --- type helpers -----------------------------------------------------------------

static SType* unwrap(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    return t;
}

static uint32_t ty_size(SType* t) { return sema_size(t); }
static uint32_t ty_align(SType* t) { return sema_align(t); }

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

uint32_t field_off(SType* t, int want) {
    int special = t->kind == SK_STRUCT && (t->is_packed || t->requested_align);
    if (t->kind == SK_STRUCT && !special)
        for (int i = 0; i < t->nfields; i++)
            if (t->fields[i].requested_align || t->fields[i].has_offset) {
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

int find_field(SType* t, const char* name, uint32_t len) {
    for (int i = 0; i < t->nfields; i++)
        if ((uint32_t)strlen(t->fields[i].name) == len &&
            memcmp(t->fields[i].name, name, len) == 0)
            return i;
    return -1;
}

static uint32_t align_up(uint32_t v, uint32_t a);

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

// imm8 of `fmov Vd, #imm` when the bit pattern is one of its 256 values (the
// sign, three exponent bits, and four fraction bits of VFPExpandImm), else -1.
int fmov_imm8(uint64_t bits, int is64) {
    int fbits = is64 ? 52 : 23, ebits = is64 ? 11 : 8, drop = fbits - 4;
    if (bits & ((UINT64_C(1) << drop) - 1)) return -1; // fraction beyond four bits
    uint32_t exp = (uint32_t)(bits >> fbits) & ((1u << ebits) - 1);
    uint32_t b = (~exp >> (ebits - 1)) & 1; // top exponent bit is the inverse of the next
    uint32_t mid = ((exp >> 2) & ((1u << (ebits - 3)) - 1)); // ebits-3 copies of b
    if (mid != (b ? (1u << (ebits - 3)) - 1 : 0)) return -1;
    return (int)(((bits >> (fbits + ebits)) & 1) << 7 | b << 6 | (exp & 3) << 4 |
                 ((bits >> drop) & 15));
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
    ins(e, ((nz ? 0x35000000u : 0x34000000u) | ((uint32_t)!!sf << 31)) | ((uint32_t)rt & 31u));
    bp_add(e, 2, lab);
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
        if (e->bp[i].kind == 0) cur |= (uint32_t)(delta >> 2) & 0x03FFFFFFu;
        else cur |= ((uint32_t)(delta >> 2) & 0x7FFFFu) << 5;
        buf_patch32(t, site, cur);
    }
    e->nbp = 0;
}

static uint32_t align_up(uint32_t v, uint32_t a) {
    return a ? (v + a - 1) / a * a : v;
}

// --- memory access ---------------------------------------------------------------------

static void ld_reg(Em* e, int rt, int base, uint32_t off, uint32_t w, int signext) {
    uint32_t b;
    if (!signext) b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u : w == 4 ? 0xB9400000u : 0xF9400000u;
    else b = w == 1 ? 0x39C00000u : w == 2 ? 0x79C00000u : w == 4 ? 0xB9400000u : 0xF8400000u;
    if (off % w == 0 && off / w < 4096) ins(e, b | ((off / w) << 10) | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
    else {
        load_imm(e, 1, 16, off);
        uint32_t rb;
        if (!signext) rb = w == 1 ? 0x38606800u : w == 2 ? 0x78606800u : w == 4 ? 0xB8606800u : 0xF8606800u;
        else rb = w == 1 ? 0x38E06800u : w == 2 ? 0x78E06800u : w == 4 ? 0xB8606800u : 0xF8606800u;
        ins(e, rb | (16u << 16) | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
    }
}

static void st_reg(Em* e, int rt, int base, uint32_t off, uint32_t w) {
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u : w == 4 ? 0xB9000000u : 0xF9000000u;
    if (off % w == 0 && off / w < 4096) ins(e, b | ((off / w) << 10) | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
    else {
        load_imm(e, 1, 16, off);
        uint32_t rb = w == 1 ? 0x38206800u : w == 2 ? 0x78206800u : w == 4 ? 0xB8206800u : 0xF8206800u;
        ins(e, rb | (16u << 16) | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
    }
}

// --- scalar moves ---------------------------------------------------------------------

static void mov_int(Em* e, int sf, int dst, int src) {
    if (dst == src) return;
    ins(e, (sf ? 0xAA0003E0u : 0x2A0003E0u) | ((uint32_t)src << 16) | ((uint32_t)dst & 31u));
}

// --- division by a constant -----------------------------------------------------------

/*
 * Division by a constant (Granlund-Montgomery, with libdivide's generator):
 * a divisor known at compile time becomes a multiply-high by a reciprocal
 * "magic" number plus shifts, and powers of two become shifts and masks.
 * Values narrower than 32 bits live extended in 32-bit registers, so every
 * integer type divides at width 32 or 64.
 */

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
DivMagic div_magic(uint64_t d, int w, int sign, int neg) {
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

// AArch64 bitmask immediate: a run of ones rotated within a power-of-two
// element that is replicated across the register.
uint32_t logical_imm_field(uint64_t v, int sf) {
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
        uint32_t imms = ((~(uint32_t)(size - 1) << 1) & 0x3F) | (uint32_t)(ones - 1);
        return (n << 12) | ((uint32_t)r << 6) | imms;
    }
    return 0;
}

// --- tagged unions -----------------------------------------------------------------

// The union type a construction/pattern base names, or NULL (sema annotated).
static SType* union_type_of_base(Node* base) {
    if (!base) return NULL;
    if (base->kind == N_IDENT) {
        Sym* ts = node_sym(base);
        return ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_UNION ? ts->type : NULL;
    }
    if (base->kind == N_INDEX) {
        SType* t = node_ty(base);
        return t && t->kind == SK_UNION ? t : NULL;
    }
    return NULL;
}

// --- branches with link ------------------------------------------------------------------

static void emit_bl_internal(Em* e, const char* p, uint32_t len) {
    ins(e, 0x94000000u); // bl .+0
    ESym* t = sym_intern(&e->o.syms, p, len);
    fix_add(e, FK_BRANCH, (uint32_t)(text(e)->len - 4), t);
}

static void emit_bl_extern(Em* e, const char* p, uint32_t len) {
    ins(e, 0x94000000u);
    ESym* t = sym_intern(&e->o.syms, p, len);
    reloc_add(&e->o.secs[SEC_TEXT], (uint32_t)(text(e)->len - 4), t, 1, 2, 1, RM_BRANCH26);
}

static void addr_of_sym(Em* e, int rd, ESym* sym);

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
const char* fn_mangle(const char* name, uint32_t len, SType* ft, Node* decl, SType** targs, int nt, int spec) {
    Buf b = {0};
    buf_write(&b, "__ace$fn$", strlen("__ace$fn$"));
    buf_write(&b, name, len);
    buf_write(&b, "$p", 2);
    if (ft && ft->kind == SK_FUNC) {
        for (int i = 0; i < ft->nparams; i++) {
            buf_put8(&b, decl && i < decl->nkids && (decl->kids[i]->flags & F_MUT) ? 'm' : 'r');
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

// --- string literals --------------------------------------------------------------------

ESym* obj_string(Obj* o, const char* p, uint32_t len) {
    Sec* c = &o->secs[SEC_CONST];
    while (c->data.len % 8) buf_put8(&c->data, 0);
    char nm[64];
    snprintf(nm, sizeof nm, "__ace$str$%d", o->str_n++);
    ESym* sym = sym_define(&o->syms, o->secs, nm, (uint32_t)strlen(nm), 0, c);
    buf_write(&c->data, p, len);
    return sym;
}

ESym* obj_located(Obj* o, Node* at, const char* what, uint32_t* len) {
    const char* path = at && at->path ? at->path : "<source>";
    size_t need = strlen(path) + strlen(what) + 64;
    char* diagnostic = (char*)arena_alloc(need);
    int n = snprintf(diagnostic, need, "%s:%d:%d: %s\n", path, at ? at->line : 0, at ? at->col : 0, what);
    *len = (uint32_t)n;
    return obj_string(o, diagnostic, (uint32_t)n);
}

static ESym* intern_string(Em* e, Node* s) { return obj_string(&e->o, s->str, (uint32_t)s->len); }

// --- symbol addresses ---------------------------------------------------------------------

// adrp rd, sym@PAGE ; add rd, rd, sym@PAGEOFF
static void addr_of_sym(Em* e, int rd, ESym* sym) {
    uint32_t site = (uint32_t)text(e)->len;
    ins(e, 0x90000000u | ((uint32_t)rd & 31u));
    reloc_add(&e->o.secs[SEC_TEXT], site, sym, 1, 2, 1, RM_PAGE21);
    site = (uint32_t)text(e)->len;
    ins(e, 0x91000000u | ((uint32_t)rd << 5) | ((uint32_t)rd & 31u));
    reloc_add(&e->o.secs[SEC_TEXT], site, sym, 0, 2, 1, RM_PAGEOFF12);
}

// `larger[int]` as a value: the address of the written specialization or of
// the stamp whose signature sema recorded on the node
const char* fn_value_mangle(Sym* s, Node* x) {
    int nt = s->spec_args ? s->spec_nargs : s->decl->c ? s->decl->c->nkids : 0;
    SType** targs = s->spec_args;
    SType* ft = s->type;
    if (!s->spec_args) {
        STypeInst* inst = sema_insts(s->decl);
        while (inst && inst->result != node_ty(x)) inst = inst->next;
        if (!inst) return NULL;
        targs = inst->args;
        ft = inst->result;
    }
    return fn_mangle(s->name, s->len, ft, s->decl, targs, nt, s->spec_args != NULL);
}

const char* fn_call_mangle(Sym* s, Node* call) {
    Node* callee = call->a;
    SType* cft = node_ty(callee);
    if (!cft || cft->kind != SK_FUNC) cft = s->type;
    SType** targs = NULL;
    int ntargs = 0;
    if (s->is_generic && (call->flags & F_ASSOC)) {
        // no receiver parameter to read args back from; the owner
        // expression's resolved type carries them instead
        SType* owner = callee->kind == N_FIELD_E ? node_ty(callee->a) : NULL;
        if (owner) { targs = owner->args; ntargs = owner->nargs; }
    } else if (s->is_generic && cft && cft->nparams > 0) {
        SType* recv = cft->params[0];
        targs = recv->args;
        ntargs = recv->nargs;
    } else if (s->spec_args) {
        targs = s->spec_args;
        ntargs = s->spec_nargs;
    }
    return fn_mangle(s->name, s->len, cft, s->decl, targs, ntargs, s->spec_args != NULL);
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
    return t->kind == SK_PRIM && ((t->prim >= P_I8 && t->prim <= P_U64) || t->prim == P_USIZE);
}

static int fold_float_target(SType* t) {
    t = unwrap(t);
    if (!t) return 0;
    if (t->kind == SK_DISTINCT) return fold_float_target(t->base);
    return t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64);
}

/*
 * Constant arithmetic for global initializers. An integer expression folds at
 * its destination's width and signedness (a null type stands for the untyped
 * i64 that literals carry); a float expression folds at its destination's
 * precision, rounding f32 after every operation as the runtime instruction
 * does. Overflow, division or remainder by zero, and an out-of-range shift
 * count are reported here, matching Safety & Debug Builds #4 and #6. Each
 * folder returns 1 with a value, 0 when the shape is not foldable (the caller
 * reports the generic diagnostic), or -1 after reporting its own.
 */
typedef __int128 Wide;
typedef unsigned __int128 UWide;

static int fold_int_prim(SType* t) {
    t = unwrap(t);
    return t && t->kind == SK_PRIM &&
        ((t->prim >= P_I8 && t->prim <= P_U64) || t->prim == P_USIZE);
}

static int fold_float_prim(SType* t) {
    t = unwrap(t);
    return t && t->kind == SK_PRIM && (t->prim == P_F32 || t->prim == P_F64);
}

static unsigned fold_bits(SType* t) { return t ? ty_size(t) * 8 : 64; }

static int fold_signed(SType* t) { return t ? ty_is_signed(t) : 1; }

static int fold_fits(SType* t, Wide v) {
    unsigned bits = fold_bits(t);
    if (fold_signed(t)) {
        Wide m = (Wide)1 << (bits - 1);
        return v >= -m && v < m;
    }
    return v >= 0 && v < ((Wide)1 << bits);
}

static Wide fold_trunc(SType* t, Wide v) {
    unsigned bits = fold_bits(t);
    UWide u = (UWide)v & (((UWide)1 << bits) - 1);
    if (fold_signed(t) && (u >> (bits - 1)) & 1) return (Wide)u - ((Wide)1 << bits);
    return (Wide)u;
}

static int fold_float(Em* e, Node* v, SType* t, double* out);

static int fold_int(Em* e, Node* v, SType* t, Wide* out) {
    Wide a, b;
    int r;
    switch (v->kind) {
    case N_INT: case N_CHAR:
        *out = v->ival;
        return fold_fits(t, *out);
    case N_IDENT: {
        Sym* s = node_sym(v);
        SType* vt = unwrap(node_ty(v));
        if (!s || !s->is_global || !s->is_const || !s->init || !vt || vt != unwrap(t)) return 0;
        return fold_int(e, s->init, t, out);
    }
    case N_UNARY:
        if (v->op != T_MINUS && v->op != T_TILDE) return 0;
        if ((r = fold_int(e, v->a, t, &a)) != 1) return r;
        if (v->op == T_TILDE) {
            *out = fold_trunc(t, ~a);
            return 1;
        }
        *out = -a;
        if (!fold_fits(t, *out)) {
            em_err(e, v, "constant integer overflow");
            return -1;
        }
        return 1;
    case N_BINARY: {
        SType* ct = t;
        if (v->op == T_SHL || v->op == T_SHR) {
            SType* bt = unwrap(node_ty(v->b));
            if (fold_int_prim(bt)) ct = bt;
        }
        if ((r = fold_int(e, v->a, t, &a)) != 1) return r;
        if ((r = fold_int(e, v->b, ct, &b)) != 1) return r;
        switch (v->op) {
        case T_PLUS: *out = a + b; break;
        case T_MINUS: *out = a - b; break;
        case T_STAR: *out = a * b; break;
        case T_SLASH: case T_PERCENT:
            if (b == 0) {
                em_err(e, v, "constant division by zero");
                return -1;
            }
            if (fold_signed(t) && b == -1 && a == -((Wide)1 << (fold_bits(t) - 1))) {
                em_err(e, v, "constant integer overflow");
                return -1;
            }
            *out = v->op == T_SLASH ? a / b : a % b;
            return 1;
        case T_AMP: *out = a & b; return 1;
        case T_PIPE: *out = a | b; return 1;
        case T_CARET: *out = a ^ b; return 1;
        case T_SHL: case T_SHR:
            if (b < 0 || b >= (Wide)fold_bits(t)) {
                em_err(e, v, "constant shift count out of range");
                return -1;
            }
            *out = v->op == T_SHL ? fold_trunc(t, (Wide)((UWide)a << (int)b)) : a >> (int)b;
            return 1;
        default:
            return 0;
        }
        if (!fold_fits(t, *out)) {
            em_err(e, v, "constant integer overflow");
            return -1;
        }
        return 1;
    }
    case N_CALL: {
        if (v->sym || v->nkids != 1 || !v->a || v->a->kind != N_IDENT) return 0;
        SType* want = unwrap(node_ty(v->a));
        if (!fold_int_prim(want) || want != unwrap(t)) return 0;
        SType* at = unwrap(node_ty(v->kids[0]));
        if (!fold_int_prim(at)) at = want;
        if ((r = fold_int(e, v->kids[0], at, &a)) != 1) return r;
        *out = fold_trunc(want, a);
        return 1;
    }
    default:
        return 0;
    }
}

static double fold_round(SType* t, double d) {
    return unwrap(t)->prim == P_F32 ? (double)(float)d : d;
}

static int fold_float(Em* e, Node* v, SType* t, double* out) {
    double a, b;
    int r;
    switch (v->kind) {
    case N_FLOAT:
        *out = fold_round(t, v->fval);
        return 1;
    case N_IDENT: {
        Sym* s = node_sym(v);
        SType* vt = unwrap(node_ty(v));
        if (!s || !s->is_global || !s->is_const || !s->init || !vt || vt != unwrap(t)) return 0;
        return fold_float(e, s->init, t, out);
    }
    case N_UNARY:
        if (v->op != T_MINUS) return 0;
        if ((r = fold_float(e, v->a, t, &a)) != 1) return r;
        *out = -a;
        return 1;
    case N_BINARY:
        if ((r = fold_float(e, v->a, t, &a)) != 1) return r;
        if ((r = fold_float(e, v->b, t, &b)) != 1) return r;
        switch (v->op) {
        case T_PLUS: *out = fold_round(t, a + b); break;
        case T_MINUS: *out = fold_round(t, a - b); break;
        case T_STAR: *out = fold_round(t, a * b); break;
        case T_SLASH: *out = fold_round(t, a / b); break;
        default: return 0;
        }
        // a NaN's sign bit is host-dependent, so only finite results fold
        return isfinite(*out) ? 1 : 0;
    case N_CALL: {
        if (v->sym || v->nkids != 1 || !v->a || v->a->kind != N_IDENT) return 0;
        SType* want = unwrap(node_ty(v->a));
        if (!fold_float_prim(want) || want != unwrap(t)) return 0;
        Node* arg = v->kids[0];
        SType* at = unwrap(node_ty(arg));
        if (fold_float_prim(at)) {
            if ((r = fold_float(e, arg, at, &a)) != 1) return r;
            *out = fold_round(want, a);
            return 1;
        }
        Wide w;
        if (at && at->kind == SK_UT) at = NULL;
        if (at && !fold_int_prim(at)) return 0;
        if ((r = fold_int(e, arg, at, &w)) != 1) return r;
        *out = want->prim == P_F32 ? (double)(float)w : (double)w;
        return 1;
    }
    default:
        return 0;
    }
}

// Arithmetic or a cast of one over a primitive destination, written as its bytes.
static int eval_fold(Em* e, Node* v, SType* t, Buf* out) {
    if (fold_int_prim(t)) {
        Wide x;
        if (fold_int(e, v, t, &x) != 1) return 0;
        put_u(out, (uint64_t)x, ty_size(t));
        return 1;
    }
    if (fold_float_prim(t)) {
        double d;
        if (fold_float(e, v, t, &d) != 1) return 0;
        if (t->prim == P_F64) {
            put_u(out, 0, 8);
            memcpy(out->d + out->len - 8, &d, 8);
        } else {
            float f = (float)d;
            uint32_t bits = 0;
            memcpy(&bits, &f, 4);
            put_u(out, bits, 4);
        }
        return 1;
    }
    return 0;
}

static int eval_const(Em* e, Node* v, SType* t, Buf* out, ConstRels* rels) {
    t = unwrap(t);
    if (!t) return 0;
    switch (v->kind) {
    case N_CALL: {
        // union variant construction with a compile-time payload
        Node* callee = v->a;
        SType* ut = callee && callee->kind == N_FIELD_E && !v->sym ? union_type_of_base(callee->a) : NULL;
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
        if (v->a && v->a->kind == N_IDENT && v->a->len == 6 && memcmp(v->a->str, "typeid", 6) == 0 && v->ival) {
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
            int arg_float = arg->kind == N_FLOAT ||
                (arg->kind == N_UNARY && arg->op == T_MINUS && arg->a->kind == N_FLOAT);
            if ((arg_int && fold_int_target(want)) ||
                (arg_float && fold_float_target(want)))
                return eval_const(e, arg, want, out, rels);
        }
        return eval_fold(e, v, t, out);
    }
    case N_BINARY:
        return eval_fold(e, v, t, out);
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
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ERROR_DOMAIN) {
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
        if (s && s->is_global && s->is_const && s->init) return eval_const(e, s->init, t, out, rels);
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
        if (v->op == T_MINUS && v->a->kind == N_FLOAT && fold_float_target(t)) {
            Node literal = *v->a; // a negated float literal folds as its negated value
            literal.fval = -literal.fval;
            return eval_const(e, &literal, t, out, rels);
        }
        return eval_fold(e, v, t, out);
    case N_ARRAYLIT: case N_STRUCTLIT: {
        if (v->kind == N_STRUCTLIT && t->kind == SK_BITS) {
            uint64_t raw = 0;
            for (int i = 0; i < v->nkids; i++) {
                int fi = find_field(t, v->kids[i]->str, v->kids[i]->len);
                uint64_t value;
                if (fi < 0 || !const_int(v->kids[i]->a, &value)) return 0;
                SField* f = &t->fields[fi];
                uint64_t low = f->bit_width == 64 ? UINT64_MAX : (UINT64_C(1) << f->bit_width) - 1;
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
        if (s && s->is_global && s->is_const && s->init) return const_string_node(s->init);
    }
    if (n->kind == N_CALL && !n->sym && n->nkids == 1) return const_string_node(n->kids[0]);
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

// A StaticMap's perfect hash, built once per map and shared by lowering (which
// embeds seed and mask) and the data emitter (which writes the slot table).
typedef struct { Sym* s; uint64_t seed; uint32_t size; uint32_t* slots; uint8_t ok; } MapPlan;
static MapPlan* map_plans;
static int nmap_plans, cmap_plans;

static MapPlan* map_plan(Sym* vs) {
    for (int i = 0; i < nmap_plans; i++) if (map_plans[i].s == vs) return &map_plans[i];
    Node* lit = vs->init;
    SType* t = vs->type;
    int count = lit ? lit->nkids : 0;
    SType* rawkey = unwrap(t->base);
    int strings = rawkey && rawkey->kind == SK_PRIM && rawkey->prim == P_STRING;
    uint32_t size = 1;
    while (size < (uint32_t)(count ? count * 2 : 1)) size <<= 1;
    uint64_t seed = 0;
    uint32_t* slots = NULL;
    int ok = 0;
    for (;;) {
        slots = calloc(size, sizeof(uint32_t));
        if (!slots) oom();
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
            if (!collision) { ok = 1; break; }
        }
        if (ok) break;
        free(slots);
        slots = NULL;
        if (size >= (1u << 30)) break;
        size <<= 1;
    }
    if (nmap_plans == cmap_plans) {
        cmap_plans = cmap_plans ? cmap_plans * 2 : 8;
        map_plans = realloc(map_plans, (size_t)cmap_plans * sizeof(*map_plans));
        if (!map_plans) oom();
    }
    map_plans[nmap_plans] = (MapPlan){ vs, seed, size, slots, (uint8_t)ok };
    return &map_plans[nmap_plans++];
}

int staticmap_hash(Sym* vs, uint64_t* seed, uint32_t* mask) {
    MapPlan* p = map_plan(vs);
    if (!p->ok) return 0;
    *seed = p->seed;
    *mask = p->size - 1;
    return 1;
}

static void emit_staticmap_global(Em* e, Node* n, Sym* vs, SType* t) {
    Node* lit = n->b;
    int count = lit ? lit->nkids : 0;
    SType* rawkey = unwrap(t->base);
    int strings = rawkey && rawkey->kind == SK_PRIM && rawkey->prim == P_STRING;
    MapPlan* plan = map_plan(vs);
    if (!plan->ok) {
        em_err(e, n, "could not construct StaticMap perfect hash");
        return;
    }
    uint64_t seed = plan->seed;
    uint32_t size = plan->size;
    uint32_t* slots = plan->slots;

    Sec* d = &e->o.secs[SEC_CONST];
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
    for (uint32_t i = 0; i < ty_size(t->ret); i++) buf_put8(&value_bytes, 0); // the miss entry, at index count

    while (d->data.len % ka) buf_put8(&d->data, 0);
    snprintf(name, sizeof name, "__ace$map$%.*s$keys", (int)vs->len, vs->name);
    ESym* keys = sym_define(&e->o.syms, e->o.secs, name, (uint32_t)strlen(name), 0, d);
    uint32_t key_start = (uint32_t)d->data.len;
    buf_write(&d->data, key_bytes.d, key_bytes.len);
    for (int i = 0; i < key_rels.n; i++)
        reloc_add(d, key_start + key_rels.v[i].off, key_rels.v[i].sym,
                  0, 3, 1, RM_UNSIGNED);

    while (d->data.len % va) buf_put8(&d->data, 0);
    snprintf(name, sizeof name, "__ace$map$%.*s$values", (int)vs->len, vs->name);
    ESym* values = sym_define(&e->o.syms, e->o.secs, name, (uint32_t)strlen(name), 0, d);
    uint32_t value_start = (uint32_t)d->data.len;
    buf_write(&d->data, value_bytes.d, value_bytes.len);
    for (int i = 0; i < value_rels.n; i++)
        reloc_add(d, value_start + value_rels.v[i].off, value_rels.v[i].sym,
                  0, 3, 1, RM_UNSIGNED);
    free(key_bytes.d); free(value_bytes.d);
    free(key_rels.v); free(value_rels.v);

    while (d->data.len % 4) buf_put8(&d->data, 0);
    snprintf(name, sizeof name, "__ace$map$%.*s$slots", (int)vs->len, vs->name);
    ESym* slot_sym = sym_define(&e->o.syms, e->o.secs, name, (uint32_t)strlen(name), 0, d);
    for (uint32_t i = 0; i < size; i++) buf_put32(&d->data, slots[i]);
    free(slots);
    plan->slots = NULL;

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
    e->globs[e->nglobs++] = (GVar){ s, sym->name, (uint32_t)strlen(sym->name) };
}

static int emit_error_handle_global(Em* e, Node* n, Sym* vs, const char* nm) {
    Node* call = n->b;
    uint64_t code = 0;
    if (call && call->kind == N_NULL) {
        Sec* d = (n->flags & F_CONST) ? &e->o.secs[SEC_CONST] : &e->o.secs[SEC_DATA];
        while (d->data.len % 8) buf_put8(&d->data, 0);
        ESym* g = sym_define(&e->o.syms, e->o.secs, nm, (uint32_t)strlen(nm), 0, d);
        buf_put64(&d->data, 0);
        add_global(e, vs, g);
        return 1;
    }
    if (call && call->kind == N_FIELD_E && node_ty(call) && node_ty(call)->kind == SK_ERROR) {
        Sec* d = (n->flags & F_CONST) ? &e->o.secs[SEC_CONST] : &e->o.secs[SEC_DATA];
        while (d->data.len % 8) buf_put8(&d->data, 0);
        ESym* g = sym_define(&e->o.syms, e->o.secs, nm, (uint32_t)strlen(nm), 0, d);
        buf_put64(&d->data, call->ival);
        add_global(e, vs, g);
        return 1;
    }
    if (!call || call->kind != N_CALL || call->nkids > 1 || (call->nkids == 1 && !const_int(call->kids[0], &code))) {
        em_err(e, n, "global Error initializer must use a domain with a constant i32 code");
        return 0;
    }
    Sec* d = (n->flags & F_CONST) ? &e->o.secs[SEC_CONST] : &e->o.secs[SEC_DATA];
    while (d->data.len % 8) buf_put8(&d->data, 0);
    ESym* g = sym_define(&e->o.syms, e->o.secs, nm, (uint32_t)strlen(nm), 0, d);
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
        Sec* b = &e->o.secs[SEC_BSS];
        b->size = align_up(b->size, align);
        ESym* s = sym_define(&e->o.syms, e->o.secs, nm, (uint32_t)strlen(nm), 0, b);
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
    int errors_before = e->o.d->errors;
    if (!eval_const(e, n->b, t, &bytes, &rels)) {
        // a fold error is already reported; otherwise the shape is not constant
        if (e->o.d->errors != errors_before) {}
        else if (t->kind == SK_UNION) em_err(e, n, "global union initializer must be a compile-time variant value");
        else em_err(e, n, "global initializer must be a compile-time constant");
        free(bytes.d);
        free(rels.v);
        return;
    }
    Sec* d = (n->flags & F_CONST) ? &e->o.secs[SEC_CONST] : &e->o.secs[SEC_DATA];
    while (d->data.len % align) buf_put8(&d->data, 0);
    ESym* g = sym_define(&e->o.syms, e->o.secs, nm, (uint32_t)strlen(nm), 0, d);
    uint32_t start = (uint32_t)d->data.len;
    buf_write(&d->data, bytes.d, bytes.len);
    for (int i = 0; i < rels.n; i++) reloc_add(d, start + rels.v[i].off, rels.v[i].sym, 0, 3, 1, RM_UNSIGNED);
    free(bytes.d);
    free(rels.v);
    add_global(e, vs, g);
}

// --- vtables ---------------------------------------------------------------------------

const char* vtable_name(SType* conc, SType* contract) {
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

static void obj_err(Obj* o, Node* n, const char* msg) {
    fprintf(stderr, "%s:%d:%d: error: %s\n", n && n->path ? n->path : o->d->path, n ? n->line : 0, n ? n->col : 0, msg);
    o->d->errors++;
}

// The thunk slot of vtable vtname calls through: target with the receiver
// advanced to the embedded field at off.
static ESym* obj_embed_thunk(Obj* o, const char* vtname, int slot, const char* target, uint32_t off) {
    char name[1024];
    snprintf(name, sizeof name, "%s$embed$%d", vtname, slot);
    ESym* prior = sym_intern(&o->syms, name, (uint32_t)strlen(name));
    if (prior->type) return prior;
    Buf* t = obj_text(o);
    ESym* out = sym_define(&o->syms, o->secs, name, (uint32_t)strlen(name), 0, &o->secs[SEC_TEXT]);
    if (strcmp(ace_target, "linux-x64") == 0) { // add rdi, field-offset; jmp target
        buf_put8(t, 0x48); buf_put8(t, 0x81); buf_put8(t, 0xC7); buf_put32(t, off);
        buf_put8(t, 0xE9);
        reloc_add(&o->secs[SEC_TEXT], (uint32_t)t->len, sym_intern(&o->syms, target, (uint32_t)strlen(target)), 1, 2, 1, 4);
        buf_put32(t, (uint32_t)-4); // R_X86_64_PLT32 addend
        return out;
    }
    buf_put32(t, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    if (off <= 4095) {
        buf_put32(t, 0x91000000u | (off << 10)); // add x0, x0, #field-offset
    } else {
        buf_put32(t, 0xD2800010u | ((off & 0xFFFF) << 5)); // movz x16, #lo
        if (off >> 16) buf_put32(t, 0xF2A00010u | ((off >> 16) << 5)); // movk x16, #hi, lsl #16
        buf_put32(t, 0x8B000000u | (16u << 16)); // add x0, x0, x16
    }
    obj_fixup(o, FK_BRANCH, (uint32_t)t->len, sym_intern(&o->syms, target, (uint32_t)strlen(target)));
    buf_put32(t, 0x94000000u); // bl target
    buf_put32(t, 0xA8C17BFD); // ldp x29, x30, [sp], #16
    buf_put32(t, 0xD65F03C0); // ret
    return out;
}

// conc's vtable for contract under conformance conf, defined once per pair:
// its method addresses in contract order in __const, and any embed thunk it
// needs in __text at the cursor, so never inside a function's code.
ESym* obj_vtable(Obj* o, SType* conc, SType* contract, Node* conf) {
    if (!conc || !contract || !conf) {
        obj_err(o, conf, "invalid contract conformance in codegen");
        return sym_intern(&o->syms, "__ace$vt$invalid", 16);
    }
    const char* nm = vtable_name(conc, contract);
    ESym* prior = sym_intern(&o->syms, nm, (uint32_t)strlen(nm));
    if (prior->type) return prior;
    Sec* c = &o->secs[SEC_CONST];
    while (c->data.len % 8) buf_put8(&c->data, 0);
    ESym* out = sym_define(&o->syms, o->secs, nm, (uint32_t)strlen(nm), 0, c);
    for (int i = 0; i < contract->decl->nkids; i++) { // contract method order
        uint32_t off = (uint32_t)c->data.len;
        Node* ref = i < conf->nkids ? conf->kids[i] : NULL;
        Sym* fsym = ref ? node_sym(ref) : NULL;
        if (!fsym) {
            Node* m = contract->decl->kids[i];
            char msg[256];
            snprintf(msg, sizeof msg, "signing misses method '%.*s' (sema gap)", (int)m->len, m->str);
            obj_err(o, conf, msg);
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
        const char* fnm = fn_mangle(fsym->name, fsym->len, ft, fsym->decl, args, nargs, 0);
        ESym* fs;
        if (ref->a) {
            fs = obj_embed_thunk(o, nm, i, fnm, field_off(conc, embed_fi));
        } else {
            fs = sym_intern(&o->syms, fnm, (uint32_t)strlen(fnm));
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
        obj_vtable(&e->o, conc, contract, signing->kids[i]);
    }
}

// --- entry ---------------------------------------------------------------------------

static void emit_bounds_failure_helper(Em* e) {
    if (!e->o.bounds_fail_used) return;
    sym_define(&e->o.syms, e->o.secs, "__ace$bounds_fail", 17, 0, &e->o.secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    ins(e, 0x910003FD); // mov x29, sp
    emit_bl_extern(e, "_write", 6);
    emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u); // brk #0 if abort unexpectedly returns
}

static void emit_arithmetic_failure_helper(Em* e) {
    if (!e->o.arithmetic_fail_used) return;
    static const char message[] = "ace: arithmetic violation\n";
    Sec* c = &e->o.secs[SEC_CONST];
    ESym* msg = sym_define(&e->o.syms, e->o.secs, "__ace$arithmetic_message", 24, 0, c);
    buf_write(&c->data, message, sizeof message - 1);

    sym_define(&e->o.syms, e->o.secs, "__ace$arithmetic_fail", 21, 0, &e->o.secs[SEC_TEXT]);
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
    if (!e->o.contract_fail_used) return;
    static const char message[] = "ace: call contract violation\n";
    Sec* c = &e->o.secs[SEC_CONST];
    ESym* msg = sym_define(&e->o.syms, e->o.secs, "__ace$contract_message", 22, 0, c);
    buf_write(&c->data, message, sizeof message - 1);
    sym_define(&e->o.syms, e->o.secs, "__ace$contract_fail", 19, 0, &e->o.secs[SEC_TEXT]);
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
    if (!e->o.null_fail_used) return;
    static const char message[] = "ace: null use violation\n";
    Sec* c = &e->o.secs[SEC_CONST];
    ESym* msg = sym_define(&e->o.syms, e->o.secs, "__ace$null_message", 18, 0, c);
    buf_write(&c->data, message, sizeof message - 1);
    sym_define(&e->o.syms, e->o.secs, "__ace$null_fail", 15, 0, &e->o.secs[SEC_TEXT]);
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
    ins(e, op | (((uint32_t)off & 0x1FFu) << 12) | ((uint32_t)rn << 5) | (uint32_t)rt);
}

// Q-register LDP/STP (op selects offset, pre-, or post-index form).
static void vpair(Em* e, uint32_t op, int rt, int rt2, int rn, int32_t off) {
    ins(e, op | (((uint32_t)(off / 16) & 0x7Fu) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt);
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
    if (e->o.mem_copy_used) {
        int copy = new_label(e), backward = new_label(e), lt16 = new_label(e);
        int lt8 = new_label(e), lt4 = new_label(e), done = new_label(e);
        int gt32 = new_label(e), big = new_label(e), loop = new_label(e);
        int bloop = new_label(e);

        // move: at most 64 bytes is overlap-safe; above that, go backward only
        // when the destination starts inside the source.
        sym_define(&e->o.syms, e->o.secs, "__ace$mem_move", 14, 0, &e->o.secs[SEC_TEXT]);
        ins(e, 0xF100001Fu | (64u << 10) | (17u << 5));   // cmp x17, #64
        bcond_(e, C_LS, copy);
        ins(e, 0xCB000000u | (16u << 16) | (15u << 5) | 8u); // x8 = dst - src
        ins(e, 0xEB00001Fu | (17u << 16) | (8u << 5));    // cmp x8, x17
        bcond_(e, C_LO, backward);

        sym_define(&e->o.syms, e->o.secs, "__ace$mem_copy", 14, 0, &e->o.secs[SEC_TEXT]);
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
    if (e->o.mem_fill_used) {
        int lt16 = new_label(e), lt8 = new_label(e), lt4 = new_label(e);
        int done = new_label(e), gt32 = new_label(e), loop = new_label(e);

        sym_define(&e->o.syms, e->o.secs, "__ace$mem_fill", 14, 0, &e->o.secs[SEC_TEXT]);
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
    if (e->o.mem_compare_used) {
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
        sym_define(&e->o.syms, e->o.secs, "__ace$mem_compare", 17, 0, &e->o.secs[SEC_TEXT]);
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
    if (!e->o.allocator_used) {
        if (e->o.mode == EMIT_DEBUG) {
            sym_define(&e->o.syms, e->o.secs, "__ace$registry_leaks", 20, 0, &e->o.secs[SEC_TEXT]);
            ins(e, 0xD65F03C0); // no allocation intrinsic exists in this unit
        }
        return;
    }
    Sec* b = &e->o.secs[SEC_BSS];
    b->size = align_up(b->size, 8);
    ESym* head = sym_define(&e->o.syms, e->o.secs, "__ace$registry_head", 19, 0, b);
    b->size += 8;

    static const char message[] = "ace: allocator violation\n";
    Sec* c = &e->o.secs[SEC_CONST];
    ESym* msg = sym_define(&e->o.syms, e->o.secs, "__ace$allocator_message", 23, 0, c);
    buf_write(&c->data, message, sizeof message - 1);
    sym_define(&e->o.syms, e->o.secs, "__ace$allocator_fail", 20, 0, &e->o.secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD); ins(e, 0x910003FD);
    load_imm(e, 1, 0, 2); addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, sizeof message - 1);
    emit_bl_extern(e, "_write", 6); emit_bl_extern(e, "_abort", 6);
    ins(e, 0xD4200000u);

    /* find(ptr,size) -> node; node layout is ptr,size,align,next. */
    sym_define(&e->o.syms, e->o.secs, "__ace$registry_find", 19, 0, &e->o.secs[SEC_TEXT]);
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

    sym_define(&e->o.syms, e->o.secs, "__ace$registry_add", 18, 0, &e->o.secs[SEC_TEXT]);
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

    sym_define(&e->o.syms, e->o.secs, "__ace$registry_remove", 21, 0, &e->o.secs[SEC_TEXT]);
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

    sym_define(&e->o.syms, e->o.secs, "__ace$registry_leaks", 20, 0, &e->o.secs[SEC_TEXT]);
    addr_of_sym(e, 16, head); ld_reg(e, 16, 16, 0, 8, 0);
    int no_leaks = new_label(e); cbz_(e, 1, 16, 0, no_leaks);
    emit_bl_internal(e, "__ace$allocator_fail", 20);
    bind_label(e, no_leaks); ins(e, 0xD65F03C0);
    patch_branches(e);
}

// Compile function n from its IR as symbol name, defining the vtables its
// constructions need first; a function the IR does not cover is a compile
// error naming why. Returns whether n compiled.
static int compile_ir(Em* e, Node* n, IrFunc* ir, const char* why, const char* name, int global) {
    if (!ir) {
        em_err(e, n, "cannot compile '%.*s': %s", (int)n->len, n->str, why ? why : "unsupported");
        return 0;
    }
    for (int k = 0; k < ir->nvtables; k++)
        obj_vtable(&e->o, ir->vtables[k].conc, ir->vtables[k].contract, ir->vtables[k].conf);
    if (strcmp(ace_target, "linux-x64") == 0) x64_func(&e->o, ir, name, (uint32_t)strlen(name), global);
    else arm64_func(&e->o, ir, name, (uint32_t)strlen(name), global);
    ir_free(ir);
    return 1;
}

int emit_object(Node* file, Diag* d, const char* out_path, CompileReport* report, EmitMode mode) {
    Em e;
    memset(&e, 0, sizeof e);
    obj_init(&e.o, d, mode);
    e.file = file;

    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        switch (n->kind) {
        case N_FUNC: {
            if (n->ival == BI_MEM_ALLOC || n->ival == BI_MEM_REALLOC ||
                n->ival == BI_MEM_RELEASE || n->ival == BI_MEM_COPY ||
                n->ival == BI_MEM_MOVE || n->ival == BI_MEM_FILL ||
                n->ival == BI_MEM_ZERO || n->ival == BI_MEM_COMPARE ||
                n->ival == BI_MEM_EQUAL || n->ival == BI_MEM_ALLOC_SIZE ||
                n->ival == BI_MEM_ALLOC_T || n->ival == BI_MEM_ZALLOC_T ||
                n->ival == BI_MEM_REALLOC_T ||
                n->ival == BI_MEM_RELEASE_T)
                break; // compiler-backed std/mem declarations have no body symbol
            if (n->ival == BI_FMT_PRINTF || n->ival == BI_STR_FORMAT ||
                (n->ival >= BI_SYS_ARGC && n->ival <= BI_SYS_ENV))
                break; // compiler-backed calls lower directly; no symbol
            Sym* decl_sym = node_sym(n);
            if (n->c && !(decl_sym && decl_sym->spec_args)) {
                // generic template: one function per instantiation memo entry
                int nt = n->c->nkids;
                for (STypeInst* inst = sema_insts(n); inst; inst = inst->next) {
                    const char* why;
                    IrFunc* ir = ir_lower(file, n, mode, inst->args, nt, &why); // stamps n's body for this instantiation either way
                    char mn[512];
                    snprintf(mn, sizeof mn, "%s", fn_mangle(decl_sym ? decl_sym->name : n->str,
                                               decl_sym ? decl_sym->len : n->len, inst->result,
                                               decl_sym ? decl_sym->decl : n, inst->args, nt, 0));
                    compile_ir(&e, n, ir, why, mn, 0);
                }
                break;
            }
            int is_main = !(n->flags & (F_METHOD | F_ASSOC)) && n->len == 4 && memcmp(n->str, "main", 4) == 0;
            int export_c = (n->flags & F_EXPO) != 0;
            const char* why;
            IrFunc* ir = ir_lower(file, n, mode, NULL, 0, &why);
            char mn[512];
            if (is_main) snprintf(mn, sizeof mn, "_main");
            else if (export_c) snprintf(mn, sizeof mn, "_%s", n->cname ? n->cname : n->str);
            else snprintf(mn, sizeof mn, "%s", fn_mangle(decl_sym->name, decl_sym->len, decl_sym->type, n, decl_sym->spec_args,
                                                     decl_sym->spec_args ? decl_sym->spec_nargs : 0, decl_sym->spec_args != NULL));
            if (compile_ir(&e, n, ir, why, mn, is_main || export_c) && is_main) emit_has_main = 1;
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
    int x64 = strcmp(ace_target, "linux-x64") == 0;
    if (x64) {
        x64_runtime(&e.o);
    } else {
        emit_bounds_failure_helper(&e);
        emit_arithmetic_failure_helper(&e);
        emit_contract_failure_helper(&e);
        emit_null_failure_helper(&e);
        emit_allocator_helpers(&e);
        emit_byte_helpers(&e);
        obj_patch_fixups(&e.o);
    }
    if (d->errors) return 1;
    if (report) report->machine_code = text(&e)->len;
    ESym* text_anchor = sym_intern(&e.o.syms, "__ace$text", 10);
    obj_debug_info(&e.o, text_anchor);
    obj_debug_line(&e.o, text_anchor);
    size_t object_size = x64 ? elf_write(&e.o, out_path) : obj_write(&e.o, out_path);
    if (report) report->object_file = object_size;
    return d->errors ? 1 : 0;
}
