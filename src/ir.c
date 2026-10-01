// ir.c — construction, verification, and printing of the per-function IR.
#include "ir.h"
#include "backend.h"

#include <limits.h>
#include <stdio.h>

static void* grow(void* p, int* cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 4;
    while (c < need) c *= 2;
    p = realloc(p, (size_t)c * elem);
    if (!p) oom();
    *cap = c;
    return p;
}

// --- construction ---------------------------------------------------------------

IrFunc* ir_func_new(Node* decl, Sym* sym) {
    IrFunc* fn = calloc(1, sizeof *fn);
    if (!fn) oom();
    fn->decl = decl;
    fn->sym = sym;
    return fn;
}

int ir_block_new(IrFunc* fn) {
    fn->blocks = grow(fn->blocks, &fn->cblocks, fn->nblocks + 1, sizeof(IrBlock));
    IrBlock* b = &fn->blocks[fn->nblocks];
    memset(b, 0, sizeof *b);
    b->succ[0] = b->succ[1] = -1;
    return fn->nblocks++;
}

static int ins_alloc(IrFunc* fn, int block, IrOp op, IrType type) {
    fn->ins = grow(fn->ins, &fn->cins, fn->nins + 1, sizeof(IrIns));
    IrIns* v = &fn->ins[fn->nins];
    memset(v, 0, sizeof *v);
    v->op = (uint8_t)op;
    v->type = (uint8_t)type;
    v->block = block;
    return fn->nins++;
}

int ir_ins_new(IrFunc* fn, int block, IrOp op, IrType type) {
    int id = ins_alloc(fn, block, op, type);
    IrBlock* b = &fn->blocks[block];
    b->ins = grow(b->ins, &b->cins, b->nins + 1, sizeof(int));
    b->ins[b->nins++] = id;
    return id;
}

int ir_phi_new(IrFunc* fn, int block, IrType type) {
    int id = ins_alloc(fn, block, IR_PHI, type);
    IrBlock* b = &fn->blocks[block];
    b->ins = grow(b->ins, &b->cins, b->nins + 1, sizeof(int));
    int at = 0; // after the block's existing phis
    while (at < b->nins && fn->ins[b->ins[at]].op == IR_PHI) at++;
    memmove(b->ins + at + 1, b->ins + at, (size_t)(b->nins - at) * sizeof(int));
    b->ins[at] = id;
    b->nins++;
    return id;
}

void ir_arg_add(IrFunc* fn, int v, int arg) {
    IrIns* x = &fn->ins[v];
    x->args = grow(x->args, &x->cargs, x->nargs + 1, sizeof(int));
    x->args[x->nargs++] = arg;
}

void ir_edge_add(IrFunc* fn, int from, int to) {
    IrBlock* f = &fn->blocks[from];
    f->succ[f->succ[0] < 0 ? 0 : 1] = to;
    IrBlock* t = &fn->blocks[to];
    t->preds = grow(t->preds, &t->cpreds, t->npreds + 1, sizeof(int));
    t->preds[t->npreds++] = from;
}

int ir_is_term(IrOp op) { return op == IR_JMP || op == IR_BR || op == IR_RET; }

void ir_free(IrFunc* fn) {
    if (!fn) return;
    for (int i = 0; i < fn->nins; i++) free(fn->ins[i].args);
    for (int i = 0; i < fn->nblocks; i++) {
        free(fn->blocks[i].ins);
        free(fn->blocks[i].preds);
    }
    free(fn->ins);
    free(fn->blocks);
    free(fn->params);
    free(fn->rets);
    free(fn->vtables);
    free(fn);
}

// --- types ------------------------------------------------------------------------

int ir_type_bits(IrType t) {
    switch (t) {
    case IT_BOOL: case IT_I8: case IT_U8: return 8;
    case IT_I16: case IT_U16: return 16;
    case IT_I32: case IT_U32: case IT_F32: return 32;
    case IT_I64: case IT_U64: case IT_PTR: case IT_F64: return 64;
    case IT_V4F32: case IT_V4I32: case IT_V2F64: return 128;
    default: return 0;
    }
}

int ir_type_signed(IrType t) { return t >= IT_I8 && t <= IT_I64; }

int ir_type_float(IrType t) { return t == IT_F32 || t == IT_F64; }

int ir_type_vector(IrType t) { return t == IT_V4F32 || t == IT_V4I32 || t == IT_V2F64; }

