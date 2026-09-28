// vector_ir.c — the IR's internal vector types through the AArch64 target
// (SPEC.md Compiler #6, auto-vectorization), before any pass creates them.
//
// Built twice by tests/run.sh: with -DEMIT and the compiler's sources, it
// builds each function's IR by hand, verifies and prints it, and writes an
// object (argv[1]); without, it links that object and checks every lane
// against the scalar operation in C, bit for bit.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef EMIT
#include "../src/backend.h"
#include "../src/ir.h"

static IrFunc* F;

static int ins(int b, IrOp op, IrType t, int a0, int a1, int a2) {
    int v = op == IR_PHI ? ir_phi_new(F, b, t) : ir_ins_new(F, b, op, t);
    if (a0 >= 0) ir_arg_add(F, v, a0);
    if (a1 >= 0) ir_arg_add(F, v, a1);
    if (a2 >= 0) ir_arg_add(F, v, a2);
    return v;
}
static int op2(int b, IrOp op, IrType t, int x, int y) { return ins(b, op, t, x, y, -1); }
static int op1(int b, IrOp op, IrType t, int x) { return ins(b, op, t, x, -1, -1); }
static int konst(int b, IrType t, uint64_t imm) {
    int v = ins(b, IR_CONST, t, -1, -1, -1);
    F->ins[v].imm = imm;
    return v;
}
static int load(int b, IrType t, int p, int off) {
    int v = op1(b, IR_LOAD, t, p);
    F->ins[v].aux = off;
    return v;
}
static void store(int b, int p, int off, int x) {
    int v = op2(b, IR_STORE, IT_VOID, p, x);
    F->ins[v].aux = off;
}

static Node decls[16];
static int ndecls;

// A void function of the given parameter types; *param receives each PARAM.
static int begin(const char* name, const IrType* ps, int n, int* param) {
    Node* d = &decls[ndecls++];
    d->str = name;
    d->len = (uint32_t)strlen(name);
    F = ir_func_new(d, NULL);
    F->params = malloc((size_t)n * sizeof(IrType));
    memcpy(F->params, ps, (size_t)n * sizeof(IrType));
    F->nparams = n;
    int b = ir_block_new(F);
    for (int k = 0; k < n; k++) {
        param[k] = ins(b, IR_PARAM, ps[k], -1, -1, -1);
        F->ins[param[k]].imm = (uint64_t)k;
    }
    return b;
}

static void finish(Obj* o, int b) {
    op1(b, IR_RET, IT_VOID, -1);
    if (!ir_verify(F)) exit(1);
    ir_print(F, stdout);
    arm64_func(o, F, F->decl->str, F->decl->len, 1);
}

static const IrType P3[] = { IT_PTR, IT_PTR, IT_PTR };

// out[k] = a op b for add, sub, mul, div (float), and neg a
static void arith(Obj* o, const char* name, IrType t) {
    int p[3], b = begin(name, P3, 3, p);
    int x = load(b, t, p[0], 0), y = load(b, t, p[1], 0);
    static const IrOp ops[] = { IR_ADD, IR_SUB, IR_MUL, IR_DIV };
    int k = 0;
    for (int i = 0; i < 4; i++)
        if (ops[i] != IR_DIV || t == IT_V4F32) store(b, p[2], 16 * k++, op2(b, ops[i], t, x, y));
    store(b, p[2], 16 * k, op1(b, IR_NEG, t, x));
    finish(o, b);
}

// out[k] = a op b for eq, ne, lt, le, gt, ge: masks
static void compare(Obj* o, const char* name, IrType t) {
    int p[3], b = begin(name, P3, 3, p);
    int x = load(b, t, p[0], 0), y = load(b, t, p[1], 0);
    for (int op = IR_EQ; op <= IR_GE; op++) store(b, p[2], 16 * (op - IR_EQ), op2(b, (IrOp)op, IT_V4I32, x, y));
    finish(o, b);
}

