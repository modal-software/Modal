// cx.h (v2): compilation context. One growable arena for everything, one Unit per source file.
#ifndef MODAL_V2_CX_H
#define MODAL_V2_CX_H

#include "v2/compiler/ast/ast.h"
#include "v2/compiler/ast/symbol.h"
#include "v2/compiler/syntax/scanner.h"
#include <stdarg.h>
#include <stdint.h>

typedef struct ArenaBlock
{
    struct ArenaBlock *prev;
    size_t cap; // usable bytes after this header
    size_t used;
} ArenaBlock;

typedef struct
{
    ArenaBlock *block; // newest block
    size_t reserved;
    size_t peak;
} Arena;

typedef struct
{
    ArenaBlock *block;
    size_t used;
} ArenaMark;

#define ARENA_BLOCK_MIN (4 * 1024)

#define MAX_UNITS 64
#define MAX_DOMAINS 32

// A source file. The file is also a module: `example/john.modal` is module `john` in domain
// (folder) `example`, and `example/mod.modal` is the domain's public face.
struct Unit
{
    const char *path;  // as shown in diagnostics
    const char *canon; // canonical path: identity for the module cache
    const char *src;
    size_t len;
    const char *name; // module name: file stem, or the folder name for mod.modal
    int name_len;
    TokenVec tokens;
    NodeVec nodes;
    NodeId root;
    Scope *scope;
    uint16_t id;
    uint16_t domain;
    uint8_t is_manifest; // this file is a mod.modal
    uint8_t bound;       // the domain's mod.modal binds this module: it is part of the face
    uint8_t broken;      // did not scan, parse or declare cleanly: later phases skip it
    int errors;
};

typedef struct
{
    Arena arena;
    Unit units[MAX_UNITS];
    uint16_t nunits;
    const char *domains[MAX_DOMAINS]; // canonical folder paths
    uint16_t ndomains;
    SymVec syms;    // slot 0 is reserved as "no symbol"
    Scope **scopes; // scope table: a 24-byte Symbol refers to its member scope by index
    uint32_t nscopes;
    uint32_t scopes_cap;
    int errors;
    uint8_t ok;
} context;

extern context cx;

// ---- arena --------------------------------------------------------------------------------
//
// Everything the compiler allocates comes from here and is freed at once at the end. The
// arena grows in blocks, on demand: a small program takes a few KB from the system, not a
// slab reserved up front. Allocations never move, so pointers into the arena stay valid.

void *arena_alloc(size_t sz); // 8-byte aligned; never returns NULL: exits on exhaustion
ArenaMark arena_mark(void);
void arena_release(ArenaMark mark); // frees everything allocated after the mark
// Gives back the tail of the most recent allocation (no-op if it is not the most recent).
void arena_shrink(void *ptr, size_t old_size, size_t new_size);
// Doubles an array of `elem`-sized items: returns the new storage, updates *cap.
void *arena_grow(void *data, size_t elem, size_t *cap);
size_t arena_used(void);     // bytes handed out
size_t arena_reserved(void); // bytes taken from the system right now
size_t arena_peak(void);     // the most ever taken from the system
void arena_drop(void);

static inline int arena_mark_equal(ArenaMark a, ArenaMark b)
{
    return a.block == b.block && a.used == b.used;
}

// ---- diagnostics ---------------------------------------------------------------------------

// Prints `level [file:line:col]: message`, the source line and a caret under the token.
// level "error" also bumps the unit's and the global error counters.
void diag_vat(Unit *u, uint32_t tok, const char *level, const char *fmt, va_list ap);
void diag_at(Unit *u, uint32_t tok, const char *level, const char *fmt, ...);

// ---- pipeline ------------------------------------------------------------------------------

// module.c
Unit *unit_load(const char *path); // read + scan + parse + declare; cached by canonical path
void modules_link(void);           // load imports, read mod.modal, check visibility
void contract_print(void);         // derived public signatures of every loaded domain
const char *domain_name(uint16_t domain);

// What a mod.modal says for a domain; `fallback` when it does not say.
int domain_config_bool(uint16_t domain, const char *key, int fallback);
// Could a wildcard import of this file (`import(x).*`) supply `name`?
int module_wildcard_provides(Unit *u, const char *name, size_t len);

// resolve.c
void sema_declare(Unit *u);
NodeId sema_callable_params(const Unit *u, NodeId decl);

// check.c
void sema_check(Unit *u); // every name and invocation in a body leads to a declaration

#endif // MODAL_V2_CX_H
