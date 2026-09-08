// lexer.c — single-pass lexer for Ace.
//
// Newline handling (SPEC.md "newline terminates statements", GRAMMAR.md §6.1):
// a newline token is emitted only when the previous token can end an
// expression — ident/literal/null/true/false, ')', ']', '}', '?', or the
// keywords return/break/continue — and the next token is not ';'. Consequence:
// parenthesized/bracketed groups and trailing-comma lists continue across
// lines for free; 'else' stays on the '}' line. ';' tokenizes only for the
// for-head clause separators; the parser rejects it everywhere else.
#include "ace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* tok_disp(TokKind kind) {
    static const char* const disp[] = {
#define T(_, s) s,
        TOKEN_LIST(T)
#undef T
    };
    return disp[kind];
}

static void oom(void) {
    fprintf(stderr, "ace: out of memory\n");
    exit(1);
}

int source_load(Source* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ace: cannot open '%s'\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "ace: cannot read '%s'\n", path);
        return -1;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        fprintf(stderr, "ace: cannot read '%s'\n", path);
        return -1;
    }
    rewind(f);
    s->buf = malloc((size_t)n + 1);
    if (!s->buf) oom();
    size_t got = fread(s->buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        fprintf(stderr, "ace: cannot read '%s'\n", path);
        free(s->buf);
        return -1;
    }
    s->buf[n] = 0;
    s->len = (size_t)n;
    s->path = path;
    if (memchr(s->buf, 0, s->len)) {
        fprintf(stderr, "%s: error: source contains a NUL byte\n", path);
        free(s->buf);
        return -1;
    }
    return 0;
}

// Chunked bump arena for decoded string bytes — pointers stay stable.
typedef struct Chunk {
    struct Chunk* next;
    size_t used, cap;
    max_align_t align;
    char data[];
} Chunk;

static Chunk* arena = NULL;

char* arena_alloc(size_t n) {
    const size_t align = _Alignof(max_align_t);
    size_t start = arena ? (arena->used + align - 1) & ~(align - 1) : 0;
    if (!arena || start > arena->cap || arena->cap - start < n) {
        size_t need = n + align - 1;
        size_t cap = need > (size_t)8192 ? need : (size_t)8192;
        Chunk* c = malloc(sizeof(Chunk) + cap);
        if (!c) oom();
        c->next = arena;
        c->used = 0;
        c->cap = cap;
        arena = c;
        start = 0;
    }
    char* p = arena->data + start;
    arena->used = start + n;
    return p;
}

static const char* arena_copy(const char* p, size_t n) {
    char* q = arena_alloc(n + 1); // NUL-terminated: names are compared with strlen
    memcpy(q, p, n);
    q[n] = 0;
    return q;
}

typedef struct {
    Source* s;
    Diag*   d;
    size_t  pos;
    int     line, col;
    // start of the token being scanned (set in the main loop)
    int     tline, tcol;
    size_t  toff;
    Token*  toks;
    size_t  n, cap;
    TokKind prev; // kind of last emitted token; T_NL = none/already emitted
    char*   scratch; // string decode buffer
    size_t  scratch_len, scratch_cap;
} Lex;

static char cur(Lex* L) { return L->pos < L->s->len ? L->s->buf[L->pos] : 0; }

static char peek(Lex* L, int k) {
    size_t p = L->pos + (size_t)k;
    return p < L->s->len ? L->s->buf[p] : 0;
}

static char adv(Lex* L) {
    if (L->pos >= L->s->len) return 0;
    char c = L->s->buf[L->pos++];
    if (c == '\n') {
        L->line++;
        L->col = 1;
    } else {
        L->col++;
    }
    return c;
}

static void err(Lex* L, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ", L->d->path, L->tline, L->tcol);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    L->d->errors++;
}

