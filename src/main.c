// main.c — compiler driver. Stages: lex → parse → sema (→ emit → link).
#include "ace.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

extern char** environ;

static const char* host_target(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin-x86-64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86-64";
#else
    return "unknown";
#endif
}

static void usage(void) {
    fprintf(stderr, "usage: ace [--tokens] [--ast] [--report] [--target=TARGET] [-c] [-o NAME] <file.ace>\n"
                    "           [<file.o> | <file.a> | -L DIR | -l NAME | -framework NAME]...\n"
                    "  (default)       full semantic check, no codegen\n"
                    "  --report        print compilation statistics to stderr\n"
                    "  --target=TARGET compile for TARGET (supported: darwin-arm64)\n"
                    "  -c              emit a Mach-O arm64 object <base>.o\n"
                    "  -o NAME         emit + link via the system cc; NAME ending in\n"
                    "                  .ace compiles that file, naming the output after it\n"
                    "  <file>.o/.a     extra linker inputs, passed to cc in order\n"
                    "  -L DIR          linker library search path\n"
                    "  -l NAME         linker named library\n"
                    "  -framework NAME linker Apple framework\n");
    exit(2);
}

typedef struct {
    char** items;
    size_t len;
    size_t cap;
} ArgList;

static void arg_push(ArgList* list, char* item) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 8;
        char** items = realloc(list->items, cap * sizeof *items);
        if (!items) {
            fprintf(stderr, "ace: out of memory\n");
            exit(1);
        }
        list->items = items;
        list->cap = cap;
    }
    list->items[list->len++] = item;
}

static void cli_error(const char* message) {
    fprintf(stderr, "ace: %s\n", message);
    exit(2);
}

static char* opt_value(int* i, int argc, char** argv, const char* option,
                       const char* what) {
    if (*i + 1 >= argc ||
        (argv[*i + 1][0] == '-' && argv[*i + 1][1] != 0)) {
        fprintf(stderr, "ace: %s requires %s\n", option, what);
        exit(2);
    }
    return argv[++*i];
}

