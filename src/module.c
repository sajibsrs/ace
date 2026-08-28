// module.c — bootstrap import resolution.
//
// The first stdlib slice is intentionally narrow: bare `import fmt` loads
// std/fmt.ace beside the compiler, lowers the public `fmt.print` overload set
// to one private module-prefixed overload set, and merges that dependency
// before the root declarations. This preserves the single-pass checker/emitter
// today while leaving general dependency graphs, path imports, and separate
// incremental objects as the next module-loader chunk.
#include "ace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void import_err(Diag* d, Node* n, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%d:%d: error: ",
            n && n->path ? n->path : d->path,
            n ? n->line : 0, n ? n->col : 0);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    d->errors++;
}

static int name_is(Node* n, const char* s) {
    size_t len = strlen(s);
    return n && n->len == len && memcmp(n->str, s, len) == 0;
}

static const char* arena_string(const char* s) {
    size_t n = strlen(s);
    char* out = arena_alloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

static void compiler_dir(char* out, size_t cap, const char* argv0) {
    const char* slash = strrchr(argv0, '/');
    if (!slash) {
        snprintf(out, cap, ".");
        return;
    }
    size_t n = (size_t)(slash - argv0);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, argv0, n);
    out[n] = 0;
}

static Node* load_module(const char* path, Diag* root) {
    const char* stable_path = arena_string(path);
    Source src;
    if (source_load(&src, stable_path) != 0) {
        root->errors++;
        return NULL;
    }
    Diag d = { stable_path, 0 };
    size_t ntok = 0;
    Token* toks = lex(&src, &d, &ntok);
    if (d.errors) {
        root->errors += d.errors;
        return NULL;
    }
    Node* file = parse(toks, ntok, &d);
    if (d.errors) {
        root->errors += d.errors;
        return NULL;
    }
    return file;
}

static void rewrite_ident(Node* n, const char* from, const char* to) {
    if (!n) return;
    if ((n->kind == N_IDENT || n->kind == N_NAMED) && name_is(n, from)) {
        n->str = to;
        n->len = (uint32_t)strlen(to);
    }
    rewrite_ident(n->a, from, to);
    rewrite_ident(n->b, from, to);
    rewrite_ident(n->c, from, to);
    for (int i = 0; i < n->nkids; i++) rewrite_ident(n->kids[i], from, to);
}

static int import_is_fmt(Node* n) {
    if (!n || n->kind != N_IMPORT || n->a) return 0;
    return n->b ? name_is(n->b, "fmt") : name_is(n, "fmt");
}

static void lower_fmt_members(Node* n, Node** imports, int nimports,
                              const char* lowered, Diag* d) {
    if (!n) return;
    if (n->kind == N_FIELD_E && n->a && n->a->kind == N_IDENT) {
        for (int i = 0; i < nimports; i++) {
            Node* im = imports[i];
            if (!import_is_fmt(im) || !name_is(n->a, im->str)) continue;
            if (!name_is(n, "print")) {
                import_err(d, n, "module 'fmt' has no public member '%.*s'",
                           (int)n->len, n->str);
                return;
            }
            int line = n->line, col = n->col;
            const char* path = n->path;
            memset(n, 0, sizeof *n);
            n->kind = N_IDENT;
            n->line = line;
            n->col = col;
            n->path = path;
            n->str = lowered;
            n->len = (uint32_t)strlen(lowered);
            return;
        }
    }
    lower_fmt_members(n->a, imports, nimports, lowered, d);
    lower_fmt_members(n->b, imports, nimports, lowered, d);
    lower_fmt_members(n->c, imports, nimports, lowered, d);
    for (int i = 0; i < n->nkids; i++)
        lower_fmt_members(n->kids[i], imports, nimports, lowered, d);
}

int resolve_imports(Node* file, Diag* d, const char* compiler_path) {
    Node* fmt_imports[32];
    int nfmt = 0;
    for (int i = 0; i < file->nkids; i++) {
        Node* n = file->kids[i];
        if (import_is_fmt(n) && nfmt < 32) fmt_imports[nfmt++] = n;
    }
    if (!nfmt) return 0;

    char dir[4096], path[4096];
    compiler_dir(dir, sizeof dir, compiler_path);
    if (strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "std/fmt.ace");
    else
        snprintf(path, sizeof path, "%s/std/fmt.ace", dir);
    Node* fmt = load_module(path, d);
    if (!fmt) return d->errors;

    const char* lowered = arena_string("fmt$print");
    int nprint = 0;
    for (int i = 0; i < fmt->nkids; i++) {
        Node* n = fmt->kids[i];
        if (n->kind == N_FUNC && name_is(n, "print")) {
            if (n->flags & F_PUB) nprint++;
            else import_err(d, n, "every standard fmt.print overload must be public");
        }
    }
    if (d->errors) return d->errors;
    if (!nprint) {
        import_err(d, fmt_imports[0], "standard module 'fmt' does not export print");
        return d->errors;
    }
    rewrite_ident(fmt, "print", lowered);
    for (int i = 0; i < fmt->nkids; i++) {
        Node* n = fmt->kids[i];
        if (n->kind == N_FUNC && name_is(n, "print")) {
            n->str = lowered;
            n->len = (uint32_t)strlen(lowered);
        }
    }

    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind != N_IMPORT)
            lower_fmt_members(file->kids[i], fmt_imports, nfmt, lowered, d);
    if (d->errors) return d->errors;

    int nroot_imports = 0, nfmt_decls = 0, nroot_decls = 0;
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_IMPORT) nroot_imports++;
        else nroot_decls++;
    for (int i = 0; i < fmt->nkids; i++)
        if (fmt->kids[i]->kind != N_IMPORT) nfmt_decls++;

    int total = nroot_imports + nfmt_decls + nroot_decls;
    Node** kids = (Node**)arena_alloc((size_t)total * sizeof(Node*));
    int at = 0;
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_IMPORT) kids[at++] = file->kids[i];
    for (int i = 0; i < fmt->nkids; i++)
        if (fmt->kids[i]->kind != N_IMPORT) kids[at++] = fmt->kids[i];
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind != N_IMPORT) kids[at++] = file->kids[i];
    file->kids = kids;
    file->nkids = total;
    return 0;
}
