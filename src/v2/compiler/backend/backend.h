// backend.h (v2): the boundary between the checked program and code generation.
//
// The front-end ends with a checked tree: every name resolved (unit->refs) and every
// expression typed (unit->types). A backend reads only that and writes output; it never
// parses, resolves or type-checks, and the front-end knows nothing about any target beyond
// the three calls below.
//
// Backends are kept in a registry, the way the Go compiler keeps its architectures: a table
// from a name to an Init function, where Init fills one struct of data and function
// pointers (cmd/compile/main.go `archInits`, filling `ssagen.ArchInfo`). Selecting a
// backend is one table lookup; adding one is one new directory and one new table row.
#ifndef MODAL_V2_BACKEND_H
#define MODAL_V2_BACKEND_H

#include "v2/compiler/cx.h"
#include <stdio.h>

struct MirFunc; // mir/mir.h

typedef struct
{
    Unit *entry; // the checked file to generate code for
    FILE *out;
    const char *path; // where `out` is being written
} BackendJob;

// Code generation is streamed. The compiler keeps the body of one function in memory at a
// time, so a backend is handed each function right after it is checked and must emit it
// then; it cannot come back to it later.
typedef struct Backend
{
    const char *name;      // what `--backend` selects
    const char *summary;   // one line for `--backends`
    const char *extension; // of the files it writes, e.g. ".c"
    // Command that turns the output into an executable, or NULL if the output is final.
    // Used by `--run` as: <toolchain> -o <executable> <output>
    const char *toolchain;
    // The output is an object file: `-o` links it into an executable with the toolchain.
    int object;
    // The output is an executable itself: no toolchain is involved, `--run` runs it.
    int executable;

    // The declarations of job->entry are checked: emit whatever needs only those (types,
    // globals, prototypes).
    void (*begin)(const BackendJob *job);
    // `fn` is checked and its body is in memory until this returns.
    void (*function)(const BackendJob *job, SymId fn);
    // Or, for a machine-code backend: the function already lowered to MIR (mir/mir.h).
    void (*function_mir)(const BackendJob *job, const struct MirFunc *f);
    // Every function has been handed over: emit the entry point and finish the output.
    // Returns the number of errors reported with diag_at for constructs it cannot translate.
    int (*end)(const BackendJob *job);
} Backend;

// registry.c
const Backend *backend_find(const char *name); // NULL if no backend has that name
const Backend *backend_default(void);
void backend_list(FILE *out);

// One Init per backend, each in its own directory.
void backend_c_init(Backend *b);      // backend/c/emit.c
void backend_native_init(Backend *b); // backend/native/emit.c

#endif // MODAL_V2_BACKEND_H
