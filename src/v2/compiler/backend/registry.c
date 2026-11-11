#include "v2/compiler/backend/backend.h"
#include <string.h>

// name -> Init. Several names may share one Init (Go maps both "mips" and "mipsle" to
// mips.Init); the Init can read the name it was selected by and adjust what it fills in.
static const struct
{
    const char *name;
    void (*init)(Backend *b);
} backend_inits[] = {
    {"c", backend_c_init},
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

void codegen_start(const struct Backend *backend, void *out_file)
{
    codegen.backend = backend;
    codegen.job.out = (FILE *)out_file;
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

void codegen_function(Unit *u, SymId fn)
{
    if (codegen.begun && u->id == 0)
    {
        uint64_t t = cx.timing ? clock_ns() : 0;
        codegen.backend->function(&codegen.job, fn);
        if (cx.timing)
        {
            phase_end(PHASE_EMIT, t);
        }
    }
}

int codegen_finish(void)
{
    if (!codegen.begun)
    {
        return 0; // the entry file did not get as far as being checked: nothing was emitted
    }
    uint64_t t = clock_ns();
    int errors = codegen.backend->end(&codegen.job);
    phase_end(PHASE_EMIT, t);
    codegen.begun = 0;
    return errors;
}
