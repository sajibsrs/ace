/* Tree-checking benchmark paired with tree.ace and tree.c, ported to C++
 * as written: the same generator, walk, and
 * checker over the same tree, written as C reads it — through pointers, with
 * a linear-probing hash table for functions.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define FUNCS 4000u
#define ROUNDS 20u
#define MAX_DEPTH 4u
#define T_INT 1u
#define T_BOOL 2u
#define T_BAD 3u
#define SLOTS 8192u

struct Str { const char* ptr; std::size_t len; };

static const Str NAMES[12] = {
    {"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}, {"e", 1}, {"f", 1},
    {"g", 1}, {"h", 1}, {"i", 1}, {"j", 1}, {"k", 1}, {"l", 1},
};

enum : std::uint8_t { K_NONE, K_LIT, K_NAME, K_ADD, K_LESS, K_CALL, K_LET, K_COND, K_RET, K_BLOCK, K_FUNC };

struct Node {
    std::uint8_t kind;
    std::uint32_t flags, line, col;
    Str text;
    std::uint64_t ival;
    std::uint32_t a, b, c, kids, nkids;
};

struct Sym { Str name; std::uint32_t ty, decl, nparams; };
struct Local { Str name; std::uint32_t ty; };
struct Slot { Str key; std::uint32_t value; int used; };

static Node* nodes; static std::size_t nnodes, cnodes;
static std::uint32_t* kids; static std::size_t nkids, ckids;
static std::uint32_t funcs[FUNCS];
static char pool[FUNCS * 8]; static std::size_t used;
static std::uint64_t seed = 42;

static Sym syms[FUNCS + 1];
static Slot slots[SLOTS];
static Local locals[64]; static std::size_t nlocals;
static std::uint32_t* nty;
static std::size_t errors;

static std::uint64_t now() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (std::uint64_t)t.tv_sec * 1000000000u + (std::uint64_t)t.tv_nsec;
}

static std::uint64_t next(std::uint64_t n) {
    std::uint64_t x = seed;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    seed = x;
    return (x >> 11) % n;
}

static std::uint32_t add(Node n) {
    if (nnodes == cnodes) { cnodes = cnodes ? cnodes * 2 : 1; nodes = (Node*)std::realloc(nodes, cnodes * sizeof *nodes); }
    nodes[nnodes] = n;
    return (std::uint32_t)nnodes++;
}

static void push_kid(std::uint32_t k) {
    if (nkids == ckids) { ckids = ckids ? ckids * 2 : 1; kids = (std::uint32_t*)std::realloc(kids, ckids * sizeof *kids); }
    kids[nkids++] = k;
}

static Str func_name(std::uint32_t i) {
    std::size_t start = used;
    char digits[10];
    int count = 0;
    pool[used++] = 'f';
    do { digits[count++] = (char)('0' + i % 10); i /= 10; } while (i);
    while (count > 0) pool[used++] = digits[--count];
    return Str{pool + start, used - start};
}

static std::uint32_t expr(std::uint32_t depth, std::uint32_t nlocal, std::uint32_t f) {
    std::uint64_t pick = next(10);
    if (depth >= MAX_DEPTH || pick < 3) {
        std::uint64_t v = next(1000);
        return add(Node{.kind = K_LIT, .ival = v});
    }
    if (pick < 6) {
        Str name = NAMES[next(nlocal)];
        return add(Node{.kind = K_NAME, .text = name});
    }
    if (pick < 8) {
        std::uint32_t a = expr(depth + 1, nlocal, f);
        std::uint32_t b = expr(depth + 1, nlocal, f);
        return add(Node{.kind = pick == 7 ? K_LESS : K_ADD, .a = a, .b = b});
    }
    std::uint32_t callee = (std::uint32_t)next(f + 1);
    std::uint32_t x = expr(depth + 1, nlocal, f);
    std::uint32_t y = expr(depth + 1, nlocal, f);
    std::uint32_t start = (std::uint32_t)nkids;
    push_kid(x);
    push_kid(y);
    return add(Node{.kind = K_CALL, .text = nodes[funcs[callee]].text, .kids = start, .nkids = 2});
}

static std::uint32_t block(std::uint32_t depth, std::uint32_t nlocal, std::uint32_t f) {
    std::uint32_t count = (std::uint32_t)(2 + next(4));
    std::uint32_t ids[8];
    std::uint32_t scope = nlocal;
    for (std::uint32_t i = 0; i < count; i++) {
        std::uint64_t pick = next(10);
        if (pick < 5 && scope < 12) {
            std::uint32_t init = expr(0, scope, f);
            ids[i] = add(Node{.kind = K_LET, .text = NAMES[scope], .a = init});
            scope++;
        } else if (pick < 8 && depth < 2) {
            std::uint32_t test = expr(0, scope, f);
            std::uint32_t body = block(depth + 1, scope, f);
            ids[i] = add(Node{.kind = K_COND, .a = test, .b = body});
        } else {
            std::uint32_t value = expr(0, scope, f);
            ids[i] = add(Node{.kind = K_RET, .a = value});
        }
    }
    std::uint32_t start = (std::uint32_t)nkids;
    for (std::uint32_t i = 0; i < count; i++) push_kid(ids[i]);
    return add(Node{.kind = K_BLOCK, .kids = start, .nkids = count});
}

static void build() {
    add(Node{.kind = K_NONE});
    for (std::uint32_t f = 0; f < FUNCS; f++) {
        Str name = func_name(f);
        std::uint32_t id = add(Node{.kind = K_FUNC, .text = name, .nkids = 2});
        funcs[f] = id;
        std::uint32_t body = block(0, 2, f);
        nodes[id].a = body;
    }
}

static std::size_t walk(std::uint32_t id) {
    const Node* n = &nodes[id];
    switch (n->kind) {
    case K_LIT: return (std::size_t)n->ival;
    case K_NAME: return n->text.len;
    case K_ADD: case K_LESS: case K_COND: return walk(n->a) + walk(n->b) + 1;
    case K_LET: case K_RET: return walk(n->a) + 2;
    case K_CALL: case K_BLOCK: {
        std::size_t sum = n->nkids;
        for (std::uint32_t i = 0; i < n->nkids; i++) sum += walk(kids[n->kids + i]);
        return sum;
    }
    default: return 0;
    }
}

static int same(Str a, Str b) { return a.len == b.len && std::memcmp(a.ptr, b.ptr, a.len) == 0; }

static std::uint64_t hash(Str s) {
    std::uint64_t h = 1469598103934665603u;
    for (std::size_t i = 0; i < s.len; i++) h = (h ^ (std::uint8_t)s.ptr[i]) * 1099511628211u;
    return h;
}

static void global_set(Str key, std::uint32_t value) {
    std::size_t at = hash(key) & (SLOTS - 1);
    while (slots[at].used && !same(slots[at].key, key)) at = (at + 1) & (SLOTS - 1);
    slots[at] = Slot{key, value, 1};
}

static int global_get(Str key, std::uint32_t* value) {
    std::size_t at = hash(key) & (SLOTS - 1);
    while (slots[at].used) {
        if (same(slots[at].key, key)) { *value = slots[at].value; return 1; }
        at = (at + 1) & (SLOTS - 1);
    }
    return 0;
}

static std::uint32_t find_local(Str name) {
    for (std::size_t i = nlocals; i > 0; i--)
        if (same(locals[i - 1].name, name)) return locals[i - 1].ty;
    return 0;
}

static std::uint32_t check_expr(std::uint32_t id) {
    const Node* n = &nodes[id];
    std::uint32_t t = T_BAD;
    switch (n->kind) {
    case K_LIT: t = T_INT; break;
    case K_NAME:
        t = find_local(n->text);
        if (!t) { errors++; t = T_BAD; }
        break;
    case K_ADD: case K_LESS: {
        std::uint32_t x = check_expr(n->a);
        std::uint32_t y = check_expr(n->b);
        if (x != y) errors++;
        t = n->kind == K_LESS ? T_BOOL : T_INT;
        break;
    }
    case K_CALL: {
        std::uint32_t s = 0;
        int ok = global_get(n->text, &s);
        if (!ok || syms[s].nparams != n->nkids) errors++;
        for (std::uint32_t i = 0; i < n->nkids; i++)
            if (check_expr(kids[n->kids + i]) != T_INT) errors++;
        if (ok) t = syms[s].ty;
        break;
    }
    default: break;
    }
    nty[id] = t;
    return t;
}

static void check_block(std::uint32_t id) {
    std::size_t mark = nlocals;
    const Node* n = &nodes[id];
    for (std::uint32_t i = 0; i < n->nkids; i++) {
        const Node* x = &nodes[kids[n->kids + i]];
        switch (x->kind) {
        case K_LET: {
            std::uint32_t t = check_expr(x->a);
            locals[nlocals++] = Local{x->text, t};
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

static void check() {
    for (std::uint32_t f = 0; f < FUNCS; f++) {
        locals[0] = Local{NAMES[0], T_INT};
        locals[1] = Local{NAMES[1], T_INT};
        nlocals = 2;
        check_block(nodes[funcs[f]].a);
        nlocals = 0;
    }
}

static void report(const char* name, std::uint64_t elapsed, std::size_t operations, std::size_t checksum) {
    std::printf("%s %llu %zu %zu\n", name, (unsigned long long)elapsed, operations, checksum);
}

int main() {
    build();
    for (std::uint32_t f = 0; f < FUNCS; f++) {
        syms[f + 1] = Sym{nodes[funcs[f]].text, T_INT, funcs[f], 2};
        global_set(nodes[funcs[f]].text, f + 1);
    }
    nty = (std::uint32_t*)std::calloc(nnodes, sizeof *nty);

    std::uint64_t started = now();
    std::size_t sum = 0;
    for (std::uint32_t r = 0; r < ROUNDS; r++)
        for (std::uint32_t f = 0; f < FUNCS; f++) sum += walk(nodes[funcs[f]].a);
    report("walk", now() - started, ROUNDS * nnodes, sum);

    started = now();
    for (std::uint32_t r = 0; r < ROUNDS; r++) {
        errors = 0;
        check();
    }
    std::size_t typed = 0;
    for (std::size_t i = 0; i < nnodes; i++) typed += nty[i];
    report("check", now() - started, ROUNDS * nnodes, errors * 1000003 + typed);
    std::free(nodes);
    std::free(kids);
    std::free(nty);
    return 0;
}