static Token mktok(Lex* L, TokKind k) {
    Token t;
    t.kind = k;
    t.line = L->tline;
    t.col = L->tcol;
    t.off = L->toff;
    t.ival = 0;
    t.fval = 0;
    t.str = NULL;
    t.len = 0;
    return t;
}

static void push(Lex* L, Token t) {
    if (L->n == L->cap) {
        L->cap = L->cap ? L->cap * 2 : 1024;
        L->toks = realloc(L->toks, L->cap * sizeof(Token));
        if (!L->toks) oom();
    }
    L->toks[L->n++] = t;
    L->prev = t.kind;
}

// Can a token of this kind end an expression/statement? (newline rule)
static int ends_expr(TokKind k) {
    switch (k) {
    case T_IDENT: case T_INT: case T_FLOAT: case T_STR: case T_CHARLIT:
    case T_NULL: case T_TRUE: case T_FALSE:
    case T_RPAREN: case T_RBRACK: case T_RBRACE:
    case T_RETURN: case T_BREAK: case T_CONTINUE:
    case T_SPECIALIZED: // ends a bodyless function declaration
    case T_INTRINSIC:
        return 1;
    default:
        return 0;
    }
}

// --- identifiers & keywords -------------------------------------------------

static const struct {
    const char* s;
    TokKind     k;
} KEYWORDS[] = {
    {"import", T_IMPORT},     {"from", T_FROM},         {"pub", T_PUB},
    {"mut", T_MUT},
    {"const", T_CONST},       {"noalias", T_NOALIAS},   {"if", T_IF},
    {"else", T_ELSE},         {"for", T_FOR},           {"in", T_IN},
    {"switch", T_SWITCH},     {"return", T_RETURN},     {"break", T_BREAK},
    {"continue", T_CONTINUE}, {"defer", T_DEFER},        {"asm", T_ASM},
    {"extern", T_EXTERN},     {"export", T_EXPORT},     {"contract", T_CONTRACT},
    {"struct", T_STRUCT},     {"embed", T_EMBED},       {"signs", T_SIGNS},
    {"distinct", T_DISTINCT},
    {"type", T_TYPE},         {"enum", T_ENUM},         {"union", T_UNION},
    {"specialized", T_SPECIALIZED},
    {"intrinsic", T_INTRINSIC},
    {"volatile", T_VOLATILE},
    {"null", T_NULL},
    {"true", T_TRUE},         {"false", T_FALSE},
};

static int ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int ident_char(char c) {
    return ident_start(c) || (c >= '0' && c <= '9');
}

static void scan_ident(Lex* L) {
    while (ident_char(cur(L))) adv(L);
    size_t n = L->pos - L->toff;
    const char* text = L->s->buf + L->toff;
    TokKind k = T_IDENT;
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strlen(KEYWORDS[i].s) == n && memcmp(KEYWORDS[i].s, text, n) == 0) {
            k = KEYWORDS[i].k;
            break;
        }
    }
    Token t = mktok(L, k);
    t.str = arena_copy(text, n); // NUL-terminated copy, not a view into source
    t.len = (uint32_t)n;
    push(L, t);
}

// --- numbers -----------------------------------------------------------------

static int digval(char c, int base) {
    int v = -1;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    if (v >= base) v = -1;
    return v;
}

// Every '_' must sit between two digits of the given base.
static int seps_ok(const char* p, size_t n, int base) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != '_') continue;
        if (i == 0 || i == n - 1) return 0;
        if (digval(p[i - 1], base) < 0 || digval(p[i + 1], base) < 0) return 0;
    }
    return 1;
}

