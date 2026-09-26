// ir.c — construction, verification, and printing of the per-function IR.
#include "ir.h"
#include "backend.h"

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
    free(fn);
}

// --- types ------------------------------------------------------------------------

int ir_type_bits(IrType t) {
    switch (t) {
    case IT_BOOL: case IT_I8: case IT_U8: return 8;
    case IT_I16: case IT_U16: return 16;
    case IT_I32: case IT_U32: case IT_F32: return 32;
    case IT_I64: case IT_U64: case IT_PTR: case IT_F64: return 64;
    default: return 0;
    }
}

int ir_type_signed(IrType t) { return t >= IT_I8 && t <= IT_I64; }

int ir_type_float(IrType t) { return t == IT_F32 || t == IT_F64; }

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
    static const char* names[] = { "void", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "ptr", "f32", "f64" };
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
static int* dominators(IrFunc* fn, int** rpo_out, int* nrpo) {
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

static int dominates(const int* idom, int a, int b) {
    while (b != a && b != 0) b = idom[b];
    return b == a;
}

int ir_verify(IrFunc* fn) {
    if (fn->nblocks == 0) return bad(fn, "no blocks%s", 0, 0);
    if (fn->blocks[0].npreds) return bad(fn, "entry block b0 has %d predecessors%s", fn->blocks[0].npreds, 0);
    int* pos = malloc((size_t)(fn->nins ? fn->nins : 1) * sizeof(int)); // index within its block
    if (!pos) oom();
    int ok = 1;
    for (int b = 0; b < fn->nblocks && ok; b++) {
        IrBlock* bb = &fn->blocks[b];
        if (!bb->nins) { ok = bad(fn, "block b%d is empty%s", b, 0); break; }
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
    int* idom = ok ? dominators(fn, &rpo, &nrpo) : NULL;
    if (ok && nrpo != fn->nblocks) ok = bad(fn, "%d blocks unreachable from the entry%s", fn->nblocks - nrpo, 0);
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
                if (fn->ins[u].type == IT_VOID) { ok = bad(fn, "%%%d uses void value %%%d", id, u); break; }
                int want_ptr = a == 0 && (v->op == IR_LOAD || v->op == IR_STORE || v->op == IR_PADD ||
                                          v->op == IR_PSUB || v->op == IR_CALLI);
                if (want_ptr && fn->ins[u].type != IT_PTR) { ok = bad(fn, "%%%d needs an address, not %%%d", id, u); break; }
                int db = fn->ins[u].block;
                // a phi operand must be available at the end of its predecessor
                int ub = v->op == IR_PHI ? bb->preds[a] : b;
                int dom = db == ub ? (v->op == IR_PHI || pos[u] < i) : dominates(idom, db, ub);
                if (!dom) { ok = bad(fn, "%%%d uses %%%d where its definition does not dominate", id, u); break; }
            }
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
    for (int i = 0; i < fn->nparams; i++) fprintf(out, "%s%s", i ? ", " : "", type_name(fn->params[i]));
    fprintf(out, "): %s {\n", type_name(fn->ret));
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
            if (v->type != IT_VOID) {
                print_value(out, num, id);
                fputs(" = ", out);
            }
            fputs(op_name((IrOp)v->op), out);
            if (v->flags & IRF_CHECKED) fputs(".chk", out);
            if (v->flags & IRF_VOLATILE) fputs(".vol", out);
            if (v->type != IT_VOID) fprintf(out, " %s", type_name((IrType)v->type));
            switch (v->op) {
            case IR_CONST:
                fputc(' ', out);
                print_imm(out, (IrType)v->type, v->imm);
                break;
            case IR_PARAM:
                fprintf(out, " %llu", (unsigned long long)v->imm);
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
                print_sym(out, v->sym);
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
            case IR_CALL: case IR_CALLI: {
                int first = v->op == IR_CALLI;
                fputc(' ', out);
                if (first) print_value(out, num, v->args[0]);
                else if (v->sym) print_sym(out, v->sym);
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
