// Modal v2 driver: scan -> parse -> declare -> link -> check -> generate.
//
//   modal-v2 [options] file.modal
#define _XOPEN_SOURCE 700
#include "v2/compiler/backend/backend.h"
#include "v2/compiler/cx.h"
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int usage(const char *argv0)
{
    fprintf(stderr,
            "modal v2 %s\n"
            "usage: %s [options] file.modal\n"
            "  check only (default): report errors, or one line saying what was verified\n"
            "  -o FILE          generate code into FILE\n"
            "  --run            generate, build with the backend's toolchain, and run\n"
            "  --backend NAME   backend to generate with (default: %s); also -b NAME\n"
            "  --backends       list the registered backends\n"
            "  --target NAME    machine and system to build for (native backend): arm64-macos,\n"
            "                   arm64-linux, x86_64-macos, x86_64-linux, and bare metal\n"
            "                   arm64-none, x86_64-none (default: this Mac)\n"
            "  --ast            print the syntax tree of every loaded file\n"
            "  --syms           print the symbol table with visibility\n"
            "  --sig            print the derived contract of every domain\n"
            "  --stats          print token, node, symbol and memory counts\n"
            "  --time           print the time spent in each phase\n"
            "  --eager          parse every body up front and keep the whole tree\n"
            "  --repeat N       compile N times in one process; --time shows the fastest run\n"
            "  --mir            print the MIR of every function generated (machine-code backends)\n"
            "  --cache          skip checking files that did not change (in .modal-cache, or\n"
            "                   $MODAL_CACHE); check-only runs\n",
            MODAL_VERSION, argv0, backend_default()->name);
    return 1;
}

extern char **environ;

