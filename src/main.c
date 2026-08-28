// main.c — compiler driver. Stages: lex → parse → sema (→ emit).
#include "ace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    fprintf(stderr, "usage: ace [--tokens] [--ast] [-c] [-o NAME] <file.ace>\n"
                    "  (default)  full semantic check, no codegen\n"
                    "  -c         emit a Mach-O arm64 object <base>.o\n"
                    "  -o NAME    emit + link via the system cc: cc <obj> -o NAME\n");
    exit(2);
}

static void dump_tokens(Token* toks, size_t n) {
    for (size_t i = 0; i < n; i++) {
        Token* t = &toks[i];
        printf("%d:%d\t%s", t->line, t->col, tok_disp(t->kind));
        switch (t->kind) {
        case T_IDENT:
            printf("\t%.*s", (int)t->len, t->str);
            break;
        case T_INT:
            printf("\t%llu", (unsigned long long)t->ival);
            break;
        case T_FLOAT:
            printf("\t%g", t->fval);
            break;
        case T_STR:
            printf("\t\"%.*s\"", (int)t->len, t->str);
            break;
        case T_CHARLIT:
            if (t->ival >= 33 && t->ival <= 126)
                printf("\t'%c'", (char)t->ival);
            else
                printf("\t%llu", (unsigned long long)t->ival);
            break;
        default:
            break;
        }
        putchar('\n');
    }
}

// "<path>/foo.ace" → "<path>/foo.o"; other suffixes just gain ".o".
static void obj_path(char* out, size_t cap, const char* src) {
    size_t n = strlen(src);
    size_t cut = n;
    if (n >= 4 && strcmp(src + n - 4, ".ace") == 0) cut = n - 4;
    if (cut + 2 + 1 > cap) cut = cap - 3; // truncated but NUL-safe
    memcpy(out, src, cut);
    out[cut] = 0;
    strncat(out, ".o", cap - cut - 1);
}

int main(int argc, char** argv) {
    int dump_tok = 0, dump_ast = 0, opt_c = 0, opt_link = 0;
    const char* path = NULL;
    const char* out_name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tokens") == 0) dump_tok = 1;
        else if (strcmp(argv[i], "--ast") == 0) dump_ast = 1;
        else if (strcmp(argv[i], "-c") == 0) opt_c = 1;
        else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) usage();
            out_name = argv[++i];
            opt_link = 1;
        }
        else if (argv[i][0] == '-' && argv[i][1] != 0) usage();
        else if (!path) path = argv[i];
        else usage();
    }
    if (!path) usage();

    Source s;
    if (source_load(&s, path) != 0) return 1;

    Diag d = {path, 0};
    size_t ntok;
    Token* toks = lex(&s, &d, &ntok);
    if (dump_tok) { // --tokens: lexing only
        dump_tokens(toks, ntok);
        if (d.errors > 0) goto done;
        return 0;
    }
    if (d.errors > 0) goto done;

    {
        Node* file = parse(toks, ntok, &d);
        if (d.errors > 0) goto done;
        if (dump_ast) { // --ast: through the parser only
            parse_dump(file);
            return 0;
        }
        sema_check(file, &d); // default mode: full semantic check
        if (d.errors > 0) goto done;

        if (opt_c || opt_link) { // codegen: Mach-O arm64 object
            char obj[4096];
            if (opt_link)
                snprintf(obj, sizeof obj, "%s.o", out_name);
            else
                obj_path(obj, sizeof obj, path);
            if (emit_object(file, &d, obj) != 0 || d.errors > 0) return 1;
            if (opt_link) {
                char cmd[8192];
                snprintf(cmd, sizeof cmd, "cc '%s' -o '%s'", obj, out_name);
                int rc = system(cmd);
                if (rc != 0) {
                    fprintf(stderr, "ace: link failed: %s\n", cmd);
                    return 1;
                }
            }
        }
    }

done:
    if (d.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", path, d.errors, d.errors == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