IrType ir_lane_type(IrType t) { return t == IT_V4F32 ? IT_F32 : t == IT_V4I32 ? IT_I32 : t == IT_V2F64 ? IT_F64 : t; }

static SType* unwrap(SType* t) {
    while (t && t->kind == SK_DISTINCT) t = t->base;
    return t;
}

int ir_type_of(SType* t) {
    t = unwrap(t);
    if (!t) return -1;
    if (t->kind == SK_PTR || t->kind == SK_FUNC) return IT_PTR;
    if (t->kind == SK_ENUM || t->kind == SK_BITS) return ir_type_of(t->base); // bits: its backing integer
    if (t->kind == SK_ERROR) return IT_U64; // domain id above the code (Errors #5)
    if (t->kind != SK_PRIM) return -1;
    switch (t->prim) {
    case P_BOOL: return IT_BOOL;
    case P_I8: return IT_I8;
    case P_I16: return IT_I16;
    case P_I32: return IT_I32;
    case P_I64: return IT_I64;
    case P_U8: return IT_U8;
    case P_U16: return IT_U16;
    case P_U32: return IT_U32;
    case P_U64: case P_USIZE: case P_TYPEID: return IT_U64;
    case P_F32: return IT_F32;
    case P_F64: return IT_F64;
    default: return -1;
    }
}

// The leaves of t at off appended to out[n..max) (none stored when out is
// NULL): the new count, or -1 with *bad the type that does not fit.
static int leaves_at(SType* t, uint32_t off, IrLeaf* out, int n, int max, SType** bad) {
    int it = ir_type_of(t);
    if (it >= 0) {
        if (n == max) {
            *bad = t;
            return -1;
        }
        if (out) out[n] = (IrLeaf){ off, (uint8_t)it };
        return n + 1;
    }
    SType* u = unwrap(t);
    if (u && (u->kind == SK_SLICE || (u->kind == SK_PRIM && u->prim == P_STRING))) { // Types #19
        if (n + 2 > max) {
            *bad = t;
            return -1;
        }
        if (out) {
            out[n] = (IrLeaf){ off, IT_PTR };
            out[n + 1] = (IrLeaf){ off + 8, IT_U64 };
        }
        return n + 2;
    }
    if (u && u->kind == SK_CONTRACT) { // a vtable pointer and a data pointer (Carved #5)
        if (n + 2 > max) {
            *bad = t;
            return -1;
        }
        if (out) {
            out[n] = (IrLeaf){ off, IT_PTR };
            out[n + 1] = (IrLeaf){ off + 8, IT_PTR };
        }
        return n + 2;
    }
    if (u && u->kind == SK_ARRAY) { // its elements in order, as AAPCS64 flattens it
        uint32_t es = sema_size(u->base);
        for (uint64_t i = 0; i < u->count && n >= 0; i++) n = leaves_at(u->base, off + (uint32_t)i * es, out, n, max, bad);
        return n;
    }
    if (!u || (u->kind != SK_STRUCT && u->kind != SK_TUPLE)) {
        *bad = t;
        return -1;
    }
    for (int i = 0; i < u->nfields && n >= 0; i++) n = leaves_at(u->fields[i].t, off + field_off(u, i), out, n, max, bad);
    return n;
}

// Whether t is a union or a struct whose fields share bytes: its bytes, not
// its members, are its value.
int ir_is_overlay(SType* t) {
    SType* u = unwrap(t);
    return u && (u->kind == SK_UNION || (u->kind == SK_STRUCT && u->is_overlap));
}

// Whether t is, or holds in a struct, tuple, or array member (at any depth),
// a union or an overlapping struct.
int ir_holds_union(SType* t) {
    SType* u = unwrap(t);
    if (!u) return 0;
    if (u->kind == SK_STRUCT && u->is_overlap) return 1;
    if (u->kind == SK_ARRAY) return ir_is_overlay(u->base) || ir_holds_union(u->base);
    if (u->kind != SK_STRUCT && u->kind != SK_TUPLE) return 0;
    for (int i = 0; i < u->nfields; i++)
        if (ir_is_overlay(u->fields[i].t) || ir_holds_union(u->fields[i].t)) return 1;
    return 0;
}

