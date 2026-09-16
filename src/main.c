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
#include <unistd.h>

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
    fprintf(stderr, "usage: ace [--checked | --release] [--tokens] [--ast] [--report]\n"
                    "           [--target=TARGET] [--tls-backend=NAME] [-c] [-o NAME] <file.ace>\n"
                    "           [<file.o> | <file.a> | -L DIR | -l NAME | -framework NAME]...\n"
                    "       ace --pkg-graph=DIR\n"
                    "       ace --pkg-check=DIR\n"
                    "       ace pkg <init|build|run|check> ...\n"
                    "  (default)       full semantic check, no codegen\n"
                    "  --checked       emit runtime diagnostics (the emission default)\n"
                    "  --release       emit without runtime diagnostic machinery\n"
                    "  --report        print compilation statistics to stderr\n"
                    "  --target=TARGET compile for TARGET (supported: darwin-arm64)\n"
                    "  --tls-backend=NAME select TLS backend (default: bearssl)\n"
                    "  -c              emit a Mach-O arm64 object <base>.o\n"
                    "  -o NAME         emit + link via the system cc; NAME ending in\n"
                    "                  .ace compiles that file, naming the output after it\n"
                    "  <file>.o/.a     extra linker inputs, passed to cc in order\n"
                    "  -L DIR          linker library search path\n"
                    "  -l NAME         linker named library\n"
                    "  -framework NAME linker Apple framework\n"
                    "  --pkg-graph=DIR resolve and print the ace.pkg dependency graph at DIR\n"
                    "  --pkg-check=DIR full semantic check of the ace.pkg package at DIR,\n"
                    "                  with cross-package imports resolved, no codegen\n"
                    "  ace pkg init <app|lib> [DIR]  create a minimal package skeleton\n"
                    "  ace pkg build [--checked|--release] [--target=TARGET]\n"
                    "                [--tls-backend=NAME] [--report]\n"
                    "                compile (and, for an app, link) the package found by\n"
                    "                walking up from the working directory to the nearest ace.pkg\n"
                    "  ace pkg run   [--checked|--release] [--target=TARGET]\n"
                    "                [--tls-backend=NAME] [--report]\n"
                    "                build, then execute the app (app-only)\n"
                    "  ace pkg check full semantic check of the enclosing package, no codegen\n");
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

static void executable_dir(char* out, size_t cap, const char* argv0) {
    const char* slash = strrchr(argv0, '/');
    if (!slash) { snprintf(out, cap, "."); return; }
    size_t n = (size_t)(slash - argv0);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, argv0, n);
    out[n] = 0;
}

typedef enum {
    RECIPE_FILE,
    RECIPE_LIBRARY,
    RECIPE_FRAMEWORK,
} RecipeItemKind;

typedef struct {
    RecipeItemKind kind;
    const char* value;
} RecipeItem;

typedef struct {
    const char* component;
    const char* target;
    const char* backend;
    const RecipeItem* items;
    size_t item_count;
} ExternRecipe;

static const RecipeItem TLS_BEARSSL_DARWIN_ARM64[] = {
    { RECIPE_FILE, "external/darwin-arm64/tls/bearssl/adapter.o" },
    { RECIPE_FILE, "external/darwin-arm64/tls/bearssl/libbearssl.a" },
    { RECIPE_FRAMEWORK, "Security" },
    { RECIPE_FRAMEWORK, "CoreFoundation" },
};

static const ExternRecipe EXTERN_RECIPES[] = {
    { "tls", "darwin-arm64", "bearssl", TLS_BEARSSL_DARWIN_ARM64,
      sizeof TLS_BEARSSL_DARWIN_ARM64 / sizeof TLS_BEARSSL_DARWIN_ARM64[0] },
};

static const ExternRecipe* extern_recipe(const char* component,
                                         const char* target,
                                         const char* backend) {
    for (size_t i = 0; i < sizeof EXTERN_RECIPES / sizeof EXTERN_RECIPES[0]; i++)
        if (strcmp(component, EXTERN_RECIPES[i].component) == 0 &&
            strcmp(target, EXTERN_RECIPES[i].target) == 0 &&
            strcmp(backend, EXTERN_RECIPES[i].backend) == 0)
            return &EXTERN_RECIPES[i];
    return NULL;
}

