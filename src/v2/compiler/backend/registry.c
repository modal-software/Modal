#include "v2/compiler/backend/backend.h"
#include "v2/compiler/mir/mir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// name -> Init. Several names may share one Init (Go maps both "mips" and "mipsle" to
// mips.Init); the Init can read the name it was selected by and adjust what it fills in.
static const struct
{
    const char *name;
    void (*init)(Backend *b);
} backend_inits[] = {
    {"c", backend_c_init},
    {"native", backend_native_init},
    {"arm64", backend_native_init},
    {"x86_64", backend_native_init},
};

#define BACKEND_COUNT (sizeof(backend_inits) / sizeof(backend_inits[0]))

// Backends are initialised on first use and kept: there are only a handful.
static Backend backends[BACKEND_COUNT];

static const Backend *backend_at(size_t i)
{
    if (!backends[i].name)
    {
        backends[i].name = backend_inits[i].name;
        backend_inits[i].init(&backends[i]);
    }
    return &backends[i];
}

const Backend *backend_find(const char *name)
{
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
        if (strcmp(backend_inits[i].name, name) == 0)
        {
            return backend_at(i);
        }
    }
    return NULL;
}

const Backend *backend_default(void)
{
    return backend_at(0);
}

void backend_list(FILE *out)
{
    for (size_t i = 0; i < BACKEND_COUNT; i++)
    {
        const Backend *b = backend_at(i);
        fprintf(out, "  %-8s %s%s\n", b->name, b->summary, i == 0 ? "  (default)" : "");
    }
}

// ---- streaming -------------------------------------------------------------------------------

static struct
{
    const Backend *backend;
    BackendJob job;
    int begun;
} codegen;

void codegen_start(const struct Backend *backend, void *out_file, const char *path)
{
    codegen.backend = backend;
    codegen.job.out = (FILE *)out_file;
    codegen.job.path = path;
    codegen.begun = 0;
}

// Code is generated for the entry file, which is always the first unit.
void codegen_begin(Unit *u)
{
    if (codegen.backend && u->id == 0)
    {
        uint64_t t = cx.timing ? clock_ns() : 0;
        codegen.job.entry = u;
        codegen.backend->begin(&codegen.job);
        codegen.begun = 1;
        if (cx.timing)
        {
            phase_end(PHASE_EMIT, t);
        }
    }
}

// Functions the backend could not get, and why: reported only if the program calls them.
static CodegenFailure *failures;
static size_t failures_cap;

const CodegenFailure *codegen_failure(SymId fn)
{
    return fn < failures_cap && failures[fn].what ? &failures[fn] : NULL;
}

void codegen_failure_clear(SymId fn) // reported once
{
    if (fn < failures_cap)
    {
        failures[fn].what = NULL;
    }
}

static void note_trap_codes(const MirFunc *f);

static void emit_mir(MirFunc *f)
{
    if (cx.show_mir)
    {
        mir_print(f, stdout);
    }
    audit_function(f);
    note_trap_codes(f);
    codegen.backend->function_mir(&codegen.job, f);
}

// Every backend gets the functions of every file: the program is linked as a whole (machine
// code drops what is unreachable; the C compiler, unused static functions).
void codegen_function(Unit *u, SymId fn)
{
    if (!codegen.begun)
    {
        return;
    }
    uint64_t t = cx.timing ? clock_ns() : 0;
    if (codegen.backend->function_mir)
    {
        // After an error nothing is written: lowering more would only add follow-on reports.
        MirFunc f;
        if (!cx.errors && mir_lower(u, fn, &f))
        {
            if (!mir_clause_hold(&f)) // a clause that goes on to the next: joined with it below
            {
                emit_mir(&f);
            }
            MirFunc joined;
            for (const MirFunc *g = &f; mir_clause_merge(g, &joined); g = &joined)
            {
                emit_mir(&joined);
            }
        }
        else if (!cx.errors && f.fail_what)
        {
            if (failures_cap < cx.syms.cap)
            {
                size_t cap = cx.syms.cap;
                void *grown = arena_alloc(cap * sizeof(*failures));
                memset(grown, 0, cap * sizeof(*failures));
                if (failures_cap)
                {
                    memcpy(grown, failures, failures_cap * sizeof(*failures));
                }
                failures = grown;
                failures_cap = cap;
            }
            int len;
            const char *name = symbol_name(fn, &len);
            failures[fn] = (CodegenFailure){u, *TOK(u, f.fail_tok), f.fail_what, name, len};
        }
    }
    else
    {
        codegen.backend->function(&codegen.job, fn);
    }
    if (cx.timing)
    {
        phase_end(PHASE_EMIT, t);
    }
}

