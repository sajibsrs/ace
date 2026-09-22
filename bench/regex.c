/* The Pike VM of pkg/regex/inc/matcher.ace in C, running the exact programs
 * Program::compile emits for the benchmark patterns (copied from
 * Program.format), over the same generated text as bench/regex/main.ace.
 * Data layout and control flow mirror the Ace matcher so the comparison
 * measures code generation, not algorithm.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIZE 1048576
#define CALLS 10000
#define UNSET UINT64_MAX
#define NIL UINT32_MAX

enum op { MATCH, BYTE, ANY, CLASS, BOL, EOL, WORD, NOT_WORD, SAVE, JMP, SPLIT };

typedef struct {
    int32_t op;
    uint32_t x;
    uint32_t y;
} Inst;

typedef struct {
    uint64_t bits[4];
} Class;

typedef struct {
    const Inst *insts;
    size_t count;
    const Class *classes;
    size_t slots;
} Program;

typedef struct {
    uint32_t *dense;
    uint32_t *sparse;
    size_t *caps;
    size_t count;
} Queue;

typedef struct {
    uint32_t pc;
    uint32_t slot;
    size_t value;
} Frame;

typedef struct {
    Queue current;
    Queue next;
    Frame *stack;
    size_t *work;
    size_t *best;
    size_t slots;
} Matcher;

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void report(const char *name, uint64_t elapsed, uint64_t operations,
                   uint64_t checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed,
           (unsigned long long)operations, (unsigned long long)checksum);
}

static void fill(uint8_t *text, size_t size) {
    uint64_t x = 88172645463325252ull;

    for (size_t i = 0; i < size; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t r = x >> 33;
        uint64_t kind = r % 16;

        if (kind == 0) text[i] = ' ';
        else if (kind == 1) text[i] = '\n';
        else if (kind <= 4) text[i] = (uint8_t)(48 + (r >> 8) % 10);
        else if (kind == 5) text[i] = '-';
        else text[i] = (uint8_t)(97 + (r >> 8) % 26);
    }
}

static bool class_has(const Class *set, uint8_t byte) {
    return ((set->bits[byte >> 6] >> (byte & 63)) & 1) != 0;
}

static void queue_init(Queue *q, size_t size, size_t slots) {
    q->dense = malloc(size * sizeof(uint32_t));
    q->sparse = calloc(size, sizeof(uint32_t));
    q->caps = malloc(size * slots * sizeof(size_t));
    q->count = 0;
}

static bool queue_has(const Queue *q, uint32_t pc) {
    uint32_t at = q->sparse[pc];
    return at < q->count && q->dense[at] == pc;
}

static void queue_insert(Queue *q, uint32_t pc) {
    q->sparse[pc] = (uint32_t)q->count;
    q->dense[q->count] = pc;
    q->count += 1;
}

static bool is_word(uint8_t byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool is_boundary(const uint8_t *text, size_t len, size_t at) {
    bool before = at > 0 && is_word(text[at - 1]);
    bool after = at < len && is_word(text[at]);
    return before != after;
}

static void matcher_init(Matcher *m, const Program *code) {
    queue_init(&m->current, code->count, code->slots);
    queue_init(&m->next, code->count, code->slots);
    m->stack = malloc((code->count + 1) * sizeof(Frame));
    m->work = malloc(code->slots * sizeof(size_t));
    m->best = malloc(code->slots * sizeof(size_t));
    m->slots = code->slots;
}

static void matcher_free(Matcher *m) {
    free(m->current.dense);
    free(m->current.sparse);
    free(m->current.caps);
    free(m->next.dense);
    free(m->next.sparse);
    free(m->next.caps);
    free(m->stack);
    free(m->work);
    free(m->best);
}

static void add(Matcher *m, const Inst *code, bool into_next, uint32_t start,
                const uint8_t *text, size_t len, size_t at) {
    Queue *q = into_next ? &m->next : &m->current;
    size_t top = 1;
    m->stack[0] = (Frame){start, NIL, 0};

    while (top > 0) {
        top -= 1;
        Frame frame = m->stack[top];

        if (frame.slot != NIL) {
            m->work[frame.slot] = frame.value;
            continue;
        }
        uint32_t pc = frame.pc;

        for (;;) {
            if (queue_has(q, pc)) break;
            queue_insert(q, pc);
            const Inst inst = code[pc];

            if (inst.op == JMP) {
                pc = inst.x;
                continue;
            }
            if (inst.op == SPLIT) {
                m->stack[top++] = (Frame){inst.y, NIL, 0};
                pc = inst.x;
                continue;
            }
            if (inst.op == SAVE) {
                m->stack[top++] = (Frame){0, inst.x, m->work[inst.x]};
                m->work[inst.x] = at;
                pc += 1;
                continue;
            }
            if (inst.op == BOL) {
                if (at == 0) { pc += 1; continue; }
                break;
            }
            if (inst.op == EOL) {
                if (at == len) { pc += 1; continue; }
                break;
            }
            if (inst.op == WORD) {
                if (is_boundary(text, len, at)) { pc += 1; continue; }
                break;
            }
            if (inst.op == NOT_WORD) {
                if (!is_boundary(text, len, at)) { pc += 1; continue; }
                break;
            }
            size_t row = (size_t)pc * m->slots;
            for (size_t i = 0; i < m->slots; i++) q->caps[row + i] = m->work[i];
            break;
        }
    }
}

static bool run(Matcher *m, const Program *code, const uint8_t *text,
                size_t len, size_t after) {
    if (after > len) return false;
    const Inst *insts = code->insts;
    m->current.count = 0;
    bool matched = false;
    size_t at = after;

    for (;;) {
        if (!matched) {
            for (size_t i = 0; i < m->slots; i++) m->work[i] = UNSET;
            add(m, insts, false, 0, text, len, at);
        }
        if (m->current.count == 0) break;
        m->next.count = 0;

        for (size_t i = 0; i < m->current.count; i++) {
            uint32_t pc = m->current.dense[i];
            size_t row = (size_t)pc * m->slots;
            bool advance = false;
            const Inst inst = insts[pc];

            if (inst.op == BYTE) {
                advance = at < len && text[at] == (uint8_t)inst.x;
            } else if (inst.op == ANY) {
                advance = at < len && text[at] != '\n';
            } else if (inst.op == CLASS) {
                advance = at < len && class_has(&code->classes[inst.x], text[at]);
            } else if (inst.op == MATCH) {
                for (size_t s = 0; s < m->slots; s++) m->best[s] = m->current.caps[row + s];
                matched = true;
                break;
            }
            if (advance) {
                for (size_t s = 0; s < m->slots; s++) m->work[s] = m->current.caps[row + s];
                add(m, insts, true, pc + 1, text, len, at + 1);
            }
        }
        if (at == len) break;
        at += 1;
        Queue swap = m->current;
        m->current = m->next;
        m->next = swap;
    }
    return matched;
}

/* The programs, copied from Program.format. */

