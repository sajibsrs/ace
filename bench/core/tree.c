/* Tree-checking benchmark paired with tree.ace: the same generator, walk, and
 * checker over the same tree, written as C reads it — through pointers, with
 * a linear-probing hash table for functions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FUNCS 4000u
#define ROUNDS 20u
#define MAX_DEPTH 4u
#define T_INT 1u
#define T_BOOL 2u
#define T_BAD 3u
#define SLOTS 8192u

typedef struct { const char* ptr; size_t len; } Str;

static const Str NAMES[12] = {
    {"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}, {"e", 1}, {"f", 1},
    {"g", 1}, {"h", 1}, {"i", 1}, {"j", 1}, {"k", 1}, {"l", 1},
};

enum { K_NONE, K_LIT, K_NAME, K_ADD, K_LESS, K_CALL, K_LET, K_COND, K_RET, K_BLOCK, K_FUNC };

typedef struct {
    uint8_t kind;
    uint32_t flags, line, col;
    Str text;
    uint64_t ival;
    uint32_t a, b, c, kids, nkids;
} Node;

typedef struct { Str name; uint32_t ty, decl, nparams; } Sym;
typedef struct { Str name; uint32_t ty; } Local;
typedef struct { Str key; uint32_t value; int used; } Slot;

static Node* nodes; static size_t nnodes, cnodes;
static uint32_t* kids; static size_t nkids, ckids;
static uint32_t funcs[FUNCS];
static char pool[FUNCS * 8]; static size_t used;
static uint64_t seed = 42;

static Sym syms[FUNCS + 1];
static Slot slots[SLOTS];
static Local locals[64]; static size_t nlocals;
static uint32_t* nty;
static size_t errors;

static uint64_t now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t next(uint64_t n) {
    uint64_t x = seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    seed = x;
    return (x >> 11) % n;
}

static uint32_t add(Node n) {
    if (nnodes == cnodes) { cnodes = cnodes ? cnodes * 2 : 1; nodes = realloc(nodes, cnodes * sizeof *nodes); }
    nodes[nnodes] = n;
    return (uint32_t)nnodes++;
}

static void push_kid(uint32_t k) {
    if (nkids == ckids) { ckids = ckids ? ckids * 2 : 1; kids = realloc(kids, ckids * sizeof *kids); }
    kids[nkids++] = k;
}

static Str func_name(uint32_t i) {
    size_t start = used;
    char digits[10];
    int count = 0;
    pool[used++] = 'f';
    do { digits[count++] = (char)('0' + i % 10); i /= 10; } while (i);
    while (count > 0) pool[used++] = digits[--count];
    return (Str){pool + start, used - start};
}

static uint32_t expr(uint32_t depth, uint32_t nlocal, uint32_t f) {
    uint64_t pick = next(10);
    if (depth >= MAX_DEPTH || pick < 3) {
        uint64_t v = next(1000);
        return add((Node){.kind = K_LIT, .ival = v});
    }
    if (pick < 6) {
        Str name = NAMES[next(nlocal)];
        return add((Node){.kind = K_NAME, .text = name});
    }
    if (pick < 8) {
        uint32_t a = expr(depth + 1, nlocal, f);
        uint32_t b = expr(depth + 1, nlocal, f);
        return add((Node){.kind = pick == 7 ? K_LESS : K_ADD, .a = a, .b = b});
    }
    uint32_t callee = (uint32_t)next(f + 1);
    uint32_t x = expr(depth + 1, nlocal, f);
    uint32_t y = expr(depth + 1, nlocal, f);
    uint32_t start = (uint32_t)nkids;
    push_kid(x);
    push_kid(y);
    return add((Node){.kind = K_CALL, .text = nodes[funcs[callee]].text, .kids = start, .nkids = 2});
}

static uint32_t block(uint32_t depth, uint32_t nlocal, uint32_t f) {
    uint32_t count = (uint32_t)(2 + next(4));
    uint32_t ids[8];
    uint32_t scope = nlocal;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t pick = next(10);
        if (pick < 5 && scope < 12) {
            uint32_t init = expr(0, scope, f);
            ids[i] = add((Node){.kind = K_LET, .text = NAMES[scope], .a = init});
            scope++;
        } else if (pick < 8 && depth < 2) {
            uint32_t test = expr(0, scope, f);
            uint32_t body = block(depth + 1, scope, f);
            ids[i] = add((Node){.kind = K_COND, .a = test, .b = body});
        } else {
            uint32_t value = expr(0, scope, f);
            ids[i] = add((Node){.kind = K_RET, .a = value});
        }
    }
    uint32_t start = (uint32_t)nkids;
    for (uint32_t i = 0; i < count; i++) push_kid(ids[i]);
    return add((Node){.kind = K_BLOCK, .kids = start, .nkids = count});
}

static void build(void) {
    add((Node){.kind = K_NONE});
    for (uint32_t f = 0; f < FUNCS; f++) {
        Str name = func_name(f);
        uint32_t id = add((Node){.kind = K_FUNC, .text = name, .nkids = 2});
        funcs[f] = id;
        uint32_t body = block(0, 2, f);
        nodes[id].a = body;
    }
}

static size_t walk(uint32_t id) {
    const Node* n = &nodes[id];
    switch (n->kind) {
    case K_LIT: return (size_t)n->ival;
    case K_NAME: return n->text.len;
    case K_ADD: case K_LESS: case K_COND: return walk(n->a) + walk(n->b) + 1;
    case K_LET: case K_RET: return walk(n->a) + 2;
    case K_CALL: case K_BLOCK: {
        size_t sum = n->nkids;
        for (uint32_t i = 0; i < n->nkids; i++) sum += walk(kids[n->kids + i]);
        return sum;
    }
    default: return 0;
    }
}

static int same(Str a, Str b) { return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0; }

static uint64_t hash(Str s) {
    uint64_t h = 1469598103934665603u;
    for (size_t i = 0; i < s.len; i++) h = (h ^ (uint8_t)s.ptr[i]) * 1099511628211u;
    return h;
}

static void global_set(Str key, uint32_t value) {
    size_t at = hash(key) & (SLOTS - 1);
    while (slots[at].used && !same(slots[at].key, key)) at = (at + 1) & (SLOTS - 1);
    slots[at] = (Slot){key, value, 1};
}

static int global_get(Str key, uint32_t* value) {
    size_t at = hash(key) & (SLOTS - 1);
    while (slots[at].used) {
        if (same(slots[at].key, key)) { *value = slots[at].value; return 1; }
        at = (at + 1) & (SLOTS - 1);
    }
    return 0;
}

static uint32_t find_local(Str name) {
    for (size_t i = nlocals; i > 0; i--)
        if (same(locals[i - 1].name, name)) return locals[i - 1].ty;
    return 0;
}

static uint32_t check_expr(uint32_t id) {
    const Node* n = &nodes[id];
    uint32_t t = T_BAD;
    switch (n->kind) {
    case K_LIT: t = T_INT; break;
    case K_NAME:
        t = find_local(n->text);
        if (!t) { errors++; t = T_BAD; }
        break;
    case K_ADD: case K_LESS: {
        uint32_t x = check_expr(n->a);
        uint32_t y = check_expr(n->b);
        if (x != y) errors++;
        t = n->kind == K_LESS ? T_BOOL : T_INT;
        break;
    }
    case K_CALL: {
        uint32_t s = 0;
        int ok = global_get(n->text, &s);
        if (!ok || syms[s].nparams != n->nkids) errors++;
        for (uint32_t i = 0; i < n->nkids; i++)
            if (check_expr(kids[n->kids + i]) != T_INT) errors++;
        if (ok) t = syms[s].ty;
        break;
    }
    default: break;
    }
    nty[id] = t;
    return t;
}

static void check_block(uint32_t id) {
    size_t mark = nlocals;
    const Node* n = &nodes[id];
    for (uint32_t i = 0; i < n->nkids; i++) {
        const Node* x = &nodes[kids[n->kids + i]];
        switch (x->kind) {
        case K_LET: {
            uint32_t t = check_expr(x->a);
            locals[nlocals++] = (Local){x->text, t};
            break;
        }
        case K_COND:
            if (check_expr(x->a) != T_BOOL) errors++;
            check_block(x->b);
            break;
        case K_RET:
            if (check_expr(x->a) != T_INT) errors++;
            break;
        default: break;
        }
    }
    nlocals = mark;
}

static void check(void) {
    for (uint32_t f = 0; f < FUNCS; f++) {
        locals[0] = (Local){NAMES[0], T_INT};
        locals[1] = (Local){NAMES[1], T_INT};
        nlocals = 2;
        check_block(nodes[funcs[f]].a);
        nlocals = 0;
    }
}

static void report(const char* name, uint64_t elapsed, size_t operations, size_t checksum) {
    printf("%s %llu %zu %zu\n", name, (unsigned long long)elapsed, operations, checksum);
}

int main(void) {
    build();
    for (uint32_t f = 0; f < FUNCS; f++) {
        syms[f + 1] = (Sym){nodes[funcs[f]].text, T_INT, funcs[f], 2};
        global_set(nodes[funcs[f]].text, f + 1);
    }
    nty = calloc(nnodes, sizeof *nty);

    uint64_t started = now();
    size_t sum = 0;
    for (uint32_t r = 0; r < ROUNDS; r++)
        for (uint32_t f = 0; f < FUNCS; f++) sum += walk(nodes[funcs[f]].a);
    report("walk", now() - started, ROUNDS * nnodes, sum);

    started = now();
    for (uint32_t r = 0; r < ROUNDS; r++) {
        errors = 0;
        check();
    }
    size_t typed = 0;
    for (size_t i = 0; i < nnodes; i++) typed += nty[i];
    report("check", now() - started, ROUNDS * nnodes, errors * 1000003 + typed);
    free(nodes);
    free(kids);
    free(nty);
    return 0;
}
