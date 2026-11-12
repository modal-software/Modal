#include "v2/compiler/backend/backend.h"
#include "v2/compiler/mir/mir.h"
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

// A machine-code backend gets the functions of every file (the program is linked as a whole and
// its unreachable functions dropped); the C backend, those of the entry file.
void codegen_function(Unit *u, SymId fn)
{
    if (!codegen.begun || (u->id != 0 && !codegen.backend->function_mir))
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
            if (cx.show_mir)
            {
                mir_print(&f, stdout);
            }
            codegen.backend->function_mir(&codegen.job, &f);
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