// Starts a program directly (no /bin/sh) and waits for it. Returns its exit status, 1 if a
// signal stopped it, -1 if it could not be started.
static int spawn_wait(char *const argv[])
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) != 0)
    {
        return -1;
    }
    int status;
    while (waitpid(pid, &status, 0) < 0)
    {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

// The toolchain command as it would be split by the shell (words separated by blanks; `$CC`
// may carry flags), then `-o OUT IN`.
static int toolchain_build(const char *toolchain, const char *out, const char *in)
{
    char words[512];
    char *argv[40];
    int n = 0;
    snprintf(words, sizeof(words), "%s", toolchain);
    for (char *w = strtok(words, " \t"); w && n < 36; w = strtok(NULL, " \t"))
    {
        argv[n++] = w;
    }
    argv[n++] = "-o";
    argv[n++] = (char *)out;
    argv[n++] = (char *)in;
    argv[n] = NULL;
    return spawn_wait(argv);
}

static char *file_read(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *p = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (p && fread(p, 1, (size_t)n, f) != (size_t)n)
    {
        free(p);
        p = NULL;
    }
    fclose(f);
    *len = p ? (size_t)n : 0;
    return p;
}

static int file_write(const char *path, const char *p, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        return 0;
    }
    int ok = fwrite(p, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

// --run with a backend that writes source for a toolchain: build the generated file and run
// the result. Built executables are kept by content ($TMPDIR/modal-run/<hash>, beside a copy of
// the source they were built from): an unchanged program is neither rebuilt nor a new file
// the system has to validate again (docs/p.md §2.2). A hit is exact: the kept source must be
// byte-identical to this one, so a hash collision only costs a rebuild.
// Returns the exit status of the program, or 1 if it could not be built.
static int build_and_run(const Backend *backend, const char *source, const char *tmp)
{
    const char *toolchain = getenv("CC") ? getenv("CC") : backend->toolchain;
    size_t len;
    char *text = file_read(source, &len);
    if (!text)
    {
        fprintf(stderr, "fatal: cannot read back '%s'\n", source);
        return 1;
    }
    uint64_t h = 0xcbf29ce484222325ull; // FNV-1a over the toolchain and the source
    for (const char *p = toolchain; *p; p++)
    {
        h = (h ^ (uint8_t)*p) * 0x100000001b3ull;
    }
    h = (h ^ 0) * 0x100000001b3ull;
    for (size_t i = 0; i < len; i++)
    {
        h = (h ^ (uint8_t)text[i]) * 0x100000001b3ull;
    }
    char dir[600], exe[700], kept[710], building[720];
    snprintf(dir, sizeof(dir), "%s/modal-run", tmp);
    mkdir(dir, 0700);
    snprintf(exe, sizeof(exe), "%s/%016llx", dir, (unsigned long long)h);
    snprintf(kept, sizeof(kept), "%s.c", exe);
    snprintf(building, sizeof(building), "%s.%ld", exe, (long)getpid());

    size_t kept_len;
    char *was = file_read(kept, &kept_len);
    int hit = was && kept_len == len && memcmp(was, text, len) == 0 && access(exe, X_OK) == 0;
    free(was);
    if (!hit)
    {
        // Built under a name of its own, then renamed: a concurrent run never sees half a file.
        int built = toolchain_build(toolchain, building, source);
        if (built != 0)
        {
            remove(building);
            free(text);
            fprintf(stderr, "fatal: '%s' failed; the generated file was kept at %s\n", toolchain,
                    source);
            return 1;
        }
        if (!file_write(kept, text, len) || rename(building, exe) != 0)
        {
            remove(building);
            remove(kept);
            free(text);
            fprintf(stderr, "fatal: cannot keep the built program in %s\n", dir);
            return 1;
        }
    }
    free(text);
    remove(source);

    char *argv[] = {exe, NULL};
    int status = spawn_wait(argv);
    if (status < 0)
    {
        fprintf(stderr, "fatal: cannot start '%s'\n", exe);
        return 1;
    }
    return status;
}

int main(int argc, char **argv)
{
    const char *entry = NULL;
    int show_ast = 0, show_syms = 0, show_stats = 0, show_sig = 0;
    int run = 0, show_time = 0, eager = 0, repeat = 1, use_cache = 0;
    char gen_path[512] = "", exe_path[512] = "";
    FILE *gen = NULL;
    const char *out_path = NULL;
    const Backend *backend = backend_default();
    int backend_given = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ast") == 0)
        {
            show_ast = 1;
        }
        else if (strcmp(argv[i], "--syms") == 0)
        {
            show_syms = 1;
        }
        else if (strcmp(argv[i], "--backends") == 0)
        {
            backend_list(stdout);
            return 0;
        }
        else if ((strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "-b") == 0) && i + 1 < argc)
        {
            backend = backend_find(argv[++i]);
            backend_given = 1;
            if (!backend)
            {
                fprintf(stderr, "%s: unknown backend '%s'. Registered backends:\n", argv[0],
                        argv[i]);
                backend_list(stderr);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc)
        {
            const MachineTarget *t = target_find(argv[++i]);
            if (!t)
            {
                fprintf(stderr, "%s: unknown target '%s'. Known targets:", argv[0], argv[i]);
                for (int k = 0; targets[k]; k++)
                {
                    fprintf(stderr, " %s", targets[k]->name);
                }
                fprintf(stderr, "\n");
                return 2;
            }
            target_set(t);
            cx.target_given = 1;
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            out_path = argv[++i];
        }
        else if (strcmp(argv[i], "--eager") == 0)
        {
            eager = 1;
        }
        else if (strcmp(argv[i], "--time") == 0)
        {
            show_time = 1;
        }
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
        {
            repeat = atoi(argv[++i]);
            if (repeat < 1)
            {
                return usage(argv[0]);
            }
        }
        else if (strcmp(argv[i], "--mir") == 0)
        {
            cx.show_mir = 1;
        }
        else if (strcmp(argv[i], "--cache") == 0)
        {
            use_cache = 1;
        }
        else if (strcmp(argv[i], "--run") == 0)
        {
            run = 1;
        }
        else if (strcmp(argv[i], "--sig") == 0)
        {
            show_sig = 1;
            cx.kinds = 1;
        }
        else if (strcmp(argv[i], "--stats") == 0)
        {
            show_stats = 1;
            cx.stats = 1;
        }
        else if (argv[i][0] == '-' || entry)
        {
            return usage(argv[0]);
        }
        else
        {
            entry = argv[i];
        }
    }
    if (!entry)
    {
        fprintf(stderr, "%s: fatal: no input files\n", argv[0]);
        return usage(argv[0]);
    }
    // A target is built by the native backend for its CPU; a native backend without a target
    // builds for this system.
    if (cx.target_given)
    {
        const char *cpu = cx.target->cpu == TCPU_X86_64 ? "x86_64" : "arm64";
        if (!backend_given || strcmp(backend->name, "native") == 0)
        {
            backend = backend_find(cpu);
        }
        else if (strcmp(backend->name, cpu) != 0)
        {
            fprintf(stderr, "%s: fatal: target '%s' is built by the native backend, not '%s'\n",
                    argv[0], cx.target->name, backend->name);
            return 2;
        }
    }
    else if (backend->executable)
    {
        target_set(target_find(strcmp(backend->name, "x86_64") == 0 ? "x86_64-macos"
                               : strcmp(backend->name, "arm64") == 0 ? "arm64-macos"
                                                                      : target_get()->name));
    }
    if (run && target_get()->os != TOS_MACOS)
    {
        fprintf(stderr, "%s: fatal: a %s executable cannot run on this machine; write it with -o\n",
                argv[0], target_get()->name);
        return 2;
    }

    // Diagnostics are buffered: unbuffered stderr makes every fprintf/fputc of a report its
    // own write(2), which cost more than parsing the file. Flushed before anything else is
    // printed, before a child process runs, and by exit().
    static char diag_buffer[1 << 16];
    setvbuf(stderr, diag_buffer, _IOFBF, sizeof(diag_buffer));

    symbol_new(&cx.units[0], SYM_NONE, 0, 0, 0, 0); // slot 0 = "no symbol"
    types_init();

    // Printing the tree needs all of it, so --ast implies --eager.
    cx.eager = (uint8_t)(eager || show_ast);
    cx.timing = (uint8_t)show_time;

    // Code generation is streamed while the program is checked, so the output is opened
    // before the first file is read. `--run` with a backend that can run the program in this
    // process writes no file at all.
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    int in_memory = run && !out_path && backend->can_run && backend->can_run();
    if (in_memory)
    {
        codegen_start(backend, NULL, NULL);
    }
    else if (out_path || run)
    {
        if (run && !out_path)
        {
            if (!backend->toolchain && !backend->executable)
            {
                fprintf(stderr, "fatal: backend '%s' has no toolchain to build with\n",
                        backend->name);
                return 1;
            }
            snprintf(exe_path, sizeof(exe_path), "%s/modal-run-%ld", tmp, (long)getpid());
            snprintf(gen_path, sizeof(gen_path), "%s%s", exe_path,
                     backend->executable ? "" : backend->extension);
        }
        else if (backend->object)
        {
            // An object file is an intermediate: it goes next to the executable and is linked
            // into it at the end.
            snprintf(gen_path, sizeof(gen_path), "%s%s", out_path, backend->extension);
        }
        else
        {
            snprintf(gen_path, sizeof(gen_path), "%s", out_path);
        }
        gen = fopen(gen_path, "wb");
        if (!gen)
        {
            fprintf(stderr, "fatal: cannot write '%s'\n", gen_path);
            return 1;
        }
        codegen_start(backend, gen, gen_path);
    }

    // --repeat: earlier runs only measure. Each starts from a clean context but keeps the
    // regions' address space, so it shows the compiler's steady-state speed, not page faults.
    if (repeat > 1 && (out_path || run))
    {
        fprintf(stderr, "%s: --repeat only measures checking; it cannot be used with -o or --run\n",
                argv[0]);
        return 1;
    }
    uint64_t best[PHASE__COUNT] = {0}, best_total = UINT64_MAX;
    for (int r = 1; r < repeat; r++)
    {
        if (unit_load(entry))
        {
            modules_link();
        }
        uint64_t total = 0;
        for (int i = 0; i < PHASE__COUNT; i++)
        {
            total += cx.phase_ns[i];
        }
        if (total < best_total)
        {
            best_total = total;
            memcpy(best, cx.phase_ns, sizeof(best));
        }
        context_reset();
        cx.eager = (uint8_t)(eager || show_ast);
        cx.timing = (uint8_t)show_time;
        symbol_new(&cx.units[0], SYM_NONE, 0, 0, 0, 0);
        types_init();
    }

    // Generating code needs every body checked in this run, so the cache is for checking.
    cx.cache = (uint8_t)(use_cache && !out_path && !run);
    cache_open(entry);

    if (!unit_load(entry))
    {
        fprintf(stderr, "%s: fatal: cannot read '%s'\n", argv[0], entry);
        if (gen)
        {
            fclose(gen);
            remove(gen_path);
        }
        arena_drop();
        return 1;
    }
    modules_link();

    if (in_memory)
    {
        codegen_finish();
    }
    else if (gen)
    {
        codegen_finish();
        uint64_t t = clock_ns();
        fclose(gen);
        phase_end(PHASE_EMIT, t);
        if (cx.errors)
        {
            remove(gen_path); // never leave a half-translated file behind
        }
    }
    fflush(stderr); // the reports come before any listing or summary on stdout

    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *u = &cx.units[i];
        if (show_ast)
        {
            printf("== ast: %s\n", u->path);
            ast.print(u);
            putchar('\n');
        }
        if (show_syms)
        {
            printf("== symbols: %s\n", u->path);
            symbols_print(u);
            putchar('\n');
        }
    }

    if (show_sig)
    {
        contract_print();
    }

    if (show_stats)
    {
        size_t source = 0, tokens = 0, nodes = 0;
        for (uint16_t i = 0; i < cx.nunits; i++)
        {
            source += cx.units[i].len;
            tokens += cx.units[i].tokens.size;
            nodes += cx.units[i].nodes.size - 1;
        }
        printf("files %u  source %zu B  tokens %zu  nodes kept %zu  symbols %zu\n", cx.nunits,
               source, tokens, nodes, cx.syms.size - 1);
        printf("memory: %.1f KB in use, %.1f KB taken from the system at peak\n",
               (double)regions_used() / 1024.0, (double)regions_touched() / 1024.0);
        regions_print(stdout);
    }

    int status = cx.errors > 0;

    if (!status && in_memory)
    {
        fflush(stdout);
        fflush(stderr);
        status = codegen_run(); // the program's output and status
        if (status < 0)
        {
            fprintf(stderr, "fatal: the program could not be loaded into memory to run; write it "
                            "with -o and start it\n");
            status = 1;
        }
    }
    else if (!status && run && !out_path)
    {
        if (backend->executable)
        {
            // The backend wrote the program itself: it only has to be started.
            chmod(gen_path, 0755);
            char *argv[] = {gen_path, NULL};
            status = spawn_wait(argv);
            remove(gen_path);
            if (status < 0)
            {
                fprintf(stderr, "fatal: cannot start '%s'\n", gen_path);
                status = 1;
            }
        }
        else
        {
            status = build_and_run(backend, gen_path, tmp); // the program's output and status
        }
    }
    else if (!status && out_path && backend->object)
    {
        const char *toolchain = getenv("CC") ? getenv("CC") : backend->toolchain;
        status = toolchain_build(toolchain, out_path, gen_path) != 0;
        remove(gen_path);
        if (status)
        {
            fprintf(stderr, "fatal: '%s' could not link %s\n", toolchain, out_path);
        }
        else
        {
            printf("ok: %s -> %s (backend %s)\n", entry, out_path, backend->name);
        }
    }
    else if (!status && out_path)
    {
        size_t n = strlen(out_path);
        if (backend->executable && !(n > 2 && strcmp(out_path + n - 2, ".o") == 0))
        {
            chmod(out_path, 0755);
        }
        printf("ok: %s -> %s (backend %s)\n", entry, out_path, backend->name);
    }
    else if (!status)
    {
        // Say what was verified: silence looks the same as not having run at all.
        printf("ok: %s: %u file%s, %zu declarations, %u invocations and %u names resolved\n", entry,
               cx.nunits, cx.nunits == 1 ? "" : "s", cx.syms.size - 1, cx.calls_resolved,
               cx.names_resolved);
    }

    if (show_time && repeat > 1)
    {
        uint64_t total = 0;
        for (int i = 0; i < PHASE__COUNT; i++)
        {
            total += cx.phase_ns[i];
        }
        if (best_total < total)
        {
            memcpy(cx.phase_ns, best, sizeof(best));
        }
        printf("fastest of %d runs:\n", repeat);
    }
    if (show_time)
    {
        static const char *const names[PHASE__COUNT] = {"scan", "parse", "declare",
                                                        "link", "check", "emit"};
        uint64_t total = 0;
        size_t source = 0;
        for (uint16_t i = 0; i < cx.nunits; i++)
        {
            source += cx.units[i].len;
        }
        for (int i = 0; i < PHASE__COUNT; i++)
        {
            total += cx.phase_ns[i];
        }
        for (int i = 0; i < PHASE__COUNT; i++)
        {
            printf("%-8s %8.3f ms  %5.1f%%\n", names[i], (double)cx.phase_ns[i] / 1e6,
                   total ? 100.0 * (double)cx.phase_ns[i] / (double)total : 0.0);
        }
        printf("%-8s %8.3f ms  %.1f MB/s of source\n", "total", (double)total / 1e6,
               total ? (double)source / 1e6 / ((double)total / 1e9) : 0.0);
    }

    if (cx.errors > 0)
    {
        if (cx.errors > MAX_REPORTED)
        {
            fprintf(stderr, "\n%d errors (the first %d are shown)\n", cx.errors, MAX_REPORTED);
        }
        else
        {
            fprintf(stderr, "\n%d error%s\n", cx.errors, cx.errors == 1 ? "" : "s");
        }
    }
    cache_save();
    arena_drop();
    return status;
}