static const Inst literal_insts[] = {
    {SAVE, 0, 0}, {BYTE, 'z', 0}, {BYTE, 'q', 0}, {BYTE, 'x', 0},
    {BYTE, 'j', 0}, {SAVE, 1, 0}, {MATCH, 0, 0},
};

static const Class lower = {{0, 0x07FFFFFE00000000ull, 0, 0}};

static const Inst word_insts[] = {
    {SAVE, 0, 0}, {CLASS, 0, 0}, {SPLIT, 1, 3}, {BYTE, 'i', 0},
    {BYTE, 'n', 0}, {BYTE, 'g', 0}, {SAVE, 1, 0}, {MATCH, 0, 0},
};

static const Class digits[] = {
    {{0x03FF000000000000ull, 0, 0, 0}},
    {{0x03FF000000000000ull, 0, 0, 0}},
};

static const Inst captures_insts[] = {
    {SAVE, 0, 0}, {SAVE, 2, 0}, {CLASS, 0, 0}, {SPLIT, 2, 4},
    {SAVE, 3, 0}, {BYTE, '-', 0}, {SAVE, 4, 0}, {CLASS, 1, 0},
    {SPLIT, 7, 9}, {SAVE, 5, 0}, {SAVE, 1, 0}, {MATCH, 0, 0},
};

static const Inst patho_insts[] = {
    {SAVE, 0, 0}, {SPLIT, 2, 10}, {SAVE, 2, 0}, {SPLIT, 4, 6},
    {BYTE, 'a', 0}, {JMP, 8, 0}, {BYTE, 'a', 0}, {BYTE, 'a', 0},
    {SAVE, 3, 0}, {JMP, 1, 0}, {BYTE, 'c', 0}, {SAVE, 1, 0},
    {MATCH, 0, 0},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

__attribute__((noinline))
static void find_all(const char *name, const Program *code, const uint8_t *text,
                     size_t len) {
    Matcher m;
    matcher_init(&m, code);
    uint64_t checksum = 0;
    size_t at = 0;
    uint64_t started = now_ns();

    while (at <= len) {
        if (!run(&m, code, text, len, at)) break;
        size_t start = m.best[0], end = m.best[1];
        checksum += 1 + start + end;
        at = end;
        if (end == start) at += 1;
    }
    report(name, now_ns() - started, len, checksum);
    matcher_free(&m);
}

__attribute__((noinline))
static void captures(const uint8_t *text, size_t len) {
    Program code = {captures_insts, COUNT(captures_insts), digits, 6};
    Matcher m;
    matcher_init(&m, &code);
    uint64_t checksum = 0;
    size_t at = 0;
    uint64_t started = now_ns();

    while (at <= len) {
        if (!run(&m, &code, text, len, at)) break;
        checksum += 1 + m.best[2] + m.best[5];
        at = m.best[1];
    }
    report("captures", now_ns() - started, len, checksum);
    matcher_free(&m);
}

__attribute__((noinline))
static void pathological(void) {
    Program code = {patho_insts, COUNT(patho_insts), NULL, 4};
    const uint8_t *text = (const uint8_t *)"aaaaaaaaaaaaaabc";
    size_t len = strlen((const char *)text);
    Matcher m;
    matcher_init(&m, &code);
    uint64_t checksum = 0;
    uint64_t started = now_ns();

    for (size_t i = 0; i < CALLS; i++) {
        if (run(&m, &code, text, len, 0)) checksum += 1 + m.best[0] + m.best[1];
    }
    report("pathological", now_ns() - started, CALLS * len, checksum);
    matcher_free(&m);
}

int main(void) {
    uint8_t *text = malloc(SIZE);
    if (text == NULL) return 1;
    fill(text, SIZE);

    Program literal = {literal_insts, COUNT(literal_insts), NULL, 2};
    Program word = {word_insts, COUNT(word_insts), &lower, 2};
    find_all("literal", &literal, text, SIZE);
    find_all("word-ing", &word, text, SIZE);
    captures(text, SIZE);
    pathological();
    free(text);
    return 0;
}
