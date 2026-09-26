// emit.c — stage four: direct AArch64 codegen → Mach-O arm64 object (tcc-style
// walk of the sema-checked AST: per function, planning scans then one emission
// walk; see SPEC.md Compiler #2).
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

// --- emitter state ----------------------------------------------------------------

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

enum { CLS_INT, CLS_FP, CLS_ADDR };
typedef struct { uint8_t cls; int idx; } EV; // int temp / fp temp / address in int temp

typedef struct { // register-promotion candidate: a scalar local or parameter
    Node*    decl;    // binder node (the Sym's decl)
    uint32_t weight;  // use_weight summed over its uses
    uint8_t  escaped; // its address is taken somewhere; stays in its slot
    uint8_t  crosses; // a clobbering call lies strictly inside its interval
    uint8_t  byref;   // a by-reference parameter: the register holds its pointer
    int      pend;    // outermost enclosing loop it must stay live through, or -1
    uint32_t first, last; // live interval in promo_scan positions
    int      lnext;   // next candidate pending on the same loop, or -1
} PromoCand;

typedef struct { // a struct local replaced by its scalar fields
    Node*   decl;     // the struct binder
    SType*  type;     // its struct type, unwrapped
    Node*   field[4]; // one placeholder binder per field, each a promotion candidate
    int     n;
    uint8_t bad;      // some use needs the struct whole in memory
    uint8_t regs;     // after planning: some field won a register
} Split;

typedef struct { Node* decl; int reg; uint32_t first, last; uint8_t byref; } PromoAsg; // a promoted binder's register
typedef struct { uint32_t weight; int ix; } PromoRank; // a candidate in promotion order
#define PASG_SCAN 16 // pasg_find scans at most this many assignments before indexing them

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
#define LEAF_REGS 6    // x14 down to x9: promoted locals whose live range crosses no call
/* temp_need counts operands, not the internal temps some lowerings add (a
 * static-map probe, a byte-sequence compare); the largest shortfall across
 * the std, test, bench, and package corpus is 2, so leaf promotion leaves 3
 * more. tmp_int's cap still rejects, never miscompiles, an expression beyond. */
#define TEMP_MARGIN 3
#define MAX_FPTEMPS 16 // v0-v7, v16-v23
#define LOOP_REGS 10   // x19-x28, callee-saved loop state and promoted locals
#define FP_PROMO_REGS 8 // d8-d15, callee-saved promoted float locals
/* d24-d31 hold nothing the emitter keeps between statements, so eight more
 * float locals may live there. They are caller-saved, and the shared byte
 * routines clobber them too, so every branch-with-link stores and reloads
 * them: the cost lands on the calling paths, never on a call-free one. A
 * function with any call or large aggregate inside a loop keeps its slots
 * rather than pay that at every iteration. */
#define FP_LEAF_REGS 8  // d24-d31, caller-saved promoted float locals
#define PARAM_FHOMES 8 // register-homed float leaves of incoming HFA parameters, in their argument registers
#define PARAM_HOMES (PARAM_FHOMES + LEAF_REGS) // plus x14 down: integer-class leaves

/*
 * One direct call to an inline function, planned before its caller is
 * emitted: the callee's parameters and body copied with fresh binders, so
 * every planner sees the expansion as the caller's own code.
 */
typedef struct {
    int      n;      // bindings: the receiver, then the parameters
    Node**   arg;    // argument expressions, a mut mark unwrapped
    Node**   bind;   // a by-reference parameter's binder, or an N_VAR holding a value
    uint8_t* byref;
    Node*    body;   // the copied body block
    SType*   ret;    // the result type, or NULL
    Node*    tail;   // a final top-level return, which falls into the join
} InlineSite;

typedef struct {
    int    join; // label after the expansion
    int    res;  // result temp
    int    fp;
    SType* ret;
    Node*  tail;
} InlineCtx;

typedef struct {
    Obj   o;
    Node* file;
    int current_is_main;
    uint64_t* fpool; // current function's float constants, 8 bytes each
    uint32_t  nfpool, cfpool;
    uint32_t* fpool_site; // (ldr literal offset, pool index) pairs
    uint32_t  nfpool_site, cfpool_site;
    GVar*  globs;
    int    nglobs, cglobs;
    StaticMapData* maps;
    int    nmaps, cmaps;
    // current function
    size_t subsp_fix;
    uint32_t frame;
    uint32_t int_spill[MAX_ITEMPS]; // per-temp spill slots, 0 until first use
    uint32_t fp_spill[MAX_FPTEMPS];
    Node**   lreg_loops; // loops whose state lives in x19-x28 (plan_loops)
    int      nlreg_loops, clreg_loops;
    int      lreg_top;   // next free loop register, as an offset from x19
    int      lreg_max;   // loop registers x19..x19+lreg_max-1
    PromoAsg* pasg;      // promoted integer-class binders and their registers
    int      npasg, cpasg;
    int      npreg;      // registers promotion opened from x19+lreg_max up
    int      nxpreg;     // registers promotion used from x14 down
    int      vti_cap;    // int temps available: MAX_ITEMPS less nxpreg
    int      scan_call;  // promo_scan met a call that may clobber x9-x14
    uint32_t scan_pos;   // promo_scan position counter
    uint32_t scan_loop[64]; // start positions of the enclosing loops
    int      scan_lpend[64]; // first candidate each loop keeps live to its end, or -1
    int      nscan_loop;
    int      scan_defer; // inside a defer statement
    Node*    scan_deferred; // that statement's call, which runs at exits
    uint32_t* scan_calls; // positions of clobbering calls, ascending
    int      nscan_calls, cscan_calls;
    PtrMap   call_pos;   // call node -> scan position + 1
    int*     scan_pend;  // candidates read since the innermost open frame
    int      nscan_pend, cscan_pend;
    int*     scan_frame; // nscan_pend at each open frame
    int      nscan_frame, cscan_frame;
    int      in_defers;  // emit_defers is expanding exit-time calls
    uint32_t bound_at;   // text offset of the last bound label
    uint32_t peep_bar;   // text offset of the last recorded site: peep folds nothing across it
    int      peep_on;    // peep may rewrite: inside a function body without asm
    int      scan_loop_branch; // promo_scan met a call or large aggregate inside a loop
    int      nsaved;     // callee-saved x registers the function saves and restores
    uint32_t lreg_save;  // depth of the callee-saved x save area
    Node*    fpreg_decl[FP_PROMO_REGS + FP_LEAF_REGS]; // promoted float locals, d8 up then d24 up
    int      nfpreg;
    int      nfpleaf;    // of nfpreg, the trailing ones living in d24-d31
    uint32_t fpreg_save; // depth of the d8-d15 save area
    uint32_t fpleaf_save; // depth of the d24-d31 save area
    PromoCand* pcands;   // plan_promotion scratch
    int      npcands, cpcands;
    PtrMap   pcand_ix;   // binder node -> index + 1 into pcands
    PtrMap   pasg_ix;    // binder node -> index + 1 into pasg, once npasg passes PASG_SCAN
    Split*   splits;     // struct locals promo_scan considered for field-by-field promotion
    int      nsplits, csplits;
    PtrMap   split_ix;   // struct binder -> index + 1 into splits
    PromoRank* prank;   // plan_promotion scratch, capacity cprank
    int      cprank;
    struct { uint32_t* iv; int n, cap; } reg_iv[32]; // each x register's promoted intervals as sorted disjoint [first, last] pairs
    uint32_t* leaf_span; // merged interiors of x9-x14 intervals, as [lo, hi] pairs
    int      nleaf_span, cleaf_span;
    int      fn_has_asm;
    struct { Node* decl; uint32_t off; int reg; } phome[PARAM_HOMES]; // register-homed parameter leaves
    int      nphome, nfhome, nxhome; // all, argument-register float, and x14-down integer homes
    int      vtf_cap;    // fp temps available: MAX_FPTEMPS less nfhome
    uint8_t  vmap[MAX_FPTEMPS]; // fp temp index -> v register (vreg)
    uint32_t out_max;
    Slot*   slots;
    int     nslots, cslots;
    PtrMap  slot_ix;     // slot key -> index + 1 into slots (first slot of a key)
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
    Node*    fnnode;   // current function's declaration
    const char* fnname; // and its symbol
    uint32_t fnnlen;
    int      self_entry; // label before the parameter reads, or -1: no self tail recursion
    Node*    self_call;  // the self call being emitted as a branch to it
    struct { Node* call; Node* at; } *defers; // captured defers, source order
    int      ndefers, cdefers;
    uint32_t retsave_off; // result-register save area across deferred calls
    // one-shot destination for a register-returned call result: emit_call
    // stores unit w at frame depth sink_off[w] instead of staging a temp
    Node*    sink_call;
    SType*   sink_ret;
    uint32_t sink_off[4];
    int      sink_n, sink_done;
    Split*   sink_split; // or moves the units into this split local's fields
    // inline expansion (Functions & ABI #8), per emitted function
    PtrMap   inl_ix;     // call node -> InlineSite*
    PtrMap   copy_node;  // callee node -> its copy, while copying one callee
    PtrMap   copy_sym;   // callee local -> its copy
    Node**   inl_path;   // callees inline_plan is expanding, outermost first
    int      ninl_path, cinl_path;
    int      nsites;     // calls inline_plan expanded in this function
    InlineCtx* inl;      // expansions being emitted, innermost last
    int      ninl, cinl;
    int      vti_base, vtf_base; // statement boundaries reset the temps to these
} Em;

static InlineSite* inline_site(Em* e, Node* call) {
    return call && call->kind == N_CALL ? (InlineSite*)ptrmap_get(&e->inl_ix, call) : NULL;
}

// a statement boundary: the temps an enclosing inline call holds stay live
static void temps_reset(Em* e) {
    e->vti = e->vti_base;
    e->vtf = e->vtf_base;
}


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

/*
 * A text offset something recorded (a relocation, fixup, or branch site):
 * peep neither absorbs the instruction emitted here into the one before it
 * nor rewrites the one before it. Called right after the site's instruction
 * is emitted, or right before it when the site is recorded first.
 */
static void peep_barrier(Em* e) { e->peep_bar = (uint32_t)text(e)->len; }

static void fix_add(Em* e, uint8_t kind, uint32_t site, ESym* target) {
    obj_fixup(&e->o, kind, site, target);
    peep_barrier(e);
}

/*
 * Rewrite the instruction just emitted to absorb w, when no label is bound
 * and no site recorded between them. Returns 1 when w was absorbed.
 *   mov t, p ; op t, ..t..         -> op t, ..p..    (op overwrites t: the copy is dead)
 *   add t, s, #a ; ldr t, [t, #b]  -> ldr t, [s, #a+b]
 *   add t, s, #a ; add t, t, #b    -> add t, s, #a+b
 *   xbfm t, p, #0, #m ; xbfm t, t, #0, #k (k <= m) -> xbfm t, p, #0, #k
 * op is an add/sub or logical immediate, sbfm/ubfm, a shifted add/sub, a
 * logical register op other than the mov alias, a 2- or 3-source op, or an
 * integer load by unsigned or register offset. A 32-bit mov feeds only 32-bit
 * ops: a 64-bit read of t would see the zero-extended copy, not p.
 */
static int peep(Em* e, uint32_t w) {
    Buf* b = text(e);
    uint32_t len = (uint32_t)b->len;
    if (!e->peep_on || len < 4 || e->bound_at == len || e->peep_bar == len) return 0;
    uint32_t prev;
    memcpy(&prev, b->d + len - 4, 4);
    uint32_t rd = w & 31u, rn = (w >> 5) & 31u, rm = (w >> 16) & 31u, ra = (w >> 10) & 31u;
    uint32_t size = w >> 30, opc = (w >> 22) & 3u;
    int ld = !((w >> 26) & 1u) && opc != 0u && !(size == 3u && opc == 2u); // integer load, not prfm
    int ld_uimm = ld && (w & 0x3B000000u) == 0x39000000u;
    int ld_roff = ld && (w & 0x3B200C00u) == 0x38200800u;
    uint32_t t = prev & 31u;
    if (t == 31 || rd != t) return 0;
    if ((prev & 0x7FE0FFE0u) == 0x2A0003E0u) { // mov t, p
        uint32_t p = (prev >> 16) & 31u, sf = w >> 31, nw = w;
        if (p == 31) return 0;
        int src1 = ((w >> 23) & 0x3Fu) == 0x22u || ((w >> 23) & 0x3Fu) == 0x24u ||   // add/sub, logical imm
                   (((w >> 23) & 0x3Fu) == 0x26u && ((w >> 29) & 3u) != 1u);       // sbfm/ubfm
        int src2 = (((w >> 24) & 0x1Fu) == 0x0Bu && !((w >> 21) & 1u)) ||            // shifted add/sub
                   (((w >> 24) & 0x1Fu) == 0x0Au && (w & 0x7FE0FFE0u) != 0x2A0003E0u) || // logical, not mov
                   (w & 0x7FE00000u) == 0x1AC00000u;                                  // 2-source
        int src3 = ((w >> 24) & 0x1Fu) == 0x1Bu;                                      // 3-source
        if (src1 || src2 || src3) {
            if (!(prev >> 31) && sf) return 0;
        } else if (ld_uimm || ld_roff) {
            if (!(prev >> 31)) return 0; // the base is always 64-bit
        } else return 0;
        if (rn == t) nw = (nw & ~(31u << 5)) | (p << 5);
        if ((src2 || src3 || ld_roff) && rm == t) nw = (nw & ~(31u << 16)) | (p << 16);
        if (src3 && ra == t) nw = (nw & ~(31u << 10)) | (p << 10);
        if (nw == w) return 0; // w does not read t
        memcpy(b->d + len - 4, &nw, 4);
        return 1;
    }
    // sbfm/ubfm from bit 0 (sxt*, uxt*, low-bit sbfx/ubfx): a second one
    // keeping no more low bits than the first reads the same bits of p
    if (((prev >> 23) & 0x3Fu) == 0x26u && ((prev >> 29) & 3u) != 1u && !((prev >> 16) & 63u) &&
        ((w >> 23) & 0x3Fu) == 0x26u && ((w >> 29) & 3u) != 1u && !((w >> 16) & 63u) &&
        rn == t && ((w >> 10) & 63u) <= ((prev >> 10) & 63u)) {
        uint32_t p = (prev >> 5) & 31u;
        if (p == t) return 0;
        uint32_t nw = (w & ~(31u << 5)) | (p << 5);
        memcpy(b->d + len - 4, &nw, 4);
        return 1;
    }
    if ((prev & 0xFFC00000u) == 0x91000000u && rn == t) { // add xt, xs, #a
        uint32_t s = (prev >> 5) & 31u, a = (prev >> 10) & 0xFFFu, nw;
        if (ld_uimm) {
            uint32_t tot = a + (((w >> 10) & 0xFFFu) << size);
            if (tot & ((1u << size) - 1u) || (tot >> size) > 0xFFFu) return 0;
            nw = (w & ~(0xFFFu << 10) & ~(31u << 5)) | ((tot >> size) << 10) | (s << 5);
        } else if ((w & 0xFFC00000u) == 0x91000000u) {
            uint32_t tot = a + ((w >> 10) & 0xFFFu);
            if (tot > 0xFFFu) return 0;
            nw = 0x91000000u | (tot << 10) | (s << 5) | t;
        } else return 0;
        memcpy(b->d + len - 4, &nw, 4);
        return 1;
    }
    return 0;
}

static void ins(Em* e, uint32_t w) {
    if (!peep(e, w)) buf_put32(text(e), w);
}

// Pad with nops to a code boundary (FUNC_ALIGN, LOOP_ALIGN in backend.h).
static void align_code(Em* e, uint32_t bytes) {
    while (text(e)->len % bytes) ins(e, 0xD503201Fu);
}

static int xreg(int t) { return t < 8 ? t : t + 1; }
// fp temp t's v register: v0-v7 then v16-v23, less the homes plan_param_homes swaps in
static int vreg(Em* e, int t) { return e->vmap[t]; }

static void vmap_reset(Em* e) {
    for (int t = 0; t < MAX_FPTEMPS; t++) e->vmap[t] = (uint8_t)(t < 8 ? t : t + 8);
}