static int has_suffix(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

// Existing aliases (relative paths, symlinks, hard links) compare by identity;
// exact text also catches a target that does not exist yet.
static int same_path(const char* a, const char* b) {
    if (strcmp(a, b) == 0) return 1;
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void reject_output_collision(const char* output, const char* input,
                                    const char* kind) {
    if (!same_path(output, input)) return;
    fprintf(stderr, "ace: %s '%s' would overwrite input '%s'\n",
            kind, output, input);
    exit(2);
}

// Launch the linker as an argument vector — no shell between ace and cc.
static int run_linker(ArgList* cc) {
    pid_t pid;
    int rc = posix_spawnp(&pid, cc->items[0], NULL, NULL, cc->items, environ);
    if (rc != 0) {
        fprintf(stderr, "ace: cannot launch cc: %s\n", strerror(rc));
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "ace: cannot wait for cc: %s\n", strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            fprintf(stderr, "ace: linker exited with status %d\n", code);
            return -1;
        }
        return 0;
    }
    fprintf(stderr, "ace: linker terminated by signal %d\n", WTERMSIG(status));
    return -1;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void print_grouped(uint64_t value) {
    if (value >= 1000) {
        print_grouped(value / 1000);
        fprintf(stderr, ",%03llu", (unsigned long long)(value % 1000));
    } else {
        fprintf(stderr, "%llu", (unsigned long long)value);
    }
}

static void report_count(const char* label, uint64_t value, const char* suffix) {
    fprintf(stderr, "%-17s", label);
    print_grouped(value);
    fprintf(stderr, "%s\n", suffix);
}

static void print_report(CompileReport* report) {
    fprintf(stderr, "Ace compile report:\n--------------------\n");
    report_count("source files:", report->source_files, "");
    report_count("source bytes:", report->source_bytes, "");
    report_count("tokens:", report->tokens, "");
    report_count("machine code:", report->machine_code, " bytes");
    report_count("object file:", report->object_file, " bytes");
    fprintf(stderr, "%-17s%.2f ms\n", "compile time:",
            (double)report->compile_ns / 1000000.0);
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
    int dump_tok = 0, dump_ast = 0, show_report = 0, opt_c = 0, opt_link = 0;
    const char* path = NULL;
    const char* target = host_target();
    char* out_name = NULL;
    ArgList link_args = {0}; // .o/.a inputs and -L/-l/-framework, written order
    ArgList link_files = {0}; // only positional .o/.a inputs, for collision checks
    for (int i = 1; i < argc; i++) {
        char* a = argv[i];
        if (strcmp(a, "--tokens") == 0) dump_tok = 1;
        else if (strcmp(a, "--ast") == 0) dump_ast = 1;
        else if (strcmp(a, "--report") == 0) show_report = 1;
        else if (strncmp(a, "--target=", 9) == 0) {
            if (a[9] == 0) cli_error("--target requires a target name");
            target = a + 9;
        }
        else if (strcmp(a, "--target") == 0)
            target = opt_value(&i, argc, argv, "--target", "a target name");
        else if (strcmp(a, "-c") == 0) opt_c = 1;
        else if (strcmp(a, "-o") == 0) {
            out_name = opt_value(&i, argc, argv, "-o", "an output name");
            opt_link = 1;
        }
        else if (strcmp(a, "-L") == 0) {
            char* dir = opt_value(&i, argc, argv, "-L", "a directory");
            arg_push(&link_args, a);
            arg_push(&link_args, dir);
        }
        else if (strcmp(a, "-l") == 0) {
            char* name = opt_value(&i, argc, argv, "-l", "a library name");
            arg_push(&link_args, a);
            arg_push(&link_args, name);
        }
        else if (strcmp(a, "-framework") == 0) {
            char* name = opt_value(&i, argc, argv, "-framework",
                                   "a framework name");
            arg_push(&link_args, a);
            arg_push(&link_args, name);
        }
        else if (a[0] == '-' && a[1] != 0) {
            fprintf(stderr, "ace: unknown option '%s'\n", a);
            exit(2);
        }
        else if (has_suffix(a, ".ace")) {
            if (path) cli_error("only one root .ace source may be compiled");
            path = a;
        }
        else if (has_suffix(a, ".o") || has_suffix(a, ".a")) {
            arg_push(&link_args, a);
            arg_push(&link_files, a);
        }
        else {
            fprintf(stderr, "ace: unrecognized input '%s' (expected .ace, .o, or .a)\n", a);
            exit(2);
        }
    }
    if (strcmp(target, "darwin-arm64") != 0) {
        fprintf(stderr, "ace: unsupported target '%s' (supported: darwin-arm64)\n", target);
        return 2;
    }
    // `-o <file>.ace` compiles that file in place: the executable is the stem.
    if (out_name && has_suffix(out_name, ".ace")) {
        size_t stem_len = strlen(out_name) - 4;
        if (stem_len == 0)
            cli_error("-o <file.ace> requires a name before '.ace'");
        char* stem = malloc(stem_len + 1);
        if (!stem) {
            fprintf(stderr, "ace: out of memory\n");
            exit(1);
        }
        memcpy(stem, out_name, stem_len);
        stem[stem_len] = 0;
        if (path && strcmp(path, out_name) != 0)
            cli_error("only one root .ace source may be compiled");
        path = out_name;
        out_name = stem;
    }
    if (!path) usage();
    if (link_args.len > 0 && opt_c)
        cli_error("additional link inputs cannot be used with -c");
    if (link_args.len > 0 && !out_name)
        cli_error("link inputs require -o");

    uint64_t started = show_report ? monotonic_ns() : 0;
    CompileReport report = {0};

    Source s;
    if (source_load(&s, path) != 0) return 1;
    if (show_report) {
        report.source_files = 1;
        report.source_bytes = s.len;
    }

    Diag d = {path, 0};
    size_t ntok;
    Token* toks = lex(&s, &d, &ntok);
    if (show_report && ntok) report.tokens = ntok - 1; // exclude EOF
    if (dump_tok) { // --tokens: lexing only
        dump_tokens(toks, ntok);
        if (d.errors > 0) goto done;
        if (show_report) {
            uint64_t ended = monotonic_ns();
            report.compile_ns = ended >= started ? ended - started : 0;
            print_report(&report);
        }
        return 0;
    }
    if (d.errors > 0) goto done;

    {
        Node* file = parse(toks, ntok, &d, target);
        if (d.errors > 0) goto done;
        if (dump_ast) { // --ast: through the parser only
            parse_dump(file);
            if (show_report) {
                uint64_t ended = monotonic_ns();
                report.compile_ns = ended >= started ? ended - started : 0;
                print_report(&report);
            }
            return 0;
        }
        resolve_imports(file, &d, argv[0], target,
                        show_report ? &report : NULL);
        if (d.errors > 0) goto done;
        sema_check(file, &d); // default mode: full semantic check
        if (d.errors > 0) goto done;

        if (opt_c || opt_link) { // codegen: Mach-O arm64 object
            char obj[4096];
            if (opt_link)
                snprintf(obj, sizeof obj, "%s.o", out_name);
            else
                obj_path(obj, sizeof obj, path);
            if (opt_link) {
                reject_output_collision(obj, path, "generated object");
                reject_output_collision(out_name, path, "linked output");
                for (size_t k = 0; k < link_files.len; k++) {
                    char* input = link_files.items[k];
                    reject_output_collision(obj, input, "generated object");
                    reject_output_collision(out_name, input, "linked output");
                }
            }
            if (emit_object(file, &d, obj, show_report ? &report : NULL) != 0 ||
                d.errors > 0)
                return 1;
            uint64_t compile_done = show_report ? monotonic_ns() : 0;
            if (opt_link) {
                ArgList cc = {0};
                arg_push(&cc, "cc");
                arg_push(&cc, obj);
                for (size_t k = 0; k < link_args.len; k++)
                    arg_push(&cc, link_args.items[k]);
                arg_push(&cc, "-o");
                arg_push(&cc, out_name);
                arg_push(&cc, NULL);
                int rc = run_linker(&cc);
                free(cc.items);
                if (rc != 0) return 1;
            }
            if (show_report) {
                report.compile_ns = compile_done >= started ? compile_done - started : 0;
                print_report(&report);
            }
        } else if (show_report) {
            uint64_t ended = monotonic_ns();
            report.compile_ns = ended >= started ? ended - started : 0;
            print_report(&report);
        }
    }

done:
    if (d.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", path, d.errors, d.errors == 1 ? "" : "s");
        return 1;
    }
    return 0;
}
