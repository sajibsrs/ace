// main.c — compiler driver. Stages: lex → parse → sema (→ emit → link).
#include "ace.h"
#include "ir.h"

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

#define SUPPORTED_TARGETS "darwin-arm64, linux-x64"

// Select target for every stage (sema checks asm against its ISA, lowering
// and the code generator follow it), or report it unsupported.
static int target_select(const char* target) {
    if (strcmp(target, "darwin-arm64") != 0 && strcmp(target, "linux-x64") != 0) {
        fprintf(stderr, "ace: unsupported target '%s' (supported: " SUPPORTED_TARGETS ")\n", target);
        return 0;
    }
    ace_target = target;
    return 1;
}

static const char* host_target(void) {
#if defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

static void usage(void) {
    fprintf(stderr, "usage: ace [--debug | --release] [--tokens] [--ast] [--ir] [--report]\n"
                    "           [--target=TARGET] [-c] [-o NAME] <file.ace>\n"
                    "           [<file.o> | <file.a> | -L DIR | -l NAME | -framework NAME]...\n"
                    "       ace --pkg-graph=DIR\n"
                    "       ace --pkg-check=DIR\n"
                    "       ace pkg <init|build|run|check|add|remove|list> ...\n"
                    "  (default)       full semantic check, no codegen\n"
                    "  --debug         emit runtime diagnostics\n"
                    "  --release       emit without runtime diagnostic machinery\n"
                    "                  (the emission default)\n"
                    "  --ir            print each function's IR after semantic checking\n"
                    "  --report        print compilation statistics to stderr\n"
                    "  --target=TARGET compile for TARGET (supported: " SUPPORTED_TARGETS ")\n"
                    "  -c              emit the target's object <base>.o (Mach-O or ELF)\n"
                    "  -o NAME         emit + link via the system cc; NAME ending in\n"
                    "                  .ace compiles that file, naming the output after it\n"
                    "  <file>.o/.a     extra linker inputs, passed to cc in order\n"
                    "  -L DIR          linker library search path\n"
                    "  -l NAME         linker named library\n"
                    "  -framework NAME linker Apple framework\n"
                    "  --pkg-graph=DIR resolve and print the ace.pkg dependency graph at DIR\n"
                    "  --pkg-check=DIR full semantic check of the ace.pkg package at DIR,\n"
                    "                  with cross-package imports resolved, no codegen\n"
                    "  ace pkg init <exe|lib> [DIR]  create a minimal package skeleton\n"
                    "  ace pkg build [--debug|--release] [--target=TARGET] [--report]\n"
                    "                compile (and, for an exe, link) the package found by\n"
                    "                walking up from the working directory to the nearest ace.pkg\n"
                    "  ace pkg CMD --dir=DIR  any pkg command but init, walking up from DIR\n"
                    "  ace pkg run   [--debug|--release] [--target=TARGET] [--report] [-- ARGS...]\n"
                    "                build, then execute the exe (exe-only), passing it ARGS\n"
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

static char* opt_value(int* i, int argc, char** argv, const char* option, const char* what) {
    if (*i + 1 >= argc || (argv[*i + 1][0] == '-' && argv[*i + 1][1] != 0)) {
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
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void reject_output_collision(const char* output, const char* input, const char* kind) {
    if (!same_path(output, input)) return;
    fprintf(stderr, "ace: %s '%s' would overwrite input '%s'\n", kind, output, input);
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
    fprintf(stderr, "%-17s%.2f ms\n", "compile time:", (double)report->compile_ns / 1000000.0);
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
        case T_STR: // escaped as an Ace literal that lexes back to the same bytes
            fputs("\t\"", stdout);
            for (uint32_t j = 0; j < t->len; j++) {
                unsigned char c = (unsigned char)t->str[j];
                if (c == '"' || c == '\\') printf("\\%c", c);
                else if (c == '\n') fputs("\\n", stdout);
                else if (c == '\t') fputs("\\t", stdout);
                else if (c == '\r') fputs("\\r", stdout);
                else if (c == 0) fputs("\\0", stdout);
                else if (c < 0x20 || c == 0x7F) printf("\\x%02x", c);
                else putchar(c);
            }
            putchar('"');
            break;
        case T_CHARLIT:
            if (t->ival >= 33 && t->ival <= 126) printf("\t'%c'", (char)t->ival);
            else printf("\t%llu", (unsigned long long)t->ival);
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
// spec/queue.md Decision Queue, "Package system v1 (local-path)", step 3. Every
// manifest-aware operation is grouped under `ace pkg`; build/run/check find
// the nearest enclosing ace.pkg by walking up from the working directory
// (spec/modules-packages.md Package System #11) rather than taking a directory argument.

static void package_usage(void) {
    fprintf(stderr, "usage: ace pkg init <exe|lib> [DIR]\n"
                    "       ace pkg build [--debug|--release] [--target=TARGET]\n"
                    "                     [--report]\n"
                    "       ace pkg run   [--debug|--release] [--target=TARGET]\n"
                    "                     [--report] [-- ARGS...]\n"
                    "       ace pkg check\n"
                    "       ace pkg add NAME VERSION\n"
                    "       ace pkg remove NAME\n"
                    "       ace pkg list\n"
                    "  --dir=DIR  start the ace.pkg walk-up in DIR instead of the working\n"
                    "             directory (every command but init)\n"
                    "  init   create a minimal exe or lib package skeleton, without\n"
                    "         overwriting an existing ace.pkg or root file\n"
                    "  build  compile (and for an exe, link) the package found by walking\n"
                    "         up from the working directory to the nearest ace.pkg\n"
                    "  run    build, then execute the exe (exe-only)\n"
                    "  check  full semantic check of the package, no codegen\n"
                    "  add    add a lib dependency found as the nearest pkg/NAME\n"
                    "  remove remove a dependency by name\n"
                    "  list   print the resolved dependency graph\n");
    exit(2);
}

// Walk up from the working directory looking for ace.pkg, mirroring how a
// nested manifest starts an independent package (spec/modules-packages.md Package System
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

// Resolve the dependency graph at dir and fully check its root
// file (lex/parse/resolve_imports/sema_check), the manifest-free counterpart
// to plain `ace file.ace`. On success *out_package/*out_file/*out_diag are set
// for a caller (pkg build) that still needs the parsed file for codegen;
// `ace pkg check` discards them. report may be NULL.
// out_order/out_norder are optional (pass NULL/NULL to skip): when given,
// the caller receives package_resolve's dependency-first array (spec/modules-packages.md
// Package System #14's native-link-input collection order) and owns freeing
// it; otherwise it is freed here.
static int package_load_and_check(const char* dir, const char* target,
                                  CompileReport* report, Package** out_package,
                                  Node** out_file, Diag* out_diag,
                                  Package*** out_order, int* out_norder) {
    Diag pd = {dir, 0};
    Package** order = NULL;
    int norder = 0;
    Package* root_package = package_resolve(dir, target, &pd, &order, &norder);
    if (!root_package || pd.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", dir, pd.errors, pd.errors == 1 ? "" : "s");
        return 1;
    }
    if (out_order) { *out_order = order; *out_norder = norder; }
    else free(order);
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
        build_roots(root_package->dir);
        rfile = parse(toks, ntok, &rd, target);
        if (rd.errors == 0) {
            resolve_imports(rfile, &rd, target, report, root_package);
            if (rd.errors == 0) sema_check(rfile, &rd);
        }
    }
    if (rd.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", root_package->root_file, rd.errors, rd.errors == 1 ? "" : "s");
        return 1;
    }
    *out_package = root_package;
    *out_file = rfile;
    *out_diag = rd;
    return 0;
}

// Reads the manifest's current bytes so a failed add/remove can restore them
// (spec/queue.md Decision Queue, "Package system v1": "the manifest file is left
// byte-for-byte unchanged" on any validation failure).
static int package_manifest_backup(const char* manifest_path, char** out_buf, size_t* out_len) {
    Source s;
    if (source_load(&s, manifest_path) != 0) return 1;
    *out_buf = s.buf;
    *out_len = s.len;
    return 0;
}

static int package_manifest_restore(const char* manifest_path, const char* buf, size_t len) {
    FILE* f = fopen(manifest_path, "w");
    if (!f) {
        fprintf(stderr, "ace: cannot restore '%s': %s\n", manifest_path, strerror(errno));
        return 1;
    }
    if (len && fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "ace: cannot restore '%s': %s\n", manifest_path, strerror(errno));
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

// Shared by add/remove: re-resolve dir's graph after a manifest edit, and on
// any error restore the manifest to backup/backup_len and report failure.
// Frees backup either way.
static int package_revalidate_or_restore(const char* dir,
                                         const char* manifest_path, char* backup,
                                         size_t backup_len, const char* verb,
                                         const char* name) {
    Diag pd = {dir, 0};
    Package** order = NULL;
    int norder = 0;
    Package* root = package_resolve(dir, host_target(), &pd, &order, &norder);
    free(order);
    if (!root || pd.errors > 0) {
        package_manifest_restore(manifest_path, backup, backup_len);
        free(backup);
        fprintf(stderr, "ace: '%s' was not %s; the manifest was left unchanged\n", name, verb);
        return 1;
    }
    free(backup);
    return 0;
}

static int package_cmd_add(int argc, char** argv) {
    if (argc != 2) package_usage();
    const char* name = argv[0];
    const char* version = argv[1];
    if (!package_valid_name(name, strlen(name))) {
        fprintf(stderr, "ace: dependency name '%s' must be a lowercase identifier\n", name);
        return 2;
    }
    const char* dir = find_package_root();
    if (!dir) {
        fprintf(stderr, "ace: no ace.pkg found in the working directory or any parent\n");
        return 1;
    }
    char manifest_path[4160];
    snprintf(manifest_path, sizeof manifest_path, "%s/ace.pkg", dir);

    char* backup;
    size_t backup_len;
    if (package_manifest_backup(manifest_path, &backup, &backup_len) != 0) return 1;
    if (package_manifest_add_dep(manifest_path, name, version) != 0) {
        free(backup);
        return 1;
    }
    return package_revalidate_or_restore(dir, manifest_path, backup, backup_len, "added", name);
}

static int package_cmd_remove(int argc, char** argv) {
    if (argc != 1) package_usage();
    const char* name = argv[0];
    const char* dir = find_package_root();
    if (!dir) {
        fprintf(stderr, "ace: no ace.pkg found in the working directory or any parent\n");
        return 1;
    }
    char manifest_path[4160];
    snprintf(manifest_path, sizeof manifest_path, "%s/ace.pkg", dir);

    char* backup;
    size_t backup_len;
    if (package_manifest_backup(manifest_path, &backup, &backup_len) != 0) return 1;
    int found = 0;
    if (package_manifest_remove_dep(manifest_path, name, &found) != 0) {
        free(backup);
        return 1;
    }
    if (!found) {
        free(backup);
        fprintf(stderr, "ace: '%s' is not a dependency of '%s'\n", name, dir);
        return 1;
    }
    return package_revalidate_or_restore(dir, manifest_path, backup, backup_len, "removed", name);
}

static int package_cmd_list(int argc, char** argv) {
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "ace: pkg list takes no arguments\n");
        return 2;
    }
    const char* dir = find_package_root();
    if (!dir) {
        fprintf(stderr, "ace: no ace.pkg found in the working directory or any parent\n");
        return 1;
    }
    Diag pd = {dir, 0};
    Package** order = NULL;
    int norder = 0;
    Package* root = package_resolve(dir, host_target(), &pd, &order, &norder);
    if (!root || pd.errors > 0) {
        fprintf(stderr, "%s: %d error%s\n", dir, pd.errors, pd.errors == 1 ? "" : "s");
        free(order);
        return 1;
    }
    for (int i = 0; i < norder; i++) {
        Package* pkg = order[i];
        const char* relation = "transitive";
        if (pkg == root) relation = "root";
        else
            for (PackageDep* dep = root->deps; dep; dep = dep->next)
                if (dep->package == pkg) { relation = "direct"; break; }
        printf("%-16s %-4s %-10s %s\n", pkg->name, pkg->is_exe ? "exe" : "lib", relation, pkg->dir);
    }
    free(order);
    return 0;
}

static int package_cmd_check(int argc, char** argv) {
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
    if (!target_select(target)) return 2;
    Package* root_package;
    Node* file;
    Diag d;
    return package_load_and_check(dir, target, NULL, &root_package, &file, &d, NULL, NULL);
}

// Shared by `pkg build` and `pkg run`: resolve + check the enclosing
// package, then for an exe compile and link it into build/<package-name>
// (spec/modules-packages.md Package System #15). A lib only checks and ensures build/
// exists, since v1 ships libraries as source with no reusable archive.
// require_exe rejects a lib package outright (used by `run`). On success
// for an exe, out_bin receives the linked executable's path.
static int package_build_or_run(int argc, char** argv, int require_exe,
                                char* out_bin, size_t out_bin_cap) {
    int mode_seen = 0, show_report = 0;
    EmitMode emit_mode = EMIT_RELEASE;
    const char* target = host_target();
    for (int i = 0; i < argc; i++) {
        char* a = argv[i];
        if (strcmp(a, "--debug") == 0) {
            if (mode_seen && emit_mode != EMIT_DEBUG) cli_error("--debug and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_DEBUG;
        } else if (strcmp(a, "--release") == 0) {
            if (mode_seen && emit_mode != EMIT_RELEASE) cli_error("--debug and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_RELEASE;
        } else if (strcmp(a, "--report") == 0) {
            show_report = 1;
        } else if (strncmp(a, "--target=", 9) == 0) {
            if (a[9] == 0) cli_error("--target requires a target name");
            target = a + 9;
        } else {
            fprintf(stderr, "ace: unknown option '%s'\n", a);
            return 2;
        }
    }
    if (!target_select(target)) return 2;
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
    Package** order;
    int norder;
    int rc = package_load_and_check(dir, target, show_report ? &report : NULL,
                                &root_package, &file, &d, &order, &norder);
    if (rc != 0) return rc;

    if (require_exe && !root_package->is_exe) {
        fprintf(stderr, "ace: 'run' requires an exe package; '%s' is type 'lib'\n", root_package->name);
        free(order);
        return 2;
    }

    char build_dir[4096];
    snprintf(build_dir, sizeof build_dir, "%s/build", root_package->dir);
    if (mkdir_p(build_dir) != 0) {
        fprintf(stderr, "ace: cannot create '%s': %s\n", build_dir, strerror(errno));
        free(order);
        return 1;
    }
    if (!root_package->is_exe) { // lib: checked, build/ ensured, nothing to link
        free(order);
        return 0;
    }
    char obj[4096];
    if ((size_t)snprintf(obj, sizeof obj, "%s/%s.o", build_dir, root_package->name) >= sizeof obj ||
        (size_t)snprintf(out_bin, out_bin_cap, "%s/%s", build_dir, root_package->name) >= out_bin_cap) {
        fprintf(stderr, "ace: build path '%s/%s' is too long\n", build_dir, root_package->name);
        free(order);
        return 1;
    }
    if (emit_object(file, &d, obj, show_report ? &report : NULL, emit_mode) != 0 || d.errors > 0) {
        free(order);
        return 1;
    }
    uint64_t compile_done = show_report ? monotonic_ns() : 0;

    // Package System #14: the transitive union of every package's declared
    // [link] inputs, dependency-first (order already matches that from
    // package_resolve), deduplicated, conflict-checked.
    int nlinkargs;
    char** link_args = package_collect_links(order, norder, &d, &nlinkargs);
    free(order);
    if (d.errors > 0) return 1;

    ArgList cc = {0};
    arg_push(&cc, "cc");
    arg_push(&cc, obj);
    for (int i = 0; i < nlinkargs; i++) arg_push(&cc, link_args[i]);
    free(link_args);
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

static int package_cmd_build(int argc, char** argv) {
    char bin[4096];
    return package_build_or_run(argc, argv, 0, bin, sizeof bin);
}

static int package_cmd_run(int argc, char** argv) {
    // Everything after the first "--" is the program's own argv, passed through untouched.
    int nflags = 0;
    while (nflags < argc && strcmp(argv[nflags], "--") != 0) nflags++;
    int nprog = nflags < argc ? argc - nflags - 1 : 0;

    char bin[4096];
    int rc = package_build_or_run(nflags, argv, 1, bin, sizeof bin);
    if (rc != 0) return rc;

    char** args = malloc(sizeof(char*) * (size_t)(nprog + 2));
    if (!args) {
        fprintf(stderr, "ace: out of memory\n");
        return 1;
    }
    args[0] = bin;
    for (int i = 0; i < nprog; i++) args[i + 1] = argv[nflags + 1 + i];
    args[nprog + 1] = NULL;
    pid_t pid;
    int spawn_rc = posix_spawn(&pid, bin, NULL, NULL, args, environ);
    free(args);
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
    int is_exe;
    if (strcmp(argv[0], "exe") == 0) is_exe = 1;
    else if (strcmp(argv[0], "lib") == 0) is_exe = 0;
    else {
        fprintf(stderr, "ace: pkg init type must be 'exe' or 'lib'\n");
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
    while (resolved_len > 1 && resolved[resolved_len - 1] == '/') resolved[--resolved_len] = 0;
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
    const char* root_rel = is_exe ? "src/main.ace" : "src/mod.ace";
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
             "name: %s\nversion: 0.1.0\nace: 0.1\ntype: %s\nroot: %s\n"
             "description:\nauthor:\nlicense:\nrepository:\n",
             base, is_exe ? "exe" : "lib", root_rel);
    if (package_write_file(manifest_path, manifest_body, strlen(manifest_body)) != 0) return 1;

    const char* root_body = is_exe ? "main(): int {\n    return 0\n}\n" : "";
    if (package_write_file(root_path, root_body, strlen(root_body)) != 0) return 1;

    return 0;
}

static int package_main(int argc, char** argv) {
    if (argc < 1) package_usage();
    // --dir=DIR starts the nearest-ace.pkg walk-up in DIR, as a prior cd
    // would; it is removed here so no subcommand sees it, and is looked for
    // only before a "--", whose words belong to the program pkg run starts.
    const char* start_dir = NULL;
    int kept = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            while (i < argc) argv[kept++] = argv[i++];
            break;
        }
        if (strncmp(argv[i], "--dir=", 6) == 0) {
            if (start_dir) cli_error("--dir given more than once");
            if (argv[i][6] == 0) cli_error("--dir requires a directory");
            start_dir = argv[i] + 6;
        } else {
            argv[kept++] = argv[i];
        }
    }
    argc = kept;
    if (start_dir) {
        if (strcmp(argv[0], "init") == 0) cli_error("pkg init takes its directory as [DIR], not --dir");
        if (chdir(start_dir) != 0) {
            fprintf(stderr, "ace: cannot enter '%s': %s\n", start_dir, strerror(errno));
            return 1;
        }
    }
    if (strcmp(argv[0], "init") == 0) return package_cmd_init(argc - 1, argv + 1);
    if (strcmp(argv[0], "build") == 0) return package_cmd_build(argc - 1, argv + 1);
    if (strcmp(argv[0], "run") == 0) return package_cmd_run(argc - 1, argv + 1);
    if (strcmp(argv[0], "check") == 0) return package_cmd_check(argc - 1, argv + 1);
    if (strcmp(argv[0], "add") == 0) return package_cmd_add(argc - 1, argv + 1);
    if (strcmp(argv[0], "remove") == 0) return package_cmd_remove(argc - 1, argv + 1);
    if (strcmp(argv[0], "list") == 0) return package_cmd_list(argc - 1, argv + 1);
    fprintf(stderr, "ace: unknown pkg subcommand '%s'\n", argv[0]);
    package_usage();
    return 2; // unreachable
}

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "pkg") == 0) return package_main(argc - 2, argv + 2);
    int dump_tok = 0, dump_ast = 0, dump_resolved = 0, dump_ir = 0, show_report = 0, opt_c = 0, opt_link = 0;
    int mode_seen = 0;
    EmitMode emit_mode = EMIT_RELEASE;
    const char* path = NULL;
    const char* package_graph_dir = NULL;
    const char* package_check_dir = NULL;
    const char* target = host_target();
    char* out_name = NULL;
    ArgList link_args = {0}; // .o/.a inputs and -L/-l/-framework, written order
    ArgList link_files = {0}; // only positional .o/.a inputs, for collision checks
    for (int i = 1; i < argc; i++) {
        char* a = argv[i];
        if (strcmp(a, "--tokens") == 0) dump_tok = 1;
        else if (strcmp(a, "--ast") == 0) dump_ast = 1;
        else if (strcmp(a, "--resolved") == 0) dump_resolved = 1;
        else if (strcmp(a, "--ir") == 0) dump_ir = 1;
        else if (strcmp(a, "--debug") == 0) {
            if (mode_seen && emit_mode != EMIT_DEBUG) cli_error("--debug and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_DEBUG;
        }
        else if (strcmp(a, "--release") == 0) {
            if (mode_seen && emit_mode != EMIT_RELEASE) cli_error("--debug and --release are mutually exclusive");
            mode_seen = 1;
            emit_mode = EMIT_RELEASE;
        }
        else if (strcmp(a, "--report") == 0) show_report = 1;
        else if (strncmp(a, "--target=", 9) == 0) {
            if (a[9] == 0) cli_error("--target requires a target name");
            target = a + 9;
        }
        else if (strcmp(a, "--target") == 0) target = opt_value(&i, argc, argv, "--target", "a target name");
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
            char* name = opt_value(&i, argc, argv, "-framework", "a framework name");
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
        Package* root = package_resolve(package_graph_dir, target, &pd, &order, &norder);
        if (!root || pd.errors > 0) {
            fprintf(stderr, "%s: %d error%s\n", package_graph_dir, pd.errors, pd.errors == 1 ? "" : "s");
            return 1;
        }
        for (int i = 0; i < norder; i++)
            printf("%s\t%s\t%s\n", order[i]->name,
                   order[i]->is_exe ? "exe" : "lib", order[i]->root_file);
        free(order);
        return 0;
    }
    if (!target_select(target)) return 2;
    if (package_check_dir) {
        Package* root_package;
        Node* file;
        Diag d;
        return package_load_and_check(package_check_dir, target, NULL, &root_package, &file, &d, NULL, NULL);
    }
    // `-o <file>.ace` compiles that file in place: the executable is the stem.
    if (out_name && has_suffix(out_name, ".ace")) {
        size_t stem_len = strlen(out_name) - 4;
        if (stem_len == 0) cli_error("-o <file.ace> requires a name before '.ace'");
        char* stem = malloc(stem_len + 1);
        if (!stem) {
            fprintf(stderr, "ace: out of memory\n");
            exit(1);
        }
        memcpy(stem, out_name, stem_len);
        stem[stem_len] = 0;
        if (path && strcmp(path, out_name) != 0) cli_error("only one root .ace source may be compiled");
        path = out_name;
        out_name = stem;
    }
    if (!path) usage();
    if (link_args.len > 0 && opt_c) cli_error("additional link inputs cannot be used with -c");
    if (link_args.len > 0 && !out_name) cli_error("link inputs require -o");

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
        char root_dir[4096];
        path_dir(root_dir, sizeof root_dir, path);
        build_roots(root_dir);
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
        resolve_imports(file, &d, target, show_report ? &report : NULL, NULL);
        if (d.errors > 0) goto done;
        if (dump_resolved) { // --resolved: through import resolution only
            parse_dump_resolved(file);
            return 0;
        }
        sema_check(file, &d); // default mode: full semantic check
        if (d.errors > 0) goto done;
        if (dump_ir) { // --ir: through lowering only
            ir_dump_file(file, emit_mode);
            return 0;
        }

        if (opt_c || opt_link) { // codegen: the target's object
            char obj[4096];
            if (opt_link) snprintf(obj, sizeof obj, "%s.o", out_name);
            else obj_path(obj, sizeof obj, path);
            if (opt_link) {
                reject_output_collision(obj, path, "generated object");
                reject_output_collision(out_name, path, "linked output");
                for (size_t k = 0; k < link_files.len; k++) {
                    char* input = link_files.items[k];
                    reject_output_collision(obj, input, "generated object");
                    reject_output_collision(out_name, input, "linked output");
                }
            }
            if (emit_object(file, &d, obj, show_report ? &report : NULL, emit_mode) != 0 || d.errors > 0) return 1;
            uint64_t compile_done = show_report ? monotonic_ns() : 0;
            if (opt_link) {
                ArgList cc = {0};
                arg_push(&cc, "cc");
                arg_push(&cc, obj);
                for (size_t k = 0; k < link_args.len; k++) arg_push(&cc, link_args.items[k]);
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