// A union or overlapping struct, or an aggregate holding one, is its bytes as
// the ABI moves them: a u64 per full word, then a u32, u16, and u8 for what a
// last partial word holds (a union's is a u32, its size a multiple of its i32
// tag's alignment).
static int word_leaves(SType* t, IrLeaf* out, int max, SType** bad) {
    uint32_t size = sema_size(t), off = 0;
    int n = 0;
    while (off < size) {
        uint32_t rest = size - off, w = rest >= 8 ? 8 : rest >= 4 ? 4 : rest >= 2 ? 2 : 1;
        if (n >= max) {
            *bad = t;
            return -1;
        }
        if (out) out[n] = (IrLeaf){ off, (uint8_t)(w == 8 ? IT_U64 : w == 4 ? IT_U32 : w == 2 ? IT_U16 : IT_U8) };
        n++;
        off += w;
    }
    return n;
}

static int is_word_type(SType* t) { return ir_is_overlay(t) || ir_holds_union(t); }

int ir_leaves(SType* t, IrLeaf* out, SType** bad) {
    if (is_word_type(t)) return word_leaves(unwrap(t), out, IR_MAX_LEAVES, bad);
    return leaves_at(t, 0, out, 0, IR_MAX_LEAVES, bad);
}

int ir_leaf_count(SType* t) {
    SType* bad;
    if (is_word_type(t)) return word_leaves(unwrap(t), NULL, INT_MAX, &bad);
    return leaves_at(t, 0, NULL, 0, INT_MAX, &bad);
}

// The 64-bit pattern of v wrapped into t: truncated, then extended by t's
// signedness. Every IR integer value is held in this form.
uint64_t ir_wrap(IrType t, uint64_t v) {
    if (t == IT_BOOL) return v != 0;
    int bits = ir_type_bits(t);
    if (bits == 64 || bits == 0) return v;
    uint64_t mask = (UINT64_C(1) << bits) - 1;
    v &= mask;
    if (ir_type_signed(t) && (v >> (bits - 1)) & 1) v |= ~mask;
    return v;
}