int codegen_finish(void)
{
    if (!codegen.begun)
    {
        return 0; // the entry file did not get as far as being checked: nothing was emitted
    }
    uint64_t t = clock_ns();
    if (codegen.backend->function_mir && !cx.errors) // the routines the compiler made
    {
        MirFunc f;
        while (mir_clause_release(&f)) // clauses whose next one never came: as they are
        {
            emit_mir(&f);
        }
        while (mir_next_helper(&f))
        {
            if (cx.show_mir)
            {
                mir_print(&f, stdout);
            }
            codegen.backend->function_mir(&codegen.job, &f);
        }
    }
    int errors = codegen.backend->end(&codegen.job);
    phase_end(PHASE_EMIT, t);
    codegen.begun = 0;
    return errors;
}

int codegen_run(void)
{
    return codegen.backend->run ? codegen.backend->run(&codegen.job) : -1;
}

// ---- trap codes ------------------------------------------------------------------------------
//
// A binary written with -o carries no source: the place a guard reports (and an assert's call as
// written) is `trap N`, and <out>.traps maps each N to its text. `--run` keeps the text: nothing
// leaves the machine. The compiler keeps the table, so a guard run at compile time still reports
// its place (sema/comptime.c).
static char **trap_texts;
static uint8_t *trap_used; // in the code generated (not only run by the compiler)
static uint32_t ntraps, ctraps;

// The codes a generated function carries: those go in the map.
static void note_trap_codes(const MirFunc *f)
{
    for (size_t i = 0; cx.trap_codes && i < f->nstrings; i++)
    {
        unsigned n = 0;
        if (f->strings[i].len > 5 && memcmp(f->strings[i].bytes, "trap ", 5) == 0 &&
            sscanf((const char *)f->strings[i].bytes + 5, "%u", &n) == 1 && n >= 1 && n <= ntraps)
        {
            trap_used[n - 1] = 1;
        }
    }
}

size_t trap_place(const char *text, size_t len, char *out, size_t cap)
{
    if (!cap)
    {
        return 0;
    }
    if (!cx.trap_codes)
    {
        size_t n = len < cap - 1 ? len : cap - 1;
        memcpy(out, text, n);
        out[n] = 0;
        return n;
    }
    for (uint32_t i = 0; i < ntraps; i++) // the same place twice (a function lowered again): one code
    {
        if (strlen(trap_texts[i]) == len && memcmp(trap_texts[i], text, len) == 0)
        {
            int k = snprintf(out, cap, "trap %u", i + 1);
            return k < 0 ? 0 : (size_t)k >= cap ? cap - 1 : (size_t)k;
        }
    }
    if (ntraps == ctraps)
    {
        ctraps = ctraps ? ctraps * 2 : 32;
        trap_texts = realloc(trap_texts, ctraps * sizeof(*trap_texts));
        trap_used = realloc(trap_used, ctraps);
    }
    trap_used[ntraps] = 0;
    char *copy = malloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = 0;
    trap_texts[ntraps++] = copy;
    int k = snprintf(out, cap, "trap %u", ntraps);
    return k < 0 ? 0 : (size_t)k >= cap ? cap - 1 : (size_t)k;
}

const char *trap_code_text(uint32_t n)
{
    return n >= 1 && n <= ntraps ? trap_texts[n - 1] : NULL;
}

int trap_codes_write(const char *path)
{
    uint32_t used = 0;
    for (uint32_t i = 0; i < ntraps; i++)
    {
        used += trap_used[i];
    }
    if (!used)
    {
        remove(path); // an old map of a binary that has no guards now
        return 0;
    }
    FILE *f = fopen(path, "w");
    if (!f)
    {
        return -1;
    }
    fprintf(f, "-- the places of the guards in this binary (`trap N` in its output)\n");
    for (uint32_t i = 0; i < ntraps; i++)
    {
        if (trap_used[i])
        {
            fprintf(f, "trap %u\t%s\n", i + 1, trap_texts[i]);
        }
    }
    fclose(f);
    return 1;
}