static const char* arena_copy(const char* s, size_t n) {
    char* p = arena_alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void line_mark(Em* e, Node* n) {
    if (!n || n->line <= 0) return;
    uint32_t addr = (uint32_t)text(e)->len;
    e->peep_bar = addr; // a line row's first instruction stays its own
    if (e->ninl) return; // inlined code keeps its call site's line
    obj_line(&e->o, addr, n->line, n->col, n->path ? n->path : e->o.d->path);
}

static void debug_fn_add(Em* e, Node* f, uint32_t addr, uint32_t size) {
    obj_debug_fn(&e->o, addr, size, f->str, f->len);
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
static void fp_leaves(Em* e, int load);
static int eval_const(Em* e, Node* v, SType* t, Buf* out, ConstRels* rels);
static void emit_stmt(Em* e, Node* s);
static int loop_regs_take(Em* e, Node* s);
static int loop_own_regs(Node* s);
static int range_up(Node* r, uint64_t* end);
static uint64_t range_value(uint64_t v, SType* rt);
static void plan_promotion(Em* e, Node* f, int export_c);
static int leaf_spanned(Em* e, uint32_t p);
static void gen_branch(Em* e, Node* x, int jump_if, int lab);
static int promo_reg(Em* e, Sym* s);
static int gen_addr(Em* e, Node* x);
static int is_lvalue_node(Node* x);
static int frame_place(Em* e, Node* x, uint32_t* depth);
static int self_return(Em* e, Node* x);
static int param_home(Em* e, Node* x);
static int split_reg(Em* e, Node* x);
static int split_type(SType* t);
static Split* split_live(Em* e, Node* x);
static void split_store_at(Em* e, Split* sp, int base, uint32_t off);
static void split_unit(Em* e, Split* sp, TU* u, int dst);
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

uint32_t field_off(SType* t, int want) {
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

int find_field(SType* t, const char* name, uint32_t len) {
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
static int hfa_leaves(SType* t, uint32_t off, uint32_t* offs, uint8_t* dbl, int n, int max) {
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
    if (!s->is_global || !s->is_const || !s->init || !is_scalar(t) || is_floatt(t)) return 0;
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

/*
 * const_global_imm's float counterpart: a global const f32/f64 initialized by
 * a float literal, its negation, or another such const is that value, so a
 * read loads it from the function's constant pool instead of its slot.
 */
static int const_global_float(Sym* s, SType* t, double* out) {
    SType* u = unwrap(t);
    if (!s->is_global || !s->is_const || !s->init || !u || u->kind != SK_PRIM ||
        (u->prim != P_F32 && u->prim != P_F64))
        return 0;
    Node* v = s->init;
    int neg = v->kind == N_UNARY && v->op == T_MINUS;
    if (neg) v = v->a;
    if (v->kind == N_FLOAT) {
        *out = neg ? -v->fval : v->fval;
        return 1;
    }
    Sym* inner = !neg && v->kind == N_IDENT ? node_sym(v) : NULL;
    return inner && inner->kind == SYM_VAR && const_global_float(inner, t, out);
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
    if (!bits) { // fmov Vd, xzr/wzr: +0.0 has no imm8 encoding
        ins(e, (is64 ? 0x9E6703E0u : 0x1E2703E0u) | (uint32_t)vr);
        return;
    }
    int imm8 = fmov_imm8(bits, is64);
    if (imm8 >= 0) { // fmov Vd, #imm: no integer temp
        ins(e, (is64 ? 0x1E601000u : 0x1E201000u) | ((uint32_t)imm8 << 13) | (uint32_t)vr);
        return;
    }
    // ldr Vd, <literal>: one load from the function's constant pool, which
    // flush_fpool places after its last instruction and patches imm19 against
    uint32_t idx = 0;
    while (idx < e->nfpool && e->fpool[idx] != bits) idx++;
    if (idx == e->nfpool) {
        if (e->nfpool == e->cfpool) {
            e->cfpool = e->cfpool ? e->cfpool * 2 : 16;
            e->fpool = realloc(e->fpool, e->cfpool * sizeof(uint64_t));
            if (!e->fpool) oom();
        }
        e->fpool[e->nfpool++] = bits;
    }
    if (e->nfpool_site + 2 > e->cfpool_site) {
        e->cfpool_site = e->cfpool_site ? e->cfpool_site * 2 : 32;
        e->fpool_site = realloc(e->fpool_site, e->cfpool_site * sizeof(uint32_t));
        if (!e->fpool_site) oom();
    }
    e->fpool_site[e->nfpool_site++] = (uint32_t)text(e)->len;
    e->fpool_site[e->nfpool_site++] = idx;
    ins(e, (is64 ? 0x5C000000u : 0x1C000000u) | (uint32_t)vr);
}

/*
 * Place the current function's float constants after its last instruction,
 * which never falls through, 8-byte aligned (an f32 in the low half), and
 * patch every ldr literal site's imm19 against its entry.
 */
static void flush_fpool(Em* e) {
    if (!e->nfpool) return;
    Buf* t = text(e);
    if (t->len & 7) buf_put32(t, 0); // udf #0 pad
    uint32_t base = (uint32_t)t->len;
    for (uint32_t i = 0; i < e->nfpool; i++) {
        buf_put32(t, (uint32_t)e->fpool[i]);
        buf_put32(t, (uint32_t)(e->fpool[i] >> 32));
    }
    for (uint32_t i = 0; i < e->nfpool_site; i += 2) {
        uint32_t site = e->fpool_site[i];
        int64_t delta = (int64_t)base + 8 * (int64_t)e->fpool_site[i + 1] - (int64_t)site;
        if (delta >= (1 << 20)) {
            fprintf(stderr, "ace: float constant out of literal-load range\n");
            e->o.d->errors++;
            break;
        }
        uint32_t w;
        memcpy(&w, t->d + site, 4);
        buf_patch32(t, site, w | (uint32_t)(delta >> 2) << 5);
    }
    e->nfpool = e->nfpool_site = 0;
    peep_barrier(e);
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
    e->bound_at = e->lab[l]; // a join point: store_preg folds nothing across it
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
    peep_barrier(e);
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

static void cset_(Em* e, int sf, int cond, int rd) {
    ins(e, (sf ? 0x9A9F07E0u : 0x1A9F07E0u) | (((uint32_t)cond ^ 1u) << 12) | (uint32_t)rd);
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

// --- frame, slots, temps --------------------------------------------------------------

static uint32_t align_up(uint32_t v, uint32_t a) {
    return a ? (v + a - 1) / a * a : v;
}

static Slot* slot_find(Em* e, Node* key) {
    uintptr_t ix = ptrmap_get(&e->slot_ix, key);
    return ix ? &e->slots[ix - 1] : NULL;
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
    if (key && !ptrmap_get(&e->slot_ix, key)) ptrmap_put(&e->slot_ix, key, (uintptr_t)e->nslots + 1);
    e->slots[e->nslots++] = (Slot){ key, off, size, 0 };
    return off;
}

static uint32_t temp_slot(Em* e, uint32_t size, uint32_t align) {
    return slot_alloc(e, NULL, size, align);
}

// rd = x29 - off; slot offsets are aligned depths below the frame record.
static void frame_addr(Em* e, int rd, uint32_t off) {
    uint32_t d = off;
    if (d <= 4095) ins(e, 0xD1000000u | (d << 10) | (29u << 5) | ((uint32_t)rd & 31u));
    else {
        load_imm(e, 1, rd, d);
        ins(e, 0xCB000000u | ((uint32_t)rd << 16) | (29u << 5) | ((uint32_t)rd & 31u));
    }
}

static int tmp_int(Em* e) {
    if (e->vti >= e->vti_cap) {
        e->o.d->errors++;
        fprintf(stderr, "ace: expression too deeply nested (int temps)\n");
        return 0;
    }
    return e->vti++;
}

static int tmp_fp(Em* e) {
    if (e->vtf >= e->vtf_cap) {
        e->o.d->errors++;
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
    return w == 1 ? 0x38000000u : w == 2 ? 0x78000000u : w == 4 ? 0xB8000000u : 0xF8000000u;
}

static uint32_t ldur_base(uint32_t w) {
    return w == 1 ? 0x38400000u : w == 2 ? 0x78400000u : w == 4 ? 0xB8400000u : 0xF8400000u;
}

static void store_frame(Em* e, int rt, uint32_t off, uint32_t w) {
    uint32_t d = off;
    if (d <= 256) { // stur rt, [x29, #-d]
        ins(e, stur_base(w) | ((((uint32_t)(0u - d)) & 0x1FFu) << 12) | (29u << 5) | ((uint32_t)rt & 31u));
        return;
    }
    int base = rt == 16 ? 17 : 16;
    frame_addr(e, base, off);
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u : w == 4 ? 0xB9000000u : 0xF9000000u;
    ins(e, b | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
}

static void load_frame(Em* e, int rt, uint32_t off, uint32_t w, int signext) {
    uint32_t d = off;
    if (d <= 256) { // ldur rt, [x29, #-d]; ldursb/ldursh wt sign-extend narrow
        uint32_t b = !signext || w >= 4 ? ldur_base(w) : w == 1 ? 0x38C00000u : 0x78C00000u;
        ins(e, b | ((((uint32_t)(0u - d)) & 0x1FFu) << 12) | (29u << 5) | ((uint32_t)rt & 31u));
        return;
    }
    // The destination can also hold the computed address: `ldr rt, [rt]`.
    // This avoids clobbering the other reserved scratch register when callers
    // deliberately load a pair into x16/x17 from deep frame slots.
    int base = rt;
    frame_addr(e, base, off);
    uint32_t b;
    if (!signext) b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u : w == 4 ? 0xB9400000u : 0xF9400000u;
    else b = w == 1 ? 0x39C00000u : w == 2 ? 0x79C00000u : w == 4 ? 0xB9400000u : 0xF8400000u;
    ins(e, b | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
}

static void store_frame_fp(Em* e, int vr, uint32_t off, int is64) {
    if (off <= 256) { // stur s|d, [x29, #-off]
        ins(e, (is64 ? 0xFC000000u : 0xBC000000u) | ((((uint32_t)(0u - off)) & 0x1FFu) << 12) | (29u << 5) | (uint32_t)vr);
        return;
    }
    frame_addr(e, 16, off);
    ins(e, (is64 ? 0xFD000000u : 0xBD000000u) | (16u << 5) | (uint32_t)vr);
}

static void load_frame_fp(Em* e, int vr, uint32_t off, int is64) {
    if (off <= 256) { // ldur s|d, [x29, #-off]
        ins(e, (is64 ? 0xFC400000u : 0xBC400000u) | ((((uint32_t)(0u - off)) & 0x1FFu) << 12) | (29u << 5) | (uint32_t)vr);
        return;
    }
    frame_addr(e, 16, off);
    ins(e, (is64 ? 0xFD400000u : 0xBD400000u) | (16u << 5) | (uint32_t)vr);
}

static void spill_temps(Em* e) {
    for (int i = 0; i < e->vti; i++) store_frame(e, xreg(i), spill_slot_int(e, i), 8);
    for (int j = 0; j < e->vtf; j++) store_frame_fp(e, vreg(e, j), spill_slot_fp(e, j), 1);
}

static void reload_temps(Em* e, int ni, int nf) {
    for (int i = 0; i < ni; i++) load_frame(e, xreg(i), spill_slot_int(e, i), 8, 0);
    for (int j = 0; j < nf; j++) load_frame_fp(e, vreg(e, j), spill_slot_fp(e, j), 1);
}

// --- memory access via address register ---------------------------------------------

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

static void ldrf_(Em* e, int vr, int base, uint32_t off, int is64) {
    uint32_t scale = is64 ? 8u : 4u; // unsigned-offset imm12 unit
    uint32_t b = is64 ? 0xFD400000u : 0xBD400000u;
    if (off % scale == 0 && off / scale < 4096)
        ins(e, b | ((off / scale) << 10) | ((uint32_t)base << 5) | (uint32_t)vr);
    else {
        load_imm(e, 1, 16, off);
        ins(e, (is64 ? 0xFC606800u : 0xBC606800u) | (16u << 16) | ((uint32_t)base << 5) | (uint32_t)vr);
    }
}

// Register-offset loads, Rt = [base, index, lsl #log2(w)]: the index is scaled
// by the access width, so the address add folds into the load.
static void ld_idx(Em* e, int rt, int base, int idx, uint32_t w, int signext) {
    uint32_t rb;
    if (!signext) rb = w == 1 ? 0x38606800u : w == 2 ? 0x78606800u : w == 4 ? 0xB8606800u : 0xF8606800u;
    else rb = w == 1 ? 0x38E06800u : w == 2 ? 0x78E06800u : w == 4 ? 0xB8606800u : 0xF8606800u;
    ins(e, rb | (w > 1 ? 1u << 12 : 0u) | ((uint32_t)idx << 16) | ((uint32_t)base << 5) | ((uint32_t)rt & 31u));
}

static void ldrf_idx(Em* e, int vr, int base, int idx, int is64) {
    ins(e, (is64 ? 0xFC606800u : 0xBC606800u) | (1u << 12) | ((uint32_t)idx << 16) |
           ((uint32_t)base << 5) | (uint32_t)vr);
}

static void strf_(Em* e, int vr, int base, uint32_t off, int is64) {
    uint32_t scale = is64 ? 8u : 4u;
    uint32_t b = is64 ? 0xFD000000u : 0xBD000000u;
    if (off % scale == 0 && off / scale < 4096)
        ins(e, b | ((off / scale) << 10) | ((uint32_t)base << 5) | (uint32_t)vr);
    else {
        load_imm(e, 1, 16, off);
        ins(e, (is64 ? 0xFC206800u : 0xBC206800u) | (16u << 16) | ((uint32_t)base << 5) | (uint32_t)vr);
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
    if (lo || !hi) ins(e, 0x91000000u | (lo << 10) | ((uint32_t)rn << 5) | (uint32_t)rd);
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
            while (align > 1 && (((doff + o) | (soff + o)) & (align - 1))) align >>= 1;
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
    e->o.mem_copy_used = 1;
    emit_bl_internal(e, "__ace$mem_copy", 14);
}

// --- scalar moves ---------------------------------------------------------------------

static void mov_int(Em* e, int sf, int dst, int src) {
    if (dst == src) return;
    ins(e, (sf ? 0xAA0003E0u : 0x2A0003E0u) | ((uint32_t)src << 16) | ((uint32_t)dst & 31u));
}

static void mov_fp(Em* e, int is64, int dst, int src) {
    if (dst == src) return;
    ins(e, (is64 ? 0x1E604000u : 0x1E204000u) | ((uint32_t)vreg(e, src) << 5) | (uint32_t)vreg(e, dst));
}

// load a scalar of type t from [base+off] into a fresh temp
static EV load_scalar(Em* e, int base, uint32_t off, SType* t) {
    if (is_floatt(t)) {
        int vt = tmp_fp(e);
        ldrf_(e, vreg(e, vt), base, off, unwrap(t)->prim == P_F64);
        return (EV){ CLS_FP, vt };
    }
    int it = tmp_int(e);
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    ld_reg(e, xreg(it), base, off, w, ty_is_signed(t) && w < 8);
    return (EV){ CLS_INT, it };
}

static void store_scalar(Em* e, EV v, int base, uint32_t off, SType* t) {
    if (v.cls == CLS_FP) strf_(e, vreg(e, v.idx), base, off, unwrap(t)->prim == P_F64);
    else st_reg(e, xreg(v.idx), base, off, (uint32_t)((scalar_bits(t) + 7) / 8));
}

// store an expression value (scalar temp or aggregate address) into [base+off]
static void store_value(Em* e, EV v, int base, uint32_t off, SType* t) {
    if (v.cls == CLS_ADDR) {
        if (base == 16 || base == 17) {
            mov_int(e, 1, 15, base);
            base = 15;
        }
        copy_mem(e, base, off, xreg(v.idx), 0, ty_size(t));
    } else store_scalar(e, v, base, off, t);
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

// Re-extend a narrow (8/16-bit) integer in rd from its low bits, sxtb/sxth or uxtb/uxth
static void narrow_wrap(Em* e, int rd, SType* t) {
    int b = scalar_bits(t);
    if (!is_intt(t) || b >= 32) return;
    uint32_t x = ty_is_signed(t) ? (b == 8 ? 0x13001C00u : 0x13003C00u) : (b == 8 ? 0x53001C00u : 0x53003C00u);
    ins(e, x | ((uint32_t)rd << 5) | (uint32_t)rd);
}

static EV gen_cast(Em* e, EV v, SType* from, SType* to) {
    if (!from || !to || from->kind == SK_ERR || to->kind == SK_ERR) return v;
    if (is_floatt(from) && is_floatt(to)) { // f32 <-> f64
        int a64 = unwrap(from)->prim == P_F64, b64 = unwrap(to)->prim == P_F64;
        if (a64 == b64) return v;
        ins(e, (b64 ? 0x1E22C000u : 0x1E624000u) | ((uint32_t)vreg(e, v.idx) << 5) | (uint32_t)vreg(e, v.idx));
        return v;
    }
    if ((is_intt(from) || is_boolt(from)) && is_floatt(to)) { // scvtf/ucvtf
        int src64 = scalar_bits(from) == 64;
        int dst64 = unwrap(to)->prim == P_F64;
        uint32_t base = src64 ? (dst64 ? 0x9E620000u : 0x9E220000u)
            : (dst64 ? 0x1E620000u : 0x1E220000u);
        if (!ty_is_signed(from)) base |= 0x10000u;
        /*
         * The result lands in a fresh fp temp: reusing the int temp's index
         * would alias a live value in the other pool (the two pools number
         * their registers independently).
         */
        int t = tmp_fp(e);
        ins(e, base | ((uint32_t)xreg(v.idx) << 5) | (uint32_t)vreg(e, t));
        e->vti = v.idx; // the int temp is consumed
        return (EV){ CLS_FP, t };
    }
    if (is_floatt(from) && (is_intt(to) || is_boolt(to))) { // fcvtzs/fcvtzu
        int src64 = unwrap(from)->prim == P_F64;
        int dst64 = scalar_bits(to) == 64;
        uint32_t base = src64 ? (dst64 ? 0x9E780000u : 0x1E780000u)
            : (dst64 ? 0x9E380000u : 0x1E380000u);
        if (!ty_is_signed(to)) base |= 0x10000u;
        int t = tmp_int(e);
        ins(e, base | ((uint32_t)vreg(e, v.idx) << 5) | (uint32_t)xreg(t));
        narrow_wrap(e, xreg(t), to); // fcvtz* saturates to 32 bits, not 8/16
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
    // Narrowing, or a signed source into an unsigned target, changes the
    // target's extension; an unsigned source widening into signed does not
    int sf = ty_is_signed(from), st = ty_is_signed(to);
    if (fa > ta || (sf && !st) || (fa == ta && sf != st)) narrow_wrap(e, xreg(v.idx), to);
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
static void emit_arith_raw(Em* e, TokKind op, int rd, int ra, int rb, SType* t) {
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
        ins(e, s | (ty_is_signed(t) ? 0x1AC02800u : 0x1AC02400u) | (rb << 16) | (ra << 5) | rd);
        return;
    default: return;
    }
}

// emit_arith_raw, then re-extend a narrow result where it can wrap unchecked
static void emit_arith(Em* e, TokKind op, int rd, int ra, int rb, SType* t) {
    emit_arith_raw(e, op, rd, ra, rb, t);
    if (e->o.mode == EMIT_RELEASE || op == T_SHL) narrow_wrap(e, rd, t);
}

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

/*
 * dst = a / c or a % c for a constant divisor. Returns 0, emitting nothing,
 * when the hardware divide must stay: a zero divisor, or signed -1, whose
 * debug-build overflow guard lives with the generic path. Uses x16/x17.
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
            ins(e, 0xD340FC00u | ((uint32_t)((sf ? 0 : 32) + m.shift) << 16) | (16u << 5) | qd);
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
            ins(e, 0x9340FC00u | ((uint32_t)((sf ? 0 : 32) + m.shift) << 16) | (16u << 5) | 16u);
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
static void emit_checked_arith(Em* e, TokKind op, int rd, int ra, int rb, SType* t) {
    int bits = scalar_bits(t);
    int sf = bits == 64;
    int sig = ty_is_signed(t);
    if (e->o.mode != EMIT_DEBUG || !is_intt(t)) {
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
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)ra << 5));
            bcond_(e, C_NE, not_min);
            load_imm(e, sf, 16, sf ? UINT64_MAX : UINT32_MAX);
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)rb << 5));
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
        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)rb << 5));
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
        ins(e, base | ((uint32_t)rb << 16) | ((uint32_t)ra << 5) | (uint32_t)rd);
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
            ins(e, (sig ? 0x9B207C00u : 0x9BA07C00u) | ((uint32_t)rb << 16) | ((uint32_t)ra << 5) | 16u);
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
            ins(e, (sig ? 0x9B407C00u : 0x9BC07C00u) | ((uint32_t)rb << 16) | ((uint32_t)ra << 5) | 16u);
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
static int emit_arith_imm(Em* e, TokKind op, int rd, int ra, uint64_t imm, SType* t) {
    if (op != T_PLUS && op != T_MINUS) return 0;
    int bits = scalar_bits(t), sf = bits == 64;
    int checked = e->o.mode == EMIT_DEBUG && is_intt(t);
    if (checked && bits < 32) return 0;
    uint32_t base = op == T_PLUS ? (checked ? 0x31000000u : 0x11000000u)
                                 : (checked ? 0x71000000u : 0x51000000u);
    ins(e, (sf ? 0x80000000u : 0) | base | ((uint32_t)imm << 10) | ((uint32_t)ra << 5) | (uint32_t)rd);
    if (!checked) narrow_wrap(e, rd, t);
    if (checked) {
        int ok = new_label(e);
        bcond_(e, ty_is_signed(t) ? 7 /* VC */ : (op == T_PLUS ? C_LO : C_HS), ok);
        emit_arithmetic_fail_call(e);
        bind_label(e, ok);
    }
    return 1;
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

// rd = ra and/or/xor constant as one instruction; 0 when it does not encode
static int emit_logical_imm(Em* e, TokKind op, int rd, int ra, uint64_t c, SType* t) {
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
    ins(e, (sf ? 0x80000000u : 0) | base | (field << 10) | ((uint32_t)ra << 5) | (uint32_t)rd);
    return 1;
}

static void emit_cmp_imm(Em* e, int sf, int ra, uint64_t imm) {
    ins(e, (sf ? 0xF100001Fu : 0x7100001Fu) | ((uint32_t)imm << 10) | ((uint32_t)ra << 5));
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
 * unsigned machine-address add/subtract. Debug mode keeps all three local
 * preconditions at the operation site; release is only multiply plus add/sub. */
static void emit_pointer_arith(Em* e, TokKind op, int pointer, int offset, uint32_t elem_size) {
    int rp = xreg(pointer), ro = xreg(offset);
    if (e->o.mode == EMIT_DEBUG) {
        int nonnull = new_label(e);
        cbz_(e, 1, rp, 1, nonnull);
        emit_arithmetic_fail_call(e);
        bind_label(e, nonnull);
    }
    if (elem_size != 1) {
        load_imm(e, 1, 16, elem_size);
        if (e->o.mode == EMIT_DEBUG) {
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
    if (e->o.mode == EMIT_DEBUG) {
        int valid = new_label(e);
        bcond_(e, op == T_PLUS ? C_LO : C_HS, valid);
        emit_arithmetic_fail_call(e);
        bind_label(e, valid);
    }
}

// float arithmetic between raw v registers
static void emit_farith_raw(Em* e, TokKind op, uint32_t rd, uint32_t ra, uint32_t rb, int is64) {
    uint32_t s = is64 ? 0x00400000u : 0;
    switch (op) {
    case T_PLUS:  ins(e, 0x1E202800u | s | (rb << 16) | (ra << 5) | rd); return;
    case T_MINUS: ins(e, 0x1E203800u | s | (rb << 16) | (ra << 5) | rd); return;
    case T_STAR:  ins(e, 0x1E200800u | s | (rb << 16) | (ra << 5) | rd); return;
    case T_SLASH: ins(e, 0x1E201800u | s | (rb << 16) | (ra << 5) | rd); return;
    default: return;
    }
}

static void emit_farith(Em* e, TokKind op, int dst, int a, int b, int is64) {
    emit_farith_raw(e, op, (uint32_t)vreg(e, dst), (uint32_t)vreg(e, a), (uint32_t)vreg(e, b), is64);
}

static void emit_fcmp(Em* e, int a, int b, int is64) {
    ins(e, (is64 ? 0x1E602000u : 0x1E202000u) | (vreg(e, b) << 16) | (vreg(e, a) << 5));
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

static EV gen_byte_sequence_eq(Em* e, Node* n, EV a, SType* at, EV b, SType* bt, int eq);

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
    int* variants = (int*)arena_alloc((size_t)(ut->nfields ? ut->nfields : 1) * sizeof(int));
    int* labs = (int*)arena_alloc((size_t)(ut->nfields ? ut->nfields : 1) * sizeof(int));
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
            ins(e, 0x91000000u | (poff << 10) | ((uint32_t)xreg(a.idx) << 5) | (uint32_t)xreg(pa));
            ins(e, 0x91000000u | (poff << 10) | ((uint32_t)xreg(b.idx) << 5) | (uint32_t)xreg(pb));
            EV nested = gen_union_eq(e, n, (EV){ CLS_ADDR, pa }, (EV){ CLS_ADDR, pb }, unwrap(pt), 1);
            t2 = nested.idx;
        } else if (is_floatt(pt)) {
            t2 = tmp_int(e);
            int is64 = unwrap(pt)->prim == P_F64;
            int v0 = tmp_fp(e), v1 = tmp_fp(e);
            ldrf_(e, vreg(e, v0), xreg(a.idx), poff, is64);
            ldrf_(e, vreg(e, v1), xreg(b.idx), poff, is64);
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
                ins(e, (bw == 8 ? 0xEB00001Fu : 0x6B00001Fu) | (17u << 16) | (16u << 5));
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
 * escapes, so no operand evaluated in between can change it; a register-homed
 * parameter leaf is read-only and read the same way. Only consumers that never
 * write their operand registers may call this.
 */
static int src_reg(Em* e, Node* x, EV* v) {
    Sym* s = x->kind == N_IDENT ? node_sym(x) : NULL;
    if (s && s->kind == SYM_VAR && s->type && is_scalar(s->type) && !is_floatt(s->type)) {
        int pr = promo_reg(e, s);
        if (pr >= 0) {
            *v = (EV){ CLS_INT, -1 };
            return pr;
        }
    }
    int home = x->kind == N_FIELD_E ? param_home(e, x) : -1;
    if (home < 0) home = split_reg(e, x); // a split local's field, exact as a promoted local
    if (home >= 0 && !is_floatt(node_ty(x))) { // a homed parameter leaf is read-only, so equally exact
        *v = (EV){ CLS_INT, -1 };
        return home;
    }
    *v = gen_expr(e, x);
    return v->cls == CLS_FP ? vreg(e, v->idx) : xreg(v->idx);
}

/*
 * The float twin of src_reg: a promoted float local or a register-homed
 * parameter leaf is read in its own v register (v->idx = -1), anything else
 * evaluates into a temp. Exact for the same reasons, and only consumers that
 * never write their operand registers may call it. The caller checks
 * v->cls == CLS_FP.
 */
static int fsrc_reg(Em* e, Node* x, EV* v) {
    Sym* s = x->kind == N_IDENT ? node_sym(x) : NULL;
    if (s && s->kind == SYM_VAR && s->type && is_floatt(s->type)) {
        int pr = promo_reg(e, s);
        if (pr >= 0) {
            *v = (EV){ CLS_FP, -1 };
            return pr;
        }
    }
    int home = x->kind == N_FIELD_E ? param_home(e, x) : -1;
    if (home < 0) home = split_reg(e, x);
    if (home >= 0 && is_floatt(node_ty(x))) {
        *v = (EV){ CLS_FP, -1 };
        return home;
    }
    *v = gen_expr(e, x);
    return vreg(e, v->idx);
}

static int is_farith_op(TokKind op) {
    return op == T_PLUS || op == T_MINUS || op == T_STAR || op == T_SLASH;
}

static EV gen_binop(Em* e, Node* n) {
    TokKind op = (TokKind)n->op;
    SType* t = node_ty(n->a);
    if (!t || t->kind == SK_UT || t->kind == SK_ERR) {
        em_err(e, n, "untyped operand reached codegen");
        return (EV){ CLS_INT, tmp_int(e) };
    }

    // null comparison: x == null / x != null
    if ((op == T_EQEQ || op == T_BANGEQ) && ((n->b->kind == N_NULL) != (n->a->kind == N_NULL))) {
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
    if (is_scalar(t) && !is_floatt(t) && !(t->kind == SK_PTR && (op == T_PLUS || op == T_MINUS))) {
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
        if ((op == T_AMP || op == T_PIPE || op == T_CARET) && const_int(n->b, &imm)) {
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
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | ((uint32_t)rb << 16) | ((uint32_t)ra << 5));
            cset_(e, sf, cond, xreg(d));
        } else {
            emit_checked_arith(e, op, xreg(d), ra, rb, t);
        }
        e->vti = d + 1;
        return (EV){ CLS_INT, d };
    }

    // float arithmetic reads promoted locals and homed parameter leaves in place
    if (is_floatt(t) && is_farith_op(op)) {
        EV a, b;
        int ra = fsrc_reg(e, n->a, &a);
        int rb = fsrc_reg(e, n->b, &b);
        if (a.cls != CLS_FP || b.cls != CLS_FP) {
            em_err(e, n, "float operand did not produce a float");
            return (EV){ CLS_INT, tmp_int(e) };
        }
        int d = a.idx >= 0 ? a.idx : b.idx >= 0 ? b.idx : tmp_fp(e);
        emit_farith_raw(e, op, (uint32_t)vreg(e, d), (uint32_t)ra, (uint32_t)rb, unwrap(t)->prim == P_F64);
        e->vtf = d + 1;
        return (EV){ CLS_FP, d };
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
            ins(e, (eq ? 0x0A000000u : 0x2A000000u) | (xreg(b.idx) << 16) | (xreg(a.idx) << 5) | (uint32_t)xreg(a.idx));
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
        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (xreg(b.idx) << 16) | (xreg(a.idx) << 5));
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

// whether gen_expr leaves x as exactly 0 or 1 (a cset or a constant)
static int bool_exact(Node* x) {
    switch (x->kind) {
    case N_BOOL: case N_CMPCHAIN:
        return 1;
    case N_UNARY:
        return x->op == T_BANG;
    case N_BINARY: {
        if (x->op == T_ANDAND || x->op == T_OROR) return 1;
        SType* t = node_ty(x->a);
        return t && is_scalar(t) && tok_cond((TokKind)x->op, t) >= 0;
    }
    default:
        return 0;
    }
}

/*
 * A && b as a value: the left operand branches on its flags as a condition
 * (gen_branch) to the short-circuit result, and the right operand's own 0 or
 * 1 is the result otherwise, so only the last compare materializes a bool.
 */
static EV gen_shortcircuit(Em* e, Node* n) {
    int is_and = n->op == T_ANDAND;
    int si = e->vti;
    int lf = new_label(e), lend = new_label(e);
    gen_branch(e, n->a, !is_and, lf); // AND: false short-circuits; OR: true does
    EV b = gen_expr(e, n->b);
    if (b.cls != CLS_INT) {
        em_err(e, n->b, "short-circuit operands must be bool");
        return b;
    }
    if (!bool_exact(n->b)) { // a stored bool: test it like `!` does
        ins(e, 0x7100001Fu | ((uint32_t)xreg(b.idx) << 5)); // cmp w, #0
        cset_(e, 0, C_NE, xreg(si));
    } else if (b.idx != si) {
        mov_int(e, 0, xreg(si), xreg(b.idx));
    }
    e->vti = si + 1;
    br_(e, lend);
    bind_label(e, lf);
    load_imm(e, 0, xreg(si), !is_and);
    bind_label(e, lend);
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
        for (int i = 0; i < nops; i++) if (!use_imm || i < nops - 1) r[i] = src_reg(e, n->kids[i], &v[i]);
        int d = tmp_int(e), sf = scalar_bits(t) == 64;
        for (int i = 1; i < nops; i++) {
            int cond = tok_cond((TokKind)n->a->kids[i - 1]->op, t);
            if (cond < 0) cond = C_EQ;
            if (use_imm && i == nops - 1) emit_cmp_imm(e, sf, r[i - 1], imm);
            else ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | ((uint32_t)r[i] << 16) | ((uint32_t)r[i - 1] << 5));
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
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (xreg(obase + i) << 16) | (xreg(obase + i - 1) << 5));
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
            for (int i = 0; i < x->nkids; i++) if (!use_imm || i < last) r[i] = src_reg(e, x->kids[i], &v[i]);
            int skip = jump_if && x->nkids > 2 ? new_label(e) : -1;
            for (int i = 1; i < x->nkids; i++) {
                int cond = tok_cond((TokKind)x->a->kids[i - 1]->op, t);
                if (cond < 0) cond = C_EQ;
                if (isf) emit_fcmp(e, v[i - 1].idx, v[i].idx, unwrap(t)->prim == P_F64);
                else if (use_imm && i == last) emit_cmp_imm(e, sf, r[i - 1], imm);
                else ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | ((uint32_t)r[i] << 16) | ((uint32_t)r[i - 1] << 5));
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
                ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | ((uint32_t)rb << 16) | ((uint32_t)ra << 5));
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

static void split_take(Em* e, Split* sp, RetCls* rc);

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
    fp_leaves(e, 0);
    ins(e, 0x94000000u); // bl .+0
    ESym* t = sym_intern(&e->o.syms, p, len);
    fix_add(e, FK_BRANCH, (uint32_t)(text(e)->len - 4), t);
    fp_leaves(e, 1);
}

static void emit_bl_extern(Em* e, const char* p, uint32_t len) {
    fp_leaves(e, 0);
    ins(e, 0x94000000u);
    ESym* t = sym_intern(&e->o.syms, p, len);
    reloc_add(&e->o.secs[SEC_TEXT], (uint32_t)(text(e)->len - 4), t, 1, 2, 1, RM_BRANCH26);
    peep_barrier(e);
    fp_leaves(e, 1);
}

static void addr_of_sym(Em* e, int rd, ESym* sym);

/*
 * Write "path:line:col: <what>" for the node at and abort. The shared
 * __ace$bounds_fail helper writes whatever message it is handed.
 */
static void emit_located_fail_call(Em* e, Node* at, const char* what) {
    e->o.bounds_fail_used = 1;
    uint32_t n;
    ESym* msg = obj_located(&e->o, at, what, &n);
    load_imm(e, 1, 0, 2);
    addr_of_sym(e, 1, msg);
    load_imm(e, 1, 2, (uint64_t)n);
    emit_bl_internal(e, "__ace$bounds_fail", 17);
}

static void emit_bounds_fail_call(Em* e, Node* at) {
    emit_located_fail_call(e, at, "bounds violation");
}

static void emit_arithmetic_fail_call(Em* e) {
    e->o.arithmetic_fail_used = 1;
    emit_bl_internal(e, "__ace$arithmetic_fail", 21);
}

static void emit_contract_fail_call(Em* e) {
    e->o.contract_fail_used = 1;
    emit_bl_internal(e, "__ace$contract_fail", 19);
}

static void emit_null_fail_call(Em* e) {
    e->o.null_fail_used = 1;
    emit_bl_internal(e, "__ace$null_fail", 15);
}

static void emit_null_guard(Em* e, int reg) {
    if (e->o.mode != EMIT_DEBUG) return;
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
static void emit_index_guard(Em* e, Node* access, int index_reg, int length_reg, uint64_t array_length) {
    if (e->o.mode != EMIT_DEBUG || (access->flags & F_BOUNDS_PROVEN)) return;
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
    if (e->o.mode != EMIT_DEBUG || (slice->flags & F_BOUNDS_PROVEN) ||
        !bound || (proof & bit) || !signed_index(bound)) return;
    int ok = new_label(e);
    int sf = scalar_bits(node_ty(bound)) == 64;
    ins(e, (sf ? 0xF100001Fu : 0x7100001Fu) | ((uint32_t)reg << 5));
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
    // No x9-x14 interval may span this bl. Exit-time defer calls are exempt:
    // nothing promoted is read after the function exits.
    if (e->nxpreg && !e->in_defers) {
        uintptr_t cp = at ? ptrmap_get(&e->call_pos, at) : 0;
        if (!cp || leaf_spanned(e, (uint32_t)(cp - 1))) { // or a call promo_scan never saw
            e->o.d->errors++;
            fprintf(stderr, "ace: call inside a caller-saved promoted local's live range\n");
        }
    }
    int si = e->vti, sf = e->vtf;
    spill_temps(e);

    // effective parameter types (variadic extras materialize to i64/f64)
    SType* eff[40];
    int n = nargs < 40 ? nargs : 40;
    if (nargs > 40) em_err(e, at, "too many arguments (max 40)");
    for (int i = 0; i < n; i++) eff[i] = i < nparams ? params[i]
              : is_floatt(node_ty(args[i].node)) ? default_f64()
                                                 : default_i64();

    TU units[160];
    uint32_t sbytes = assign_cc(eff, muts, n, internal, varargs_from, units);
    if (sbytes > e->out_max) e->out_max = sbytes;

    // by-value aggregate arguments read straight from a frame slot, by depth,
    // or built from a split local's fields
    uint8_t framed[40] = {0};
    uint32_t fdepths[40] = {0};
    Split* splits[40] = {0};

    // evaluate arguments into temps
    for (int i = 0; i < n; i++) {
        if (args[i].pre) continue;
        Node* a = args[i].node;
        uint32_t arg_size = ty_size(eff[i]);
        uint32_t hfa_offs[5];
        uint8_t hfa_dbl[5];
        int is_hfa = classify_hfa(eff[i], hfa_offs, hfa_dbl) >= 0;
        int byref = (internal && muts && i < nparams && muts[i]) || (arg_size > 16 && !is_hfa);
        Split* sp = a && node_ty(a) && ty_size(node_ty(a)) == arg_size ? split_live(e, a) : NULL;
        if (sp) {
            int regs = !byref;
            for (int w = 0; regs && w < 4; w++) {
                TU* u = &units[i * 4 + w];
                if (u->kind == TU_STACK || u->kind == TU_STACK_AGGR) regs = 0;
            }
            if (regs) { // units built from the fields at marshal time
                splits[i] = sp;
                args[i].val = (EV){ CLS_FP, 0 }; // never spilled or compared
                continue;
            }
            // a copy in memory: its address travels, or its bytes are staged
            uint32_t copy = temp_slot(e, ty_size(sp->type), ty_align(sp->type));
            int p = tmp_int(e);
            frame_addr(e, xreg(p), copy);
            split_store_at(e, sp, xreg(p), 0);
            args[i].val = (EV){ byref ? CLS_INT : CLS_ADDR, p };
            continue;
        }
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
                if (v.cls != CLS_ADDR) em_err(e, a, "aggregate argument must materialize in memory");
                args[i].val = v;
                args[i].ptrval = 1; // the materialized address is what travels
            }
            continue;
        }
        // a by-value aggregate that is a plain frame-resident local and rides
        // registers is loaded from its slot at marshal time: no address temp
        uint32_t fdepth;
        if (!is_scalar(eff[i]) && units[i * 4].kind != TU_STACK_AGGR && (a->kind == N_IDENT || a->kind == N_FIELD_E) &&
            !(e->o.mode == EMIT_DEBUG && eff[i]->kind == SK_SLICE) && node_ty(a) && ty_size(node_ty(a)) == arg_size &&
            frame_place(e, a, &fdepth)) {
            framed[i] = 1;
            fdepths[i] = fdepth;
            args[i].val = (EV){ CLS_FP, 0 }; // never spilled or compared
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
    if (e->o.mode == EMIT_DEBUG && decl) {
        int guard_ni = e->vti, guard_nf = e->vtf;
        spill_temps(e);
        uint32_t ranges[40];
        memset(ranges, 0, sizeof ranges);
        for (int i = 0; i < n && i < decl->nkids; i++) {
            SType* pt = eff[i];
            if (!pt || pt->kind != SK_SLICE || args[i].val.cls != CLS_ADDR) continue;
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
                if (!ranges[j] || !((decl->kids[i]->flags | decl->kids[j]->flags) & F_NOALIAS)) continue;
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
        for (int w = 1; w < 4; w++) if (units[i * 4 + w].kind != TU_XREG || units[i * 4 + w].reg >= 0) single = 0;
        for (int j = 0; j < n; j++) if (j != i && args[j].val.cls != CLS_FP && args[j].val.idx == v.idx) single = 0;
        if (tgt.kind == TK_REG && tgt.reg == v.idx) single = 0;
        if (!single) continue;
        placed[i] = 1;
        if (v.idx >= si) skip_spill[v.idx] = 1;
    }
    for (int t = si; t < e->vti; t++) if (!(t < 64 && skip_spill[t])) store_frame(e, xreg(t), spill_slot_int(e, t), 8);
    for (int j = 0; j < e->vtf; j++) store_frame_fp(e, vreg(e, j), spill_slot_fp(e, j), 1);
    for (int i = 0; i < n; i++) {
        if (placed[i]) continue;
        if (splits[i]) {
            for (int w = 0; w < 4; w++) {
                TU* u = &units[i * 4 + w];
                if ((u->kind == TU_XREG && u->reg >= 0) || u->kind == TU_VREG) split_unit(e, splits[i], u, u->reg);
            }
            continue;
        }
        if (framed[i]) { // aggregate leaves straight from their frame slot
            for (int w = 0; w < 4; w++) {
                TU* u = &units[i * 4 + w];
                if (u->kind == TU_XREG && u->reg >= 0) load_frame(e, u->reg, fdepths[i] - u->off, 8, 0);
                else if (u->kind == TU_VREG) load_frame_fp(e, u->reg, fdepths[i] - u->off, u->size == 8);
            }
            continue;
        }
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
                if (v.cls == CLS_FP) load_frame_fp(e, u->reg, spill_slot_fp(e, v.idx), u->size == 8);
                else // HFA leaf
                    ldrf_(e, u->reg, 17, u->off, u->size == 8);
            } else if (u->kind == TU_STACK) {
                if (v.cls == CLS_FP) {
                    // v0-v7 may already hold earlier arguments; use a
                    // non-argument scratch register for a spilled FP value.
                    load_frame_fp(e, 16, spill_slot_fp(e, v.idx), 1);
                    strf_(e, 16, 31, u->soff, u->size == 8);
                    continue;
                } else if (is_agg) ld_reg(e, 16, 17, u->off, 8, 0);
                else load_frame(e, 16, aslot, 8, 0);
                uint32_t scale = u->size >= 4 ? u->size : 4;
                ins(e, (u->size == 8 ? 0xF9000000u : u->size == 4 ? 0xB9000000u :
                       0x39000000u) |
                       ((u->soff / scale) << 10) | (31u << 5) | 16u);
            } else { // TU_STACK_AGGR: small composite/HFA passed on stack
                load_frame(e, 14, aslot, 8, 0); // base outside copy_mem's x15-x17
                if (u->soff <= 4095) ins(e, 0x91000000u | (u->soff << 10) | (31u << 5) | 15u);
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

    // a returned self call: the arguments sit where the parameter reads
    // expect them, so branch back to those reads in this frame
    if (at && at == e->self_call) {
        if (tgt.kind != TK_INTERNAL || tgt.nlen != e->fnnlen || memcmp(tgt.name, e->fnname, tgt.nlen) != 0)
            em_err(e, at, "self call resolved to another target");
        br_(e, e->self_entry);
        e->vti = si; e->vtf = sf;
        return (EV){ CLS_INT, 0 };
    }

    // the call itself
    if (tgt.kind == TK_INTERNAL) emit_bl_internal(e, tgt.name, tgt.nlen);
    else if (tgt.kind == TK_EXTERN) emit_bl_extern(e, tgt.name, tgt.nlen);
    else { // target reloaded from its spill slot (marshaling cannot clobber it)
        load_frame(e, 16, spill_slot_int(e, tgt.reg), 8, 0);
        emit_null_guard(e, 16);
        fp_leaves(e, 0);
        ins(e, 0xD63F0000u | (16u << 5)); // blr x16
        fp_leaves(e, 1);
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
    } else if (rc.nunits >= 1 && e->sink_call == at && e->sink_ret == ret && e->sink_n == rc.nunits) {
        // the statement gave this call's result a frame destination or split local
        if (e->sink_split) split_take(e, e->sink_split, &rc);
        else for (int w = 0; w < rc.nunits; w++) {
            if (rc.units[w].kind == TU_XREG) store_frame(e, rc.units[w].reg, e->sink_off[w], 8);
            else store_frame_fp(e, rc.units[w].reg, e->sink_off[w], rc.units[w].size == 8);
        }
        e->sink_done = 1;
        reload_temps(e, si, sf);
        e->vti = si; e->vtf = sf;
    } else if (rc.nunits >= 1) {
        // <=16B aggregate or multi-chunk return: materialize into a frame slot
        uint32_t off = temp_slot(e, align_up(ty_size(ret), 8), ty_align(ret));
        for (int w = 0; w < rc.nunits; w++) {
            // Slots grow down from x29, while aggregate offsets grow upward.
            frame_addr(e, 16, off - rc.units[w].off);
            if (rc.units[w].kind == TU_XREG) st_reg(e, rc.units[w].reg, 16, 0, 8);
            else strf_(e, rc.units[w].reg, 16, 0, rc.units[w].size == 8);
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

static int byte_sequence_parts(Em* e, Node* at, EV value, SType* t, int* ptr, int* len) {
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
static EV gen_byte_sequence_eq(Em* e, Node* n, EV a, SType* at, EV b, SType* bt, int eq) {
    int aptr, alen, bptr, blen;
    byte_sequence_parts(e, n->a ? n->a : n, a, at, &aptr, &alen);
    byte_sequence_parts(e, n->b ? n->b : n, b, bt, &bptr, &blen);

    int out = tmp_int(e);
    load_imm(e, 0, xreg(out), 0);
    int done = new_label(e);
    ins(e, 0xEB00001Fu | ((uint32_t)xreg(blen) << 16) |
           ((uint32_t)xreg(alen) << 5)); // cmp alen, blen
    bcond_(e, C_NE, done);
    e->o.mem_compare_used = 1;
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
                uint64_t packed = ((uint64_t)dt->type_id << 32) | (uint32_t)entry->ival;
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
            return sym_intern(&e->o.syms, e->globs[i].name, e->globs[i].len);
    return NULL;
}

// adrp rd, sym@PAGE ; add rd, rd, sym@PAGEOFF
static void addr_of_sym(Em* e, int rd, ESym* sym) {
    uint32_t site = (uint32_t)text(e)->len;
    ins(e, 0x90000000u | ((uint32_t)rd & 31u));
    reloc_add(&e->o.secs[SEC_TEXT], site, sym, 1, 2, 1, RM_PAGE21);
    peep_barrier(e);
    site = (uint32_t)text(e)->len;
    ins(e, 0x91000000u | ((uint32_t)rd << 5) | ((uint32_t)rd & 31u));
    reloc_add(&e->o.secs[SEC_TEXT], site, sym, 0, 2, 1, RM_PAGEOFF12);
    peep_barrier(e);
}

// a binder's promotion register assignment, or NULL
static PromoAsg* pasg_find(Em* e, Node* decl) {
    if (e->npasg <= PASG_SCAN) {
        for (int i = 0; i < e->npasg; i++) if (e->pasg[i].decl == decl) return &e->pasg[i];
        return NULL;
    }
    uintptr_t ix = ptrmap_get(&e->pasg_ix, decl);
    return ix ? &e->pasg[ix - 1] : NULL;
}

/*
 * Register holding a promoted local, or -1 for slot storage: an x register
 * number for integer-class locals (x19 up, or x14 down), a v register number
 * (8-15, 24-31) for float locals.
 */
static int promo_reg(Em* e, Sym* s) {
    if (!s || !s->decl) return -1;
    PromoAsg* a = pasg_find(e, s->decl);
    if (a && !a->byref) return a->reg;
    for (int i = 0; i < e->nfpreg; i++)
        if (e->fpreg_decl[i] == s->decl) return i < FP_PROMO_REGS ? 8 + i : 24 + (i - FP_PROMO_REGS);
    return -1;
}

// x register holding a by-reference parameter's pointer, or -1 for its slot
static int bref_reg(Em* e, Sym* s) {
    if (!s || !s->decl) return -1;
    PromoAsg* a = pasg_find(e, s->decl);
    return a && a->byref ? a->reg : -1;
}

// the split record of a struct binder, or NULL
static Split* split_at(Em* e, Node* decl) {
    uintptr_t ix = decl ? ptrmap_get(&e->split_ix, decl) : 0;
    return ix ? &e->splits[ix - 1] : NULL;
}

// the split record of the struct local x names, or NULL
static Split* split_of(Em* e, Node* x) {
    Sym* s = x && x->kind == N_IDENT ? node_sym(x) : NULL;
    return s && s->kind == SYM_VAR ? split_at(e, s->decl) : NULL;
}

// a struct local emitted field by field: some field lives in a register
static Split* split_live(Em* e, Node* x) {
    Split* sp = split_of(e, x);
    return sp && sp->regs ? sp : NULL;
}

// register holding field f of a split local, or -1 for the struct's slot
static int split_freg(Em* e, Split* sp, int f) {
    return promo_reg(e, node_sym(sp->field[f]));
}

// register holding the field `x.f` names over a split local, or -1
static int split_reg(Em* e, Node* x) {
    if (!x || x->kind != N_FIELD_E) return -1;
    Split* sp = split_live(e, x->a);
    int f = sp ? find_field(sp->type, x->str, x->len) : -1;
    return f >= 0 ? split_freg(e, sp, f) : -1;
}

// fmov between raw v registers (mov_fp takes temp indices)
static void fmov_raw(Em* e, int is64, int dst, int src) {
    if (dst == src) return;
    ins(e, (is64 ? 0x1E604000u : 0x1E204000u) | ((uint32_t)src << 5) | (uint32_t)dst);
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
        ins(e, (ty_is_signed(t) ? 0x13000000u : 0x53000000u) | (imms << 10) | ((uint32_t)src << 5) | (uint32_t)r);
    }
}

/*
 * Retarget the instruction just emitted, which produced dead register src, to
 * write promoted register r directly instead of copying after it. Only when
 * no label is bound here (a branch could arrive with src already written),
 * the instruction takes no input from its destination (not movk, bfm, or
 * ccmp/ccmn, whose low bits are flags), and its result already has exactly
 * the form set_preg would leave at the type's width.
 */
static int fold_preg(Em* e, int r, int src, SType* t) {
    Buf* b = text(e);
    if (b->len < 4 || e->bound_at == (uint32_t)b->len || src == 31) return 0;
    uint32_t x;
    memcpy(&x, b->d + b->len - 4, 4);
    if ((int)(x & 31u) != src) return 0;
    int dpi = ((x >> 26) & 7u) == 4u, dpr = ((x >> 25) & 7u) == 5u;
    if (dpi && ((x >> 24) & 3u) == 0u) return 0;                             // adr/adrp
    if (dpi && ((x >> 23) & 7u) == 5u && ((x >> 29) & 3u) == 3u) return 0;   // movk
    if (dpi && ((x >> 23) & 7u) == 6u && ((x >> 29) & 3u) == 1u) return 0;   // bfm
    if (dpr && (x & 0x1FE00000u) == 0x1A400000u) return 0;                   // ccmp/ccmn
    uint32_t size = x >> 30, opc = (x >> 22) & 3u, mode = (x >> 10) & 3u;
    int ldst = (x & 0x3B000000u) == 0x39000000u ||                           // unsigned offset
               ((x & 0x3B000000u) == 0x38000000u &&                           // ldur, register offset
                (mode == 0u ? !((x >> 21) & 1u) : mode == 2u && ((x >> 21) & 1u)));
    int load = ldst && !((x >> 26) & 1u) && opc != 0u && !(size == 3u && opc == 2u); // not prfm
    if (!dpi && !dpr && !load) return 0;
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    int ok;
    if (w >= 8) ok = 1;
    else if (w == 4) ok = load ? (opc == 1u && size <= 2u) || (opc == 3u && size <= 1u) : !(x >> 31);
    else if (load) ok = (ty_is_signed(t) ? opc == 3u : opc == 1u) && size + 1u <= w;
    else if ((x & 0xFFFF0FE0u) == 0x1A9F07E0u) ok = 1;                        // cset: 0 or 1
    else if ((x & 0xFFE00000u) == 0x52800000u) {                               // movz w, #imm16
        uint32_t imm = (x >> 5) & 0xFFFFu;
        ok = imm < (ty_is_signed(t) ? (w == 1 ? 0x80u : 0x8000u) : (w == 1 ? 0x100u : 0x10000u));
    } else ok = 0;
    if (!ok) return 0;
    x = (x & ~31u) | (uint32_t)r;
    memcpy(b->d + b->len - 4, &x, 4);
    return 1;
}

/*
 * fold_preg's float counterpart: retarget the FP instruction just emitted,
 * which produced dead v register src, to write v register r. Only a scalar
 * FP data-processing op, fmov immediate, int-to-float conversion, or FP load,
 * none of which reads its destination.
 */
static int fold_fp(Em* e, int r, int src) {
    Buf* b = text(e);
    if (b->len < 4 || e->bound_at == (uint32_t)b->len) return 0;
    uint32_t x;
    memcpy(&x, b->d + b->len - 4, 4);
    if ((int)(x & 31u) != src) return 0;
    int op = (x & 0xFF200C00u) == 0x1E200800u ||   // 2-source
             (x & 0xFF207C00u) == 0x1E204000u ||   // 1-source
             (x & 0xFF000000u) == 0x1F000000u ||   // 3-source
             (x & 0xFF201FE0u) == 0x1E201000u ||   // fmov immediate
             (x & 0x7F3E0000u) == 0x1E220000u ||   // scvtf/ucvtf
             (x & 0x7F3FFC00u) == 0x1E270000u ||   // fmov from a general register
             (x & 0x3F000000u) == 0x1C000000u ||   // ldr, literal
             (x & 0x3F400000u) == 0x3D400000u ||   // ldr, unsigned offset
             (x & 0x3F600C00u) == 0x3C400000u ||   // ldur
             (x & 0x3F600C00u) == 0x3C600800u;     // ldr, register offset
    if (!op) return 0;
    x = (x & ~31u) | (uint32_t)r;
    if ((x & 0xFF3FFC00u) == 0x1E204000u && (int)((x >> 5) & 31u) == r &&
        e->peep_bar < (uint32_t)b->len - 4) { // fmov r, r: drop it
        b->len -= 4;
        return 1;
    }
    memcpy(b->d + b->len - 4, &x, 4);
    return 1;
}

// store a value into promoted register r; v's temp is dead afterwards
static void store_preg(Em* e, int r, EV v, SType* t) {
    if (v.cls == CLS_ADDR) v = load_scalar(e, xreg(v.idx), 0, t);
    if (!is_floatt(t)) {
        if (!fold_preg(e, r, xreg(v.idx), t)) set_preg(e, r, xreg(v.idx), t);
        return;
    }
    int is64 = unwrap(t)->prim == P_F64;
    if (v.cls == CLS_FP) // fmov s keeps the low 32 bits, as a slot would
        fmov_raw(e, is64, r, vreg(e, v.idx));
    else // raw bits in an int temp: fmov d, x / s, w
        ins(e, (is64 ? 0x9E670000u : 0x1E270000u) |
               ((uint32_t)xreg(v.idx) << 5) | (uint32_t)r);
}

// a promoted sequence-loop value binder, or a split local's field, takes
// the scalar at [p+off] directly
static void load_binder(Em* e, int r, int p, uint32_t off, SType* t) {
    if (is_floatt(t)) {
        ldrf_(e, r, p, off, unwrap(t)->prim == P_F64);
        return;
    }
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    ld_reg(e, r, p, off, w, ty_is_signed(t) && w < 4); // ldrsb/ldrsh w, as a slot reload
}

// address of a variable's storage in a fresh int temp
static int gen_var_addr(Em* e, Sym* s, Node* at) {
    int t = tmp_int(e);
    Split* sp = s->kind == SYM_VAR ? split_at(e, s->decl) : NULL;
    if (promo_reg(e, s) >= 0 || (sp && sp->regs)) { // plan_promotion missed an escape
        em_err(e, at, "internal compiler error: address of register local '%.*s'", (int)s->len, s->name);
        return t;
    }
    ESym* g = global_sym(e, s);
    if (g) {
        addr_of_sym(e, xreg(t), g);
        return t;
    }
    int br = bref_reg(e, s);
    if (br >= 0) { // the pointer lives in a register
        mov_int(e, 1, xreg(t), br);
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
    else frame_addr(e, xreg(t), sl->off);
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
        load_frame_fp(e, vreg(e, vt), depth, unwrap(t)->prim == P_F64);
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
        if (!s || s->kind != SYM_VAR || promo_reg(e, s) >= 0 || split_live(e, x)) return 0;
        Slot* sl = local_slot(e, s);
        if (!sl || sl->byref) return 0;
        *depth = sl->off;
        return 1;
    }
    if (x->kind != N_FIELD_E || !is_lvalue_node(x)) return 0;
    Split* sp = split_live(e, x->a);
    if (sp) { // a field left in the struct's slot, which exists only for such fields
        int f = find_field(sp->type, x->str, x->len);
        Slot* sl = local_slot(e, node_sym(x->a));
        if (f < 0 || split_freg(e, sp, f) >= 0 || !sl) return 0;
        *depth = sl->off - field_off(sp->type, f);
        return 1;
    }
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
 * A read-only by-value struct parameter that arrives in registers and whose
 * every use is a constant field read of one whole leaf keeps the leaves it
 * reads in registers: float leaves of an HFA in the argument registers they
 * arrive in, 8-byte integer leaves of a struct of at most 16 bytes in x14
 * downward, copied once from theirs, and never spilled to a slot. `p.f.g` over
 * struct fields resolves to its base declaration and byte offset.
 */
static Node* param_chain(Em* e, Node* x, uint32_t* off) {
    *off = 0;
    while (x && x->kind == N_FIELD_E) {
        SType* bt = lvalue_type(e, x->a);
        if (!bt) bt = node_ty(x->a);
        bt = bt ? unwrap(bt) : NULL;
        if (!bt || bt->kind != SK_STRUCT) return NULL;
        int fi = find_field(bt, x->str, x->len);
        if (fi < 0) return NULL;
        *off += field_off(bt, fi);
        x = x->a;
    }
    Sym* s = x && x->kind == N_IDENT ? node_sym(x) : NULL;
    return s && s->kind == SYM_VAR && !s->is_global ? s->decl : NULL;
}

static int param_reg(Em* e, Node* decl, uint32_t off) {
    for (int i = 0; i < e->nphome; i++)
        if (e->phome[i].decl == decl && e->phome[i].off == off) return e->phome[i].reg;
    return -1;
}

// a whole 8-byte integer-class leaf: the shape an x-register home holds
static int int_leaf(SType* t) {
    return t && is_scalar(t) && !is_floatt(t) && ty_size(t) == 8;
}

// register holding the leaf a field expression names (v for float, x for integer), or -1
static int param_home(Em* e, Node* x) {
    if (!e->nphome || x->kind != N_FIELD_E) return -1;
    SType* t = node_ty(x);
    if (!t) t = lvalue_type(e, x);
    uint32_t off;
    Node* d = t && (is_floatt(t) || int_leaf(t)) ? param_chain(e, x, &off) : NULL;
    return d ? param_reg(e, d, off) : -1;
}

/*
 * Scalar variable read without a separate address temp: a plain slot loads
 * directly (one ldur within x29 - 256); a by-reference parameter loads the
 * value through its pointer register, or loads its pointer from the slot and
 * then the value into the same register.
 */
// a promoted register's value, copied into a fresh temp
static EV load_preg(Em* e, int r, SType* t) {
    if (is_floatt(t)) {
        int vt = tmp_fp(e);
        fmov_raw(e, unwrap(t)->prim == P_F64, vreg(e, vt), r);
        return (EV){ CLS_FP, vt };
    }
    int it = tmp_int(e);
    mov_int(e, 1, xreg(it), r);
    return (EV){ CLS_INT, it };
}

static EV load_var(Em* e, Sym* s, SType* t, Node* at) {
    int pr = promo_reg(e, s);
    if (pr >= 0) return load_preg(e, pr, t);
    Slot* sl = local_slot(e, s);
    int br = bref_reg(e, s);
    if (!sl && br < 0) {
        int a = gen_var_addr(e, s, at);
        return load_in_place(e, a, 0, t);
    }
    if (br < 0 && !sl->byref) return load_frame_value(e, sl->off, t);
    int is64 = unwrap(t)->prim == P_F64;
    uint32_t w = (uint32_t)((scalar_bits(t) + 7) / 8);
    int signext = ty_is_signed(t) && w < 8;
    int it = tmp_int(e);
    int p = br >= 0 ? br : xreg(it);
    if (br < 0) load_frame(e, p, sl->off, 8, 0); // slot holds the pointer
    if (is_floatt(t)) {
        int vt = tmp_fp(e);
        ldrf_(e, vreg(e, vt), p, 0, is64);
        e->vti--;
        return (EV){ CLS_FP, vt };
    }
    ld_reg(e, xreg(it), p, 0, w, signext);
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
        if (x->a && x->a->kind == N_IDENT && node_sym(x->a) && node_sym(x->a)->kind == SYM_TYPE) return 0;
        if (x->a && x->a->kind == N_INDEX && node_ty(x->a) && node_ty(x->a)->kind == SK_UNION) return 0;
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

/*
 * Address of an indexed element. When fold_w is nonzero and equals the element
 * size (1, 2, 4, or 8), the scaled index add is left out: the result is the
 * sequence base and *fold_ri names the index register, still holding its value
 * until the caller's next instruction, which must load [base, index, lsl n].
 */
static int index_addr(Em* e, Node* x, uint32_t fold_w, int* fold_ri) {
    SType* bt = lvalue_type(e, x->a);
    bt = bt ? unwrap(bt) : NULL;
    SType* seq = bt;
    int descriptor = bt && (bt->kind == SK_SLICE || (bt->kind == SK_PRIM && bt->prim == P_STRING));
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
    int need_guard = e->o.mode == EMIT_DEBUG && !(x->flags & F_BOUNDS_PROVEN);
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
    int narrow = it && it->kind != SK_UT && it->kind != SK_ERR && scalar_bits(it) != 64;
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
    emit_index_guard(e, x, ri, lenreg, seq && seq->kind == SK_ARRAY ? seq->count : 0);
    if (e->o.mode != EMIT_DEBUG || (x->flags & F_BOUNDS_PROVEN)) widen_idx(e, x->b, ri);
    if (fold_w && esz == fold_w && (esz == 1 || esz == 2 || esz == 4 || esz == 8)) {
        *fold_ri = ri; // the consumer loads [ta, ri, lsl #log2(esz)]
    } else if (esz == 1) {
        ins(e, 0x8B000000u | ((uint32_t)ri << 16) | ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
    } else if ((esz & (esz - 1)) == 0) {
        int sh = 0;
        while ((1u << sh) < esz) sh++;
        ins(e, 0x8B000000u | ((uint32_t)sh << 10) |
               ((uint32_t)ri << 16) |
               ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
    } else {
        load_imm(e, 1, 16, esz);
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)ri << 5) | 16u);
        ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
    }
    e->vti = svi; e->vtf = svf; // index temp freed
    return ta;
}

/*
 * A field's address as a base register plus a constant byte offset, so a
 * scalar read can fold the offset into its load; gen_addr adds it instead.
 * The offset is zero for bit-field projections and error handles.
 */
static int field_base(Em* e, Node* x, uint32_t* off) {
    *off = 0;
    if (param_home(e, x) >= 0 || split_reg(e, x) >= 0) {
        em_err(e, x, "internal: register-held field has no address");
        return tmp_int(e);
    }
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
        if (is_lvalue_node(x->a)) ld_reg(e, xreg(ta), xreg(ta), 0, 8, 0);
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
    *off = field_off(bt, fi);
    return ta;
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

static int gen_generic_value(Em* e, Node* x) {
    const char* mn = fn_value_mangle(node_sym(x), x);
    if (!mn) {
        em_err(e, x, "generic stamp not found");
        return tmp_int(e);
    }
    int t = tmp_int(e);
    addr_of_sym(e, xreg(t), sym_intern(&e->o.syms, mn, (uint32_t)strlen(mn)));
    return t;
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
                                              : (s->decl->cname ? s->decl->cname : s->name);
                char nm[512];
                snprintf(nm, sizeof nm, "_%s", cn);
                fs = sym_intern(&e->o.syms, nm, (uint32_t)strlen(nm));
            } else {
                fs = sym_intern(&e->o.syms,
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
        uint32_t off;
        int ta = field_base(e, x, &off);
        if (off) {
            if (off <= 4095) ins(e, 0x91000000u | (off << 10) | ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
            else {
                load_imm(e, 1, 16, off);
                ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)xreg(ta) << 5) | (uint32_t)xreg(ta));
            }
        }
        return ta;
    }
    case N_INDEX:
        return index_addr(e, x, 0, NULL);
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

static void registry_add(Em* e, Node* at, int ptr, int size, uint64_t align, int align_reg) {
    if (e->o.mode != EMIT_DEBUG) return;
    e->o.allocator_used = 1;
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
    e->o.allocator_used = 1;
    CallArg args[2] = {
        { NULL, { CLS_INT, ptr }, 1, 0 },
        { NULL, { CLS_INT, size }, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_INTERNAL, "__ace$registry_find", 19, 0 };
    return emit_call(e, at, args, 2, params, muts, 2, 0, -1, tgt, raw_pointer_type());
}

static void registry_remove(Em* e, Node* at, int ptr, int size) {
    if (e->o.mode != EMIT_DEBUG) return;
    e->o.allocator_used = 1;
    CallArg args[2] = {
        { NULL, { CLS_INT, ptr }, 1, 0 },
        { NULL, { CLS_INT, size }, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_INTERNAL, "__ace$registry_remove", 21, 0 };
    (void)emit_call(e, at, args, 2, params, muts, 2, 0, -1, tgt, NULL);
}

static EV gen_process_array(Em* e, Node* call, const char* getter, uint32_t getter_len) {
    CallTarget tgt = { TK_EXTERN, getter, getter_len, 0 };
    EV outer = emit_call(e, call, NULL, 0, NULL, NULL, 0, 0, -1, tgt, raw_pointer_type());
    ld_reg(e, xreg(outer.idx), xreg(outer.idx), 0, 8, 0);
    return outer; // char ** returned through Darwin's char *** accessor
}

static EV gen_process_count_builtin(Em* e, Node* call, int environment) {
    if (!environment) {
        CallTarget tgt = { TK_EXTERN, "__NSGetArgc", 11, 0 };
        EV p = emit_call(e, call, NULL, 0, NULL, NULL, 0, 0, -1, tgt, raw_pointer_type());
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
    EV base = gen_process_array(e, call, environment ? "__NSGetEnviron" : "__NSGetArgv", environment ? 14 : 11);
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
    EV ptr = emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt, raw_pointer_type());
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
    if (e->o.mode == EMIT_DEBUG) record = registry_find(e, call, ptemp, old_size);
    CallArg args[2] = {
        { NULL, { CLS_INT, ptemp }, 1, 0 },
        { NULL, size, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_EXTERN, "_realloc", 8, 0 };
    EV ptr = emit_call(e, call, args, 2, params, muts, 2, 0, -1, tgt, raw_pointer_type());
    cbz_(e, 1, xreg(ptr.idx), 0, done);
    if (e->o.mode == EMIT_DEBUG) {
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
static EV gen_alloc_typed_builtin(Em* e, Node* call, int zeroed) {
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

    EV ptr;
    if (zeroed) { // calloc(bytes, 1): the platform zero-fills, and overflow was ruled out above
        int one = tmp_int(e);
        load_imm(e, 1, xreg(one), 1);
        CallArg args[2] = { { NULL, { CLS_INT, bytes }, 1, 0 }, { NULL, { CLS_INT, one }, 1, 0 } };
        SType* params[2] = { default_i64(), default_i64() };
        uint8_t muts[2] = { 0, 0 };
        CallTarget tgt = { TK_EXTERN, "_calloc", 7, 0 };
        ptr = emit_call(e, call, args, 2, params, muts, 2, 0, -1, tgt, raw_pointer_type());
    } else {
        CallArg arg = { NULL, { CLS_INT, bytes }, 1, 0 };
        SType* param = default_i64();
        uint8_t mut = 0;
        CallTarget tgt = { TK_EXTERN, "_malloc", 7, 0 };
        ptr = emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt, raw_pointer_type());
    }
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
    if (e->o.mode == EMIT_DEBUG) record = registry_find(e, call, ptemp, old_bytes);
    CallArg args[2] = {
        { NULL, { CLS_INT, ptemp }, 1, 0 },
        { NULL, { CLS_INT, bytes }, 1, 0 },
    };
    SType* params[2] = { raw_pointer_type(), default_i64() };
    uint8_t muts[2] = { 0, 0 };
    CallTarget tgt = { TK_EXTERN, "_realloc", 8, 0 };
    EV ptr = emit_call(e, call, args, 2, params, muts, 2, 0, -1, tgt, raw_pointer_type());
    cbz_(e, 1, xreg(ptr.idx), 0, done);
    if (e->o.mode == EMIT_DEBUG) {
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
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(length) << 5) | (uint32_t)xreg(bytes));
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
        em_err(e, call->kids[0], "mem.usable data must materialize as a slice");
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
    return emit_call(e, call, &arg, 1, &param, &mut, 1, 0, -1, tgt, default_i64());
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
        if (e->o.mode == EMIT_DEBUG) {
            int lengths_ok = new_label(e);
            ins(e, 0xEB00001Fu | ((uint32_t)xreg(slen) << 16) | ((uint32_t)xreg(dlen) << 5));
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
                ins(e, 0xEB00001Fu | (17u << 16) | ((uint32_t)xreg(dst) << 5));
                bcond_(e, C_HS, disjoint);
                ins(e, 0xEB00001Fu | (16u << 16) | ((uint32_t)xreg(src) << 5));
                bcond_(e, C_HS, disjoint);
                emit_contract_fail_call(e);
                bind_label(e, disjoint);
                bind_label(e, empty);
            }
        }
        // Empty slices may carry null pointers; the routines touch no bytes
        // for a zero count.
        e->o.mem_copy_used = 1;
        mov_int(e, 1, 15, xreg(dst));
        mov_int(e, 1, 16, xreg(src));
        mov_int(e, 1, 17, xreg(slen));
        if (intrinsic == BI_MEM_MOVE) emit_bl_internal(e, "__ace$mem_move", 14);
        else emit_bl_internal(e, "__ace$mem_copy", 14);
    } else {
        e->o.mem_fill_used = 1;
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
    e->o.mem_compare_used = 1;
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
    const char* mn = fn_mangle(helper->name, helper->len, ft, helper->decl, targs, ntargs, targs != NULL);
    CallTarget tgt = { TK_INTERNAL, arena_copy(mn, strlen(mn)),
                       (uint32_t)strlen(mn), 0 };
    return emit_call(e, at, args, n, ft->params, muts, ft->nparams, 1, -1, tgt, ft->ret);
}

static EV gen_printf_builtin(Em* e, Node* call) {
    PrintfPlan* plan = (PrintfPlan*)call->aux;
    if (!plan) {
        em_err(e, call, "fmt.printf call has no checked format plan");
        return (EV){ CLS_INT, tmp_int(e) };
    }
    int si = e->vti, sf = e->vtf;
    int nargs = call->nkids - 1;
    uint32_t* slots = (uint32_t*)arena_alloc((size_t)(nargs ? nargs : 1) * sizeof(uint32_t));

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
                uint64_t base = part->kind == PF_BINARY ? 2 : part->kind == PF_OCTAL ? 8 : 16;
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

static int is_bits_integer_op(int intrinsic) {
    return (intrinsic >= BI_BITS_POP_COUNT && intrinsic <= BI_BITS_ROTATE_RIGHT) ||
           (intrinsic >= BI_BITS_WRAP_ADD && intrinsic <= BI_BITS_WRAP_MUL);
}

/* Emit the exact Apple-arm64 scalar operation used by both direct intrinsic
 * calls and their ordinary callable symbols.  Narrow rotations duplicate the
 * source pattern through a 32-bit word, so rorv's hardware count reduction is
 * also the required modulo-8/modulo-16 reduction. */
static void emit_bits_integer(Em* e, int intrinsic, int bits, int value, int count, int vector) {
    int sf = bits == 64;
    switch (intrinsic) {
    case BI_BITS_POP_COUNT:
        ins(e, 0x9E670000u | ((uint32_t)value << 5) | (uint32_t)vector);
        ins(e, 0x0E205800u | ((uint32_t)vector << 5) | (uint32_t)vector);
        ins(e, 0x0E31B800u | ((uint32_t)vector << 5) | (uint32_t)vector);
        ins(e, 0x0E013C00u | ((uint32_t)vector << 5) | (uint32_t)value);
        return;
    case BI_BITS_LEAD_ZEROS:
        ins(e, (sf ? 0xDAC01000u : 0x5AC01000u) | ((uint32_t)value << 5) | (uint32_t)value);
        if (bits < 32) ins(e, 0x51000000u | ((uint32_t)(32 - bits) << 10) | ((uint32_t)value << 5) | (uint32_t)value);
        return;
    case BI_BITS_TRAIL_ZEROS:
        if (bits < 32) {
            load_imm(e, 0, 16, UINT64_C(1) << bits);
            ins(e, 0x2A000000u | (16u << 16) | ((uint32_t)value << 5) | (uint32_t)value);
        }
        ins(e, (sf ? 0xDAC00000u : 0x5AC00000u) | ((uint32_t)value << 5) | (uint32_t)value);
        ins(e, (sf ? 0xDAC01000u : 0x5AC01000u) | ((uint32_t)value << 5) | (uint32_t)value);
        return;
    case BI_BITS_BYTE_SWAP:
        if (bits == 16) ins(e, 0x5AC00400u | ((uint32_t)value << 5) | (uint32_t)value);
        else if (bits == 32) ins(e, 0x5AC00800u | ((uint32_t)value << 5) | (uint32_t)value);
        else if (bits == 64) ins(e, 0xDAC00C00u | ((uint32_t)value << 5) | (uint32_t)value);
        return;
    case BI_BITS_WRAP_ADD:
    case BI_BITS_WRAP_SUB:
    case BI_BITS_WRAP_MUL: {
        /* Operands are zero-extended in their registers; the 32/64-bit form
         * wraps naturally and narrow results are re-truncated. */
        uint32_t op = intrinsic == BI_BITS_WRAP_ADD ? (sf ? 0x8B000000u : 0x0B000000u) :
                      intrinsic == BI_BITS_WRAP_SUB ? (sf ? 0xCB000000u : 0x4B000000u) :
                                                      (sf ? 0x9B007C00u : 0x1B007C00u);
        ins(e, op | ((uint32_t)count << 16) | ((uint32_t)value << 5) | (uint32_t)value);
        if (bits < 32) ins(e, uxtw_of(bits) | ((uint32_t)value << 5) | (uint32_t)value);
        return;
    }
    case BI_BITS_ROTATE_LEFT:
    case BI_BITS_ROTATE_RIGHT:
        if (bits == 8) {
            ins(e, 0x2A002000u | ((uint32_t)value << 16) | ((uint32_t)value << 5) | (uint32_t)value);
            ins(e, 0x2A004000u | ((uint32_t)value << 16) | ((uint32_t)value << 5) | (uint32_t)value);
        } else if (bits == 16) {
            ins(e, 0x2A004000u | ((uint32_t)value << 16) | ((uint32_t)value << 5) | (uint32_t)value);
        }
        if (intrinsic == BI_BITS_ROTATE_LEFT)
            ins(e, (sf ? 0xCB0003E0u : 0x4B0003E0u) |
                   ((uint32_t)count << 16) | (uint32_t)count);
        ins(e, (sf ? 0x9AC02C00u : 0x1AC02C00u) | ((uint32_t)count << 16) | ((uint32_t)value << 5) | (uint32_t)value);
        if (bits < 32) ins(e, uxtw_of(bits) | ((uint32_t)value << 5) | (uint32_t)value);
        return;
    default:
        return;
    }
}

/* In-place exact IEEE operation on one FP register: fsqrt, frintm (floor),
 * frintp (ceil), single or double.  Shared by inline calls and the ordinary
 * callable symbol. */
static void emit_math_exact(Em* e, int intrinsic, int is64, int reg) {
    uint32_t op = intrinsic == BI_MATH_SQRT ? 0x1E21C000u :
                  intrinsic == BI_MATH_FLOOR ? 0x1E254000u : 0x1E24C000u;
    ins(e, op | (is64 ? 0x00400000u : 0u) | ((uint32_t)reg << 5) | (uint32_t)reg);
}

/* fmadd: a*b + c rounded once, single or double, on physical FP registers. */
static void emit_math_fused_raw(Em* e, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc, int is64) {
    ins(e, (is64 ? 0x1F400000u : 0x1F000000u) | (rb << 16) | (rc << 10) | (ra << 5) | rd);
}

static void emit_math_fused(Em* e, int dst, int a, int b, int c, int is64) {
    emit_math_fused_raw(e, (uint32_t)vreg(e, dst), (uint32_t)vreg(e, a), (uint32_t)vreg(e, b),
                        (uint32_t)vreg(e, c), is64);
}

/*
 * A call to an inline function, planned by inline_plan: bind each argument
 * to its copied parameter, emit the copied body in place, and join its
 * returns after it with the result in a temp. The enclosing expression's
 * temps stay live: the body's statements reset to the temps above them.
 */
static EV gen_inline(Em* e, InlineSite* site) {
    int fp = site->ret && is_floatt(site->ret);
    int res = !site->ret ? 0 : fp ? tmp_fp(e) : tmp_int(e);
    int held_i = e->vti, held_f = e->vtf;
    int base_i = e->vti_base, base_f = e->vtf_base;
    e->vti_base = held_i;
    e->vtf_base = held_f;
    if (e->ninl == e->cinl) {
        e->cinl = e->cinl ? e->cinl * 2 : 8;
        e->inl = realloc(e->inl, (size_t)e->cinl * sizeof(InlineCtx));
        if (!e->inl) oom();
    }
    int join = new_label(e);
    e->inl[e->ninl++] = (InlineCtx){ join, res, fp, site->ret, site->tail };

    for (int i = 0; i < site->n; i++) {
        temps_reset(e);
        if (!site->byref[i]) {
            emit_stmt(e, site->bind[i]); // the value parameter's N_VAR
            continue;
        }
        Node* a = site->arg[i];
        int addr;
        if (is_lvalue_node(a)) {
            addr = gen_addr(e, a);
        } else {
            EV v = gen_expr(e, a);
            if (v.cls != CLS_ADDR) em_err(e, a, "aggregate argument must materialize in memory");
            addr = v.idx;
        }
        Node* b = site->bind[i];
        int br = bref_reg(e, node_sym(b));
        if (br >= 0) {
            mov_int(e, 1, br, xreg(addr));
        } else {
            uint32_t slot = slot_alloc(e, b, 8, 8);
            e->slots[e->nslots - 1].byref = 1;
            store_frame(e, xreg(addr), slot, 8);
        }
    }
    for (int i = 0; i < site->body->nkids; i++) emit_stmt(e, site->body->kids[i]);
    if (site->ret && !site->tail) { // falls off only where no path reaches
        if (fp) ins(e, (unwrap(site->ret)->prim == P_F64 ? 0x9E6703E0u : 0x1E2703E0u) | (uint32_t)vreg(e, res));
        else load_imm(e, 1, xreg(res), 0);
    }
    bind_label(e, join);
    e->ninl--;
    e->vti_base = base_i;
    e->vtf_base = base_f;
    e->vti = held_i;
    e->vtf = held_f;
    if (!site->ret) return (EV){ CLS_INT, 0 };
    return (EV){ fp ? CLS_FP : CLS_INT, res };
}

// a return inside an inline expansion: the result to its temp, then the join
static void inline_return(Em* e, Node* s) {
    InlineCtx* c = &e->inl[e->ninl - 1];
    if (s->a && c->ret) {
        EV v = gen_expr(e, s->a);
        if (c->fp) {
            if (v.idx != c->res) fmov_raw(e, unwrap(c->ret)->prim == P_F64, vreg(e, c->res), vreg(e, v.idx));
        } else if (v.idx != c->res) {
            mov_int(e, scalar_bits(c->ret) == 64, xreg(c->res), xreg(v.idx));
        }
    }
    if (s != c->tail) br_(e, c->join);
}

static EV gen_call(Em* e, Node* call) {
    Node* callee = call->a;
    InlineSite* site = inline_site(e, call);
    if (site) return gen_inline(e, site);

    // usize -> data pointer: identical bits, plus the local checked alignment
    // diagnostic promised by the explicit trust boundary.
    if (callee->kind == N_PTR && !call->sym) {
        EV address = gen_expr(e, call->kids[0]);
        SType* pt = node_ty(call);
        uint32_t align = pt && pt->base ? ty_align(pt->base) : 1;
        if (e->o.mode == EMIT_DEBUG && align > 1 && !(call->flags & F_BOUNDS_PROVEN)) {
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
        if (e->o.mode == EMIT_DEBUG && !(call->flags & F_BOUNDS_PROVEN)) {
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
    if (callee->kind == N_IDENT && !call->sym && callee->len == 6 && memcmp(callee->str, "typeid", 6) == 0) {
        int t = tmp_int(e);
        load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }

    // A nominal error domain packs its compiler-assigned id above raw i32 bits.
    if (callee->kind == N_IDENT && !call->sym && node_ty(callee) && node_ty(callee)->kind == SK_ERROR_DOMAIN) {
        int out = tmp_int(e);
        load_imm(e, 1, xreg(out), call->ival << 32);
        if (call->nkids == 1) {
            EV code = gen_expr(e, call->kids[0]);
            mov_int(e, 0, xreg(code.idx), xreg(code.idx)); // clear upper 32 bits
            ins(e, 0xAA000000u | ((uint32_t)xreg(code.idx) << 16) | ((uint32_t)xreg(out) << 5) | (uint32_t)xreg(out));
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
                load_f64(e, unwrap(node_ty(call))->prim == P_F64, vreg(e, vt), arg->fval);
            } else {
                load_imm(e, 1, 16, (uint64_t)arg->ival);
                uint32_t scvtf = unwrap(node_ty(call))->prim == P_F64 ? 0x9E620000u : 0x9E220000u;
                ins(e, scvtf | (16u << 5) | (uint32_t)vreg(e, vt));
            }
            return (EV){ CLS_FP, vt };
        }
        EV v = gen_expr(e, call->kids[0]);
        return gen_cast(e, v, node_ty(call->kids[0]), node_ty(call));
    }
    // sizeof builtin (constant folded by sema)
    if (callee->kind == N_IDENT && !call->sym && callee->len == 6 && memcmp(callee->str, "sizeof", 6) == 0) {
        int t = tmp_int(e);
        load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }
    // blank builtin: the canonical zero of the stamped type, no runtime work
    if (callee->kind == N_IDENT && !call->sym && callee->len == 5 && memcmp(callee->str, "blank", 5) == 0) {
        SType* t = node_ty(call);
        if (is_floatt(t)) {
            int vt = tmp_fp(e);
            load_f64(e, unwrap(t)->prim == P_F64, vreg(e, vt), 0.0);
            return (EV){ CLS_FP, vt };
        }
        if (is_scalar(t)) {
            int it = tmp_int(e);
            load_imm(e, 1, xreg(it), 0);
            return (EV){ CLS_INT, it };
        }
        uint32_t size = ty_size(t);
        uint32_t off = temp_slot(e, size ? size : 8, 8);
        int it = tmp_int(e);
        frame_addr(e, 16, off);
        for (uint32_t o = 0; o < size; o += 8)
            st_reg(e, 31, 16, o, o + 8 <= size ? 8 : size - o);
        frame_addr(e, xreg(it), off);
        return (EV){ CLS_ADDR, it };
    }
    // alignof builtin (constant folded by sema)
    if (callee->kind == N_IDENT && !call->sym && callee->len == 7 && memcmp(callee->str, "alignof", 7) == 0) {
        int t = tmp_int(e);
        load_imm(e, 1, xreg(t), call->ival);
        return (EV){ CLS_INT, t };
    }
    if (callee->kind == N_IDENT && !call->sym && callee->len == 8 && memcmp(callee->str, "offsetof", 8) == 0) {
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
        return gen_alloc_typed_builtin(e, call, 0);
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival == BI_MEM_ZALLOC_T && call->ival == BI_MEM_ZALLOC_T)
        return gen_alloc_typed_builtin(e, call, 1);
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
    if (intrinsic && intrinsic->decl && intrinsic->decl->ival == BI_MEM_EQUAL && call->ival == BI_MEM_EQUAL) {
        EV a = gen_expr(e, call->kids[0]);
        EV b = gen_expr(e, call->kids[1]);
        return gen_byte_sequence_eq(e, call, a, node_ty(call->kids[0]), b, node_ty(call->kids[1]), 1);
    }
    if (intrinsic && intrinsic->decl && intrinsic->decl->ival == BI_BITS_FROM_FLOAT) {
        EV value = gen_expr(e, call->kids[0]);
        int bits = tmp_int(e);
        int is64 = unwrap(node_ty(call->kids[0]))->prim == P_F64;
        uint32_t op = is64 ? 0x9E660000u : 0x1E260000u;
        ins(e, op | ((uint32_t)vreg(e, value.idx) << 5) |
               (uint32_t)xreg(bits)); // fmov integer bits from fp register
        e->vtf = value.idx;
        return (EV){ CLS_INT, bits };
    }
    if (intrinsic && intrinsic->decl &&
        intrinsic->decl->ival >= BI_MATH_SQRT && intrinsic->decl->ival <= BI_MATH_CEIL) {
        EV value = gen_expr(e, call->kids[0]);
        emit_math_exact(e, intrinsic->decl->ival, unwrap(node_ty(call))->prim == P_F64, vreg(e, value.idx));
        return value;
    }
    if (intrinsic && intrinsic->decl && intrinsic->decl->ival == BI_MATH_FMA) {
        EV a = gen_expr(e, call->kids[0]);
        EV b = gen_expr(e, call->kids[1]);
        EV c = gen_expr(e, call->kids[2]);
        emit_math_fused(e, a.idx, a.idx, b.idx, c.idx, unwrap(node_ty(call))->prim == P_F64);
        e->vtf = a.idx + 1; // free b and c
        return a;
    }
    if (intrinsic && intrinsic->decl && intrinsic->decl->ival == BI_BITS_TO_FLOAT) {
        EV bits = gen_expr(e, call->kids[0]);
        int value = tmp_fp(e);
        int is64 = unwrap(node_ty(call))->prim == P_F64;
        uint32_t op = is64 ? 0x9E670000u : 0x1E270000u;
        ins(e, op | ((uint32_t)xreg(bits.idx) << 5) |
               (uint32_t)vreg(e, value)); // fmov fp value from integer bits
        e->vti = bits.idx;
        return (EV){ CLS_FP, value };
    }
    if (intrinsic && intrinsic->decl && is_bits_integer_op(intrinsic->decl->ival)) {
        int operation = intrinsic->decl->ival;
        EV value = gen_expr(e, call->kids[0]);
        EV count = { CLS_INT, 0 };
        if (call->nkids == 2) count = gen_expr(e, call->kids[1]);
        int vector = 0;
        int saved_vtf = e->vtf;
        if (operation == BI_BITS_POP_COUNT) vector = vreg(e, tmp_fp(e));
        emit_bits_integer(e, operation, scalar_bits(node_ty(call->kids[0])), xreg(value.idx), xreg(count.idx), vector);
        e->vtf = saved_vtf;
        e->vti = value.idx + 1;
        return value;
    }
    if (intrinsic && intrinsic->decl && intrinsic->decl->ival == BI_INTERNAL_LOAD_U64) {
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
    // assert(condition): debug evaluates once; release removes the complete
    // condition expression, including side effects.
    if (callee->kind == N_IDENT && !call->sym &&
        (call->ival == 2 || call->ival == 3) &&
        callee->len == 6 && memcmp(callee->str, "assert", 6) == 0) {
        if (e->o.mode != EMIT_DEBUG || call->ival == 3) return (EV){ CLS_INT, 0 };
        int ok = new_label(e);
        gen_branch(e, call->kids[0], 1, ok);
        const char* path = call->path ? build_path(call->path) : "<source>";
        size_t need = strlen(path) + 80;
        char* diagnostic = (char*)arena_alloc(need);
        int n = snprintf(diagnostic, need, "%s:%d:%d: assertion failed\n", path, call->line, call->col);
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
                if (m->len == callee->len && memcmp(m->str, callee->str, callee->len) == 0) { midx = i; break; }
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
                const char* mn = fn_call_mangle(s, call);
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
                !(call->flags & (F_METHOD | F_ASSOC))) { // instance method
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
            if (callee->kind == N_FIELD_E && !s->is_extern && recv_mut) muts[0] = 1;
            for (int i = 0; i < n && s->decl && i < s->decl->nkids; i++)
                muts[i] = (s->decl->kids[i]->flags & F_MUT) != 0;
            if (s->decl && (s->decl->flags & F_METHOD) && n > 0)
                muts[0] = 1; // uniform method receiver ABI; semantic mut is separate
            int varargs_from = s->is_extern && s->decl && (s->decl->flags & F_VARARGS) ? ft->nparams : -1;
            return emit_call(e, call, args, n, ft->params, muts, ft->nparams, !c_boundary, varargs_from, tgt, ft->ret);
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
        uint32_t w = value_size - o >= 8 ? 8 : value_size - o >= 4 ? 4 : value_size - o >= 2 ? 2 : 1;
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
        ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        ins(e, 0xD340FC00u | (30u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(q));
        ins(e, 0xCA000000u | ((uint32_t)xreg(q) << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 16, UINT64_C(0xbf58476d1ce4e5b9));
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        ins(e, 0xD340FC00u | (27u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(q));
        ins(e, 0xCA000000u | ((uint32_t)xreg(q) << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 16, UINT64_C(0x94d049bb133111eb));
        ins(e, 0x9B007C00u | (16u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        ins(e, 0xD340FC00u | (31u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(q));
        ins(e, 0xCA000000u | ((uint32_t)xreg(q) << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 16, md->mask);
        ins(e, 0x8A000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        addr_of_sym(e, 16, md->slots);
        ins(e, 0x8B000000u | ((uint32_t)xreg(h) << 16) | (2u << 10) | (16u << 5) | 16u);
        ld_reg(e, xreg(entry), 16, 0, 4, 0);
        cbz_(e, 0, xreg(entry), 0, miss);
        ins(e, 0x51000400u | ((uint32_t)xreg(entry) << 5) | (uint32_t)xreg(entry));
        addr_of_sym(e, 16, md->keys);
        uint32_t ksz = ty_size(map->base);
        if ((ksz & (ksz - 1)) == 0) {
            uint32_t sh = ksz == 1 ? 0 : ksz == 2 ? 1 : ksz == 4 ? 2 : 3;
            ins(e, 0x8B000000u | ((uint32_t)xreg(entry) << 16) | (sh << 10) | (16u << 5) | 16u);
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
        ins(e, 0xEB00001Fu | ((uint32_t)xreg(len) << 16) | ((uint32_t)xreg(at) << 5));
        bcond_(e, C_HS, hashed);
        ins(e, 0x38606810u | ((uint32_t)xreg(at) << 16) | ((uint32_t)xreg(ptr) << 5));
        ins(e, 0xCA000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        load_imm(e, 1, 17, UINT64_C(1099511628211));
        ins(e, 0x9B007C00u | (17u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        ins(e, 0x91000400u | ((uint32_t)xreg(at) << 5) | (uint32_t)xreg(at));
        br_(e, loop);
        bind_label(e, hashed);
        load_imm(e, 1, 16, md->mask);
        ins(e, 0x8A000000u | (16u << 16) | ((uint32_t)xreg(h) << 5) | (uint32_t)xreg(h));
        addr_of_sym(e, 16, md->slots);
        ins(e, 0x8B000000u | ((uint32_t)xreg(h) << 16) | (2u << 10) | (16u << 5) | 16u);
        ld_reg(e, xreg(entry), 16, 0, 4, 0);
        cbz_(e, 0, xreg(entry), 0, miss);
        ins(e, 0x51000400u | ((uint32_t)xreg(entry) << 5) | (uint32_t)xreg(entry));
        addr_of_sym(e, 16, md->keys);
        ins(e, 0x8B000000u | ((uint32_t)xreg(entry) << 16) | (4u << 10) | (16u << 5) | 16u);
        ld_reg(e, 17, 16, 8, 8, 0);
        ins(e, 0xEB00001Fu | (17u << 16) | ((uint32_t)xreg(len) << 5));
        bcond_(e, C_NE, miss);
        ld_reg(e, 16, 16, 0, 8, 0);
        e->o.mem_compare_used = 1;
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
            ldrf_(e, vreg(e, svf), xreg(out), 0, unwrap(map->ret)->prim == P_F64);
            e->vti = svi;
            e->vtf = svf + 1;
            return (EV){ CLS_FP, svf };
        }
        uint32_t w = (uint32_t)((scalar_bits(map->ret) + 7) / 8);
        ld_reg(e, xreg(svi), xreg(out), 0, w, ty_is_signed(map->ret) && w < 8);
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
            load_f64(e, unwrap(t)->prim == P_F64, vreg(e, vt), (double)x->ival);
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
        load_f64(e, is64, vreg(e, vt), x->fval);
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
        if (s->kind == SYM_FUNC) return (EV){ CLS_INT, gen_addr(e, x) };
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
        double folded_float;
        if (const_global_float(s, t, &folded_float)) {
            int vt = tmp_fp(e);
            load_f64(e, unwrap(t)->prim == P_F64, vreg(e, vt), folded_float);
            return (EV){ CLS_FP, vt };
        }
        if (is_scalar(t)) return load_var(e, s, t, x);
        return (EV){ CLS_ADDR, gen_var_addr(e, s, x) };
    }
    case N_TUPLE_E: {
        SType* t = node_ty(x);
        uint32_t size = t ? ty_size(t) : (uint32_t)(x->nkids * 8);
        uint32_t off = temp_slot(e, size, 8);
        for (int i = 0; i < x->nkids; i++) {
            SType* ft = t && t->kind == SK_TUPLE && i < t->nfields ? t->fields[i].t
                : node_ty(x->kids[i]);
            int svi = e->vti, svf = e->vtf;
            EV v = gen_expr(e, x->kids[i]);
            uint32_t member = t && t->kind == SK_TUPLE ? field_off(t, i) : (uint32_t)i * 8;
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
                if (e->o.mode == EMIT_DEBUG && f->bit_width < 64) {
                    int fail = new_label(e), ok = new_label(e); uint64_t hi;
                    if (f->bit_signed) {
                        uint64_t lo = (uint64_t)(-(INT64_C(1) << (f->bit_width - 1)));
                        hi = (UINT64_C(1) << (f->bit_width - 1)) - 1;
                        load_imm(e, sf, 16, lo);
                        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)xreg(v.idx) << 5));
                        bcond_(e, C_LT, fail);
                    } else hi = (UINT64_C(1) << f->bit_width) - 1;
                    load_imm(e, sf, 16, hi);
                    ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)xreg(v.idx) << 5));
                    bcond_(e, f->bit_signed ? C_LE : C_LS, ok);
                    bind_label(e, fail); emit_arithmetic_fail_call(e); bind_label(e, ok);
                }
                uint64_t low = f->bit_width == 64 ? UINT64_MAX : (UINT64_C(1) << f->bit_width) - 1;
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
            int uniform = total > 0 && eval_const(e, x->a, t->base, &cb, &cr) && cr.n == 0 && cb.len == esz;
            for (uint32_t i = 1; uniform && i < esz; i++) uniform = cb.d[i] == cb.d[0];
            uint8_t byte = uniform ? cb.d[0] : 0;
            free(cb.d);
            free(cr.v);
            if (uniform) {
                e->o.mem_fill_used = 1;
                frame_addr(e, 15, off);
                load_imm(e, 1, 16, byte);
                load_imm(e, 1, 17, total);
                emit_bl_internal(e, "__ace$mem_fill", 14);
            } else if (t->count > 0) {
                EV v = gen_expr(e, x->a);
                if (esz == 1 && v.cls == CLS_INT) {
                    e->o.mem_fill_used = 1;
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
                        uint64_t n = t->count - done < done ? t->count - done : done;
                        e->o.mem_copy_used = 1;
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
            uint32_t fo = x->kind == N_ARRAYLIT ? (uint32_t)fidx * ty_size(t->base)
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
        if (x->op == T_AMP) return (EV){ CLS_INT, gen_addr(e, x) };
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
                ins(e, (is64 ? 0x1E614000u : 0x1E214000u) | ((uint32_t)vreg(e, v.idx) << 5) | (uint32_t)vreg(e, v.idx));
                return v;
            }
            int sf = !t || scalar_bits(t) == 64;
            if (e->o.mode == EMIT_DEBUG && t && is_intt(t)) {
                int bits = scalar_bits(t);
                uint64_t min = bits == 64 ? UINT64_C(0x8000000000000000)
                                          : (uint32_t)(-(INT64_C(1) << (bits - 1)));
                int ok = new_label(e);
                load_imm(e, sf, 16, min);
                ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)xreg(v.idx) << 5));
                bcond_(e, C_NE, ok);
                emit_arithmetic_fail_call(e);
                bind_label(e, ok);
            }
            ins(e, (sf ? 0xCB0003E0u : 0x4B0003E0u) | ((uint32_t)xreg(v.idx) << 16) | (uint32_t)xreg(v.idx));
            if (e->o.mode == EMIT_RELEASE && t) narrow_wrap(e, xreg(v.idx), t);
            return v;
        }
        if (x->op == T_BANG || x->op == T_TILDE) {
            SType* t = node_ty(x->a);
            EV v = gen_expr(e, x->a);
            if (v.cls == CLS_FP) {
                em_err(e, x, "invalid operand for '%s'", tok_disp((TokKind)x->op));
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
            if (t && !ty_is_signed(t)) narrow_wrap(e, xreg(v.idx), t);
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
        if (ts && ts->kind == SYM_TYPE && ts->type && ts->type->kind == SK_ERROR_DOMAIN) {
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
            if (frame_place(e, x->a, &depth)) return load_frame_value(e, depth - 8, node_ty(x));
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
            else return gen_error_description(e, value, x->len == 7);
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
        int sr = split_reg(e, x);
        if (sr >= 0) return load_preg(e, sr, t);
        int home = param_home(e, x);
        if (home >= 0 && is_floatt(t)) {
            int vt = tmp_fp(e);
            fmov_raw(e, unwrap(t)->prim == P_F64, vreg(e, vt), home);
            return (EV){ CLS_FP, vt };
        }
        if (home >= 0) {
            int it = tmp_int(e);
            mov_int(e, 1, xreg(it), home);
            return (EV){ CLS_INT, it };
        }
        uint32_t depth;
        if (t && is_scalar(t) && frame_place(e, x, &depth)) return load_frame_value(e, depth, t);
        if (t && is_scalar(t)) {
            uint32_t off;
            int a = field_base(e, x, &off);
            return load_in_place(e, a, off, t);
        }
        return (EV){ CLS_ADDR, gen_addr(e, x) };
    }
    case N_INDEX: {
        if (x->sym) return (EV){ CLS_INT, gen_generic_value(e, x) };
        SType* bt = node_ty(x->a);
        if (bt && bt->kind == SK_STATICMAP) return gen_staticmap_lookup(e, x, bt);
        SType* t = node_ty(x);
        if (!t) t = lvalue_type(e, x);
        uint32_t w = !t || !is_scalar(t) ? 0 : is_floatt(t) ? (unwrap(t)->prim == P_F64 ? 8u : 4u)
                                                            : (uint32_t)((scalar_bits(t) + 7) / 8);
        int ri = -1;
        int a = index_addr(e, x, w, &ri);
        if (t && is_scalar(t)) {
            if (ri >= 0) { // element size equals the access width: index folded into the load
                if (is_floatt(t)) {
                    int vt = tmp_fp(e);
                    ldrf_idx(e, vreg(e, vt), xreg(a), ri, w == 8);
                    return (EV){ CLS_FP, vt };
                }
                ld_idx(e, xreg(a), xreg(a), ri, w, ty_is_signed(t) && w < 8);
                return (EV){ CLS_INT, a };
            }
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
        int guard = e->o.mode == EMIT_DEBUG && !(x->flags & F_BOUNDS_PROVEN);
        int guard_source = guard && !(proof & BP_SOURCE);
        int open_len = !x->c && bt->kind != SK_ARRAY; // an omitted hi reads the source length
        uint32_t len_slot = guard_source || open_len ? temp_slot(e, 8, 8) : 0;
        int svi = e->vti, svf = e->vtf;
        int base;
        if (is_lvalue_node(x->a)) base = gen_addr(e, x->a);
        else {
            EV v = gen_expr(e, x->a);
            base = v.idx;
        }
        // Sequences #2: the source descriptor once, then lo, then hi
        if (len_slot) {
            if (bt->kind == SK_ARRAY) {
                load_imm(e, 1, 16, bt->count);
            } else {
                ld_reg(e, 16, base, 8, 8, 0);
            }
            store_frame(e, 16, len_slot, 8);
        }
        // data pointer (slice/string: load; array: the base address)
        if (bt->kind != SK_ARRAY) ld_reg(e, base, base, 0, 8, 0);
        if (x->b) {
            EV lo = gen_expr(e, x->b);
            emit_nonnegative_guard(e, x, x->b, xreg(lo.idx));
            widen_idx(e, x->b, xreg(lo.idx));
            store_frame(e, xreg(lo.idx), lo_slot, 8);
        } else {
            load_imm(e, 1, 16, 0);
            store_frame(e, 16, lo_slot, 8);
        }
        if (x->c) {
            EV hi = gen_expr(e, x->c);
            emit_nonnegative_guard(e, x, x->c, xreg(hi.idx));
            widen_idx(e, x->c, xreg(hi.idx));
            store_frame(e, xreg(hi.idx), hi_slot, 8);
        } else if (bt->kind == SK_ARRAY) {
            load_imm(e, 1, 16, bt->count);
            store_frame(e, 16, hi_slot, 8);
        } else {
            load_frame(e, 16, len_slot, 8, 0);
            store_frame(e, 16, hi_slot, 8);
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
        // ptr = data + lo*esz
        load_frame(e, 16, lo_slot, 8, 0);
        if (esz > 1) {
            load_imm(e, 1, 17, esz);
            ins(e, 0x9B007C00u | (17u << 16) | (16u << 5) | 16u); // mul x16
        }
        ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)base << 5) | (uint32_t)base);
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
    em_err(e, x, "codegen for '%s' expressions comes in the next chunk", node_kind_name((NKind)x->kind));
    return (EV){ CLS_INT, tmp_int(e) };
}

// --- statements -----------------------------------------------------------------------

static void store_to_slot(Em* e, EV v, uint32_t off, SType* t) {
    if (v.cls == CLS_ADDR) {
        frame_addr(e, 15, off);
        copy_mem(e, 15, 0, xreg(v.idx), 0, ty_size(t));
    } else if (v.cls == CLS_FP) {
        store_frame_fp(e, vreg(e, v.idx), off, is_scalar(t) ? unwrap(t)->prim == P_F64 : 1);
    } else {
        store_frame(e, xreg(v.idx), off, (uint32_t)((scalar_bits(t) + 7) / 8));
    }
}

// assignment store: into a promoted register (direct 2, doff = register), in
// place for a direct local slot (direct 1), else through ta
static void store_target(Em* e, EV v, int direct, uint32_t doff, int ta, SType* t) {
    if (direct == 2) store_preg(e, (int)doff, v, t);
    else if (direct) store_to_slot(e, v, doff, t);
    else store_scalar(e, v, xreg(ta), 0, t);
}

// frame depth of field f of a split local, in the slot its unpromoted fields keep
static uint32_t split_depth(Em* e, Split* sp, int f) {
    Slot* sl = slot_find(e, sp->decl);
    return sl ? sl->off - field_off(sp->type, f) : 0;
}

// field f of a split local in a fresh temp
static EV split_get(Em* e, Split* sp, int f) {
    SType* ft = sp->type->fields[f].t;
    int r = split_freg(e, sp, f);
    return r >= 0 ? load_preg(e, r, ft) : load_frame_value(e, split_depth(e, sp, f), ft);
}

// write field f of a split local from v
static void split_set(Em* e, Split* sp, int f, EV v) {
    SType* ft = sp->type->fields[f].t;
    int r = split_freg(e, sp, f);
    if (r >= 0) store_preg(e, r, v, ft);
    else store_to_slot(e, v, split_depth(e, sp, f), ft);
}

static void sink_arm(Em* e, Node* call, SType* ret, RetCls* rc);
static int sink_take(Em* e);

// evaluate a struct literal's field values into temps, in source order,
// indexed by field; sema completes it with every field
static void lit_values(Em* e, Node* lit, SType* t, EV* vals, uint8_t* have) {
    int positional = lit->nkids > 0 && lit->kids[0]->str == NULL;
    for (int i = 0; i < lit->nkids; i++) {
        Node* fi = lit->kids[i];
        int f = positional ? i : find_field(t, fi->str, fi->len);
        if (f < 0 || f >= t->nfields || f >= 4) {
            em_err(e, fi, "no such field");
            continue;
        }
        vals[f] = gen_expr(e, fi->a);
        have[f] = 1;
    }
}

/*
 * `place = T{...}` for a struct of split_type, with the place's address in
 * base: every value is evaluated before any store, as building the literal
 * in a temp would, then stored straight into the place at its field's width.
 * Nothing is staged, so a later whole-word load of the place is not left
 * waiting on narrower stores it cannot forward from.
 */
static void lit_store_at(Em* e, Node* lit, SType* t, int base) {
    int svi = e->vti, svf = e->vtf;
    EV vals[4];
    uint8_t have[4] = {0};
    lit_values(e, lit, t, vals, have);
    for (int f = 0; f < t->nfields; f++) {
        if (have[f]) store_scalar(e, vals[f], base, field_off(t, f), t->fields[f].t);
        else em_err(e, lit, "struct literal is missing a field");
    }
    e->vti = svi; e->vtf = svf;
}

/*
 * `x = rhs` for a split local. A literal, which sema completes with every
 * field, evaluates every value before writing any, since a value may read x.
 * A call returning in registers moves its units straight into the fields
 * (split_take). Another
 * split local is read whole before any write, since the two may share
 * registers where one's interval ends as the other's begins. Anything else
 * materializes an address the fields load from, each at its own width.
 */
static void split_write(Em* e, Split* sp, Node* rhs) {
    int svi = e->vti, svf = e->vtf;
    EV vals[4];
    uint8_t have[4] = {0};
    Split* src = split_live(e, rhs);
    if (rhs->kind == N_STRUCTLIT) {
        lit_values(e, rhs, sp->type, vals, have);
    } else if (src) {
        for (int f = 0; f < sp->n; f++) {
            vals[f] = split_get(e, src, f);
            have[f] = 1;
        }
    } else {
        RetCls rc;
        SType* ret = rhs->kind == N_CALL ? node_ty(rhs) : NULL;
        if (ret) classify_ret(ret, &rc);
        if (ret && !rc.sret && rc.nunits >= 1 && ty_size(ret) == ty_size(sp->type)) {
            sink_arm(e, rhs, ret, &rc);
            e->sink_split = sp;
        }
        EV v = gen_expr(e, rhs);
        if (sink_take(e)) {
            e->vti = svi; e->vtf = svf;
            return;
        }
        if (v.cls != CLS_ADDR) {
            em_err(e, rhs, "struct value did not materialize in memory");
            return;
        }
        for (int f = 0; f < sp->n; f++) {
            SType* ft = sp->type->fields[f].t;
            uint32_t fo = field_off(sp->type, f);
            int r = split_freg(e, sp, f);
            if (r >= 0) {
                load_binder(e, r, xreg(v.idx), fo, ft);
                continue;
            }
            int si = e->vti, sf = e->vtf;
            EV fv = load_scalar(e, xreg(v.idx), fo, ft);
            store_to_slot(e, fv, split_depth(e, sp, f), ft);
            e->vti = si; e->vtf = sf;
        }
        e->vti = svi; e->vtf = svf;
        return;
    }
    for (int f = 0; f < sp->n; f++) {
        if (have[f]) split_set(e, sp, f, vals[f]);
        else em_err(e, rhs, "struct literal is missing a field");
    }
    e->vti = svi; e->vtf = svf;
}

// store a split local whole to [base+off], one store per field
static void split_store_at(Em* e, Split* sp, int base, uint32_t off) {
    for (int f = 0; f < sp->n; f++) {
        SType* ft = sp->type->fields[f].t;
        uint32_t fo = off + field_off(sp->type, f);
        int r = split_freg(e, sp, f);
        if (r >= 0 && is_floatt(ft)) strf_(e, r, base, fo, unwrap(ft)->prim == P_F64);
        else if (r >= 0) st_reg(e, r, base, fo, (uint32_t)((scalar_bits(ft) + 7) / 8));
        else { // raw bits through x17, which touches no temp or argument register
            uint32_t w = ty_size(ft);
            load_frame(e, 17, split_depth(e, sp, f), w, 0);
            st_reg(e, 17, base, fo, w);
        }
    }
}

/*
 * Build argument or result unit u of a split local in register dst: an HFA
 * leaf is its float field; an integer-class chunk packs the fields within its
 * 8 bytes, the first by mov and the rest by bfi (a narrow field's canonical
 * form may carry sign bits above it, which the next insert overwrites). Only
 * x16 and x17 are scratch, so no temp or argument register is disturbed.
 */
static void split_unit(Em* e, Split* sp, TU* u, int dst) {
    for (int f = 0; f < sp->n; f++) {
        SType* ft = sp->type->fields[f].t;
        uint32_t fo = field_off(sp->type, f), w = ty_size(ft);
        int r = split_freg(e, sp, f);
        if (u->kind == TU_VREG) {
            if (fo != u->off) continue;
            if (r >= 0) fmov_raw(e, w == 8, dst, r);
            else load_frame_fp(e, dst, split_depth(e, sp, f), w == 8);
            return;
        }
        if (fo < u->off || fo >= u->off + 8) continue;
        int src = r;
        if (r >= 0 && is_floatt(ft)) { // fmov x16, d | w16, s
            ins(e, (w == 8 ? 0x9E660000u : 0x1E260000u) | ((uint32_t)r << 5) | 16u);
            src = 16;
        } else if (r < 0) {
            load_frame(e, 16, split_depth(e, sp, f), w, 0);
            src = 16;
        }
        uint32_t lsb = (fo - u->off) * 8;
        if (!lsb) mov_int(e, 1, dst, src);
        else // bfi dst, src, #lsb, #width
            ins(e, 0xB3400000u | (((64u - lsb) & 63u) << 16) | ((w * 8 - 1) << 10) |
                   ((uint32_t)src << 5) | (uint32_t)dst);
    }
}

/*
 * The inverse for a call result: move each field out of the unit holding
 * it, an integer field by ubfx at its width (then sxtb/sxth for a narrow
 * signed one, as a slot reload leaves it) and a float from an HFA leaf or
 * from the chunk's bits.
 */
static void split_take(Em* e, Split* sp, RetCls* rc) {
    for (int f = 0; f < sp->n; f++) {
        SType* ft = sp->type->fields[f].t;
        uint32_t fo = field_off(sp->type, f), w = ty_size(ft);
        int r = split_freg(e, sp, f);
        TU* u = NULL;
        for (int k = 0; k < rc->nunits && !u; k++)
            if (rc->units[k].kind == TU_VREG ? rc->units[k].off == fo
                                             : fo >= rc->units[k].off && fo < rc->units[k].off + 8)
                u = &rc->units[k];
        if (!u) {
            em_err(e, sp->decl, "internal: no result unit holds a field");
            return;
        }
        if (u->kind == TU_VREG) {
            if (r >= 0) fmov_raw(e, w == 8, r, u->reg);
            else store_frame_fp(e, u->reg, split_depth(e, sp, f), w == 8);
            continue;
        }
        uint32_t lsb = (fo - u->off) * 8;
        int src = u->reg;
        if (lsb || (r >= 0 && w < 8 && !is_floatt(ft))) { // ubfx x16|r, chunk, #lsb, #width
            int d = r >= 0 && !is_floatt(ft) ? r : 16;
            ins(e, 0xD3400000u | (lsb << 16) | ((lsb + w * 8 - 1) << 10) | ((uint32_t)src << 5) | (uint32_t)d);
            src = d;
        }
        if (r < 0) store_frame(e, src, split_depth(e, sp, f), w);
        else if (is_floatt(ft)) // fmov d, x | s, w
            ins(e, (w == 8 ? 0x9E670000u : 0x1E270000u) | ((uint32_t)src << 5) | (uint32_t)r);
        else if (w < 4 && ty_is_signed(ft)) set_preg(e, r, r, ft);
        else mov_int(e, 1, r, src);
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
        !tgt->is_extern && !(call->flags & (F_METHOD | F_ASSOC));
    SType* baset = callee->kind == N_FIELD_E ? node_ty(callee->a) : NULL;
    int contract = callee->kind == N_FIELD_E && !call->sym && baset && baset->kind == SK_CONTRACT;
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
            if (a->kind == N_MUTARG) sc->kids[i] = defer_mutarg(a, defer_capture_arg(e, a, 1));
            else sc->kids[i] = defer_capture_arg(e, a, 0);
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
            if (u->kind == TU_XREG) store_frame(e, u->reg, e->retsave_off - (uint32_t)w * 8, 8);
            else store_frame_fp(e, u->reg, e->retsave_off - (uint32_t)w * 8, u->size == 8);
        }
    }
    for (int i = e->ndefers - 1; i >= 0; i--) {
        line_mark(e, e->defers[i].at); // exit-time marks cite the defer line
        e->vti = 0;
        e->vtf = 0;
        e->in_defers++;
        gen_expr(e, e->defers[i].call); // results discarded
        e->in_defers--;
    }
    if (save) {
        for (int w = 0; w < rc.nunits; w++) {
            TU* u = &rc.units[w];
            if (u->kind == TU_XREG) load_frame(e, u->reg, e->retsave_off - (uint32_t)w * 8, 8, 0);
            else load_frame_fp(e, u->reg, e->retsave_off - (uint32_t)w * 8, u->size == 8);
        }
    }
}

// copy member i of a tuple result (at address `ra`) into frame offset `off`
static void store_member(Em* e, int ra, SType* tup, int i, uint32_t off) {
    SType* ft = tup && tup->kind == SK_TUPLE && i < tup->nfields ? tup->fields[i].t : NULL;
    uint32_t mo = tup && tup->kind == SK_TUPLE ? field_off(tup, i) : (uint32_t)i * 8;
    if (!ft) {
        em_err(e, NULL, "missing tuple member type");
        return;
    }
    if (is_scalar(ft)) {
        EV v = load_scalar(e, ra, mo, ft);
        if (v.cls == CLS_FP) store_frame_fp(e, vreg(e, v.idx), off, unwrap(ft)->prim == P_F64);
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
 * reuse the temp registers), the i32 tag is loaded once and each pattern
 * compares it, and matching
 * arms copy their payload binder, or load a promoted one (or a split one's
 * register fields) straight from the payload, before running the body. An unmatched tag
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
    ld_reg(e, 17, xreg(v.idx), 0, 4, 0); // tag, loaded once for every pattern
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
                int base = tmp_int(e); // scrutinee, outside copy_mem's x15-x17
                load_frame(e, xreg(base), sslot, 8, 0);
                int bpr = is_scalar(pt) && !is_floatt(pt) ? promo_reg(e, bs) : -1;
                if (bpr >= 0) { // promoted binder: the payload loads straight into its register
                    uint32_t w = (uint32_t)((scalar_bits(pt) + 7) / 8);
                    ld_reg(e, bpr, xreg(base), poff, w, ty_is_signed(pt) && w < 4);
                    e->vti = base;
                    continue;
                }
                Split* sp = bs ? split_at(e, bs->decl) : NULL;
                if (sp && sp->regs) { // field by field: a slot only for fields left out of registers
                    for (int f = 0; f < sp->n; f++)
                        if (split_freg(e, sp, f) < 0) {
                            slot_alloc(e, bs->decl, ty_size(pt), ty_align(pt));
                            break;
                        }
                    for (int f = 0; f < sp->n; f++) {
                        SType* ft = sp->type->fields[f].t;
                        uint32_t fo = poff + field_off(sp->type, f);
                        int r = split_freg(e, sp, f);
                        if (r >= 0) load_binder(e, r, xreg(base), fo, ft);
                        else { // raw bits through x17
                            ld_reg(e, 17, xreg(base), fo, ty_size(ft), 0);
                            store_frame(e, 17, split_depth(e, sp, f), ty_size(ft));
                        }
                    }
                    e->vti = base;
                    continue;
                }
                uint32_t bslot = slot_alloc(e, bs && bs->decl ? bs->decl : pat->b, ty_size(pt), ty_align(pt));
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

static void emit_bits_field_assign(Em* e, Node* target, Node* rhs, TokKind op, SType* bits, int fi) {
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
        ins(e, base | ((uint32_t)f->bit_offset << 16) | (imms << 10) | ((uint32_t)xreg(vr) << 5) | (uint32_t)xreg(vr));
        EV rv = gen_expr(e, rhs);
        emit_checked_arith(e, compound_op(op), xreg(vr), xreg(vr), xreg(rv.idx), f->t);
        value = (EV){ CLS_INT, vr };
    } else value = gen_expr(e, rhs);
    if (e->o.mode == EMIT_DEBUG && f->bit_width < 64) {
        int fail = new_label(e), ok = new_label(e);
        uint64_t hi;
        if (f->bit_signed) {
            uint64_t lo = (uint64_t)(-(INT64_C(1) << (f->bit_width - 1)));
            hi = (UINT64_C(1) << (f->bit_width - 1)) - 1;
            load_imm(e, sf, 16, lo);
            ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)xreg(value.idx) << 5));
            bcond_(e, C_LT, fail);
        } else hi = (UINT64_C(1) << f->bit_width) - 1;
        load_imm(e, sf, 16, hi);
        ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (16u << 16) | ((uint32_t)xreg(value.idx) << 5));
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

// an expression that emits no call: literals, names, field reads, and unary
// or binary operators over scalars (aggregate operands compare through
// mem_compare), so registers holding earlier results survive its evaluation
static int call_free_scalar_expr(Node* n) {
    if (!n) return 1;
    switch (n->kind) {
    case N_INT: case N_FLOAT: case N_CHAR: case N_BOOL: case N_IDENT:
        return 1;
    case N_FIELD_E:
        return call_free_scalar_expr(n->a);
    case N_UNARY:
        return n->op != T_AMP && n->op != T_VOLATILE && call_free_scalar_expr(n->a);
    case N_BINARY: {
        SType* ta = node_ty(n->a);
        SType* tb = node_ty(n->b);
        if ((ta && !is_scalar(ta)) || (tb && !is_scalar(tb))) return 0;
        return call_free_scalar_expr(n->a) && call_free_scalar_expr(n->b);
    }
    default:
        return 0;
    }
}

// whether x reads the float parameter leaf homed in v<r>
static int reads_home(Em* e, Node* x, int r) {
    if (!x) return 0;
    if (x->kind == N_FIELD_E && param_home(e, x) == r) return 1;
    if (reads_home(e, x->a, r) || reads_home(e, x->b, r) || reads_home(e, x->c, r)) return 1;
    for (int i = 0; i < x->nkids; i++) if (reads_home(e, x->kids[i], r)) return 1;
    return 0;
}

/*
 * A struct literal or tuple returned in registers, one whole scalar leaf per
 * result register (8-byte integer chunks, or one float per HFA member),
 * evaluates each leaf straight into its register instead of a frame temp.
 * Leaf w is computed with temp w free, so it lands in x<w>/d<w> with the
 * earlier leaves still live below it, the instruction that computed a float
 * leaf retargeted there when it can be; leaves that could emit a call stay on
 * the temp path. Returns 0, having emitted nothing, when the shape does not fit.
 */
static int gen_return_regs(Em* e, Node* x, RetCls* rc) {
    if (x->kind != N_STRUCTLIT && x->kind != N_TUPLE_E) return 0;
    SType* t = node_ty(x);
    if (!t || (t->kind != SK_STRUCT && t->kind != SK_TUPLE)) return 0;
    if (rc->sret || rc->nunits < 1 || rc->nunits > 4) return 0;
    if (t->nfields != rc->nunits || x->nkids != rc->nunits) return 0;
    int positional = x->kind == N_STRUCTLIT && x->kids[0]->str == NULL;
    Node* byunit[4] = {0};
    for (int i = 0; i < x->nkids; i++) {
        Node* fi = x->kids[i];
        Node* value = x->kind == N_TUPLE_E ? fi : fi->a;
        int fidx = i;
        if (x->kind == N_STRUCTLIT && !positional) fidx = find_field(t, fi->str, fi->len);
        if (!value || fidx < 0 || fidx >= t->nfields) return 0;
        SType* ft = t->fields[fidx].t;
        if (!ft || !is_scalar(ft) || !call_free_scalar_expr(value)) return 0;
        SType* vt = node_ty(value);
        if (vt && ty_size(vt) != ty_size(ft)) return 0;
        uint32_t fo = field_off(t, fidx);
        int w = 0;
        while (w < rc->nunits && rc->units[w].off != fo) w++;
        if (w == rc->nunits || byunit[w]) return 0;
        TU* u = &rc->units[w];
        if ((u->kind == TU_VREG) != (is_floatt(ft) != 0)) return 0;
        if (u->size != ty_size(ft)) return 0;
        byunit[w] = value;
    }
    int svi = e->vti, svf = e->vtf;
    int later[4] = {-1, -1, -1, -1};
    for (int w = 0; w < rc->nunits; w++) {
        TU* u = &rc->units[w];
        int fp = u->kind == TU_VREG;
        e->vti = fp ? 0 : w;
        e->vtf = fp ? w : 0;
        EV v = gen_expr(e, byunit[w]);
        if (fp && v.cls == CLS_FP) {
            // d<w> may be the home of a leaf a later value reads: the value
            // then waits in its temp, which that home's swap put in v16-v23
            int keep = 0;
            for (int k = w + 1; k < rc->nunits && !keep; k++) keep = reads_home(e, byunit[k], w);
            if (keep) { // parked in temp w, which the next leaf leaves alone
                if (v.idx != w) fmov_raw(e, u->size == 8, vreg(e, w), vreg(e, v.idx));
                later[w] = vreg(e, w);
            }
            else if (!fold_fp(e, w, vreg(e, v.idx))) fmov_raw(e, u->size == 8, w, vreg(e, v.idx));
        }
        else if (!fp && v.cls == CLS_INT) mov_int(e, 1, w, xreg(v.idx));
        else em_err(e, byunit[w], "return leaf has the wrong register class");
    }
    for (int w = 0; w < rc->nunits; w++)
        if (later[w] >= 0) fmov_raw(e, rc->units[w].size == 8, w, later[w]);
    e->vti = svi;
    e->vtf = svf;
    return 1;
}

/*
 * A call whose result returns in registers as whole 8-byte chunks (an HFA, or
 * a struct or tuple whose size is a multiple of 8) can store them straight
 * into a frame destination; anything else keeps the staged temp.
 */
static int sink_shape(SType* ret, RetCls* rc) {
    classify_ret(ret, rc);
    if (!ret || is_scalar(ret) || rc->sret || rc->nunits < 1) return 0;
    int fp = rc->units[0].kind == TU_VREG;
    for (int w = 1; w < rc->nunits; w++) if ((rc->units[w].kind == TU_VREG) != fp) return 0;
    return fp || ty_size(ret) % 8 == 0;
}

static void sink_arm(Em* e, Node* call, SType* ret, RetCls* rc) {
    e->sink_call = call;
    e->sink_ret = ret;
    e->sink_n = rc->nunits;
    e->sink_done = 0;
    e->sink_split = NULL;
}

// consume the one-shot sink; nonzero when emit_call stored the result
static int sink_take(Em* e) {
    int done = e->sink_done;
    e->sink_call = NULL;
    e->sink_ret = NULL;
    e->sink_done = 0;
    e->sink_split = NULL;
    return done;
}

// evaluate `call` into the aggregate at frame depth `depth`, storing its
// result registers directly when the return shape allows
static void gen_call_to_slot(Em* e, Node* call, uint32_t depth, SType* t) {
    SType* ret = node_ty(call);
    RetCls rc;
    if (ret && ty_size(ret) == ty_size(t) && sink_shape(ret, &rc)) {
        sink_arm(e, call, ret, &rc);
        for (int w = 0; w < rc.nunits; w++) e->sink_off[w] = depth - rc.units[w].off;
    }
    EV v = gen_expr(e, call);
    if (!sink_take(e)) store_to_slot(e, v, depth, t);
}

// typed destructuring of a tuple-returning call into the binders' slots: unit
// i lands in binder i when each is one whole 8-byte scalar of its unit's
// class. Returns 0, having emitted nothing, when the shape does not fit.
static int gen_call_to_binders(Em* e, Node* call, SType** wants, uint32_t* offs, int nb) {
    SType* tup = node_ty(call);
    RetCls rc;
    if (!tup || tup->kind != SK_TUPLE || tup->nfields != nb || !sink_shape(tup, &rc) || rc.nunits != nb) return 0;
    for (int i = 0; i < nb; i++) {
        SType* ft = tup->fields[i].t;
        TU* u = &rc.units[i];
        int fp = u->kind == TU_VREG;
        if (!ft || !is_scalar(ft) || u->off != field_off(tup, i) ||
            fp != (is_floatt(ft) != 0) || ty_size(ft) != (fp ? u->size : 8) ||
            ty_size(wants[i]) != ty_size(ft) ||
            is_floatt(wants[i]) != is_floatt(ft))
            return 0;
    }
    sink_arm(e, call, tup, &rc);
    for (int i = 0; i < nb; i++) e->sink_off[i] = offs[i];
    EV v = gen_expr(e, call);
    if (sink_take(e)) return 1;
    if (v.cls != CLS_ADDR) {
        em_err(e, call, "destructuring initializer must be a tuple");
        return 1;
    }
    for (int i = 0; i < nb; i++) store_member(e, xreg(v.idx), tup, i, offs[i]);
    return 1;
}

static void emit_stmt(Em* e, Node* s) {
    line_mark(e, s->kind == N_EXPRSTMT ? s->a : s);
    temps_reset(e); // statement boundary
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
            int all_new = s->b->kind == N_CALL;
            SType* bt[16] = {0};
            for (int i = 0; i < s->nkids; i++) {
                if (s->kids[i]->kind != N_PARAM) all_new = 0;
                else bt[i] = node_sym(s->kids[i])->type;
            }
            if (all_new && gen_call_to_binders(e, s->b, bt, offs, s->nkids)) return;
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
                    SType* ft = tup && tup->kind == SK_TUPLE && i < tup->nfields ? tup->fields[i].t : t;
                    EV v = load_scalar(e, xreg(rhs.idx), field_off(tup, i), ft);
                    store_scalar(e, v, xreg(ta), 0, ft);
                } else {
                    copy_mem(e, xreg(ta), 0, xreg(rhs.idx), field_off(tup, i), ty_size(t));
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
        Split* sp = nb == 1 ? split_at(e, s->kids[0]) : NULL;
        if (sp && sp->regs) { // field by field: a slot only for fields left out of registers
            for (int f = 0; f < sp->n; f++)
                if (split_freg(e, sp, f) < 0) {
                    slot_alloc(e, s->kids[0], ty_size(sp->type), ty_align(sp->type));
                    break;
                }
            if (s->b) split_write(e, sp, s->b);
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
            Split* src = split_live(e, s->b);
            if (src) {
                int ta = tmp_int(e);
                frame_addr(e, xreg(ta), offs[0]);
                split_store_at(e, src, xreg(ta), 0);
                return;
            }
            if (s->b->kind == N_CALL && !is_scalar(wants[0])) {
                gen_call_to_slot(e, s->b, offs[0], wants[0]);
                return;
            }
            EV v = gen_expr(e, s->b);
            store_to_slot(e, v, offs[0], wants[0]);
            return;
        }
        if (s->b->kind == N_CALL && gen_call_to_binders(e, s->b, wants, offs, nb)) return;
        // typed destructuring: rhs is a tuple in memory
        EV rhs = gen_expr(e, s->b);
        if (rhs.cls != CLS_ADDR) {
            em_err(e, s, "destructuring initializer must be a tuple");
            return;
        }
        SType* tup = node_ty(s->b);
        for (int i = 0; i < nb; i++) store_member(e, xreg(rhs.idx), tup, i, offs[i]);
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
                    SType* ft = tup && tup->kind == SK_TUPLE && i < tup->nfields ? tup->fields[i].t : tt;
                    EV v = load_scalar(e, xreg(rhs.idx),
                                       tup && tup->kind == SK_TUPLE
                                           ? field_off(tup, i) : (uint32_t)i * 8, ft);
                    store_scalar(e, v, xreg(ta), 0, ft);
                } else if (tt) {
                    uint32_t mo = tup && tup->kind == SK_TUPLE ? field_off(tup, i) : (uint32_t)i * 8;
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
        if (s->op == T_ASSIGN && !(s->flags & F_VOLATILE)) { // a split local, whole
            Split* sp = split_live(e, s->kids[0]);
            if (sp) {
                split_write(e, sp, s->a);
                return;
            }
            if ((sp = split_live(e, s->a))) {
                int ta = gen_addr(e, s->kids[0]);
                split_store_at(e, sp, xreg(ta), 0);
                return;
            }
        }
        // a register-returned aggregate call assigned to a frame-resident
        // local stores its result registers straight into it
        uint32_t sdepth;
        if (s->op == T_ASSIGN && !(s->flags & F_VOLATILE) && !is_scalar(t) &&
            s->a->kind == N_CALL && frame_place(e, s->kids[0], &sdepth)) {
            gen_call_to_slot(e, s->a, sdepth, t);
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
        int pr = local ? promo_reg(e, ls) : is_scalar(t) ? split_reg(e, lhs) : -1;
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
            if (pr >= 0 && is_floatt(t) && is_farith_op(op)) { // computed straight into the register
                int svf = e->vtf;
                EV rhs;
                int rb = fsrc_reg(e, s->a, &rhs);
                if (rhs.cls != CLS_FP) {
                    em_err(e, s, "float operand did not produce a float");
                    return;
                }
                emit_farith_raw(e, op, (uint32_t)pr, (uint32_t)pr, (uint32_t)rb, unwrap(t)->prim == P_F64);
                e->vtf = svf;
                return;
            }
            // A promoted integer target is read in place and the result
            // computed into a fresh temp before it is stored back.
            int in_place = pr >= 0 && !is_floatt(t) && !pointer;
            uint64_t imm;
            if (in_place && scalar_bits(t) >= 32 && op != T_SLASH &&
                op != T_PERCENT) { // compute straight into the register
                if (imm12_const(s->a, &imm) && emit_arith_imm(e, op, pr, pr, imm, t)) return;
                EV rhs;
                int rb = src_reg(e, s->a, &rhs);
                emit_checked_arith(e, op, pr, pr, rb, t);
                return;
            }
            EV old = in_place ? (EV){ CLS_INT, tmp_int(e) }
                   : direct == 2 ? load_preg(e, pr, t)
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
                emit_farith(e, op, old.idx, old.idx, rhs.idx, unwrap(t)->prim == P_F64);
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
                    if (imm12_const(r->b, &imm) && emit_arith_imm(e, rop, pr, ra, imm, rt)) return;
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
        if (pr >= 0 && is_floatt(t) && s->a->kind == N_BINARY && is_farith_op((TokKind)s->a->op)) {
            Node* r = s->a; // x = a op b: the last operation targets x's register
            SType* rt = node_ty(r->a);
            if (rt && is_floatt(rt) && unwrap(rt)->prim == unwrap(t)->prim) {
                int svf = e->vtf;
                EV a, b;
                int ra = fsrc_reg(e, r->a, &a);
                int rb = fsrc_reg(e, r->b, &b);
                if (a.cls != CLS_FP || b.cls != CLS_FP) {
                    em_err(e, s, "float operand did not produce a float");
                    return;
                }
                emit_farith_raw(e, (TokKind)r->op, (uint32_t)pr, (uint32_t)ra, (uint32_t)rb, unwrap(t)->prim == P_F64);
                e->vtf = svf;
                return;
            }
        }
        if (!direct && !(s->flags & F_VOLATILE) && s->a->kind == N_STRUCTLIT && split_type(t)) {
            lit_store_at(e, s->a, unwrap(t), xreg(ta));
            return;
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
            uint64_t endc;
            int up = lr >= 0 ? range_up(s->b, &endc) : 0;
            if (up) {
                // Ascending by one: the cursor runs from the start to the end
                // held in a register or an immediate, with no trip count.
                int rcur = lr, rend = lr + 1;
                uint64_t loc;
                const_int(s->b->a, &loc);
                load_imm(e, 1, rcur, range_value(loc, rt));
                if (up == 1 && const_int(s->b->b, &endc)) {
                    load_imm(e, 1, rend, range_value(endc, rt));
                } else if (up == 1) { // an unsigned end, zero-extended
                    int svi = e->vti;
                    EV hi = gen_expr(e, s->b->b);
                    if (!fold_preg(e, rend, xreg(hi.idx), rt)) set_preg(e, rend, xreg(hi.idx), rt);
                    e->vti = svi;
                }
                align_code(e, LOOP_ALIGN);
                bind_label(e, lcond);
                if (up == 2) ins(e, 0xF100001Fu | ((uint32_t)endc << 10) | ((uint32_t)rcur << 5)); // cmp cursor, #end
                else ins(e, 0xEB00001Fu | ((uint32_t)rend << 16) | ((uint32_t)rcur << 5));        // cmp cursor, end
                bcond_(e, ty_is_signed(rt) ? C_GE : C_HS, lbreak);
                if (vpr >= 0) set_preg(e, vpr, rcur, elem);
                else store_frame(e, rcur, vs, (uint32_t)((scalar_bits(elem) + 7) / 8));
                temps_reset(e);
                if (e->nctx >= 64) {
                    em_err(e, s, "control flow nested too deeply");
                    return;
                }
                e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
                emit_block(e, s->c);
                e->nctx--;
                bind_label(e, lcont);
                ins(e, 0x91000400u | ((uint32_t)rcur << 5) | (uint32_t)rcur); // add cursor, #1
                e->lreg_top -= loop_own_regs(s);
                br_(e, lcond);
                bind_label(e, lbreak);
                return;
            }
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
                uint64_t sc;
                if (e->o.mode == EMIT_DEBUG && !const_int(s->b->c, &sc)) {
                    // a runtime step must be a positive magnitude: checked once at entry
                    int ok = new_label(e);
                    uint32_t x = (uint32_t)xreg(step.idx);
                    if (rbits < 64) {
                        ins(e, (ty_is_signed(rt) ? sxtw_of(rbits) : uxtw_of(rbits)) | (x << 5) | 16u);
                        x = 16;
                    }
                    ins(e, 0xF100001Fu | (x << 5)); // cmp step, #0
                    bcond_(e, ty_is_signed(rt) ? C_GT : C_NE, ok);
                    emit_located_fail_call(e, s->b->c, "range step violation");
                    bind_label(e, ok);
                }
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
                temps_reset(e);
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
            if (vpr >= 0) set_preg(e, vpr, xreg(ti), elem);
            else store_frame(e, xreg(ti), vs, scalar_bits(elem) > rbits ? 8u : (uint32_t)((scalar_bits(elem) + 7) / 8));
            temps_reset(e);
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
            ins(e, 0x8B000000u | (xreg(td) << 16) | (xreg(ti2) << 5) | xreg(ti2));
            store_frame(e, xreg(ti2), islot, rsz);
            int tc2 = tmp_int(e);
            load_frame(e, xreg(tc2), cslot, 8, 0);
            ins(e, 0xD1000000u | (1u << 10) | (xreg(tc2) << 5) | xreg(tc2));
            store_frame(e, xreg(tc2), cslot, 8);
            e->vti = e->vti_base;
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
            if (ipr < 0) ivs = slot_alloc(e, ixv && ixv->decl ? ixv->decl : index_binder, 8, 8);
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
            if (vpr >= 0) load_binder(e, vpr, rp, 0, elem);
            else if (mut_iter) store_frame(e, rp, vs, 8);
            else {
                frame_addr(e, 15, vs);
                copy_mem(e, 15, 0, rp, 0, esz);
            }
            if (ipr >= 0) mov_int(e, 1, ipr, rix);
            else if (index_binder) store_frame(e, rix, ivs, 8);
            temps_reset(e);
            if (e->nctx >= 64) {
                em_err(e, s, "control flow nested too deeply");
                return;
            }
            e->ctx[e->nctx++] = (Ctx){ lbreak, lcont, s->str, s->len, 1 };
            emit_block(e, s->c);
            e->nctx--;
            bind_label(e, lcont);
            if (esz <= 4095) ins(e, 0x91000000u | (esz << 10) | ((uint32_t)rp << 5) | (uint32_t)rp);
            else {
                load_imm(e, 1, 16, esz);
                ins(e, 0x8B000000u | (16u << 16) | ((uint32_t)rp << 5) | (uint32_t)rp);
            }
            if (index_binder) ins(e, 0x91000000u | (1u << 10) | ((uint32_t)rix << 5) | (uint32_t)rix);
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
        if (vpr >= 0) load_binder(e, vpr, xreg(tp), 0, elem);
        else if (mut_iter) store_frame(e, xreg(tp), vs, 8);
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
        temps_reset(e);
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
        if (esz <= 4095) ins(e, 0x91000000u | (esz << 10) | (xreg(tp2) << 5) | xreg(tp2));
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
        e->vti = e->vti_base;
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
                    ins(e, (sf ? 0xEB00001Fu : 0x6B00001Fu) | (xreg(p.idx) << 16) | (xreg(vi) << 5));
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
        if (e->ninl) {
            inline_return(e, s);
            return;
        }
        if (self_return(e, s->a)) { // emit_call branches back to the parameter reads
            e->self_call = s->a;
            gen_expr(e, s->a);
            e->self_call = NULL;
            e->ended_return = 1;
            return;
        }
        SType* rt = e->fnret;
        RetCls rc;
        classify_ret(rt, &rc);
        if (!s->a) {
            emit_defers(e, rt);
            emit_epilogue(e);
            return;
        }
        Split* sp = split_live(e, s->a);
        if (sp && rc.sret) { // field by field into the caller's buffer
            load_frame(e, 15, e->sret_slot, 8, 0);
            split_store_at(e, sp, 15, 0);
            emit_defers(e, rt);
            emit_epilogue(e);
            return;
        }
        if (sp && rc.nunits >= 1) { // each result register built from the fields
            for (int w = 0; w < rc.nunits; w++) split_unit(e, sp, &rc.units[w], rc.units[w].reg);
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
            if (v.cls == CLS_FP && !fold_fp(e, 0, vreg(e, v.idx)))
                fmov_raw(e, unwrap(rt)->prim == P_F64, 0, vreg(e, v.idx));
            else mov_int(e, scalar_bits(rt) == 64, 0, xreg(v.idx));
            emit_defers(e, rt);
            emit_epilogue(e);
            return;
        }
        if (rc.nunits >= 1) { // <=16B aggregate/multi-chunk
            if (gen_return_regs(e, s->a, &rc)) {
                emit_defers(e, rt);
                emit_epilogue(e);
                return;
            }
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
                } else ldrf_(e, rc.units[w].reg, xreg(v.idx), rc.units[w].off, rc.units[w].size == 8);
            }
            if (defer >= 0) ld_reg(e, rc.units[defer].reg, xreg(v.idx), rc.units[defer].off, 8, 0);
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
        em_err(e, s, "codegen for '%s' statements comes in the next chunk", node_kind_name((NKind)s->kind));
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
    if ((wlen == 3 && memcmp(p + start, "svc", 3) == 0) || (wlen == 3 && memcmp(p + start, "brk", 3) == 0)) {
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
    e->subsp_fix = e->o.nfix;
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
        ins(e, op | ((imm & 0x7Fu) << 15) | ((uint32_t)(first + k + 1) << 10) | (29u << 5) | (uint32_t)(first + k));
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

/* Promoted float locals in the caller-saved d24-d31 do not survive a branch-
 * with-link, so every one stores them first and reloads them on return. */
static void fp_leaves(Em* e, int load) {
    if (e->nfpleaf) save_regs(e, 24, e->nfpleaf, e->fpleaf_save, 1, load);
}

static void callee_saved(Em* e, int load) {
    if (e->nsaved) save_regs(e, 19, e->nsaved, e->lreg_save, 0, load);
    int own = e->nfpreg - e->nfpleaf;
    if (own) save_regs(e, 8, own, e->fpreg_save, 1, load);
}

static void emit_epilogue(Em* e) {
    e->ended_return = 1;
    if (e->o.mode == EMIT_DEBUG && e->current_is_main) {
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
    if (d <= 4095) ins(e, 0x91000000u | (d << 10) | (29u << 5) | ((uint32_t)rd & 31u));
    else {
        load_imm(e, 1, rd, d);
        ins(e, 0x8B000000u | ((uint32_t)rd << 16) | (29u << 5) | ((uint32_t)rd & 31u));
    }
}

// --- loop registers --------------------------------------------------------------------

// a range endpoint's constant as the exact 64-bit value its register holds
static uint64_t range_value(uint64_t v, SType* rt) {
    uint32_t bits = 8u * ty_size(rt);
    if (bits >= 64) return v;
    v &= (UINT64_C(1) << bits) - 1;
    if (ty_is_signed(rt) && (v >> (bits - 1)) & 1) v |= ~((UINT64_C(1) << bits) - 1);
    return v;
}

/*
 * A range loop that counts up from its start with no direction test or trip
 * count: its step is one (omitted or a constant 1), and it ascends by
 * construction, from a constant zero over an unsigned type or between two
 * constant endpoints in order. Returns 0 for the general lowering, 1 when the
 * end is held in a register, and 2 when *end is a cmp immediate.
 */
static int range_up(Node* r, uint64_t* end) {
    SType* rt = node_ty(r);
    if (!rt || rt->kind == SK_UT) rt = default_i64();
    uint64_t lo, hi, step;
    if (r->c && (!const_int(r->c, &step) || range_value(step, rt) != 1)) return 0;
    if (!const_int(r->a, &lo)) return 0;
    lo = range_value(lo, rt);
    int sig = ty_is_signed(rt);
    if (const_int(r->b, &hi)) {
        hi = range_value(hi, rt);
        if (sig ? (int64_t)lo > (int64_t)hi : lo > hi) return 0;
        *end = hi;
        return hi <= 4095 ? 2 : 1;
    }
    return !sig && lo == 0;
}

// --- inline expansion planning (Functions & ABI #8) ---------------------------------

// The inline function a direct call names, with the type arguments its body
// is stamped under; mirrors gen_call's target selection.
static Node* inline_target(Node* call, SType*** targs, int* ntargs) {
    Node* callee = call->a;
    *targs = NULL;
    *ntargs = 0;
    if (callee->kind == N_INDEX && callee->sym) {
        Sym* gs = node_sym(callee);
        if (!gs->decl || !(gs->decl->flags & F_INLINE)) return NULL;
        for (STypeInst* inst = sema_insts(gs->decl); inst; inst = inst->next)
            if (inst->result == callee->ty) {
                *targs = inst->args;
                *ntargs = gs->decl->c ? gs->decl->c->nkids : 0;
                return gs->decl;
            }
        return NULL;
    }
    Sym* s = call->sym ? node_sym(call) : NULL;
    if (!s || s->kind != SYM_FUNC || s->is_extern || !s->decl || !(s->decl->flags & F_INLINE)) return NULL;
    if (s->is_generic) { // a method of a generic type: its owner's arguments
        SType* cft = node_ty(callee);
        if (!cft || cft->kind != SK_FUNC) cft = s->type;
        SType* owner = (call->flags & F_ASSOC) ? (callee->kind == N_FIELD_E ? node_ty(callee->a) : NULL)
                     : cft && cft->nparams > 0 ? cft->params[0] : NULL;
        if (!owner) return NULL;
        *targs = owner->args;
        *ntargs = owner->nargs;
    }
    return s->decl;
}

// n and everything under it, each node once; Node.d only as a general for's body
static Node* inline_copy(Em* e, Node* n) {
    if (!n) return NULL;
    Node* c = (Node*)ptrmap_get(&e->copy_node, n);
    if (c) return c;
    c = (Node*)arena_alloc(sizeof(Node));
    *c = *n;
    ptrmap_put(&e->copy_node, n, (uintptr_t)c);
    c->a = inline_copy(e, n->a);
    c->b = inline_copy(e, n->b);
    c->c = inline_copy(e, n->c);
    if (n->kind == N_FOR && (n->flags & F_FORGEN)) c->d = inline_copy(e, n->d);
    if (n->nkids) {
        c->kids = (Node**)arena_alloc((size_t)n->nkids * sizeof(Node*));
        for (int i = 0; i < n->nkids; i++) c->kids[i] = inline_copy(e, n->kids[i]);
    }
    return c;
}

// point a copy's names at fresh locals whose binders are the copied ones
static void inline_resym(Em* e, Node* c, PtrMap* seen) {
    if (!c || ptrmap_get(seen, c)) return;
    ptrmap_put(seen, c, 1);
    Sym* s = node_sym(c);
    if (s && s->kind == SYM_VAR && !s->is_global && s->decl) {
        Node* d = (Node*)ptrmap_get(&e->copy_node, s->decl);
        if (d) {
            Sym* ns = (Sym*)ptrmap_get(&e->copy_sym, s);
            if (!ns) {
                ns = (Sym*)arena_alloc(sizeof(Sym));
                *ns = *s;
                ns->decl = d;
                ptrmap_put(&e->copy_sym, s, (uintptr_t)ns);
            }
            c->sym = ns;
        }
    }
    inline_resym(e, c->a, seen);
    inline_resym(e, c->b, seen);
    inline_resym(e, c->c, seen);
    if (c->kind == N_FOR && (c->flags & F_FORGEN)) inline_resym(e, c->d, seen);
    for (int i = 0; i < c->nkids; i++) inline_resym(e, c->kids[i], seen);
}

static void inline_plan(Em* e, Node* n);

// plan the expansion of one call to an inline function, and of those in its copy
static void inline_plan_call(Em* e, Node* call) {
    SType** targs;
    int ntargs;
    Node* f = inline_target(call, &targs, &ntargs);
    if (!f || !f->b) return;
    for (int i = 0; i < e->ninl_path; i++)
        if (e->inl_path[i] == f) { // sema reports cycles it can see before stamping
            em_err(e, call, "inline function '%.*s' reaches itself through inline calls; a recursive call cannot be expanded",
                   (int)f->len, f->str);
            return;
        }
    Node* callee = call->a;
    Node* args[41];
    int n = 0;
    if (callee->kind == N_FIELD_E && !(call->flags & (F_METHOD | F_ASSOC))) args[n++] = callee->a; // receiver
    if (n + call->nkids != f->nkids || f->nkids > 40) return;
    for (int i = 0; i < call->nkids; i++) args[n++] = call->kids[i];
    Sym* fs = node_sym(f);
    if (f->c && !(fs && fs->spec_args)) sema_stamp_body(f, targs, ntargs);

    ptrmap_clear(&e->copy_node);
    ptrmap_clear(&e->copy_sym);
    InlineSite* site = (InlineSite*)arena_alloc(sizeof(InlineSite));
    memset(site, 0, sizeof *site);
    site->n = n;
    site->arg = (Node**)arena_alloc((size_t)(n ? n : 1) * sizeof(Node*));
    site->bind = (Node**)arena_alloc((size_t)(n ? n : 1) * sizeof(Node*));
    site->byref = (uint8_t*)arena_alloc((size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) site->bind[i] = inline_copy(e, f->kids[i]);
    site->body = inline_copy(e, f->b);
    PtrMap seen = {0};
    for (int i = 0; i < n; i++) inline_resym(e, site->bind[i], &seen);
    inline_resym(e, site->body, &seen);
    ptrmap_free(&seen);

    for (int i = 0; i < n; i++) {
        Node* p = f->kids[i];
        Sym* ps = node_sym(site->bind[i]);
        int mut = (p->flags & F_MUT) || ((f->flags & F_METHOD) && i == 0);
        Node* a = args[i];
        if (a->kind == N_MUTARG) a = a->a;
        site->arg[i] = a;
        // a read-only aggregate binds to the argument's storage, as the
        // by-address parameter ABI allows (Functions & ABI #3)
        site->byref[i] = mut || !ps || !is_scalar(ps->type);
        if (site->byref[i]) continue;
        Node* v = (Node*)arena_alloc(sizeof(Node));
        memset(v, 0, sizeof *v);
        v->kind = N_VAR;
        v->path = call->path;
        v->line = call->line;
        v->col = call->col;
        v->kids = (Node**)arena_alloc(sizeof(Node*));
        v->kids[0] = site->bind[i];
        v->nkids = 1;
        v->b = a;
        site->bind[i] = v;
    }
    SType* ret = node_ty(call);
    site->ret = ret && unwrap(ret)->kind != SK_VOID ? ret : NULL;
    Node* body = site->body;
    if (body->nkids && body->kids[body->nkids - 1]->kind == N_RETURN) site->tail = body->kids[body->nkids - 1];
    ptrmap_put(&e->inl_ix, call, (uintptr_t)site);

    if (e->ninl_path == e->cinl_path) {
        e->cinl_path = e->cinl_path ? e->cinl_path * 2 : 16;
        e->inl_path = realloc(e->inl_path, (size_t)e->cinl_path * sizeof(Node*));
        if (!e->inl_path) oom();
    }
    e->nsites++;
    e->inl_path[e->ninl_path++] = f;
    if (f->flags & F_INLCALLS) inline_plan(e, site->body);
    e->ninl_path--;
}

/*
 * Before a function is emitted, attach a copy of the callee to every direct
 * call of an inline function in its body, innermost calls included. A
 * deferred call stays a call: it runs at the exits; its arguments may inline.
 */
static void inline_plan(Em* e, Node* n) {
    if (!n) return;
    if (n->kind == N_DEFER && n->a && n->a->kind == N_CALL) {
        Node* c = n->a;
        inline_plan(e, c->a);
        for (int i = 0; i < c->nkids; i++) inline_plan(e, c->kids[i]);
        return;
    }
    inline_plan(e, n->a);
    inline_plan(e, n->b);
    inline_plan(e, n->c);
    if (n->kind == N_FOR && (n->flags & F_FORGEN)) inline_plan(e, n->d);
    for (int i = 0; i < n->nkids; i++) inline_plan(e, n->kids[i]);
    if (n->kind == N_CALL) inline_plan_call(e, n);
}

// registers a range or sequence loop keeps in x19-x28 (0: general loop)
static int loop_own_regs(Node* s) {
    if (s->kind != N_FOR || (s->flags & F_FORGEN)) return 0;
    if (s->b->kind == N_RANGE) {
        uint64_t end;
        int up = range_up(s->b, &end);
        return up == 2 ? 1 : up ? 2 : 3; // cursor [, end] or cursor, count, step
    }
    return s->nkids == 2 ? 3 : 2;           // cursor, end [, index]
}

/*
 * One linear walk over a function's statements before it is emitted. Returns
 * the register need of the deepest loop chain in s. A loop takes registers
 * when its own chain fits the pool, so inner loops win and no nested chain of
 * register loops can exceed LOOP_REGS; loops beyond that keep frame slots.
 */
static int plan_loops(Em* e, Node* s);

// the loop register need of the inline expansions within expression x
static int plan_inline_loops(Em* e, Node* x) {
    if (!x || !e->nsites) return 0;
    int need = 0, n;
    InlineSite* site = inline_site(e, x);
    if (site) need = plan_loops(e, site->body);
    if ((n = plan_inline_loops(e, x->a)) > need) need = n;
    if ((n = plan_inline_loops(e, x->b)) > need) need = n;
    if ((n = plan_inline_loops(e, x->c)) > need) need = n;
    for (int i = 0; i < x->nkids; i++) if ((n = plan_inline_loops(e, x->kids[i])) > need) need = n;
    return need;
}

static int plan_loops(Em* e, Node* s) {
    if (!s) return 0;
    int inner = 0, n;
    switch (s->kind) {
    case N_BLOCK:
        for (int i = 0; i < s->nkids; i++) if ((n = plan_loops(e, s->kids[i])) > inner) inner = n;
        return inner;
    case N_IF:
        inner = plan_inline_loops(e, s->a);
        if ((n = plan_loops(e, s->b)) > inner) inner = n;
        if ((n = plan_loops(e, s->c)) > inner) inner = n;
        return inner;
    case N_SWITCH:
        inner = plan_inline_loops(e, s->a);
        for (int i = 0; i < s->nkids; i++) {
            if ((n = plan_inline_loops(e, s->kids[i]->a)) > inner) inner = n;
            for (int j = 0; j < s->kids[i]->nkids; j++)
                if ((n = plan_inline_loops(e, s->kids[i]->kids[j])) > inner) inner = n;
            if ((n = plan_loops(e, s->kids[i]->b)) > inner) inner = n;
        }
        return inner;
    case N_LABEL:
        return plan_loops(e, s->a);
    case N_ASM:
        e->fn_has_asm = 1;
        return 0;
    case N_FOR: {
        if (s->flags & F_FORGEN) { // a general loop keeps its state in frame slots
            inner = plan_loops(e, s->d);
            if ((n = plan_loops(e, s->a)) > inner) inner = n;
            if ((n = plan_inline_loops(e, s->b)) > inner) inner = n;
            if ((n = plan_loops(e, s->c)) > inner) inner = n;
            return inner;
        }
        inner = plan_inline_loops(e, s->b); // the header runs before the loop
        int below = loop_own_regs(s) + plan_loops(e, s->c);
        if (below <= LOOP_REGS) {
            if (e->nlreg_loops == e->clreg_loops) {
                e->clreg_loops = e->clreg_loops ? e->clreg_loops * 2 : 16;
                e->lreg_loops = realloc(e->lreg_loops, (size_t)e->clreg_loops * sizeof(Node*));
                if (!e->lreg_loops) oom();
            }
            e->lreg_loops[e->nlreg_loops++] = s;
            if (below > e->lreg_max) e->lreg_max = below;
        }
        return below > inner ? below : inner;
    }
    default:
        return plan_inline_loops(e, s);
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
 * further use is a round-trip through memory the register avoids. x9-x14
 * need no save, so a single use qualifies there for a local no call clobbers.
 */
#define PROMO_MIN_WEIGHT 2

static uint32_t use_weight(int depth) {
    if (depth <= 0) return 1;
    return 1u << (3 * (depth < 5 ? depth : 5));
}

static PromoCand* promo_find(Em* e, Node* x) {
    Sym* s = x && x->kind == N_IDENT ? node_sym(x) : NULL;
    if (!s || !s->decl) return NULL;
    uintptr_t ix = ptrmap_get(&e->pcand_ix, s->decl);
    return ix ? &e->pcands[ix - 1] : NULL;
}

// binder of a scalar local, loop binder, or by-value parameter; with byref, a
// by-reference parameter of any type, whose pointer is the candidate
static void promo_add(Em* e, Node* binder, uint8_t byref) {
    Sym* s = node_sym(binder);
    if (!s || s->kind != SYM_VAR || s->is_global || s->decl != binder || (!byref && !is_scalar(s->type))) return;
    if (e->npcands == e->cpcands) {
        e->cpcands = e->cpcands ? e->cpcands * 2 : 16;
        e->pcands = realloc(e->pcands, (size_t)e->cpcands * sizeof(PromoCand));
        if (!e->pcands) oom();
    }
    if (!ptrmap_get(&e->pcand_ix, binder)) ptrmap_put(&e->pcand_ix, binder, (uintptr_t)e->npcands + 1);
    uint32_t at = ++e->scan_pos; // defined here: after its initializer, at entry for a parameter
    e->pcands[e->npcands++] = (PromoCand){ binder, 0, 0, 0, byref, -1, at, at, -1 };
}

// the candidate of a split local's field placeholder
static PromoCand* promo_cand(Em* e, Node* decl) {
    uintptr_t ix = ptrmap_get(&e->pcand_ix, decl);
    return ix ? &e->pcands[ix - 1] : NULL;
}

// a struct with at most four direct scalar fields and no padding
static int split_type(SType* t) {
    t = unwrap(t);
    if (!t || t->kind != SK_STRUCT || t->nfields < 1 || t->nfields > 4 || t->is_packed || t->requested_align) return 0;
    uint32_t sum = 0;
    for (int i = 0; i < t->nfields; i++) {
        if (!is_scalar(t->fields[i].t) || t->fields[i].requested_align) return 0;
        sum += ty_size(t->fields[i].t);
    }
    return sum == ty_size(t);
}

/*
 * Binder of a single-binder struct local of split_type: each field gets a
 * placeholder binder of its own type that competes for a register as any
 * scalar local does. A use that needs the struct whole in memory marks it bad,
 * and its fields keep the slot.
 */
static void split_add(Em* e, Node* binder) {
    Sym* s = node_sym(binder);
    if (!s || s->kind != SYM_VAR || s->is_global || s->decl != binder || !split_type(s->type)) return;
    if (e->nsplits == e->csplits) {
        e->csplits = e->csplits ? e->csplits * 2 : 16;
        e->splits = realloc(e->splits, (size_t)e->csplits * sizeof(Split));
        if (!e->splits) oom();
    }
    SType* t = unwrap(s->type);
    Split* sp = &e->splits[e->nsplits];
    *sp = (Split){ binder, t, {0}, t->nfields, 0, 0 };
    for (int i = 0; i < t->nfields; i++) {
        Node* ph = (Node*)arena_alloc(sizeof(Node));
        Sym* fs = (Sym*)arena_alloc(sizeof(Sym));
        memset(ph, 0, sizeof *ph);
        memset(fs, 0, sizeof *fs);
        ph->kind = N_PARAM;
        ph->path = binder->path;
        ph->line = binder->line;
        ph->col = binder->col;
        fs->name = s->name;
        fs->len = s->len;
        fs->kind = SYM_VAR;
        fs->type = t->fields[i].t;
        fs->decl = ph;
        ph->sym = fs;
        ph->ty = fs->type;
        sp->field[i] = ph;
        promo_add(e, ph, 0);
    }
    ptrmap_put(&e->split_ix, binder, (uintptr_t)++e->nsplits);
}

// an address use needs the slot; a by-reference pointer is that address already
static void promo_escape(Em* e, Node* x) {
    Split* sp = split_of(e, x); // the struct itself, or a field as a place (&x.f, x.p.g)
    if (!sp && x && x->kind == N_FIELD_E) sp = split_of(e, x->a);
    if (sp) sp->bad = 1;
    PromoCand* c = promo_find(e, x);
    if (c && !c->byref) c->escaped = 1;
}

/*
 * Whether gen_call lowers `call` without a bl that may clobber x9-x14: casts,
 * constructors, compile-time builtins, assert/panic (which never return), and
 * intrinsics that emit inline code or the x8/x15-x17-only byte helpers. This
 * mirrors gen_call's dispatch conservatively; emit_call rejects any real call
 * inside the live range of a local promoted to x9-x14, so a missed case cannot
 * miscompile.
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
            "typeid", "sizeof", "alignof", "blank", "offsetof", "static_assert",
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
        return is_bits_integer_op((int)bi) ||
               (bi >= BI_MATH_SQRT && bi <= BI_MATH_FMA);
    }
}

/*
 * Operand-count estimate of the int temps evaluating `n` holds at once:
 * operands are evaluated in order with earlier results held, so an expression
 * needs the most of i + (its i-th operand's need), and at least one temp per
 * operand plus its result. Statements hold nothing across their parts.
 * Lowerings with internal temps can exceed it; see TEMP_MARGIN.
 */
static int temp_need(Em* e, Node* n) {
    if (!n) return 0;
    InlineSite* site = inline_site(e, n);
    if (site) { // its result, above each binding and each body statement
        int need = 1, kn;
        for (int i = 0; i < site->n; i++)
            if ((kn = 1 + temp_need(e, site->byref[i] ? site->arg[i] : site->bind[i])) > need) need = kn;
        for (int i = 0; i < site->body->nkids; i++)
            if ((kn = 1 + temp_need(e, site->body->kids[i])) > need) need = kn;
        return need;
    }
    Node* part[4] = { n->a, n->b, n->c, NULL };
    if (n->kind == N_FOR && (n->flags & F_FORGEN)) part[3] = n->d;
    int held = 0, need = 0, count = 0;
    int stmt = n->kind == N_BLOCK || n->kind == N_IF || n->kind == N_FOR ||
               n->kind == N_SWITCH || n->kind == N_CASE || n->kind == N_LABEL ||
               n->kind == N_DEFER;
    for (int i = 0; i < 4 + n->nkids; i++) {
        Node* k = i < 4 ? part[i] : n->kids[i - 4];
        if (!k) continue;
        int kn = temp_need(e, k) + (stmt ? 0 : held);
        if (kn > need) need = kn;
        held++;
        count++;
    }
    if (!stmt && count + 1 > need) need = count + 1;
    return need;
}

// pending in-place reads are resolved when the innermost frame closes
static void scan_open(Em* e) {
    if (e->nscan_frame == e->cscan_frame) {
        e->cscan_frame = e->cscan_frame ? e->cscan_frame * 2 : 64;
        e->scan_frame = realloc(e->scan_frame, (size_t)e->cscan_frame * sizeof(int));
        if (!e->scan_frame) oom();
    }
    e->scan_frame[e->nscan_frame++] = e->nscan_pend;
}

// the reads made since the frame opened stay live to position `at`
static void scan_close(Em* e, uint32_t at) {
    int start = e->scan_frame[--e->nscan_frame];
    for (int i = start; i < e->nscan_pend; i++) {
        PromoCand* c = &e->pcands[e->scan_pend[i]];
        if (c->last < at) c->last = at;
    }
    e->nscan_pend = start;
}

static void promo_scan(Em* e, Node* n, int depth);
static void scan_use(Em* e, PromoCand* c, int depth);

/*
 * Whether gen_call passes `call`'s arguments through emit_call, which builds
 * a split local's argument units from its fields: a direct call to a
 * non-builtin function, a generic stamp, a function value, or a contract
 * method. Casts, constructors, and builtins evaluate their operands otherwise.
 */
static int split_call_ok(Node* call) {
    Node* callee = call->a;
    if (call_is_inline(call) || (call->d && call->d->kind == N_CONFORMANCE)) return 0;
    if (callee->kind == N_INDEX) return callee->sym != NULL;
    if (!call->sym) {
        SType* bt = callee->kind == N_FIELD_E ? node_ty(callee->a) : NULL;
        return bt && bt->kind == SK_CONTRACT;
    }
    Sym* s = node_sym(call);
    return s->kind == SYM_VAR || (s->kind == SYM_FUNC && s->decl && !s->decl->ival);
}

// a whole-struct read or write of a split local uses every field
static void split_use(Em* e, Split* sp, int depth) {
    for (int i = 0; i < sp->n; i++) scan_use(e, promo_cand(e, sp->field[i]), depth);
}

// a value a whole-struct assignment reads: a split local is read field by field
static void scan_value(Em* e, Node* n, int depth) {
    Split* sp = split_of(e, n);
    if (sp) split_use(e, sp, depth);
    else promo_scan(e, n, depth);
}

// one statement part: the emitter reads promoted operands in place by its end
static void scan_part(Em* e, Node* n, int depth) {
    if (!n) return;
    scan_open(e);
    promo_scan(e, n, depth);
    scan_close(e, ++e->scan_pos);
}

// a loop, or a labeled block `continue` may restart
static void scan_loop_push(Em* e) {
    uint32_t at = ++e->scan_pos;
    if (e->nscan_loop < 64) { // deeper nesting fails in emission
        e->scan_loop[e->nscan_loop] = at;
        e->scan_lpend[e->nscan_loop] = -1;
    }
    e->nscan_loop++;
}

// A value defined before the loop and used in it stays live to its back edge.
// Its pend names that loop from its first such use until the loop closes, so
// each candidate waits on at most one loop's list.
static void scan_loop_pop(Em* e) {
    uint32_t end = ++e->scan_pos;
    int k = --e->nscan_loop;
    if (k >= 64) return;
    for (int i = e->scan_lpend[k]; i >= 0; ) {
        PromoCand* c = &e->pcands[i];
        if (c->last < end) c->last = end;
        c->pend = -1;
        i = c->lnext;
    }
}

static void scan_use(Em* e, PromoCand* c, int depth) {
    c->weight += use_weight(depth);
    uint32_t at = ++e->scan_pos;
    if (e->scan_defer) { // read again at every exit, after other defers' calls
        c->last = UINT32_MAX;
        c->crosses = 1;
    } else {
        if (c->last < at) c->last = at;
        if (e->nscan_pend == e->cscan_pend) {
            e->cscan_pend = e->cscan_pend ? e->cscan_pend * 2 : 64;
            e->scan_pend = realloc(e->scan_pend, (size_t)e->cscan_pend * sizeof(int));
            if (!e->scan_pend) oom();
        }
        e->scan_pend[e->nscan_pend++] = (int)(c - e->pcands);
    }
    int nl = e->nscan_loop < 64 ? e->nscan_loop : 64;
    for (int k = 0; k < nl; k++) // the outermost loop begun after its definition
        if (e->scan_loop[k] > c->first) {
            if (c->pend < 0) {
                c->pend = k;
                c->lnext = e->scan_lpend[k];
                e->scan_lpend[k] = (int)(c - e->pcands);
            }
            break;
        }
}

/*
 * One walk over a function body before it is emitted: collect single-binder
 * scalar declarations, loop binders, and integer union payload binders,
 * weight every use by its loop depth, and mark each place the emitter takes a
 * variable's address or needs its storage (&x, a volatile read or store, a mut
 * argument, a field/index/slice base, a tuple assignment target, a sequence
 * loop's iterable). Follows a/b/c/kids only; Node.d is read solely as a
 * general for's body, since sema's d links form cycles.
 *
 * The walk numbers positions in emission order and gives every candidate a
 * live interval [first, last]. A use extends it to where the emitter last
 * reads the value, which for an operand read in place is its consumer: a
 * direct call argument is consumed by that call's bl, any other use by the end
 * of its statement part. A use inside a loop begun after the definition
 * extends it to that loop's end. A real call is recorded at its bl, after its
 * arguments; a deferred call runs at the exits instead, and a use inside a
 * defer is live to them.
 */
static void promo_scan(Em* e, Node* n, int depth) {
    if (!n) return;
    if (depth > 0 && !e->scan_loop_branch) {
        SType* t = node_ty(n);
        if (t && !is_scalar(t) && ty_size(t) > 32) e->scan_loop_branch = 1;
    }
    switch (n->kind) {
    case N_IDENT: {
        Split* sp = split_of(e, n);
        if (sp) { // a struct local anywhere split_use does not cover
            sp->bad = 1;
            return;
        }
        PromoCand* c = promo_find(e, n);
        if (c) scan_use(e, c, depth);
        return;
    }
    case N_VAR:
        if (n->flags & F_MIXED) {
            for (int i = 0; i < n->nkids; i++) if (n->kids[i]->kind != N_PARAM) promo_escape(e, n->kids[i]);
        } else if (n->nkids == 1) {
            if (n->b) {
                scan_open(e);
                scan_value(e, n->b, depth);
                scan_close(e, ++e->scan_pos);
            }
            promo_add(e, n->kids[0], 0);
            split_add(e, n->kids[0]);
            promo_scan(e, n->kids[0], depth);
            return;
        }
        break;
    case N_ASSIGN: // one frame: the target's read and write order varies
        if (n->nkids > 1 || (n->flags & F_VOLATILE)) {
            for (int i = 0; i < n->nkids; i++) promo_escape(e, n->kids[i]);
        } else if (n->op == T_ASSIGN && (split_of(e, n->kids[0]) || split_of(e, n->a))) {
            Split* dst = split_of(e, n->kids[0]);
            scan_value(e, n->a, depth);
            if (dst) split_use(e, dst, depth);
            else promo_scan(e, n->kids[0], depth);
            return;
        }
        break;
    case N_UNARY:
        if (n->op == T_AMP || n->op == T_VOLATILE) promo_escape(e, n->a);
        break;
    case N_FIELD_E: {
        Split* sp = split_of(e, n->a);
        if (sp) { // one field's own use; a method or anything else needs the struct
            int f = find_field(sp->type, n->str, n->len);
            if (f < 0) sp->bad = 1;
            else scan_use(e, promo_cand(e, sp->field[f]), depth);
            return;
        }
        promo_escape(e, n->a);
        break;
    }
    case N_MUTARG: case N_INDEX: case N_SLICE_E:
        promo_escape(e, n->a);
        break;
    case N_CALL: {
        InlineSite* site = inline_site(e, n);
        if (site) { // the bindings, then the body, in emission order
            for (int i = 0; i < site->n; i++) {
                if (!site->byref[i]) {
                    promo_scan(e, site->bind[i], depth);
                    continue;
                }
                promo_escape(e, site->arg[i]); // its storage's address is bound
                scan_part(e, site->arg[i], depth);
                promo_add(e, site->bind[i], 1);
            }
            for (int i = 0; i < site->body->nkids; i++) scan_part(e, site->body->kids[i], depth);
            return;
        }
        if (n->a && n->a->kind == N_FIELD_E) { // a callee field: the call takes it as a place
            Split* sp = split_of(e, n->a->a);
            if (sp) sp->bad = 1;
        }
        int real = !call_is_inline(n);
        if (real) e->scan_call = 1;
        // Any call inside a loop may branch: a real one, or an inline lowering
        // that reaches a shared byte routine.
        if (depth > 0) e->scan_loop_branch = 1;
        scan_open(e);
        promo_scan(e, n->a, depth);
        promo_scan(e, n->b, depth);
        promo_scan(e, n->c, depth);
        // a split local passed by value is read field by field at the bl,
        // except in a defer, whose arguments are captured into slots, and
        // except when the ABI passes it by address: kept whole in its slot,
        // that address travels without a copy
        int by_value = !e->scan_defer && split_call_ok(n);
        for (int i = 0; i < n->nkids; i++) {
            Split* sp = by_value ? split_of(e, n->kids[i]) : NULL;
            uint32_t offs[5];
            uint8_t dbl[5];
            if (sp && ty_size(sp->type) > 16 && classify_hfa(sp->type, offs, dbl) < 1) sp->bad = 1;
            else if (by_value) scan_value(e, n->kids[i], depth);
            else promo_scan(e, n->kids[i], depth);
        }
        uint32_t at = ++e->scan_pos;
        scan_close(e, at);
        ptrmap_put(&e->call_pos, n, (uintptr_t)at + 1);
        if (real && n != e->scan_deferred) {
            if (e->nscan_calls == e->cscan_calls) {
                e->cscan_calls = e->cscan_calls ? e->cscan_calls * 2 : 64;
                e->scan_calls = realloc(e->scan_calls, (size_t)e->cscan_calls * sizeof(uint32_t));
                if (!e->scan_calls) oom();
            }
            e->scan_calls[e->nscan_calls++] = at;
        }
        return;
    }
    case N_DEFER: { // its arguments are captured here, but its call runs at exits
        Node* outer = e->scan_deferred;
        e->scan_defer++;
        e->scan_deferred = n->a;
        scan_part(e, n->a, depth);
        e->scan_deferred = outer;
        e->scan_defer--;
        return;
    }
    case N_LABEL:
        scan_loop_push(e);
        scan_part(e, n->a, depth);
        scan_loop_pop(e);
        return;
    case N_RETURN:
        if (split_of(e, n->a)) { // returned field by field
            split_use(e, split_of(e, n->a), depth);
            return;
        }
        break;
    case N_CASE: // patterns bind before the body runs
        for (int i = 0; i < n->nkids; i++) scan_part(e, n->kids[i], depth);
        scan_part(e, n->a, depth);
        scan_part(e, n->b, depth);
        scan_part(e, n->c, depth);
        return;
    case N_VARPAT:
        if (n->b && node_sym(n->b) && !is_floatt(node_sym(n->b)->type)) promo_add(e, n->b, 0);
        if (n->b) split_add(e, n->b);
        break;
    case N_FOR:
        if (n->flags & F_FORGEN) {
            scan_part(e, n->a, depth);
            scan_loop_push(e);
            scan_part(e, n->b, depth + 1);
            scan_part(e, n->c, depth + 1);
            scan_part(e, n->d, depth + 1);
            scan_loop_pop(e);
        } else {
            // binders are per-iteration copies, except an `in mut` value alias
            int alias = (n->flags & F_MUT) && n->b->kind != N_RANGE;
            if (n->b->kind != N_RANGE) promo_escape(e, n->b);
            scan_part(e, n->b, depth);
            scan_loop_push(e);
            for (int i = 0; i < n->nkids; i++) if (!(alias && i == n->nkids - 1)) promo_add(e, n->kids[i], 0);
            scan_part(e, n->c, depth + 1);
            scan_loop_pop(e);
        }
        return;
    case N_BLOCK: case N_IF: case N_SWITCH:
        scan_part(e, n->a, depth);
        scan_part(e, n->b, depth);
        scan_part(e, n->c, depth);
        for (int i = 0; i < n->nkids; i++) scan_part(e, n->kids[i], depth);
        return;
    default:
        break;
    }
    promo_scan(e, n->a, depth);
    promo_scan(e, n->b, depth);
    promo_scan(e, n->c, depth);
    for (int i = 0; i < n->nkids; i++) promo_scan(e, n->kids[i], depth);
}

// heaviest first, ties in declaration order
static int promo_rank_cmp(const void* a, const void* b) {
    const PromoRank* x = a;
    const PromoRank* y = b;
    if (x->weight != y->weight) return x->weight > y->weight ? -1 : 1;
    return (x->ix > y->ix) - (x->ix < y->ix);
}

// the non-escaping candidates of one register class (a by-reference pointer
// is integer-class) weighing at least min_weight, heaviest first
static int promo_rank(Em* e, int fp, uint32_t min_weight, PromoRank* out) {
    int n = 0;
    for (int i = 0; i < e->npcands; i++) {
        PromoCand* c = &e->pcands[i];
        if (!c->escaped && c->weight >= min_weight &&
            (!c->byref && is_floatt(node_sym(c->decl)->type)) == fp)
            out[n++] = (PromoRank){ c->weight, i };
    }
    qsort(out, (size_t)n, sizeof *out, promo_rank_cmp);
    return n;
}

// whether a recorded call lies strictly inside (first, last)
static int scan_crosses(Em* e, uint32_t first, uint32_t last) {
    int lo = 0, hi = e->nscan_calls; // the first call after `first`
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (e->scan_calls[mid] <= first) lo = mid + 1;
        else hi = mid;
    }
    return lo < e->nscan_calls && e->scan_calls[lo] < last;
}

// give c register r, if no interval already there overlaps c's
static int promo_assign(Em* e, PromoCand* c, int r) {
    // Disjoint intervals sorted by start also end in order, so only the last
    // one starting by c's end can reach c's start.
    uint32_t* iv = e->reg_iv[r].iv;
    int lo = 0, hi = e->reg_iv[r].n; // the first interval starting after c's end
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (iv[2 * mid] <= c->last) lo = mid + 1;
        else hi = mid;
    }
    if (lo > 0 && iv[2 * lo - 1] >= c->first) return 0;
    if (e->reg_iv[r].n * 2 + 2 > e->reg_iv[r].cap) {
        e->reg_iv[r].cap = e->reg_iv[r].cap ? e->reg_iv[r].cap * 2 : 32;
        e->reg_iv[r].iv = iv = realloc(iv, (size_t)e->reg_iv[r].cap * sizeof(uint32_t));
        if (!iv) oom();
    }
    memmove(&iv[2 * lo + 2], &iv[2 * lo], (size_t)(e->reg_iv[r].n - lo) * 2 * sizeof(uint32_t));
    iv[2 * lo] = c->first;
    iv[2 * lo + 1] = c->last;
    e->reg_iv[r].n++;
    if (e->npasg == e->cpasg) {
        e->cpasg = e->cpasg ? e->cpasg * 2 : 32;
        e->pasg = realloc(e->pasg, (size_t)e->cpasg * sizeof(PromoAsg));
        if (!e->pasg) oom();
    }
    e->pasg[e->npasg++] = (PromoAsg){ c->decl, r, c->first, c->last, c->byref };
    if (e->npasg == PASG_SCAN + 1) { // past a short scan: index every binder
        ptrmap_clear(&e->pasg_ix);
        for (int i = 0; i < e->npasg; i++) ptrmap_put(&e->pasg_ix, e->pasg[i].decl, (uintptr_t)i + 1);
    } else if (e->npasg > PASG_SCAN + 1) {
        ptrmap_put(&e->pasg_ix, c->decl, (uintptr_t)e->npasg);
    }
    return 1;
}

static int span_cmp(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

// the positions strictly inside some x9-x14 interval, as sorted disjoint
// [lo, hi] pairs for leaf_spanned
static void leaf_spans(Em* e) {
    int n = 0;
    for (int i = 0; i < e->npasg; i++) {
        PromoAsg* a = &e->pasg[i];
        if (a->reg > 14 || a->last - a->first < 2) continue;
        if (2 * n + 2 > e->cleaf_span) {
            e->cleaf_span = e->cleaf_span ? e->cleaf_span * 2 : 64;
            e->leaf_span = realloc(e->leaf_span, (size_t)e->cleaf_span * sizeof(uint32_t));
            if (!e->leaf_span) oom();
        }
        e->leaf_span[2 * n] = a->first + 1;
        e->leaf_span[2 * n + 1] = a->last - 1;
        n++;
    }
    qsort(e->leaf_span, (size_t)n, 2 * sizeof(uint32_t), span_cmp);
    int m = 0;
    for (int i = 0; i < n; i++) {
        uint32_t lo = e->leaf_span[2 * i], hi = e->leaf_span[2 * i + 1];
        if (m && lo <= e->leaf_span[2 * m - 1]) {
            if (hi > e->leaf_span[2 * m - 1]) e->leaf_span[2 * m - 1] = hi;
            continue;
        }
        e->leaf_span[2 * m] = lo;
        e->leaf_span[2 * m + 1] = hi;
        m++;
    }
    e->nleaf_span = m;
}

// whether position p lies strictly inside some x9-x14 interval
static int leaf_spanned(Em* e, uint32_t p) {
    if (e->npasg <= PASG_SCAN) {
        for (int i = 0; i < e->npasg; i++)
            if (e->pasg[i].reg <= 14 && e->pasg[i].first < p && p < e->pasg[i].last) return 1;
        return 0;
    }
    int lo = 0, hi = e->nleaf_span; // the first span starting after p
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (e->leaf_span[2 * mid] <= p) lo = mid + 1;
        else hi = mid;
    }
    return lo > 0 && p <= e->leaf_span[2 * lo - 1];
}

/*
 * Choose the locals that live in registers, heaviest first, ties in
 * declaration order. An integer-class local whose interval crosses no call
 * takes x14 down to x9, which need no save: as many as the body's temp bound
 * leaves above the temps. Otherwise, at PROMO_MIN_WEIGHT, it takes the
 * x19-x28 registers loop state leaves free. Either way it shares a register
 * already in use when their intervals do not overlap. Float locals get d8-d15
 * and then d24-d31 for the whole body. A parameter the internal ABI passes by
 * reference (mut, method receivers) competes as its pointer, integer-class.
 */
static void plan_promotion(Em* e, Node* f, int export_c) {
    e->npcands = e->npasg = 0;
    e->scan_pos = 0;
    e->nscan_loop = e->scan_defer = 0;
    e->scan_deferred = NULL;
    e->nscan_calls = e->nscan_pend = e->nscan_frame = 0;
    e->nleaf_span = 0;
    for (int r = 0; r < 32; r++) e->reg_iv[r].n = 0;
    ptrmap_clear(&e->pcand_ix);
    ptrmap_clear(&e->call_pos);
    e->nsplits = 0;
    ptrmap_clear(&e->split_ix);
    for (int i = 0; i < f->nkids; i++)
        if (!(f->kids[i]->flags & F_MUT) && !((f->flags & F_METHOD) && i == 0))
            promo_add(e, f->kids[i], 0);
        else if (!export_c)
            promo_add(e, f->kids[i], 1);
    e->scan_call = e->scan_loop_branch = 0;
    scan_part(e, f->b, 0);
    for (int i = 0; i < e->nsplits; i++)
        for (int f = 0; e->splits[i].bad && f < e->splits[i].n; f++)
            promo_cand(e, e->splits[i].field[f])->escaped = 1;
    for (int i = 0; i < e->npcands; i++) {
        PromoCand* c = &e->pcands[i];
        if (scan_crosses(e, c->first, c->last)) c->crosses = 1;
    }
    int room = MAX_ITEMPS - temp_need(e, f->b) - TEMP_MARGIN;
    if (room > LEAF_REGS) room = LEAF_REGS;
    if (e->npcands > e->cprank) {
        e->cprank = e->npcands;
        e->prank = realloc(e->prank, (size_t)e->cprank * sizeof(PromoRank));
        if (!e->prank) oom();
    }
    int nrank = promo_rank(e, 0, 1, e->prank);
    for (int r = 0; r < nrank; r++) {
        PromoCand* c = &e->pcands[e->prank[r].ix];
        uint32_t weight = c->weight;
        int got = 0;
        for (int k = 0; !c->crosses && !got && k < room; k++) {
            got = promo_assign(e, c, 14 - k);
            if (got && k + 1 > e->nxpreg) e->nxpreg = k + 1;
        }
        if (got || weight < PROMO_MIN_WEIGHT) continue;
        for (int k = 0; !got && k < e->npreg; k++) got = promo_assign(e, c, 19 + e->lreg_max + k);
        if (!got && e->lreg_max + e->npreg < LOOP_REGS) {
            promo_assign(e, c, 19 + e->lreg_max + e->npreg);
            e->npreg++;
        }
    }
    e->vti_cap = MAX_ITEMPS - e->nxpreg;
    if (e->nxpreg && e->npasg > PASG_SCAN) leaf_spans(e);
    nrank = promo_rank(e, 1, PROMO_MIN_WEIGHT, e->prank);
    int r = 0;
    for (; r < nrank && e->nfpreg < FP_PROMO_REGS; r++)
        e->fpreg_decl[e->nfpreg++] = e->pcands[e->prank[r].ix].decl;
    if (!e->scan_loop_branch)
        for (; r < nrank && e->nfpreg < FP_PROMO_REGS + FP_LEAF_REGS; r++) {
            e->fpreg_decl[e->nfpreg++] = e->pcands[e->prank[r].ix].decl;
            e->nfpleaf++;
        }
    for (int i = 0; i < e->nsplits; i++)
        for (int f = 0; f < e->splits[i].n; f++)
            if (split_freg(e, &e->splits[i], f) >= 0) e->splits[i].regs = 1;
}

typedef struct {
    Node*   decl[40];
    int     n[40];    // register units of an eligible parameter, else 0
    uint8_t fp[40];   // float leaves of an HFA (else 8-byte integer leaves)
    uint8_t ok[40];
    uint8_t mask[40]; // leaves read, by unit index
    TU*     units;
    int     np;
} ParamPlan;

static int param_find(ParamPlan* p, Node* decl) {
    for (int i = 0; i < p->np; i++) if (p->decl[i] == decl && p->n[i]) return i;
    return -1;
}

// A parameter stays eligible while every mention is a constant leaf read.
static void param_scan(Em* e, ParamPlan* p, Node* n) {
    if (!n) return;
    uint32_t off;
    int i;
    switch (n->kind) {
    case N_IDENT: {
        Sym* s = node_sym(n);
        i = s && s->decl ? param_find(p, s->decl) : -1;
        if (i >= 0) p->ok[i] = 0;
        return;
    }
    case N_FIELD_E: {
        Node* d = param_chain(e, n, &off);
        SType* t = node_ty(n);
        i = d && t ? param_find(p, d) : -1;
        if (i >= 0 && (p->fp[i] ? !is_floatt(t) : !int_leaf(t))) i = -1;
        for (int w = 0; i >= 0 && w < p->n[i]; w++)
            if (p->units[i * 4 + w].off == off) {
                p->mask[i] |= (uint8_t)(1u << w);
                return;
            }
        break;
    }
    case N_UNARY:
        if (n->op != T_AMP && n->op != T_VOLATILE) break;
        // fallthrough
    case N_MUTARG: {
        Node* d = param_chain(e, n->a, &off);
        i = d ? param_find(p, d) : -1;
        if (i >= 0) p->ok[i] = 0;
        break;
    }
    case N_FOR:
        if (n->flags & F_FORGEN) {
            param_scan(e, p, n->a);
            param_scan(e, p, n->b);
            param_scan(e, p, n->c);
            param_scan(e, p, n->d);
        } else {
            param_scan(e, p, n->b);
            param_scan(e, p, n->c);
        }
        return;
    default:
        break;
    }
    param_scan(e, p, n->a);
    param_scan(e, p, n->b);
    param_scan(e, p, n->c);
    for (int k = 0; k < n->nkids; k++) param_scan(e, p, n->kids[k]);
}

/*
 * Choose the struct parameters that live in registers: a call-free function's
 * read-only by-value parameters that arrive whole in registers and are only
 * read leaf by leaf. Float leaves of an HFA stay in their argument registers:
 * each home's temp index, from the top of the fp temps the body's expressions
 * leave free, trades v registers with its argument register's index, so a
 * temp never lands on a live home. 8-byte integer leaves of a struct of at
 * most 16 bytes get the x14-down registers leaf promotion leaves unused,
 * within the int temps. A parameter that does not fit whole keeps its slot.
 */
static void plan_param_homes(Em* e, Node* f, SType** pt, const uint8_t* muts, TU* units, int np, uint8_t* homed) {
    e->nphome = e->nfhome = e->nxhome = 0;
    if (!f->b || e->fn_has_asm || e->scan_call) return;
    int froom = MAX_FPTEMPS - temp_need(e, f->b) - TEMP_MARGIN;
    if (froom > PARAM_FHOMES) froom = PARAM_FHOMES;
    int xroom = MAX_ITEMPS - temp_need(e, f->b) - TEMP_MARGIN;
    if (xroom > LEAF_REGS) xroom = LEAF_REGS;
    xroom -= e->nxpreg;
    if (froom <= 0 && xroom <= 0) return;
    ParamPlan p = { .units = units, .np = np };
    int any = 0;
    for (int i = 0; i < np; i++) {
        uint32_t offs[5];
        uint8_t dbl[5];
        SType* t = unwrap(pt[i]);
        p.decl[i] = f->kids[i];
        if (muts[i] || !t || t->kind != SK_STRUCT) continue;
        int n = classify_hfa(pt[i], offs, dbl);
        int fp = n >= 1;
        uint32_t size = ty_size(pt[i]);
        if (!fp && size >= 1 && size <= 16) n = (int)((size + 7) / 8);
        if (n < 1 || (fp ? froom : xroom) <= 0) continue;
        int ok = 1;
        for (int w = 0; w < n; w++) {
            TU* u = &units[i * 4 + w];
            if (fp ? u->kind != TU_VREG : (u->kind != TU_XREG || u->reg < 0)) ok = 0;
        }
        p.n[i] = n;
        p.fp[i] = (uint8_t)fp;
        p.ok[i] = (uint8_t)ok;
        any |= ok;
    }
    if (!any) return;
    param_scan(e, &p, f->b);
    for (int i = 0; i < np; i++) {
        if (!p.n[i] || !p.ok[i]) continue;
        int cnt = __builtin_popcount(p.mask[i]);
        if (p.fp[i] ? e->nfhome + cnt > froom : e->nxhome + cnt > xroom) continue;
        homed[i] = 1;
        for (int w = 0; w < p.n[i]; w++) {
            if (!(p.mask[i] & (1u << w))) continue;
            e->phome[e->nphome].decl = f->kids[i];
            e->phome[e->nphome].off = units[i * 4 + w].off;
            if (p.fp[i]) { // the leaf stays in its argument register, whose temp takes v23 down
                int in = units[i * 4 + w].reg, top = MAX_FPTEMPS - 1 - e->nfhome++;
                e->vmap[in] = e->vmap[top];
                e->vmap[top] = (uint8_t)in;
                e->phome[e->nphome].reg = in;
            } else {
                e->phome[e->nphome].reg = 14 - e->nxpreg - e->nxhome++;
            }
            e->nphome++;
        }
    }
    e->vtf_cap = MAX_FPTEMPS - e->nfhome;
    e->vti_cap -= e->nxhome;
}

// --- self tail recursion ---------------------------------------------------------------

/*
 * Release lowers `return f(args)` inside f itself to a branch back to f's
 * parameter reads: the frame is reused, so every recursion level shares one
 * copy of each local. That is unobservable only when nothing can hold an
 * address into the frame, so the function keeps every value it owns in one
 * register-sized or two-word view (no aggregate local or by-value aggregate
 * parameter, whose storage the ABI may pass by address) and takes no address
 * of its own storage anywhere. Only a call to f itself branches; a call to
 * another function in tail position stays a call. Debug builds keep every
 * call: a backtrace shows each level.
 */

// a value that holds no storage of its own frame: a scalar or a slice view
static int self_flat(SType* t) {
    t = unwrap(t);
    return t && (is_scalar(t) || t->kind == SK_SLICE || (t->kind == SK_PRIM && t->prim == P_STRING));
}

// whether place x is storage in this frame: a local or by-value parameter, or
// part of one reached without a pointer or view
static int self_frame_place(Em* e, Node* x) {
    while (x) {
        if (x->kind == N_IDENT) {
            Sym* s = node_sym(x);
            if (!s || s->kind != SYM_VAR || s->is_global) return 0;
            Node* f = e->fnnode;
            int receiver = (f->flags & F_METHOD) && f->nkids > 0 && s->decl == f->kids[0];
            return !receiver && !(s->is_param && s->decl && (s->decl->flags & F_MUT));
        }
        if (x->kind != N_FIELD_E && x->kind != N_INDEX && x->kind != N_SLICE_E) return 0;
        SType* t = node_ty(x->a);
        t = t ? unwrap(t) : NULL;
        if (t && (t->kind == SK_PTR || t->kind == SK_SLICE || (t->kind == SK_PRIM && t->prim == P_STRING))) return 0;
        x = x->a;
    }
    return 0;
}

// whether the body keeps every binder flat and takes no address of its frame
static int self_safe(Em* e, Node* n) {
    if (!n) return 1;
    Sym* s = n->sym ? node_sym(n) : NULL;
    if (s && s->kind == SYM_VAR && !s->is_global && s->decl == n && !self_flat(s->type)) return 0;
    switch (n->kind) {
    case N_UNARY:
        if ((n->op == T_AMP || n->op == T_VOLATILE) && self_frame_place(e, n->a)) return 0;
        break;
    case N_MUTARG:
        if (self_frame_place(e, n->a)) return 0;
        break;
    case N_CALL: // an instance method receives its receiver's address
        if (n->a && n->a->kind == N_FIELD_E && s && s->kind == SYM_FUNC && !s->is_extern &&
            !(n->flags & (F_METHOD | F_ASSOC)) && self_frame_place(e, n->a->a))
            return 0;
        break;
    case N_FOR:
        if (n->flags & F_FORGEN) return self_safe(e, n->a) && self_safe(e, n->b) && self_safe(e, n->c) && self_safe(e, n->d);
        for (int i = 0; i < n->nkids; i++) if (!self_safe(e, n->kids[i])) return 0;
        return self_safe(e, n->b) && self_safe(e, n->c);
    default:
        break;
    }
    if (!self_safe(e, n->a) || !self_safe(e, n->b) || !self_safe(e, n->c)) return 0;
    for (int i = 0; i < n->nkids; i++) if (!self_safe(e, n->kids[i])) return 0;
    return 1;
}

// whether f's returned self calls may branch: every parameter arrives in a
// register and is flat or by reference, and nothing addresses the frame
static int self_eligible(Em* e, Node* f, SType** pt, const uint8_t* muts, TU* units, int np, RetCls* rc) {
    if (e->o.mode != EMIT_RELEASE || e->current_is_main || e->fn_has_asm || e->nsites || rc->sret || !f->b ||
        (f->flags & (F_EXPO | F_INTRINSIC)) || f->c)
        return 0;
    Sym* fs = node_sym(f);
    if (!fs || fs->is_generic || fs->spec_args) return 0;
    for (int i = 0; i < np; i++) {
        if (!muts[i] && !self_flat(pt[i])) return 0;
        for (int w = 0; w < 4; w++)
            if (units[i * 4 + w].kind == TU_STACK || units[i * 4 + w].kind == TU_STACK_AGGR) return 0;
    }
    return self_safe(e, f->b);
}

// whether `return x` is a returned self call: a direct call to this function with
// no defer pending, whose by-reference receiver is a place outside the frame
static int self_return(Em* e, Node* x) {
    if (e->self_entry < 0 || e->ndefers || e->in_defers || !x || x->kind != N_CALL || x->d || !x->sym ||
        inline_site(e, x))
        return 0;
    Sym* s = node_sym(x);
    Node* callee = x->a;
    if (s->kind != SYM_FUNC || s->decl != e->fnnode || s->is_extern || s->is_generic || s->spec_args) return 0;
    if (callee->kind == N_IDENT) return 1;
    if (callee->kind != N_FIELD_E) return 0;
    return (x->flags & (F_METHOD | F_ASSOC)) || is_lvalue_node(callee->a);
}

static void emit_func(Em* e, Node* f, const char* symname, uint32_t symlen, SType* ret, int is_main, int export_c) {
    align_code(e, FUNC_ALIGN);
    uint32_t debug_addr = (uint32_t)text(e)->len;
    line_mark(e, f);
    e->frame = 0;
    memset(e->int_spill, 0, sizeof e->int_spill);
    memset(e->fp_spill, 0, sizeof e->fp_spill);
    e->out_max = 0;
    e->nslots = 0;
    ptrmap_clear(&e->slot_ix);
    e->vti = e->vtf = 0;
    e->nlab = 0;
    e->nbp = 0;
    e->nctx = 0;
    e->ended_return = 0;
    e->fnret = ret;
    e->current_is_main = is_main;
    e->fnbody = f->b;
    e->fnnode = f;
    e->fnname = symname;
    e->fnnlen = symlen;
    e->self_entry = -1;
    e->self_call = NULL;
    e->ndefers = 0;
    e->retsave_off = 0;
    e->sink_call = NULL;
    e->sink_ret = NULL;
    e->sink_done = 0;
    e->sink_split = NULL;
    e->nlreg_loops = 0;
    e->lreg_top = e->lreg_max = 0;
    e->fn_has_asm = 0;
    e->ninl = 0;
    e->vti_base = e->vtf_base = 0;
    ptrmap_clear(&e->inl_ix);
    e->ninl_path = 1;
    if (!e->cinl_path) {
        e->cinl_path = 16;
        e->inl_path = malloc((size_t)e->cinl_path * sizeof(Node*));
        if (!e->inl_path) oom();
    }
    e->inl_path[0] = f; // a call back into f cannot expand in f
    e->nsites = 0;
    if (f->flags & F_INLCALLS) inline_plan(e, f->b);
    e->ninl_path = 0;
    plan_loops(e, f->b);
    e->npreg = e->nfpreg = e->nxpreg = e->nfpleaf = e->npasg = 0;
    e->vti_cap = MAX_ITEMPS;
    e->vtf_cap = MAX_FPTEMPS;
    e->nphome = e->nfhome = e->nxhome = 0;
    vmap_reset(e);
    e->nsplits = 0;
    ptrmap_clear(&e->split_ix);
    if (e->fn_has_asm) { // asm bindings may name any x register
        e->nlreg_loops = 0;
        e->lreg_max = 0;
    } else {
        plan_promotion(e, f, export_c);
    }
    e->nsaved = e->lreg_max + e->npreg;
    sym_define(&e->o.syms, e->o.secs, symname, symlen, is_main || export_c, &e->o.secs[SEC_TEXT]);
    if (is_main) emit_has_main = 1;
    emit_prologue(e);
    e->peep_on = !e->fn_has_asm;
    if (e->nsaved) e->lreg_save = slot_alloc(e, NULL, 8u * (uint32_t)e->nsaved, 8);
    if (e->nfpreg - e->nfpleaf)
        e->fpreg_save = slot_alloc(e, NULL, 8u * (uint32_t)(e->nfpreg - e->nfpleaf), 8);
    if (e->nfpleaf) e->fpleaf_save = slot_alloc(e, NULL, 8u * (uint32_t)e->nfpleaf, 8);
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
    uint8_t homed[40] = {0};
    plan_param_homes(e, f, pt, muts, units, np, homed);
    if (self_eligible(e, f, pt, muts, units, np, &rc)) { // returned self calls re-enter here
        e->self_entry = new_label(e);
        bind_label(e, e->self_entry);
    }
    for (int i = 0; i < np; i++) {
        if (homed[i]) { // leaves read in place: an integer leaf moves to its home, no slot
            for (int w = 0; w < 4; w++) {
                TU* u = &units[i * 4 + w];
                int home = (u->kind == TU_VREG || u->kind == TU_XREG) && u->reg >= 0 ? param_reg(e, f->kids[i], u->off) : -1;
                if (home < 0) continue;
                if (u->kind == TU_VREG) fmov_raw(e, u->size == 8, home, u->reg);
                else mov_int(e, 1, home, u->reg);
            }
            continue;
        }
        SType* t = pt[i];
        uint32_t size = ty_size(t);
        uint32_t hfa_offs[5];
        uint8_t hfa_dbl[5];
        int is_hfa = classify_hfa(t, hfa_offs, hfa_dbl) >= 0;
        int byref = (!export_c && muts[i]) || (size > 16 && !is_hfa);
        int br = byref ? bref_reg(e, node_sym(f->kids[i])) : -1;
        if (br >= 0) { // the incoming pointer moves into its register: no slot
            TU* u = &units[i * 4];
            if (u->kind == TU_XREG) {
                mov_int(e, 1, br, u->reg);
            } else {
                incoming_addr(e, 16, u->soff);
                ld_reg(e, br, 16, 0, 8, 0);
            }
            continue;
        }
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
        uint32_t slot = slot_alloc(e, f->kids[i], storage, byref ? 8 : ty_align(t));
        if (byref) e->slots[e->nslots - 1].byref = 1;
        for (int w = 0; w < 4; w++) {
            TU* u = &units[i * 4 + w];
            if (u->kind == TU_XREG) {
                if (u->reg < 0) continue;
                uint32_t bw = byref ? 8 : is_scalar(t) ? (uint32_t)((scalar_bits(t) + 7) / 8) : 8;
                // unit u->off bytes into the slot sits at depth slot - off
                store_frame(e, u->reg, byref ? slot : slot - u->off, bw);
            } else if (u->kind == TU_VREG) {
                store_frame_fp(e, u->reg, slot - u->off, u->size == 8);
            } else if (u->kind == TU_STACK) {
                incoming_addr(e, 16, u->soff);
                if (is_floatt(t)) { // v16: no argument register, which may be a home
                    ldrf_(e, 16, 16, 0, u->size == 8);
                    store_frame_fp(e, 16, slot, u->size == 8);
                } else {
                    // u->size is the ABI stack-slot width (Darwin pads to a
                    // 4-byte minimum); homing into the frame must only copy
                    // the scalar's actual width, or a narrow trailing scalar
                    // (e.g. a stack-passed bool) overwrites adjacent bytes.
                    uint32_t slotw = u->size < 8 ? u->size : 8;
                    uint32_t bw = byref ? 8 : is_scalar(t) ? (uint32_t)((scalar_bits(t) + 7) / 8) : slotw;
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
        } else if (is_bits_integer_op(f->ival)) {
            emit_bits_integer(e, f->ival, scalar_bits(pt[0]), 0, 1, 0);
        } else if (f->ival >= BI_MATH_SQRT && f->ival <= BI_MATH_CEIL) {
            emit_math_exact(e, f->ival, unwrap(pt[0])->prim == P_F64, 0);
        } else if (f->ival == BI_MATH_FMA) {
            emit_math_fused_raw(e, 0, 0, 1, 2, unwrap(pt[0])->prim == P_F64); // fmadd d0,d0,d1,d2
        }
        emit_epilogue(e);
        e->ended_return = 1;
    }

    if (f->b) for (int i = 0; i < f->b->nkids; i++) emit_stmt(e, f->b->kids[i]);

    if (!e->ended_return) { // fall-off end: zero the result registers
        emit_defers(e, NULL); // no result register is live here
        if (rc.nunits == 1 && is_scalar(ret) && is_floatt(ret))
            ins(e, unwrap(ret)->prim == P_F64 ? 0x9E6703E0u : 0x1E2703E0u);
        else if (rc.nunits >= 1) {
            for (int w = 0; w < rc.nunits; w++) {
                if (rc.units[w].kind == TU_XREG) load_imm(e, 1, rc.units[w].reg, 0);
                else ins(e, (rc.units[w].size == 8 ? 0x9E6703E0u : 0x1E2703E0u) | (uint32_t)rc.units[w].reg);
            }
        }
        emit_epilogue(e);
    }
    e->peep_on = 0;
    e->o.fix[e->subsp_fix].frame = align_up(e->frame + e->out_max, 16);
    patch_branches(e);
    debug_fn_add(e, f, debug_addr, (uint32_t)text(e)->len - debug_addr);
    flush_fpool(e);
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

static ESym* emit_embed_thunk(Em* e, const char* vtname, int slot, const char* target, uint32_t off) {
    char name[1024];
    snprintf(name, sizeof name, "%s$embed$%d", vtname, slot);
    ESym* prior = sym_intern(&e->o.syms, name, (uint32_t)strlen(name));
    if (prior->type) return prior;
    int skip = -1;
    if (e->frame) {
        skip = new_label(e);
        br_(e, skip); // a generic vtable may be materialized inside a function
    }
    ESym* out = sym_define(&e->o.syms, e->o.secs, name, (uint32_t)strlen(name), 0, &e->o.secs[SEC_TEXT]);
    ins(e, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
    if (off <= 4095) {
        ins(e, 0x91000000u | (off << 10)); // add x0, x0, #field-offset
    } else {
        load_imm(e, 1, 16, off);
        ins(e, 0x8B000000u | (16u << 16)); // add x0, x0, x16
    }
    ins(e, 0x94000000u);
    ESym* fn = sym_intern(&e->o.syms, target, (uint32_t)strlen(target));
    fix_add(e, FK_BRANCH, (uint32_t)(text(e)->len - 4), fn);
    ins(e, 0xA8C17BFD); // ldp x29, x30, [sp], #16
    ins(e, 0xD65F03C0); // ret
    if (skip >= 0) bind_label(e, skip);
    return out;
}

static ESym* emit_vtable(Em* e, SType* conc, SType* contract, Node* conf) {
    if (!conc || !contract || !conf) {
        em_err(e, conf, "invalid contract conformance in codegen");
        return sym_intern(&e->o.syms, "__ace$vt$invalid", 16);
    }
    const char* nm = vtable_name(conc, contract);
    ESym* prior = sym_intern(&e->o.syms, nm, (uint32_t)strlen(nm));
    if (prior->type) return prior;
    Sec* c = &e->o.secs[SEC_CONST];
    while (c->data.len % 8) buf_put8(&c->data, 0);
    ESym* out = sym_define(&e->o.syms, e->o.secs, nm, (uint32_t)strlen(nm), 0, c);
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
        const char* fnm = fn_mangle(fsym->name, fsym->len, ft, fsym->decl, args, nargs, 0);
        ESym* fs;
        if (ref->a) {
            fs = emit_embed_thunk(e, nm, i, fnm, field_off(conc, embed_fi));
        } else {
            fs = sym_intern(&e->o.syms, fnm, (uint32_t)strlen(fnm));
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

int emit_object(Node* file, Diag* d, const char* out_path, CompileReport* report, EmitMode mode) {
    Em e;
    memset(&e, 0, sizeof e);
    obj_init(&e.o, d, mode);
    vmap_reset(&e);
    e.vti_cap = MAX_ITEMPS;
    e.vtf_cap = MAX_FPTEMPS;
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
            int is_main = !(n->flags & (F_METHOD | F_ASSOC)) && n->len == 4 && memcmp(n->str, "main", 4) == 0;
            int export_c = (n->flags & F_EXPO) != 0;
            SType* ret = n->a ? sema_build_type(n->a) : NULL;
            const char* why;
            IrFunc* ir = ir_lower(n, mode, &why); // the IR backend where it covers the function
            if (ir && !arm64_covers(ir)) {
                ir_free(ir);
                ir = NULL;
            }
            if (ir) {
                Sym* fs = node_sym(n);
                char mn[512];
                snprintf(mn, sizeof mn, "%s", is_main ? "_main" : fn_mangle(fs->name, fs->len, fs->type, n,
                                                                              fs->spec_args, fs->spec_args ? fs->spec_nargs : 0,
                                                                              fs->spec_args != NULL));
                arm64_func(&e.o, ir, mn, (uint32_t)strlen(mn), is_main);
                if (is_main) emit_has_main = 1;
                ir_free(ir);
            } else if (is_main) {
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
    obj_patch_fixups(&e.o);
    if (d->errors) return 1;
    if (report) report->machine_code = text(&e)->len;
    ESym* text_anchor = sym_intern(&e.o.syms, "__ace$text", 10);
    obj_debug_info(&e.o, text_anchor);
    obj_debug_line(&e.o, text_anchor);
    size_t object_size = obj_write(&e.o, out_path);
    if (report) report->object_file = object_size;
    return d->errors ? 1 : 0;
}