static void scan_number(Lex* L) {
    Source* s = L->s;
    int base = 10;
    if (cur(L) == '0' && (peek(L, 1) == 'x' || peek(L, 1) == 'X')) {
        adv(L); adv(L);
        base = 16;
    } else if (cur(L) == '0' && (peek(L, 1) == 'b' || peek(L, 1) == 'B')) {
        adv(L); adv(L);
        base = 2;
    }

    size_t dstart = L->pos;
    for (;;) {
        char c = cur(L);
        if (c == '_' || digval(c, base) >= 0) adv(L);
        else break;
    }
    size_t dend = L->pos;

    int is_float = 0;
    if (base == 10 && cur(L) == '.' && digval(peek(L, 1), 10) >= 0) {
        is_float = 1;
        adv(L);
        while (cur(L) == '_' || digval(cur(L), 10) >= 0) adv(L);
        if (cur(L) == 'e' || cur(L) == 'E') {
            adv(L);
            if (cur(L) == '+' || cur(L) == '-') adv(L);
            if (digval(cur(L), 10) < 0) err(L, "malformed float exponent");
            while (cur(L) == '_' || digval(cur(L), 10) >= 0) adv(L);
        }
    } else if (base == 10 && (cur(L) == 'e' || cur(L) == 'E')) {
        char n1 = peek(L, 1), n2 = peek(L, 2);
        if (digval(n1, 10) >= 0 || ((n1 == '+' || n1 == '-') && digval(n2, 10) >= 0))
            err(L, "float exponent requires a '.' — write 1.0e5, not 1e5");
    }

    if (!seps_ok(s->buf + dstart, L->pos - dstart, base))
        err(L, "malformed '_' separator (must sit between digits)");
    if (dend == dstart)
        err(L, "missing digits after base prefix");
    else if (base == 2 && digval(cur(L), 10) >= 0)
        err(L, "invalid binary digit '%c'", cur(L));

    if (is_float) {
        size_t n = L->pos - L->toff;
        char* clean = arena_alloc(n + 1);
        size_t j = 0;
        for (size_t i = 0; i < n; i++)
            if (s->buf[L->toff + i] != '_') clean[j++] = s->buf[L->toff + i];
        clean[j] = 0;
        Token t = mktok(L, T_FLOAT);
        t.fval = strtod(clean, NULL);
        push(L, t);
    } else {
        uint64_t v = 0;
        int of = 0;
        for (size_t i = dstart; i < dend; i++) {
            char c = s->buf[i];
            if (c == '_') continue;
            int dv = digval(c, base);
            if (v > (UINT64_MAX - (uint64_t)dv) / (uint64_t)base) {
                of = 1;
                break;
            }
            v = v * (uint64_t)base + (uint64_t)dv;
        }
        if (of) err(L, "integer literal out of range");
        Token t = mktok(L, T_INT);
        t.ival = v; // 0 on overflow; the error above fails the build
        push(L, t);
    }
}

// --- strings & chars -----------------------------------------------------------

static void sb_push(Lex* L, char c) {
    if (L->scratch_len == L->scratch_cap) {
        L->scratch_cap = L->scratch_cap ? L->scratch_cap * 2 : 64;
        L->scratch = realloc(L->scratch, L->scratch_cap);
        if (!L->scratch) oom();
    }
    L->scratch[L->scratch_len++] = c;
}

// Called with the backslash already consumed.
static char read_escape(Lex* L) {
    char c = cur(L);
    if (c == 0) {
        err(L, "unterminated escape sequence");
        return '?';
    }
    adv(L);
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case '\\': return '\\';
    case '"': return '"';
    case '\'': return '\'';
    case 'x': {
        int h = digval(cur(L), 16);
        if (h < 0) {
            err(L, "\\x escape needs two hex digits");
            return '?';
        }
        adv(L);
        int l = digval(cur(L), 16);
        if (l < 0) {
            err(L, "\\x escape needs two hex digits");
            return '?';
        }
        adv(L);
        return (char)(h * 16 + l);
    }
    default:
        err(L, "unknown escape sequence '\\%c'", c);
        return '?';
    }
}