// out = select(m, a, b) as floats and integers, and with the result
// computed over each operand (a later use keeps the others live)
static void select_(Obj* o) {
    static const IrType ps[] = { IT_PTR, IT_PTR, IT_PTR, IT_PTR };
    int p[4], b = begin("_v_select", ps, 4, p);
    int m = load(b, IT_V4I32, p[0], 0);
    int x = load(b, IT_V4F32, p[1], 0), y = load(b, IT_V4F32, p[2], 0);
    int xi = load(b, IT_V4I32, p[1], 0), yi = load(b, IT_V4I32, p[2], 0);
    store(b, p[3], 0, ins(b, IR_SELECT, IT_V4F32, m, x, y));
    store(b, p[3], 16, ins(b, IR_SELECT, IT_V4I32, m, xi, yi));
    int y2 = load(b, IT_V4F32, p[2], 0); // dies at its select: bit over it
    store(b, p[3], 32, ins(b, IR_SELECT, IT_V4F32, m, x, y2));
    int x2 = load(b, IT_V4F32, p[1], 0); // dies at its select: bif over it
    store(b, p[3], 48, ins(b, IR_SELECT, IT_V4F32, m, x2, y));
    int m2 = load(b, IT_V4I32, p[0], 0); // dies at its select: bsl over it
    store(b, p[3], 64, ins(b, IR_SELECT, IT_V4F32, m2, x, y));
    store(b, p[3], 80, op2(b, IR_ADD, IT_V4F32, x, y)); // x, y, m live throughout
    store(b, p[3], 96, op2(b, IR_ADD, IT_V4I32, m, m));
    finish(o, b);
}

// out = splat(x), splat(y), splat(2.5), splat(-7), and scvtf of a
static void splat(Obj* o) {
    static const IrType ps[] = { IT_PTR, IT_F32, IT_I32, IT_PTR };
    int p[4], b = begin("_v_splat", ps, 4, p);
    store(b, p[0], 0, op1(b, IR_SPLAT, IT_V4F32, p[1]));
    store(b, p[0], 16, op1(b, IR_SPLAT, IT_V4I32, p[2]));
    store(b, p[0], 32, op1(b, IR_SPLAT, IT_V4F32, konst(b, IT_F32, 0x40200000u)));
    store(b, p[0], 48, op1(b, IR_SPLAT, IT_V4I32, konst(b, IT_I32, (uint64_t)-7)));
    store(b, p[0], 64, op1(b, IR_CAST, IT_V4F32, load(b, IT_V4I32, p[3], 0)));
    finish(o, b);
}

// out = lanes, a and b, a or b, a xor b, a << 3, a >> 1, a >> 31 (arithmetic),
// and splats of 0.0, 200, and 256 (movi, movi, and dup: not an 8-bit lane)
static void bits(Obj* o) {
    int p[3], b = begin("_v_bits", P3, 3, p);
    int x = load(b, IT_V4I32, p[0], 0), y = load(b, IT_V4I32, p[1], 0);
    store(b, p[2], 0, ins(b, IR_LANES, IT_V4I32, -1, -1, -1));
    store(b, p[2], 16, op2(b, IR_AND, IT_V4I32, x, y));
    store(b, p[2], 32, op2(b, IR_OR, IT_V4I32, x, y));
    store(b, p[2], 48, op2(b, IR_XOR, IT_V4I32, x, y));
    store(b, p[2], 64, op2(b, IR_SHL, IT_V4I32, x, konst(b, IT_I32, 3)));
    store(b, p[2], 80, op2(b, IR_SHR, IT_V4I32, x, konst(b, IT_I32, 1)));
    store(b, p[2], 96, op2(b, IR_SHR, IT_V4I32, x, konst(b, IT_I32, 31)));
    store(b, p[2], 112, op1(b, IR_SPLAT, IT_V4F32, konst(b, IT_F32, 0)));
    store(b, p[2], 128, op1(b, IR_SPLAT, IT_V4I32, konst(b, IT_I32, 200)));
    store(b, p[2], 144, op1(b, IR_SPLAT, IT_V4I32, konst(b, IT_I32, 256)));
    finish(o, b);
}