static const char* type_name(IrType t) {
    static const char* names[] = { "void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "ptr", "f32", "f64", "v4f32", "v4i32", "v2f64" };
    return names[t];
}

static const char* op_name(IrOp op) {
    static const char* names[] = {
#define X(name, text) text,
        IR_OPS(X)
#undef X
    };
    return names[op];
}

// --- verification -------------------------------------------------------------------

static int bad(IrFunc* fn, const char* fmt, int a, int b) {
    fprintf(stderr, "ace: internal IR error in '%.*s': ", (int)fn->decl->len, fn->decl->str);
    fprintf(stderr, fmt, a, b);
    fputc('\n', stderr);
    return 0;
}

// Immediate dominators by the Cooper-Harvey-Kennedy iteration over a reverse
// postorder; idom[b] = -1 for blocks unreachable from the entry.
int* ir_dominators(IrFunc* fn, int** rpo_out, int* nrpo) {
    int n = fn->nblocks;
    int* order = malloc((size_t)n * sizeof(int));
    int* num = malloc((size_t)n * sizeof(int));   // reverse postorder index
    int* stack = malloc((size_t)n * 2 * sizeof(int));
    uint8_t* seen = calloc((size_t)n, 1);
    int* idom = malloc((size_t)n * sizeof(int));
    if (!order || !num || !stack || !seen || !idom) oom();
    int npost = 0, sp = 0;
    stack[sp++] = 0; stack[sp++] = 0;
    seen[0] = 1;
    while (sp) { // iterative DFS: (block, next successor slot)
        int b = stack[sp - 2], k = stack[sp - 1];
        if (k < 2) {
            stack[sp - 1] = k + 1;
            int s = fn->blocks[b].succ[k];
            if (s >= 0 && !seen[s]) {
                seen[s] = 1;
                stack[sp++] = s; stack[sp++] = 0;
            }
            continue;
        }
        order[npost++] = b;
        sp -= 2;
    }
    for (int i = 0; i < npost / 2; i++) { int t = order[i]; order[i] = order[npost - 1 - i]; order[npost - 1 - i] = t; }
    for (int i = 0; i < n; i++) { num[i] = -1; idom[i] = -1; }
    for (int i = 0; i < npost; i++) num[order[i]] = i;
    idom[0] = 0;
    for (int changed = 1; changed;) {
        changed = 0;
        for (int i = 1; i < npost; i++) {
            int b = order[i], nd = -1;
            IrBlock* bb = &fn->blocks[b];
            for (int p = 0; p < bb->npreds; p++) {
                int q = bb->preds[p];
                if (idom[q] < 0) continue;
                if (nd < 0) { nd = q; continue; }
                int x = q, y = nd;
                while (x != y) {
                    while (num[x] > num[y]) x = idom[x];
                    while (num[y] > num[x]) y = idom[y];
                }
                nd = x;
            }
            if (nd >= 0 && idom[b] != nd) { idom[b] = nd; changed = 1; }
        }
    }
    free(num); free(stack); free(seen);
    *rpo_out = order;
    *nrpo = npost;
    return idom;
}

int ir_dominates(const int* idom, int a, int b) {
    while (b != a && b != 0) b = idom[b];
    return b == a;
}

// Whether instruction id uses vectors only as the vector operations allow
// (ir.h IR_OPS); a scalar instruction touching none passes.
static int vector_ok(IrFunc* fn, int id) {
    IrIns* v = &fn->ins[id];
    IrType t = (IrType)v->type;
    int any = ir_type_vector(t);
    for (int a = 0; a < v->nargs; a++) any |= ir_type_vector((IrType)fn->ins[v->args[a]].type);
    if (!any) return v->op != IR_SPLAT && v->op != IR_SELECT && v->op != IR_LANES;
    IrType a0 = v->nargs ? (IrType)fn->ins[v->args[0]].type : IT_VOID;
    IrType a1 = v->nargs > 1 ? (IrType)fn->ins[v->args[1]].type : IT_VOID;
    if (v->flags & IRF_CHECKED) return 0;
    switch (v->op) {
    case IR_PHI:
        for (int a = 0; a < v->nargs; a++)
            if (fn->ins[v->args[a]].type != t) return 0;
        return 1;
    case IR_ADD: case IR_SUB: case IR_MUL: return v->nargs == 2 && ir_type_vector(t) && a0 == t && a1 == t;
    case IR_DIV: return v->nargs == 2 && (t == IT_V4F32 || t == IT_V2F64) && a0 == t && a1 == t;
    case IR_NEG: return v->nargs == 1 && ir_type_vector(t) && a0 == t;
    case IR_AND: case IR_OR: case IR_XOR: return v->nargs == 2 && t == IT_V4I32 && a0 == t && a1 == t;
    case IR_SHL: case IR_SHR: {
        const IrIns* c = v->nargs == 2 ? &fn->ins[v->args[1]] : NULL;
        return c && t == IT_V4I32 && a0 == t && a1 == IT_I32 && c->op == IR_CONST && c->imm >= 1 && c->imm <= 31;
    }
    case IR_LANES: return v->nargs == 0 && t == IT_V4I32;
    case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        return v->nargs == 2 && t == IT_V4I32 && ir_type_vector(a0) && a0 != IT_V2F64 && a1 == a0;
    case IR_CAST: return v->nargs == 1 && t == IT_V4F32 && a0 == IT_V4I32;
    case IR_LOAD: return v->nargs == 1 && a0 == IT_PTR; // a vector result
    case IR_STORE: return v->nargs == 2 && a0 == IT_PTR && ir_type_vector(a1);
    case IR_SPLAT: return v->nargs == 1 && ir_type_vector(t) && a0 == ir_lane_type(t);
    case IR_SELECT:
        return v->nargs == 3 && a0 == IT_V4I32 && ir_type_vector(t) && t != IT_V2F64 && a1 == t &&
               (IrType)fn->ins[v->args[2]].type == t;
    default: return 0;
    }
}

int ir_verify(IrFunc* fn) {
    if (fn->nblocks == 0) return bad(fn, "no blocks", 0, 0);
    if (fn->blocks[0].npreds) return bad(fn, "entry block b0 has %d predecessors", fn->blocks[0].npreds, 0);
    int* pos = malloc((size_t)(fn->nins ? fn->nins : 1) * sizeof(int)); // index within its block
    if (!pos) oom();
    int ok = 1;
    for (int b = 0; b < fn->nblocks && ok; b++) {
        IrBlock* bb = &fn->blocks[b];
        if (!bb->nins) { ok = bad(fn, "block b%d is empty", b, 0); break; }
        int phis = 1;
        for (int i = 0; i < bb->nins; i++) {
            int id = bb->ins[i];
            IrIns* v = &fn->ins[id];
            pos[id] = i;
            if (v->block != b) { ok = bad(fn, "%%%d listed in block b%d it does not belong to", id, b); break; }
            if (v->op == IR_PHI && !phis) { ok = bad(fn, "phi %%%d after a non-phi in b%d", id, b); break; }
            if (v->op != IR_PHI) phis = 0;
            if (ir_is_term((IrOp)v->op) != (i == bb->nins - 1)) {
                ok = bad(fn, "block b%d: terminator misplaced at %%%d", b, id);
                break;
            }
        }
        if (!ok) break;
        IrIns* t = &fn->ins[bb->ins[bb->nins - 1]];
        int want = t->op == IR_JMP ? 1 : t->op == IR_BR ? 2 : 0;
        int have = (bb->succ[0] >= 0) + (bb->succ[1] >= 0);
        if (want != have) { ok = bad(fn, "block b%d has %d successors for its terminator", b, have); break; }
        for (int k = 0; k < have; k++) { // each edge appears once in the successor's preds
            IrBlock* s = &fn->blocks[bb->succ[k]];
            int count = 0;
            for (int p = 0; p < s->npreds; p++) count += s->preds[p] == b;
            if (count != 1 + (have == 2 && bb->succ[0] == bb->succ[1])) {
                ok = bad(fn, "edge b%d -> b%d missing from predecessors", b, bb->succ[k]);
                break;
            }
        }
    }
    int* rpo = NULL, nrpo = 0;
    int* idom = ok ? ir_dominators(fn, &rpo, &nrpo) : NULL;
    if (ok && nrpo != fn->nblocks) ok = bad(fn, "%d blocks unreachable from the entry", fn->nblocks - nrpo, 0);
    for (int b = 0; b < fn->nblocks && ok; b++) {
        IrBlock* bb = &fn->blocks[b];
        for (int i = 0; i < bb->nins && ok; i++) {
            int id = bb->ins[i];
            IrIns* v = &fn->ins[id];
            if (v->op == IR_PHI && v->nargs != bb->npreds) {
                ok = bad(fn, "phi %%%d has %d operands", id, v->nargs);
                break;
            }
            for (int a = 0; a < v->nargs; a++) {
                int u = v->args[a];
                if (u < 0 || u >= fn->nins || fn->ins[u].block < 0) { ok = bad(fn, "%%%d uses removed value %%%d", id, u); break; }
                if (v->op == IR_PROJ) { // the call or asm, or the proj before it, directly precedes it
                    int prev = i ? bb->ins[i - 1] : -1;
                    if (fn->ins[u].op != IR_CALL && fn->ins[u].op != IR_CALLI && fn->ins[u].op != IR_ASM) {
                        ok = bad(fn, "proj %%%d of non-call %%%d", id, u); break;
                    }
                    if (prev != u && (prev < 0 || fn->ins[prev].op != IR_PROJ || fn->ins[prev].args[0] != u)) {
                        ok = bad(fn, "proj %%%d does not follow call %%%d", id, u);
                        break;
                    }
                } else if (fn->ins[u].type == IT_VOID) { ok = bad(fn, "%%%d uses void value %%%d", id, u); break; }
                int want_ptr = (a == 0 && (v->op == IR_LOAD || v->op == IR_STORE || v->op == IR_PADD ||
                                           v->op == IR_PSUB || v->op == IR_CALLI || v->op == IR_RTCALL)) ||
                               (a == v->nargs - 1 && (v->flags & IRF_SRET));
                if (want_ptr && fn->ins[u].type != IT_PTR) { ok = bad(fn, "%%%d needs an address, not %%%d", id, u); break; }
                if (v->op == IR_CHECK && fn->ins[u].type != IT_BOOL) { ok = bad(fn, "check %%%d of non-bool %%%d", id, u); break; }
                int db = fn->ins[u].block;
                // a phi operand must be available at the end of its predecessor
                int ub = v->op == IR_PHI ? bb->preds[a] : b;
                int dom = db == ub ? (v->op == IR_PHI || pos[u] < i) : ir_dominates(idom, db, ub);
                if (!dom) { ok = bad(fn, "%%%d uses %%%d where its definition does not dominate", id, u); break; }
            }
            if (ok && !vector_ok(fn, id)) ok = bad(fn, "%%%d misuses a vector type", id, 0);
        }
    }
    free(pos); free(rpo); free(idom);
    return ok;
}

// --- printing -------------------------------------------------------------------------

static void print_value(FILE* out, const int* num, int v) { fprintf(out, "%%%d", num[v]); }

static void print_sym(FILE* out, Sym* s) { // owner::name, as the source spells it
    Node* d = s->decl;
    if (d && d->d) fprintf(out, "%.*s::", (int)d->d->len, d->d->str);
    fprintf(out, "%.*s", (int)s->len, s->name);
}

static void print_imm(FILE* out, IrType t, uint64_t imm) {
    if (ir_type_float(t)) { // the shortest decimal that reads back to the same bits
        char buf[40];
        if (t == IT_F32) {
            float f;
            uint32_t b = (uint32_t)imm;
            memcpy(&f, &b, 4);
            for (int p = 1; p <= 9; p++) {
                snprintf(buf, sizeof buf, "%.*g", p, (double)f);
                if (strtof(buf, NULL) == f || f != f) break;
            }
        } else {
            double d;
            memcpy(&d, &imm, 8);
            for (int p = 1; p <= 17; p++) {
                snprintf(buf, sizeof buf, "%.*g", p, d);
                if (strtod(buf, NULL) == d || d != d) break;
            }
        }
        if (!strpbrk(buf, ".eni")) strcat(buf, ".0"); // a float stays visibly one
        fputs(buf, out);
        return;
    } else if (t == IT_BOOL) fputs(imm ? "true" : "false", out);
    else if (ir_type_signed(t)) fprintf(out, "%lld", (long long)imm);
    else fprintf(out, "%llu", (unsigned long long)imm);
}

// Leaf types, an aggregate part's in braces.
static void print_types(FILE* out, const IrType* t, int n, const IrPart* parts, int nparts) {
    int k = 0;
    for (int p = 0; k < n; p++) {
        const IrPart* pt = parts && p < nparts ? &parts[p] : NULL;
        int agg = pt && pt->agg && !pt->addr, m = pt ? pt->n : 1;
        fputs(p ? ", " : "", out);
        if (agg) fputc('{', out);
        for (int i = 0; i < m; i++, k++) fprintf(out, "%s%s", i ? ", " : "", type_name(t[k]));
        if (agg) fputc('}', out);
    }
}

void ir_print(IrFunc* fn, FILE* out) {
    // values are numbered in block order, so the text is independent of the
    // order lowering created them in
    int* num = malloc((size_t)(fn->nins ? fn->nins : 1) * sizeof(int));
    if (!num) oom();
    int next = 0;
    for (int b = 0; b < fn->nblocks; b++)
        for (int i = 0; i < fn->blocks[b].nins; i++) num[fn->blocks[b].ins[i]] = next++;
    Node* d = fn->decl;
    if (d->d) fprintf(out, "%.*s::", (int)d->d->len, d->d->str);
    fprintf(out, "%.*s(", (int)d->len, d->str);
    print_types(out, fn->params, fn->nparams, fn->sig ? fn->sig->parts : NULL, fn->sig ? fn->sig->nparts : 0);
    fputs("): ", out);
    if (fn->ret_mem) fprintf(out, "mem %u", sema_size(fn->ret_mem));
    else if (!fn->nrets) fputs("void", out);
    else if (fn->sig && fn->sig->ret.agg) print_types(out, fn->rets, fn->nrets, &fn->sig->ret, 1);
    else fputs(type_name(fn->rets[0]), out);
    fputs(" {\n", out);
    for (int b = 0; b < fn->nblocks; b++) {
        IrBlock* bb = &fn->blocks[b];
        fprintf(out, "b%d:", b);
        if (bb->npreds) {
            fputs(" ; preds", out);
            for (int p = 0; p < bb->npreds; p++) fprintf(out, "%s b%d", p ? "," : "", bb->preds[p]);
        }
        fputc('\n', out);
        for (int i = 0; i < bb->nins; i++) {
            int id = bb->ins[i];
            IrIns* v = &fn->ins[id];
            fputs("    ", out);
            // a call or asm whose leaves its projs read
            int agg = (v->sig && v->sig->ret.agg) || (v->op == IR_ASM && v->asmb->nout > 0);
            if (v->type != IT_VOID || agg) {
                print_value(out, num, id);
                fputs(" = ", out);
            }
            fputs(op_name((IrOp)v->op), out);
            if (v->flags & IRF_CHECKED) fputs(".chk", out);
            if (v->flags & IRF_VOLATILE) fputs(".vol", out);
            if (v->flags & IRF_SRET) fputs(".mem", out);
            if (v->type != IT_VOID) fprintf(out, " %s", type_name((IrType)v->type));
            if (agg) fprintf(out, " {%d}", v->sig ? v->sig->ret.n : v->asmb->nout);
            switch (v->op) {
            case IR_CONST:
                fputc(' ', out);
                print_imm(out, (IrType)v->type, v->imm);
                break;
            case IR_PARAM:
                fprintf(out, " %llu", (unsigned long long)v->imm);
                break;
            case IR_PROJ:
                fputc(' ', out);
                print_value(out, num, v->args[0]);
                fprintf(out, ", %llu", (unsigned long long)v->imm);
                break;
            case IR_PHI:
                for (int a = 0; a < v->nargs; a++) {
                    fprintf(out, "%s [", a ? "," : "");
                    print_value(out, num, v->args[a]);
                    fprintf(out, ", b%d]", bb->preds[a]);
                }
                break;
            case IR_SLOT:
                fprintf(out, " %llu, %d", (unsigned long long)v->imm, (int)v->aux);
                break;
            case IR_GLOBAL:
                fputc(' ', out);
                if (v->sym) print_sym(out, v->sym);
                else fprintf(out, "@%s", v->name);
                break;
            case IR_STR:
                fputs(" \"", out);
                for (uint64_t k = 0; k < v->imm; k++) {
                    unsigned char c = (unsigned char)v->name[k];
                    if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
                    else if (c >= 32 && c < 127) fputc(c, out);
                    else fprintf(out, "\\x%02x", c);
                }
                fputc('"', out);
                break;
            case IR_LOAD: case IR_STORE:
                fputs(" [", out);
                print_value(out, num, v->args[0]);
                if (v->aux) fprintf(out, " + %d", (int)v->aux);
                fputc(']', out);
                if (v->op == IR_STORE) {
                    fputs(", ", out);
                    print_value(out, num, v->args[1]);
                }
                break;
            case IR_PADD: case IR_PSUB:
                fputc(' ', out);
                print_value(out, num, v->args[0]);
                fputs(", ", out);
                print_value(out, num, v->args[1]);
                fprintf(out, " * %llu", (unsigned long long)v->imm);
                break;
            case IR_CALL: case IR_CALLI: case IR_RTCALL: {
                int first = v->op == IR_CALLI;
                fputc(' ', out);
                if (first) print_value(out, num, v->args[0]);
                else if (v->sym && v->sym->name) print_sym(out, v->sym);
                else fprintf(out, "@%s", v->name);
                fputc('(', out);
                for (int a = first; a < v->nargs; a++) {
                    if (a > first) fputs(", ", out);
                    if ((v->flags & IRF_VARARGS) && a == v->aux) fputs("... ", out);
                    print_value(out, num, v->args[a]);
                }
                fputc(')', out);
                break;
            }
            case IR_CHECK:
                fputc(' ', out);
                print_value(out, num, v->args[0]);
                if (v->name) fprintf(out, ", %d:%d %s", v->at ? v->at->line : 0, v->at ? v->at->col : 0, v->name);
                else fputs(", contract", out);
                break;
            case IR_ASM:
                fprintf(out, " \"%s\"", v->name);
                for (int a = 0; a < v->nargs; a++) {
                    fprintf(out, " %s=", asm_register_name(v->asmb->reg[a]));
                    print_value(out, num, v->args[a]);
                }
                if (v->asmb->nout) {
                    fputs(" ->", out);
                    for (int k = 0; k < v->asmb->nout; k++) fprintf(out, " %s", asm_register_name(v->asmb->out[k]));
                }
                if (v->asmb->reserved) fprintf(out, ", clobbers 0x%x", v->asmb->reserved);
                break;
            case IR_JMP:
                fprintf(out, " b%d", bb->succ[0]);
                break;
            case IR_BR:
                fputc(' ', out);
                print_value(out, num, v->args[0]);
                fprintf(out, ", b%d, b%d", bb->succ[0], bb->succ[1]);
                break;
            default:
                for (int a = 0; a < v->nargs; a++) {
                    fputs(a ? ", " : " ", out);
                    print_value(out, num, v->args[a]);
                }
                break;
            }
            fputc('\n', out);
        }
    }
    fputs("}\n", out);
    free(num);
}