static void scan_string(Lex* L) {
    adv(L); // opening quote
    L->scratch_len = 0;
    for (;;) {
        char c = cur(L);
        if (c == '"') {
            adv(L);
            break;
        }
        if (c == 0 || c == '\n') {
            err(L, "unterminated string literal");
            break;
        }
        if (c == '\\') {
            adv(L);
            sb_push(L, read_escape(L));
        } else {
            sb_push(L, adv(L)); // UTF-8 bytes pass through
        }
    }
    Token t = mktok(L, T_STR);
    t.str = arena_copy(L->scratch, L->scratch_len);
    t.len = (uint32_t)L->scratch_len;
    push(L, t);
}

static void scan_char(Lex* L) {
    adv(L); // opening quote
    uint64_t val = 0;
    int ok = 1;
    if (cur(L) == 0 || cur(L) == '\n') {
        err(L, "unterminated char literal");
        ok = 0;
    } else if (cur(L) == '\'') {
        adv(L);
        err(L, "empty char literal");
        ok = 0;
    } else if (cur(L) == '\\') {
        adv(L);
        val = (unsigned char)read_escape(L);
    } else {
        val = (unsigned char)adv(L);
    }
    if (ok) {
        if (cur(L) != '\'') {
            err(L, "char literal must hold exactly one character");
            while (cur(L) && cur(L) != '\'' && cur(L) != '\n') adv(L);
            if (cur(L) == '\'') adv(L);
        } else {
            adv(L);
        }
    }
    Token t = mktok(L, T_CHARLIT);
    t.ival = val;
    push(L, t);
}

// --- operators & punctuation ---------------------------------------------------

static void scan_punct(Lex* L, char c) {
    TokKind k;
    switch (c) {
    case '(': adv(L); k = T_LPAREN; break;
    case ')': adv(L); k = T_RPAREN; break;
    case '[': adv(L); k = T_LBRACK; break;
    case ']': adv(L); k = T_RBRACK; break;
    case '{': adv(L); k = T_LBRACE; break;
    case '}': adv(L); k = T_RBRACE; break;
    case ',': adv(L); k = T_COMMA; break;
    case ':': adv(L); k = cur(L) == '=' ? (adv(L), T_DECLARE) : T_COLON; break;
    case '~': adv(L); k = T_TILDE; break;
    case '+': adv(L); k = cur(L) == '=' ? (adv(L), T_ADDEQ) : T_PLUS; break;
    case '-': adv(L); k = cur(L) == '=' ? (adv(L), T_SUBEQ) : T_MINUS; break;
    case '*': adv(L); k = cur(L) == '=' ? (adv(L), T_MULEQ) : T_STAR; break;
    case '%': adv(L); k = cur(L) == '=' ? (adv(L), T_MODEQ) : T_PERCENT; break;
    case '^': adv(L); k = cur(L) == '=' ? (adv(L), T_XOREQ) : T_CARET; break;
    case '=': adv(L); k = cur(L) == '=' ? (adv(L), T_EQEQ) : T_ASSIGN; break;
    case '!': adv(L); k = cur(L) == '=' ? (adv(L), T_BANGEQ) : T_BANG; break;
    case '/': adv(L); k = cur(L) == '=' ? (adv(L), T_DIVEQ) : T_SLASH; break;
    case '&':
        adv(L);
        if (cur(L) == '&') { adv(L); k = T_ANDAND; }
        else if (cur(L) == '=') { adv(L); k = T_ANDEQ; }
        else k = T_AMP;
        break;
    case '|':
        adv(L);
        if (cur(L) == '|') { adv(L); k = T_OROR; }
        else if (cur(L) == '=') { adv(L); k = T_OREQ; }
        else k = T_PIPE;
        break;
    case '<':
        adv(L);
        if (cur(L) == '=') { adv(L); k = T_LE; }
        else if (cur(L) == '<') {
            adv(L);
            if (cur(L) == '=') { adv(L); k = T_SHLEQ; } else k = T_SHL;
        } else k = T_LT;
        break;
    case '>':
        adv(L);
        if (cur(L) == '=') { adv(L); k = T_GE; }
        else if (cur(L) == '>') {
            adv(L);
            if (cur(L) == '=') { adv(L); k = T_SHREQ; } else k = T_SHR;
        } else k = T_GT;
        break;
    case '.':
        adv(L);
        if (cur(L) == '.') {
            adv(L);
            if (cur(L) == '.') { adv(L); k = T_DOTDOTDOT; } else k = T_DOTDOT;
        } else k = T_DOT;
        break;
    case ';':
        adv(L);
        k = T_SEMI; // valid only between for-head clauses (parser enforces)
        break;
    case '@': adv(L); k = T_AT; break;
    default:
        adv(L);
        if ((unsigned char)c >= 0x80)
            err(L, "non-ASCII byte 0x%02X (identifiers are ASCII in v1)", (unsigned char)c);
        else if (c > ' ' && c <= '~')
            err(L, "unexpected character '%c'", c);
        else
            err(L, "unexpected byte 0x%02X", (unsigned char)c);
        return;
    }
    push(L, mktok(L, k));
}