// A counted loop carrying vector phis: acc += p[i] (a register index scaled
// by 16), iv += splat(1) from splat(0), and a pair swapped every iteration (a
// move cycle); then out = acc, float(iv), and the pair.
static void loop(Obj* o) {
    static const IrType ps[] = { IT_PTR, IT_U64, IT_PTR };
    int p[3], b0 = begin("_v_loop", ps, 3, p);
    int b1 = ir_block_new(F), b2 = ir_block_new(F), b3 = ir_block_new(F);
    int zero = op1(b0, IR_SPLAT, IT_V4F32, konst(b0, IT_F32, 0));
    int izero = op1(b0, IR_SPLAT, IT_V4I32, konst(b0, IT_I32, 0));
    int one = op1(b0, IR_SPLAT, IT_V4I32, konst(b0, IT_I32, 1));
    int s0 = load(b0, IT_V4F32, p[0], 0), t0 = load(b0, IT_V4F32, p[0], 16), i0 = konst(b0, IT_U64, 0);
    op1(b0, IR_JMP, IT_VOID, -1);
    ir_edge_add(F, b0, b1);
    int i = ins(b1, IR_PHI, IT_U64, -1, -1, -1), acc = ins(b1, IR_PHI, IT_V4F32, -1, -1, -1);
    int iv = ins(b1, IR_PHI, IT_V4I32, -1, -1, -1);
    int s = ins(b1, IR_PHI, IT_V4F32, -1, -1, -1), t = ins(b1, IR_PHI, IT_V4F32, -1, -1, -1);
    op1(b1, IR_BR, IT_VOID, op2(b1, IR_LT, IT_BOOL, i, p[1]));
    ir_edge_add(F, b1, b2);
    ir_edge_add(F, b1, b3);
    int at = op2(b2, IR_PADD, IT_PTR, p[0], i);
    F->ins[at].imm = 16;
    int acc1 = op2(b2, IR_ADD, IT_V4F32, acc, load(b2, IT_V4F32, at, 0));
    int iv1 = op2(b2, IR_ADD, IT_V4I32, iv, one);
    int i1 = op2(b2, IR_ADD, IT_U64, i, konst(b2, IT_U64, 1));
    op1(b2, IR_JMP, IT_VOID, -1);
    ir_edge_add(F, b2, b1);
    int from0[] = { i0, zero, izero, s0, t0 }, from2[] = { i1, acc1, iv1, t, s };
    int phi[] = { i, acc, iv, s, t };
    for (int k = 0; k < 5; k++) { ir_arg_add(F, phi[k], from0[k]); ir_arg_add(F, phi[k], from2[k]); }
    store(b3, p[2], 0, acc);
    store(b3, p[2], 16, op1(b3, IR_CAST, IT_V4F32, iv));
    store(b3, p[2], 32, s);
    store(b3, p[2], 48, t);
    finish(o, b3);
}

// 32 vectors live at once (more than the registers: spills), summed in order
static void pressure(Obj* o) {
    static const IrType ps[] = { IT_PTR, IT_PTR };
    int p[2], b = begin("_v_pressure", ps, 2, p);
    int v[32];
    for (int k = 0; k < 32; k++) v[k] = load(b, IT_V4F32, p[0], 16 * k);
    int s = v[0];
    for (int k = 1; k < 32; k++) s = op2(b, IR_ADD, IT_V4F32, s, v[k]);
    store(b, p[1], 0, s);
    for (int k = 0; k < 32; k++) store(b, p[1], 16 + 16 * k, op2(b, IR_MUL, IT_V4F32, v[k], s));
    finish(o, b);
}

// A vector live across a call, which disturbs every v register's upper half
static void across(Obj* o) {
    static const IrType ps[] = { IT_PTR, IT_PTR };
    int p[2], b = begin("_v_call", ps, 2, p);
    static Sym ext;
    ext.is_extern = 1;
    int x = load(b, IT_V4F32, p[0], 0), y = load(b, IT_V4I32, p[0], 16);
    int c = ins(b, IR_CALL, IT_VOID, -1, -1, -1);
    F->ins[c].name = "_clobber";
    F->ins[c].sym = &ext;
    store(b, p[1], 0, op2(b, IR_ADD, IT_V4F32, x, x));
    store(b, p[1], 16, op2(b, IR_ADD, IT_V4I32, y, y));
    finish(o, b);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    Diag d = { "vector_ir", 0 };
    Obj o;
    obj_init(&o, &d, EMIT_RELEASE);
    arith(&o, "_vf_arith", IT_V4F32);
    arith(&o, "_vi_arith", IT_V4I32);
    compare(&o, "_vf_cmp", IT_V4F32);
    compare(&o, "_vi_cmp", IT_V4I32);
    select_(&o);
    splat(&o);
    bits(&o);
    loop(&o);
    pressure(&o);
    across(&o);
    obj_patch_fixups(&o);
    ESym* anchor = sym_intern(&o.syms, "__ace$text", 10);
    obj_debug_info(&o, anchor);
    obj_debug_line(&o, anchor);
    obj_write(&o, argv[1]);
    return d.errors != 0;
}