static void bundled_path(char* out, size_t cap, const char* argv0,
                         const char* relative) {
    char dir[4096];
    executable_dir(dir, sizeof dir, argv0);
    snprintf(out, cap, "%s/%s", dir, relative);
}

static void require_regular_file(const char* path, const char* what) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) return;
    fprintf(stderr, "ace: bundled %s is unavailable at '%s'\n", what, path);
    exit(1);
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

// --- `ace pkg` subcommand group (Package System v1, real CLI) ---------------
// SPEC.md Decision Queue, "Package system v1 (local-path)", step 3. Every
// manifest-aware operation is grouped under `ace pkg`; build/run/check find
// the nearest enclosing ace.pkg by walking up from the working directory
// (SPEC.md Package System #11) rather than taking a directory argument.

static void package_usage(void) {
    fprintf(stderr, "usage: ace pkg init <app|lib> [DIR]\n"
                    "       ace pkg build [--checked|--release] [--target=TARGET]\n"
                    "                     [--tls-backend=NAME] [--report]\n"
                    "       ace pkg run   [--checked|--release] [--target=TARGET]\n"
                    "                     [--tls-backend=NAME] [--report]\n"
                    "       ace pkg check\n"
                    "  init   create a minimal app or lib package skeleton, without\n"
                    "         overwriting an existing ace.pkg or root file\n"
                    "  build  compile (and for an app, link) the package found by walking\n"
                    "         up from the working directory to the nearest ace.pkg\n"
                    "  run    build, then execute the app (app-only)\n"
                    "  check  full semantic check of the package, no codegen\n");
    exit(2);
}

// Walk up from the working directory looking for ace.pkg, mirroring how a
// nested manifest starts an independent package (SPEC.md Package System
// #11). Returns arena storage, or NULL if no ace.pkg is found up to '/'.
static const char* find_package_root(void) {
    char dir[4096];
    if (!getcwd(dir, sizeof dir)) return NULL;
    for (;;) {
        char manifest[4160];
        snprintf(manifest, sizeof manifest, "%s/ace.pkg", dir);
        struct stat st;
        if (stat(manifest, &st) == 0 && S_ISREG(st.st_mode)) return arena_string(dir);
        if (strcmp(dir, "/") == 0) return NULL;
        char* slash = strrchr(dir, '/');
        if (!slash) return NULL;
        if (slash == dir) dir[1] = 0; // "/child" -> "/"
        else *slash = 0;
    }
}