// --- driver --------------------------------------------------------------------

Token* lex(Source* s, Diag* d, size_t* count) {
    Lex L = {0};
    L.s = s;
    L.d = d;
    L.line = 1;
    L.col = 1;
    L.prev = T_NL;
    if (s->len >= 3 && (unsigned char)s->buf[0] == 0xEF &&
        (unsigned char)s->buf[1] == 0xBB && (unsigned char)s->buf[2] == 0xBF)
        L.pos = 3; // UTF-8 BOM

    for (;;) {
        int nl = 0;
        for (;;) { // trivia: whitespace, newlines, comments
            char c = cur(&L);
            if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
                adv(&L);
            } else if (c == '\n') {
                adv(&L);
                nl = 1;
            } else if (c == '\r') { // CRLF and lone CR both count once
                adv(&L);
                if (cur(&L) == '\n') adv(&L);
                nl = 1;
            } else if (c == '/' && peek(&L, 1) == '/') {
                while (cur(&L) && cur(&L) != '\n') adv(&L);
            } else if (c == '/' && peek(&L, 1) == '*') {
                int sl = L.line, sc = L.col;
                adv(&L); adv(&L);
                for (;;) {
                    if (!cur(&L)) {
                        fprintf(stderr, "%s:%d:%d: error: unterminated block comment\n",
                                s->path, sl, sc);
                        d->errors++;
                        break;
                    }
                    if (cur(&L) == '*' && peek(&L, 1) == '/') {
                        adv(&L); adv(&L);
                        break;
                    }
                    if (cur(&L) == '\n') nl = 1;
                    adv(&L);
                }
            } else {
                break;
            }
        }

        if (!cur(&L)) { // end of file: terminate the last statement, then stop
            if (L.n > 0 && L.prev != T_NL) {
                L.tline = L.line;
                L.tcol = L.col;
                L.toff = L.pos;
                push(&L, mktok(&L, T_NL));
            }
            L.tline = L.line;
            L.tcol = L.col;
            L.toff = L.pos;
            push(&L, mktok(&L, T_EOF));
            break;
        }

        // No newline token before a ';': for-head clauses stay joined even
        // across a physical line so the parser owns the one-line rule, and a
        // stray ';' elsewhere still ends its statement in the parser.
        if (nl && ends_expr(L.prev) && cur(&L) != ';') {
            L.tline = L.line;
            L.tcol = L.col;
            L.toff = L.pos;
            push(&L, mktok(&L, T_NL));
        }

        L.tline = L.line;
        L.tcol = L.col;
        L.toff = L.pos;

        char c = cur(&L);
        if (ident_start(c)) scan_ident(&L);
        else if (c >= '0' && c <= '9') scan_number(&L);
        else if (c == '"') scan_string(&L);
        else if (c == '\'') scan_char(&L);
        else scan_punct(&L, c);
    }

    *count = L.n;
    return L.toks;
}