#else

typedef union { float f[4]; int32_t i[4]; uint32_t u[4]; } V;

void vf_arith(const V*, const V*, V*);
void vi_arith(const V*, const V*, V*);
void vf_cmp(const V*, const V*, V*);
void vi_cmp(const V*, const V*, V*);
void v_select(const V*, const V*, const V*, V*);
void v_splat(V*, float, int32_t, const V*);
void v_bits(const V*, const V*, V*);
void v_loop(const V*, uint64_t, V*);
void v_pressure(const V*, V*);
void v_call(const V*, V*);

// Every v register changes except the callee-saved low halves of v8-v15.
void clobber(void) {
    __asm__ volatile("movi v0.16b, #0x55\n movi v1.16b, #0x55\n movi v2.16b, #0x55\n movi v3.16b, #0x55\n"
                     "movi v4.16b, #0x55\n movi v5.16b, #0x55\n movi v6.16b, #0x55\n movi v7.16b, #0x55\n"
                     "mov v8.d[1], xzr\n mov v9.d[1], xzr\n mov v10.d[1], xzr\n mov v11.d[1], xzr\n"
                     "mov v12.d[1], xzr\n mov v13.d[1], xzr\n mov v14.d[1], xzr\n mov v15.d[1], xzr\n"
                     "movi v16.16b, #0x55\n movi v17.16b, #0x55\n movi v18.16b, #0x55\n movi v19.16b, #0x55\n"
                     "movi v20.16b, #0x55\n movi v21.16b, #0x55\n movi v22.16b, #0x55\n movi v23.16b, #0x55\n"
                     "movi v24.16b, #0x55\n movi v25.16b, #0x55\n movi v26.16b, #0x55\n movi v27.16b, #0x55\n"
                     "movi v28.16b, #0x55\n movi v29.16b, #0x55\n movi v30.16b, #0x55\n movi v31.16b, #0x55\n"
                     ::: "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20",
                         "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}

static int fails;

static void expect(const char* what, int k, const V* got, const V* want) {
    if (memcmp(got, want, sizeof(V)) == 0) return;
    fails++;
    fprintf(stderr, "%s[%d]: got %08x %08x %08x %08x, want %08x %08x %08x %08x\n", what, k, got->u[0], got->u[1],
            got->u[2], got->u[3], want->u[0], want->u[1], want->u[2], want->u[3]);
}

static uint32_t mask(int c) { return c ? 0xFFFFFFFFu : 0; }

int main(void) {
    const float nan = __builtin_nanf(""), inf = __builtin_inff();
    V fa[] = { { .f = { 1.5f, -0.0f, nan, inf } }, { .f = { 3.0f, 1e-45f, -inf, 0.1f } } };
    V fb[] = { { .f = { 1.5f, 0.0f, 2.0f, inf } }, { .f = { -7.25f, 1e38f, nan, 0.3f } } };
    V ia[] = { { .i = { 1, INT32_MAX, -5, INT32_MIN } }, { .i = { 0, -1, 70000, 3 } } };
    V ib[] = { { .i = { 1, 1, 7, -1 } }, { .i = { -1, -1, 70000, -3 } } };
    for (int c = 0; c < 2; c++) {
        V out[6], want[6];
        vf_arith(&fa[c], &fb[c], out);
        for (int l = 0; l < 4; l++) {
            float x = fa[c].f[l], y = fb[c].f[l];
            want[0].f[l] = x + y; want[1].f[l] = x - y; want[2].f[l] = x * y; want[3].f[l] = x / y;
            want[4].f[l] = -x;
        }
        for (int k = 0; k < 5; k++) expect("vf_arith", k, &out[k], &want[k]);
        vi_arith(&ia[c], &ib[c], out);
        for (int l = 0; l < 4; l++) {
            uint32_t x = ia[c].u[l], y = ib[c].u[l];
            want[0].u[l] = x + y; want[1].u[l] = x - y; want[2].u[l] = x * y; want[3].u[l] = 0u - x;
        }
        for (int k = 0; k < 4; k++) expect("vi_arith", k, &out[k], &want[k]);
        vf_cmp(&fa[c], &fb[c], out);
        for (int l = 0; l < 4; l++) {
            float x = fa[c].f[l], y = fb[c].f[l];
            want[0].u[l] = mask(x == y); want[1].u[l] = mask(x != y); want[2].u[l] = mask(x < y);
            want[3].u[l] = mask(x <= y); want[4].u[l] = mask(x > y); want[5].u[l] = mask(x >= y);
        }
        for (int k = 0; k < 6; k++) expect("vf_cmp", k, &out[k], &want[k]);
        vi_cmp(&ia[c], &ib[c], out);
        for (int l = 0; l < 4; l++) {
            int32_t x = ia[c].i[l], y = ib[c].i[l];
            want[0].u[l] = mask(x == y); want[1].u[l] = mask(x != y); want[2].u[l] = mask(x < y);
            want[3].u[l] = mask(x <= y); want[4].u[l] = mask(x > y); want[5].u[l] = mask(x >= y);
        }
        for (int k = 0; k < 6; k++) expect("vi_cmp", k, &out[k], &want[k]);
    }
    {
        V m = { .u = { 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu } }, out[7], want[7];
        v_select(&m, &fa[0], &fb[1], out);
        for (int l = 0; l < 4; l++) {
            uint32_t s = m.u[l] ? fa[0].u[l] : fb[1].u[l];
            for (int k = 0; k < 5; k++) want[k].u[l] = s;
            want[5].f[l] = fa[0].f[l] + fb[1].f[l];
            want[6].u[l] = m.u[l] + m.u[l];
        }
        for (int k = 0; k < 7; k++) expect("v_select", k, &out[k], &want[k]);
    }
    {
        V out[5], want[5], n = { .i = { 0, -1, INT32_MAX, 16777217 } };
        v_splat(out, -0.0f, -123456, &n);
        for (int l = 0; l < 4; l++) {
            want[0].f[l] = -0.0f; want[1].i[l] = -123456; want[2].f[l] = 2.5f; want[3].i[l] = -7;
            want[4].f[l] = (float)n.i[l];
        }
        for (int k = 0; k < 5; k++) expect("v_splat", k, &out[k], &want[k]);
    }
    for (int c = 0; c < 2; c++) {
        V out[10], want[10];
        v_bits(&ia[c], &ib[c], out);
        for (int l = 0; l < 4; l++) {
            int32_t x = ia[c].i[l], y = ib[c].i[l];
            want[0].i[l] = l; want[1].i[l] = x & y; want[2].i[l] = x | y; want[3].i[l] = x ^ y;
            want[4].u[l] = (uint32_t)x << 3; want[5].i[l] = x >> 1; want[6].i[l] = x >> 31;
            want[7].f[l] = 0.0f; want[8].i[l] = 200; want[9].i[l] = 256;
        }
        for (int k = 0; k < 10; k++) expect("v_bits", k, &out[k], &want[k]);
    }
    {
        V p[40], out[4], want[4];
        for (int k = 0; k < 40; k++)
            for (int l = 0; l < 4; l++) p[k].f[l] = (float)(k * 4 + l) * 0.37f - 20.0f;
        for (uint64_t n = 0; n < 40; n += 13) {
            v_loop(p, n, out);
            memset(want, 0, sizeof want);
            for (int l = 0; l < 4; l++) {
                float acc = 0;
                for (uint64_t k = 0; k < n; k++) acc += p[k].f[l];
                want[0].f[l] = acc;
                want[1].f[l] = (float)n;
                want[2].f[l] = n % 2 ? p[1].f[l] : p[0].f[l];
                want[3].f[l] = n % 2 ? p[0].f[l] : p[1].f[l];
            }
            for (int k = 0; k < 4; k++) expect("v_loop", (int)n * 10 + k, &out[k], &want[k]);
        }
        V out2[33], want2[33];
        v_pressure(p, out2);
        for (int l = 0; l < 4; l++) {
            float s = p[0].f[l];
            for (int k = 1; k < 32; k++) s += p[k].f[l];
            want2[0].f[l] = s;
            for (int k = 0; k < 32; k++) want2[1 + k].f[l] = p[k].f[l] * s;
        }
        for (int k = 0; k < 33; k++) expect("v_pressure", k, &out2[k], &want2[k]);
        v_call(p, out);
        for (int l = 0; l < 4; l++) {
            want[0].f[l] = p[0].f[l] + p[0].f[l];
            want[1].u[l] = p[1].u[l] + p[1].u[l];
        }
        for (int k = 0; k < 2; k++) expect("v_call", k, &out[k], &want[k]);
    }
    if (fails) return 1;
    puts("vector_ir: ok");
    return 0;
}

#endif