// mkdir -p: create every missing path component. Tolerates an already-
// existing directory; any other failure is reported by the caller via errno.
static int mkdir_p(const char* path) {
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int package_write_file(const char* path, const char* data, size_t len) {
    FILE* f = fopen(path, "wx"); // C11 exclusive mode: fails if it exists
    if (!f) {
        fprintf(stderr, "ace: cannot create '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        fprintf(stderr, "ace: cannot write '%s': %s\n", path, strerror(errno));
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

// Resolve the local-path dependency graph at dir and fully check its root
// file (lex/parse/resolve_imports/sema_check), the manifest-free counterpart
// to plain `ace file.ace`. On success *out_package/*out_file/*out_diag are set
// for a caller (pkg build) that still needs the parsed file for codegen;
// `ace pkg check` discards them. report may be NULL.
static int package_load_and_check(const char* dir, const char* argv0, const char* target,
                                  CompileReport* report, Package** out_package,
                                  Node** out_file, Diag* out_diag) {
    Diag pd = {dir, 0};
    Package** order = NULL;
    int norder = 0;
    Package* root_package = package_resolve(dir, &pd, argv0, &order, &norder);
    if (!root_package || pd.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", dir, pd.errors, pd.errors == 1 ? "" : "s");
        return 1;
    }
    free(order);
    Source s;
    if (source_load(&s, root_package->root_file) != 0) return 1;
    if (report) {
        report->source_files = 1;
        report->source_bytes = s.len;
    }
    Diag rd = {root_package->root_file, 0};
    size_t ntok;
    Token* toks = lex(&s, &rd, &ntok);
    if (report && ntok) report->tokens = ntok - 1; // exclude EOF
    Node* rfile = NULL;
    if (rd.errors == 0) {
        rfile = parse(toks, ntok, &rd, target);
        if (rd.errors == 0) {
            resolve_imports(rfile, &rd, argv0, target, report, root_package);
            if (rd.errors == 0) sema_check(rfile, &rd);
        }
    }
    if (rd.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", root_package->root_file, rd.errors,
                rd.errors == 1 ? "" : "s");
        return 1;
    }
    *out_package = root_package;
    *out_file = rfile;
    *out_diag = rd;
    return 0;
}

static int package_cmd_check(int argc, char** argv, const char* argv0) {
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "ace: pkg check takes no arguments\n");
        return 2;
    }
    const char* dir = find_package_root();
    if (!dir) {
        fprintf(stderr, "ace: no ace.pkg found in the working directory or any parent\n");
        return 1;
    }
    const char* target = host_target();
    if (strcmp(target, "darwin-arm64") != 0) {
        fprintf(stderr, "ace: unsupported target '%s' (supported: darwin-arm64)\n", target);
        return 2;
    }
    Package* root_package;
    Node* file;
    Diag d;
    return package_load_and_check(dir, argv0, target, NULL, &root_package, &file, &d);
}

// Shared by `pkg build` and `pkg run`: resolve + check the enclosing
// package, then for an app compile and link it into build/<package-name>
// (SPEC.md Package System #15). A lib only checks and ensures build/
// exists, since v1 ships libraries as source with no reusable archive.
// require_app rejects a lib package outright (used by `run`). On success
// for an app, out_bin receives the linked executable's path.
static int package_build_or_run(int argc, char** argv, const char* argv0, int require_app,
                                char* out_bin, size_t out_bin_cap) {
    int mode_seen = 0, show_report = 0;
    EmitMode emit_mode = EMIT_CHECKED;
    const char* target = host_target();
    const char* tls_backend_name = "bearssl";
    for (int i = 0; i < argc; i++) {
        char* a = argv[i];
        if (strcmp(a, "--checked") == 0) {
            if (mode_seen && emit_mode != EMIT_CHECKED)
                cli_error("--checked and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_CHECKED;
        } else if (strcmp(a, "--release") == 0) {
            if (mode_seen && emit_mode != EMIT_RELEASE)
                cli_error("--checked and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_RELEASE;
        } else if (strcmp(a, "--report") == 0) {
            show_report = 1;
        } else if (strncmp(a, "--target=", 9) == 0) {
            if (a[9] == 0) cli_error("--target requires a target name");
            target = a + 9;
        } else if (strncmp(a, "--tls-backend=", 14) == 0) {
            if (a[14] == 0) cli_error("--tls-backend requires a backend name");
            tls_backend_name = a + 14;
        } else {
            fprintf(stderr, "ace: unknown option '%s'\n", a);
            return 2;
        }
    }
    if (strcmp(target, "darwin-arm64") != 0) {
        fprintf(stderr, "ace: unsupported target '%s' (supported: darwin-arm64)\n", target);
        return 2;
    }
    const ExternRecipe* configured_tls = extern_recipe("tls", target, tls_backend_name);
    if (!configured_tls) {
        fprintf(stderr, "ace: unsupported TLS backend '%s' for target '%s'\n",
                tls_backend_name, target);
        return 2;
    }
    const char* dir = find_package_root();
    if (!dir) {
        fprintf(stderr, "ace: no ace.pkg found in the working directory or any parent\n");
        return 1;
    }

    uint64_t started = show_report ? monotonic_ns() : 0;
    CompileReport report = {0};

    Package* root_package;
    Node* file;
    Diag d;
    int rc = package_load_and_check(dir, argv0, target, show_report ? &report : NULL,
                                &root_package, &file, &d);
    if (rc != 0) return rc;

    if (require_app && !root_package->is_app) {
        fprintf(stderr, "ace: 'run' requires an app package; '%s' is kind 'lib'\n",
                root_package->name);
        return 2;
    }

    char build_dir[4096];
    snprintf(build_dir, sizeof build_dir, "%s/build", root_package->dir);
    if (mkdir_p(build_dir) != 0) {
        fprintf(stderr, "ace: cannot create '%s': %s\n", build_dir, strerror(errno));
        return 1;
    }
    if (!root_package->is_app) return 0; // lib: checked, build/ ensured, nothing to link

    uint32_t external = extern_requirements(file);
    char tls_files[8][8192];
    if (external & EXTERN_TLS) {
        for (size_t k = 0; k < configured_tls->item_count; k++) {
            const RecipeItem* item = &configured_tls->items[k];
            if (item->kind != RECIPE_FILE) continue;
            bundled_path(tls_files[k], sizeof tls_files[k], argv0, item->value);
            require_regular_file(tls_files[k], "TLS external artifact");
        }
    }

    char obj[4096];
    snprintf(obj, sizeof obj, "%s/%s.o", build_dir, root_package->name);
    if (out_bin_cap) snprintf(out_bin, out_bin_cap, "%s/%s", build_dir, root_package->name);
    if (emit_object(file, &d, obj, show_report ? &report : NULL, emit_mode) != 0 ||
        d.errors > 0)
        return 1;
    uint64_t compile_done = show_report ? monotonic_ns() : 0;

    ArgList cc = {0};
    arg_push(&cc, "cc");
    arg_push(&cc, obj);
    if (external & EXTERN_TLS) {
        for (size_t k = 0; k < configured_tls->item_count; k++) {
            const RecipeItem* item = &configured_tls->items[k];
            if (item->kind == RECIPE_FILE) {
                arg_push(&cc, tls_files[k]);
            } else if (item->kind == RECIPE_LIBRARY) {
                arg_push(&cc, "-l");
                arg_push(&cc, (char*)item->value);
            } else {
                arg_push(&cc, "-framework");
                arg_push(&cc, (char*)item->value);
            }
        }
    }
    arg_push(&cc, "-o");
    arg_push(&cc, out_bin);
    arg_push(&cc, NULL);
    int link_rc = run_linker(&cc);
    free(cc.items);
    if (link_rc != 0) return 1;

    if (show_report) {
        report.compile_ns = compile_done >= started ? compile_done - started : 0;
        print_report(&report);
    }
    return 0;
}

static int package_cmd_build(int argc, char** argv, const char* argv0) {
    char bin[4096];
    return package_build_or_run(argc, argv, argv0, 0, bin, sizeof bin);
}

static int package_cmd_run(int argc, char** argv, const char* argv0) {
    char bin[4096];
    int rc = package_build_or_run(argc, argv, argv0, 1, bin, sizeof bin);
    if (rc != 0) return rc;

    char* args[] = {bin, NULL};
    pid_t pid;
    int spawn_rc = posix_spawn(&pid, bin, NULL, NULL, args, environ);
    if (spawn_rc != 0) {
        fprintf(stderr, "ace: cannot launch '%s': %s\n", bin, strerror(spawn_rc));
        return 1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "ace: cannot wait for '%s': %s\n", bin, strerror(errno));
            return 1;
        }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    fprintf(stderr, "ace: '%s' terminated by signal %d\n", bin, WTERMSIG(status));
    return 1;
}

static int package_cmd_init(int argc, char** argv) {
    if (argc < 1 || argc > 2) package_usage();
    int is_app;
    if (strcmp(argv[0], "app") == 0) is_app = 1;
    else if (strcmp(argv[0], "lib") == 0) is_app = 0;
    else {
        fprintf(stderr, "ace: pkg init kind must be 'app' or 'lib'\n");
        return 2;
    }
    const char* dir_arg = argc == 2 ? argv[1] : ".";

    // Derive the package name from the target directory before creating
    // anything, so an invalid name leaves no directory behind.
    char resolved[4096];
    if (strcmp(dir_arg, ".") == 0) {
        if (!getcwd(resolved, sizeof resolved)) {
            fprintf(stderr, "ace: cannot resolve the working directory: %s\n", strerror(errno));
            return 1;
        }
    } else {
        snprintf(resolved, sizeof resolved, "%s", dir_arg);
    }
    size_t resolved_len = strlen(resolved);
    while (resolved_len > 1 && resolved[resolved_len - 1] == '/')
        resolved[--resolved_len] = 0;
    const char* rslash = strrchr(resolved, '/');
    const char* base = rslash ? rslash + 1 : resolved;
    if (!package_valid_name(base, strlen(base))) {
        fprintf(stderr,
                "ace: directory name '%s' is not a valid package name (must be a lowercase "
                "identifier); rename the directory or pass a different DIR\n",
                base);
        return 2;
    }

    if (mkdir_p(dir_arg) != 0) {
        fprintf(stderr, "ace: cannot create '%s': %s\n", dir_arg, strerror(errno));
        return 1;
    }
    const char* canon = canonicalize(dir_arg);
    if (!canon) {
        fprintf(stderr, "ace: cannot resolve '%s'\n", dir_arg);
        return 1;
    }

    char manifest_path[4160];
    snprintf(manifest_path, sizeof manifest_path, "%s/ace.pkg", canon);
    struct stat st;
    if (stat(manifest_path, &st) == 0) {
        fprintf(stderr, "ace: '%s' already exists\n", manifest_path);
        return 1;
    }
    const char* root_rel = is_app ? "src/main.ace" : "src/mod.ace";
    char root_path[4160];
    snprintf(root_path, sizeof root_path, "%s/%s", canon, root_rel);
    if (stat(root_path, &st) == 0) {
        fprintf(stderr, "ace: '%s' already exists\n", root_path);
        return 1;
    }

    char src_dir[4160];
    snprintf(src_dir, sizeof src_dir, "%s/src", canon);
    if (mkdir_p(src_dir) != 0) {
        fprintf(stderr, "ace: cannot create '%s': %s\n", src_dir, strerror(errno));
        return 1;
    }

    char manifest_body[512];
    snprintf(manifest_body, sizeof manifest_body,
             "name: %s\nversion: 0.1.0\nace: 0.1\nkind: %s\nroot: %s\n",
             base, is_app ? "app" : "lib", root_rel);
    if (package_write_file(manifest_path, manifest_body, strlen(manifest_body)) != 0) return 1;

    const char* root_body = is_app ? "main(): int {\n    return 0\n}\n" : "";
    if (package_write_file(root_path, root_body, strlen(root_body)) != 0) return 1;

    return 0;
}

static int package_main(int argc, char** argv, const char* argv0) {
    if (argc < 1) package_usage();
    if (strcmp(argv[0], "init") == 0) return package_cmd_init(argc - 1, argv + 1);
    if (strcmp(argv[0], "build") == 0) return package_cmd_build(argc - 1, argv + 1, argv0);
    if (strcmp(argv[0], "run") == 0) return package_cmd_run(argc - 1, argv + 1, argv0);
    if (strcmp(argv[0], "check") == 0) return package_cmd_check(argc - 1, argv + 1, argv0);
    fprintf(stderr, "ace: unknown pkg subcommand '%s'\n", argv[0]);
    package_usage();
    return 2; // unreachable
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "pkg") == 0)
        return package_main(argc - 2, argv + 2, argv[0]);
    int dump_tok = 0, dump_ast = 0, show_report = 0, opt_c = 0, opt_link = 0;
    int mode_seen = 0;
    EmitMode emit_mode = EMIT_CHECKED;
    const char* path = NULL;
    const char* package_graph_dir = NULL;
    const char* package_check_dir = NULL;
    const char* target = host_target();
    const char* tls_backend_name = "bearssl";
    char* out_name = NULL;
    ArgList link_args = {0}; // .o/.a inputs and -L/-l/-framework, written order
    ArgList link_files = {0}; // only positional .o/.a inputs, for collision checks
    for (int i = 1; i < argc; i++) {
        char* a = argv[i];
        if (strcmp(a, "--tokens") == 0) dump_tok = 1;
        else if (strcmp(a, "--ast") == 0) dump_ast = 1;
        else if (strcmp(a, "--checked") == 0) {
            if (mode_seen && emit_mode != EMIT_CHECKED)
                cli_error("--checked and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_CHECKED;
        }
        else if (strcmp(a, "--release") == 0) {
            if (mode_seen && emit_mode != EMIT_RELEASE)
                cli_error("--checked and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_RELEASE;
        }
        else if (strcmp(a, "--report") == 0) show_report = 1;
        else if (strncmp(a, "--target=", 9) == 0) {
            if (a[9] == 0) cli_error("--target requires a target name");
            target = a + 9;
        }
        else if (strcmp(a, "--target") == 0)
            target = opt_value(&i, argc, argv, "--target", "a target name");
        else if (strncmp(a, "--tls-backend=", 14) == 0) {
            if (a[14] == 0) cli_error("--tls-backend requires a backend name");
            tls_backend_name = a + 14;
        }
        else if (strcmp(a, "--tls-backend") == 0)
            tls_backend_name = opt_value(&i, argc, argv, "--tls-backend",
                                         "a backend name");
        else if (strncmp(a, "--pkg-graph=", 12) == 0) {
            if (a[12] == 0) cli_error("--pkg-graph requires a directory");
            package_graph_dir = a + 12;
        }
        else if (strcmp(a, "--pkg-graph") == 0)
            package_graph_dir = opt_value(&i, argc, argv, "--pkg-graph", "a directory");
        else if (strncmp(a, "--pkg-check=", 12) == 0) {
            if (a[12] == 0) cli_error("--pkg-check requires a directory");
            package_check_dir = a + 12;
        }
        else if (strcmp(a, "--pkg-check") == 0)
            package_check_dir = opt_value(&i, argc, argv, "--pkg-check", "a directory");
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
    if (package_graph_dir) {
        Diag pd = {package_graph_dir, 0};
        Package** order = NULL;
        int norder = 0;
        Package* root = package_resolve(package_graph_dir, &pd, argv[0], &order, &norder);
        if (!root || pd.errors > 0) {
            fprintf(stderr, "%s: %d error%s\n", package_graph_dir, pd.errors,
                    pd.errors == 1 ? "" : "s");
            return 1;
        }
        for (int i = 0; i < norder; i++)
            printf("%s\t%s\t%s\n", order[i]->name,
                   order[i]->is_app ? "app" : "lib", order[i]->root_file);
        free(order);
        return 0;
    }
    if (strcmp(target, "darwin-arm64") != 0) {
        fprintf(stderr, "ace: unsupported target '%s' (supported: darwin-arm64)\n", target);
        return 2;
    }
    if (package_check_dir) {
        Package* root_package;
        Node* file;
        Diag d;
        return package_load_and_check(package_check_dir, argv[0], target, NULL, &root_package, &file, &d);
    }
    const ExternRecipe* configured_tls =
        extern_recipe("tls", target, tls_backend_name);
    if (!configured_tls) {
        fprintf(stderr, "ace: unsupported TLS backend '%s' for target '%s'\n",
                tls_backend_name, target);
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
                        show_report ? &report : NULL, NULL);
        if (d.errors > 0) goto done;
        sema_check(file, &d); // default mode: full semantic check
        if (d.errors > 0) goto done;

        uint32_t external = (opt_c || opt_link) ? extern_requirements(file) : EXTERN_NONE;
        const ExternRecipe* selected_tls = NULL;
        char tls_files[8][8192];
        if (external & EXTERN_TLS) {
            selected_tls = configured_tls;
            if (opt_c) {
                fprintf(stderr,
                        "ace: TLS external requirements cannot be recorded by -c; use -o to link an executable\n");
                return 2;
            }
            if (opt_link) {
                for (size_t k = 0; k < selected_tls->item_count; k++) {
                    const RecipeItem* item = &selected_tls->items[k];
                    if (item->kind != RECIPE_FILE) continue;
                    bundled_path(tls_files[k], sizeof tls_files[k], argv[0],
                                 item->value);
                    require_regular_file(tls_files[k], "TLS external artifact");
                }
            }
        }

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
                if (selected_tls) {
                    for (size_t k = 0; k < selected_tls->item_count; k++) {
                        if (selected_tls->items[k].kind != RECIPE_FILE) continue;
                        reject_output_collision(obj, tls_files[k], "generated object");
                        reject_output_collision(out_name, tls_files[k], "linked output");
                    }
                }
            }
            if (emit_object(file, &d, obj, show_report ? &report : NULL,
                            emit_mode) != 0 ||
                d.errors > 0)
                return 1;
            uint64_t compile_done = show_report ? monotonic_ns() : 0;
            if (opt_link) {
                ArgList cc = {0};
                arg_push(&cc, "cc");
                arg_push(&cc, obj);
                for (size_t k = 0; k < link_args.len; k++)
                    arg_push(&cc, link_args.items[k]);
                if (selected_tls) {
                    for (size_t k = 0; k < selected_tls->item_count; k++) {
                        const RecipeItem* item = &selected_tls->items[k];
                        if (item->kind == RECIPE_FILE) {
                            arg_push(&cc, tls_files[k]);
                        } else if (item->kind == RECIPE_LIBRARY) {
                            arg_push(&cc, "-l");
                            arg_push(&cc, (char*)item->value);
                        } else {
                            arg_push(&cc, "-framework");
                            arg_push(&cc, (char*)item->value);
                        }
                    }
                }
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
