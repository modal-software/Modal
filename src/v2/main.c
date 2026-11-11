// Modal v2 driver: scan -> parse -> declare -> link -> check -> generate.
//
//   modal-v2 [options] file.modal
#define _XOPEN_SOURCE 700
#include "v2/compiler/backend/backend.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
            "  --backend NAME   backend to generate with (default: %s)\n"
            "  --backends       list the registered backends\n"
            "  --ast            print the syntax tree of every loaded file\n"
            "  --syms           print the symbol table with visibility\n"
            "  --sig            print the derived contract of every domain\n"
            "  --stats          print token, node, symbol and memory counts\n"
            "  --time           print the time spent in each phase\n"
            "  --eager          parse every body up front and keep the whole tree\n"
            "  --repeat N       compile N times in one process; --time shows the fastest run\n"
            "  --cache          skip checking files that did not change (in .modal-cache, or\n"
            "                   $MODAL_CACHE); check-only runs\n",
            MODAL_VERSION, argv0, backend_default()->name);
    return 1;
}

// --run: build the generated file with the backend's toolchain and run the result.
// Returns the exit status of the program, or 1 if it could not be built.
static int build_and_run(const Backend *backend, const char *source, const char *exe)
{
    const char *toolchain = getenv("CC") ? getenv("CC") : backend->toolchain;
    char cmd[1600];

    snprintf(cmd, sizeof(cmd), "%s -o '%s' '%s'", toolchain, exe, source);
    fflush(stdout);
    fflush(stderr);
    int status = system(cmd);
    if (status != 0)
    {
        fprintf(stderr, "fatal: '%s' failed; the generated file was kept at %s\n", toolchain,
                source);
        return 1;
    }
    remove(source);

    fflush(stdout);
    fflush(stderr);
    snprintf(cmd, sizeof(cmd), "'%s'", exe);
    status = system(cmd);
    remove(exe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
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
            if (!backend)
            {
                fprintf(stderr, "%s: unknown backend '%s'. Registered backends:\n", argv[0],
                        argv[i]);
                backend_list(stderr);
                return 2;
            }
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
        }
        else if (strcmp(argv[i], "--stats") == 0)
        {
            show_stats = 1;
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
    // before the first file is read.
    if (out_path || run)
    {
        if (run && !out_path)
        {
            const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
            if (!backend->toolchain)
            {
                fprintf(stderr, "fatal: backend '%s' has no toolchain to build with\n",
                        backend->name);
                return 1;
            }
            snprintf(exe_path, sizeof(exe_path), "%s/modal-run-%ld", tmp, (long)getpid());
            snprintf(gen_path, sizeof(gen_path), "%s%s", exe_path, backend->extension);
        }
        else
        {
            snprintf(gen_path, sizeof(gen_path), "%s", out_path);
        }
        gen = fopen(gen_path, "w");
        if (!gen)
        {
            fprintf(stderr, "fatal: cannot write '%s'\n", gen_path);
            return 1;
        }
        codegen_start(backend, gen);
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

    if (gen)
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
    }

    int status = cx.errors > 0;

    if (!status && run && !out_path)
    {
        status = build_and_run(backend, gen_path, exe_path); // the program's output and status
    }
    else if (!status && out_path)
    {
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
