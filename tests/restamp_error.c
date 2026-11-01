// restamp_error.c — an error raised while lowering re-checks a generic body
// fails the build. Sema accepts every program that reaches lowering, so no
// source can raise one: this edits a checked body before emitting it.
//
// Linked with the compiler's sources by tests/run.sh; argv[1] is the program,
// argv[2] the object to emit, which must be left unwritten.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/backend.h"
#include "../src/sema.h"

// Renames every use of `x` under n.
static void rename_uses(Node* n) {
    if (!n) return;
    if (n->kind == N_IDENT && n->len == 1 && n->str[0] == 'x') {
        n->str = "gone";
        n->len = 4;
    }
    rename_uses(n->a);
    rename_uses(n->b);
    rename_uses(n->c);
    for (int i = 0; i < n->nkids; i++) rename_uses(n->kids[i]);
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    Source s;
    if (source_load(&s, argv[1]) != 0) return 2;
    Diag d = {argv[1], 0};
    size_t ntok;
    Token* toks = lex(&s, &d, &ntok);
    build_roots(".");
    Node* file = parse(toks, ntok, &d, "darwin-arm64");
    if (d.errors == 0) resolve_imports(file, &d, "darwin-arm64", NULL, NULL);
    if (d.errors == 0) sema_check(file, &d);
    if (d.errors != 0) return 2; // the program itself must be valid
    for (int i = 0; i < file->nkids; i++)
        if (file->kids[i]->kind == N_FUNC && file->kids[i]->c) rename_uses(file->kids[i]->b);
    if (emit_object(file, &d, argv[2], NULL, EMIT_DEBUG, 0) == 0) {
        printf("re-stamp error did not fail the build\n");
        return 1;
    }
    if (d.errors == 0 || access(argv[2], F_OK) == 0) {
        printf("re-stamp error went uncounted or wrote an object\n");
        return 1;
    }
    return 0;
}
