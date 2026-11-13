// cx.h (v2): compilation context. One growable arena for everything, one Unit per source file.
#ifndef MODAL_V2_CX_H
#define MODAL_V2_CX_H

#include "v2/compiler/ast/ast.h"
#include "v2/compiler/ast/symbol.h"
#include "v2/compiler/sema/types.h"
#include "v2/compiler/syntax/scanner.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

// Compiler phases, for `--time` (the same idea as `go tool compile -bench`).
typedef enum
{
    PHASE_SCAN,
    PHASE_PARSE,
    PHASE_DECLARE,
    PHASE_LINK,
    PHASE_CHECK,
    PHASE_EMIT,
    PHASE__COUNT,
} Phase;

// A region is one reserved range of address space that is filled from the start, like a
// stack. Reserving costs nothing but address space: the system supplies a page only when it is
// first written, so a small program touches a few pages and a large one never pays for a
// realloc copy. An array that is the last thing in its region grows in place and never moves.
// (The technique of Zig's and Jai's allocators, the RAD debugger, and Go's heap arenas.)
//
// A small program would still touch one page per region (16 pages of 16 KB on arm64 macOS)
// for a few KB of data. So the regions whose users already expect their arrays to move (every
// array in them is reached through a pointer that region_grow_array reassigns) start in a slice
// of one shared page, and move once, to their own reservation, when they outgrow it: at most
// one copy of at most the slice. The others never move: code keeps pointers into them.
typedef struct
{
    char *base;      // NULL until first used
    size_t used;     // bytes handed out, from base
    size_t peak;     // the most ever handed out (since base was set)
    size_t reserved; // size of the reservation (of the slice, while shared)
    int shared;      // base is a slice of the shared page
} Region;

typedef enum
{
    REGION_ARENA,  // general allocations: sources, strings, scopes, side tables
    REGION_TOKENS, // the token arrays of every file, one after another
    REGION_NODES,  // the node arrays of every file, one after another
    REGION_SYMS,   // cx.syms
    REGION_TYPES,  // cx.types
    REGION_SCOPES, // cx.scopes
    REGION_NAMES,  // cx.names: the interned names
    REGION_SYMNAMES, // cx.sym_names: the name of each symbol, parallel to cx.syms
    REGION_CODE,   // machine code being generated (backend native)
    REGION_MIR,    // the MIR of the function being generated: emptied for each function
    // The native backend's lists, each growing in place (doubling in the arena would leave
    // every old copy behind): functions, calls, runtime calls, C library calls, branches.
    REGION_FNS,
    REGION_CALLS,
    REGION_RTCALLS,
    REGION_EXTERNS,
    REGION_JUMPS,
    REGION_STAGE, // list elements being parsed, before they are laid out consecutively
    REGION__COUNT,
} RegionId;

typedef struct
{
    size_t used;
} ArenaMark;

#define MAX_UNITS 64
#define MAX_REPORTED 20 // errors printed in full; the rest are only counted
#define MAX_DOMAINS 32

// A source file. The file is also a module: `example/john.modal` is module `john` in domain
// (folder) `example`, and `example/mod.modal` is the domain's public face.
struct Unit
{
    const char *path;  // as shown in diagnostics
    const char *canon; // absolute, lexically normalized path: identity for the module cache
    uint64_t dev, ino; // the file's identity on disk, which also catches a path through a link
    const char *src;
    size_t len;
    const char *name; // module name: file stem, or the folder name for mod.modal
    int name_len;
    TokenVec tokens;
    NodeVec nodes;
    NodeId root;
    Scope *scope;
    TypeId *types;   // per node: the type of each expression, filled by the checker
    SymId *refs;     // per node: the declaration a name or invocation resolved to
    size_t side_cap; // entries allocated in types and refs
    uint16_t id;
    uint16_t domain;
    size_t max_body;     // bytes of the largest deferred body: a bound on its tokens and nodes
    uint32_t nimports;   // import(...) expressions in this file
    uint8_t is_manifest; // this file is a mod.modal
    uint8_t provided;    // made by the compiler, not read from disk: import("target")
    uint8_t bound;       // the domain's mod.modal binds this module: it is part of the face
    uint8_t broken;      // did not scan, parse or declare cleanly: later phases skip it
    uint8_t cached;      // --cache: its bodies were checked in an earlier run; skipped now
    uint64_t src_hash;   // --cache: the whole file
    uint64_t decl_hash;  // --cache: its declarations only, bodies left out: what others see
    int errors;
};

// The machine the program is compiled for. Every size, alignment and byte order the compiler
// needs is read from here (sema/layout.c), never assumed from the host.
typedef enum
{
    TCPU_ARM64,
    TCPU_X86_64,
} TargetCpu;

typedef enum
{
    TOS_MACOS, // Mach-O, the C library for system calls
    TOS_LINUX, // static ELF, system calls made directly
    TOS_NONE,  // bare metal: an ELF image at the memory map's flash, no system at all
} TargetOs;

typedef struct
{
    const char *name;
    uint8_t cpu;        // TargetCpu
    uint8_t os;         // TargetOs
    uint8_t ptr_size;   // bytes in a pointer
    uint8_t word_size;  // bytes in a general register
    uint8_t align_i64;  // alignment of u64/i64 (4 on some 32-bit ABIs)
    uint8_t big_endian;
    uint8_t has_64;     // 64-bit arithmetic is native (otherwise done in pairs of registers)
    uint8_t caps;       // TargetCaps: what the system gives the program
} MachineTarget;

// What a target's system provides (revo's comptime Caps, Zig's builtin.os): known when the
// program is compiled, so a program that needs what the target lacks is an error at the line
// that needs it, never at run time.
typedef enum
{
    TCAP_SYSOUT = 1u << 0, // a byte output (stdout): print without an `out`, std.sys.put
    TCAP_EXIT = 1u << 1,   // a process that ends with a status (main's result)
} TargetCaps;

typedef struct
{
    uint32_t size; // 0: the type has no layout (yet)
    uint32_t align;
} Layout;

typedef struct
{
    Region regions[REGION__COUNT];
    Unit units[MAX_UNITS];
    uint16_t nunits;
    uint16_t linked;                  // units whose imports and manifest have been processed
    const char *domains[MAX_DOMAINS]; // canonical folder paths
    const char *domain_names[MAX_DOMAINS]; // their last component, computed once
    uint8_t domain_manifest[MAX_DOMAINS]; // 0: not looked for yet, 1: looked for (loaded or none)
    uint16_t ndomains;
    SymVec syms;    // slot 0 is reserved as "no symbol"
    TypeVec types;  // the type table (see sema/types.h)
    Scope **scopes; // scope table: a 24-byte Symbol refers to its member scope by index
    Name *names;         // interned names; slot 0 is "no name"
    size_t nnames, names_cap;
    uint64_t *name_table; // open addressing: hash << 32 | NameId, 0 = empty; a power of two
    size_t name_table_cap;
    NameId *sym_names; // per symbol: its name (kept out of Symbol so that stays 24 bytes)
    size_t sym_names_cap;
    uint32_t nscopes;
    uint32_t scopes_cap;
    int errors;
    int quiet;        // > 0: diagnostics are counted in quiet_errors, not reported (comptime)
    int quiet_errors;
    uint64_t phase_ns[PHASE__COUNT]; // time spent per phase
    uint32_t names_resolved;         // names and invocations the body checker led to a declaration
    uint32_t calls_resolved;
    uint8_t ok;
    uint8_t eager;  // parse every body up front and keep the whole tree (--eager, --ast)
    uint8_t timing; // --time: measure the per-function phases too
    uint8_t cache;  // --cache: skip checking the bodies of files that have not changed
    uint8_t show_mir; // --mir: print each function's MIR as it is generated
    const char *std_dir; // where std's .modal files are ($MODAL_STD, or std/ next to the compiler); NULL: none
    uint8_t kinds;    // --sig: note what each function does, for its kind (sema/check.c)
    uint8_t stats;    // --stats: backends report what they produced (sizes, memory)
    const MachineTarget *target; // see target_get()
    uint8_t target_given;        // --target named it
} context;

extern context cx;

// ---- regions and the arena ---------------------------------------------------------------

void *region_push(RegionId r, size_t bytes); // `bytes` more at the end; never returns NULL
void region_set_end(RegionId r, const void *end); // give back everything after `end`
void *region_end(RegionId r);                     // where the next push will land
// region_end(), after checking that `bytes` more would fit. Nothing is handed out: the caller
// writes there and then claims what it wrote with region_set_end.
void *region_room(RegionId r, size_t bytes);
// Makes room for `need` items of `elem` bytes in an array of the region holding `size` items
// in `*cap` slots. The array grows in place when it ends where the region ends, which is the
// normal case; otherwise it is moved to the end, like realloc. Returns the array.
void *region_grow_array(RegionId r, void *data, size_t elem, size_t *cap, size_t need);
size_t regions_used(void);    // bytes handed out, all regions
size_t regions_touched(void); // pages the system has supplied at peak, in bytes
void regions_print(FILE *out); // per region (--stats)

// Everything the compiler allocates without its own region comes from the arena and is
// freed at once at the end. Allocations never move, so pointers into it stay valid.
void *arena_alloc(size_t sz); // 8-byte aligned; never returns NULL: exits on exhaustion
ArenaMark arena_mark(void);
void arena_release(ArenaMark mark); // frees everything allocated after the mark
// Gives back the tail of the most recent allocation (no-op if it is not the most recent).
void arena_shrink(void *ptr, size_t old_size, size_t new_size);
// Doubles an array of `elem`-sized items: returns the new storage, updates *cap.
void *arena_grow(void *data, size_t elem, size_t *cap);
void arena_drop(void);
// A fresh context for another compilation in the same process: every region is emptied but
// keeps its address space and pages.
void context_reset(void);

static inline int arena_mark_equal(ArenaMark a, ArenaMark b)
{
    return a.used == b.used;
}

// ---- nodes -----------------------------------------------------------------------------------

void node_array_grow(Unit *u); // the rare case, out of line

// Appends a node: inlined into the parser (a call through the `ast` table cannot be).
static inline NodeId node_new(Unit *u, AstNodeKind kind, uint32_t tok, NodeId a, NodeId b)
{
    if (u->nodes.size == u->nodes.cap)
    {
        node_array_grow(u);
    }
    // A node is a list of one until a list lays it out with others (see list_finish).
    u->nodes.data[u->nodes.size] =
        (AstNode){.kind = (uint8_t)kind, .flags = AST_F_LAST, .tok = tok, .a = a, .b = b};
    return (NodeId)u->nodes.size++;
}

// Appends a whole node (a list element being laid out, see list_finish).
// The value of an integer literal: decimal, or hexadecimal after 0x, with `_` allowed between
// digits (0x4000_0000, 1_000_000). Returns 0 if the text is not one (a fraction); *overflow is
// set when it does not fit 64 bits (the value is then meaningless).
static inline int number_value(const char *s, uint32_t len, uint64_t *out, int *overflow)
{
    uint64_t v = 0;
    int over = 0, base = 10, any = 0;
    uint32_t i = 0;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        i = 2;
    }
    for (; i < len; i++)
    {
        char ch = s[i];
        int d = ch >= '0' && ch <= '9'   ? ch - '0'
                : base == 16 && ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                : base == 16 && ch >= 'A' && ch <= 'F' ? ch - 'A' + 10
                                                       : -1;
        if (ch == '_')
        {
            continue;
        }
        if (d < 0)
        {
            return 0;
        }
        over |= v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base;
        v = v * (uint64_t)base + (uint64_t)d;
        any = 1;
    }
    *out = v;
    if (overflow)
    {
        *overflow = over;
    }
    return any;
}

static inline NodeId node_append(Unit *u, AstNode n)
{
    if (u->nodes.size == u->nodes.cap)
    {
        node_array_grow(u);
    }
    u->nodes.data[u->nodes.size] = n;
    return (NodeId)u->nodes.size++;
}

// A function (AST_METHOD_DECL or AST_FN_LIT) has three parts but a node two children: its
// parameters, and a signature node holding the return type and the body.
static inline NodeId node_new_fn(Unit *u, AstNodeKind kind, uint32_t tok, NodeId params,
                                 NodeId ret, NodeId body)
{
    NodeId sig = node_new(u, AST_SIG, tok, ret, body);
    return node_new(u, kind, tok, params, sig);
}

static inline NodeId fn_ret_node(const Unit *u, NodeId fn) // 0: no return type
{
    NodeId sig = u->nodes.data[fn].b;
    return sig ? u->nodes.data[sig].a : 0;
}

static inline NodeId fn_body(const Unit *u, NodeId fn) // or the TOK_BODY token, if AST_F_LAZY
{
    NodeId sig = u->nodes.data[fn].b;
    return sig ? u->nodes.data[sig].b : 0;
}

static inline void fn_set_body(Unit *u, NodeId fn, NodeId body)
{
    u->nodes.data[u->nodes.data[fn].b].b = body;
}

// ---- timing ----------------------------------------------------------------------------------

uint64_t clock_ns(void);

// phase_end(PHASE_X, start) adds the time since `start` to the phase and returns "now", so
// consecutive phases chain: t = phase_end(A, t); ...; t = phase_end(B, t);
static inline uint64_t phase_end(Phase phase, uint64_t start)
{
    uint64_t now = clock_ns();
    cx.phase_ns[phase] += now - start;
    return now;
}

// ---- diagnostics ---------------------------------------------------------------------------

// Prints `level [file:line:col]: message`, the source line and a caret under the token.
// level "error" also bumps the unit's and the global error counters.
void diag_vat(Unit *u, uint32_t tok, const char *level, const char *fmt, va_list ap);
void diag_at(Unit *u, uint32_t tok, const char *level, const char *fmt, ...);
void diag_tok(Unit *u, const Token *t, const char *level, const char *fmt, ...);
void diag_vtok(Unit *u, const Token *t, const char *level, const char *fmt, va_list ap);

// ---- pipeline ------------------------------------------------------------------------------

// module.c
Unit *unit_load(const char *path); // read + scan + parse + declare; cached by canonical path
void modules_link(void);           // load imports, read mod.modal, check visibility and bodies
// A function body was just parsed into nodes [from, size): resolve the imports written in it
// and check its module paths.
void module_body_nodes(Unit *u, NodeId from);
// Module-path checks over the nodes [from, to) of a file.
void module_check_paths(Unit *u, NodeId from, NodeId to);
void contract_print(void); // derived public signatures of every loaded domain
SymId module_member(Unit *u, NodeId base, uint32_t name_tok); // `base.name` through imports
SymId module_binding_function(SymId binding); // the function `f := import(x).f` names, or 0
const char *domain_name(uint16_t domain);

// What a mod.modal says for a domain; `fallback` when it does not say.
int domain_config_bool(uint16_t domain, const char *key, int fallback);
uint64_t domain_config_int(uint16_t domain, const char *key, uint64_t fallback);
// Could a wildcard import of this file (`import(x).*`) supply `name`?
int module_wildcard_provides(Unit *u, const char *name, size_t len);
SymId module_domain_function(const Unit *u, const Token *name); // declared in the domain's mod.modal
Unit *module_target_unit(void); // import("target"): made by the compiler

// cache.c: Go's build cache, at the granularity of a file. Check-only runs.
uint64_t hash_bytes(const void *data, size_t len, uint64_t seed);
void cache_unit_loaded(Unit *u);   // after declarations: computes the unit's hashes
void cache_open(const char *entry); // reads the cache of this program, if any
int cache_lookup(Unit *u);          // 1: unchanged and checked clean before; counters replayed
void cache_record(Unit *u, uint32_t names, uint32_t calls); // checked clean just now
void cache_save(void);              // writes the cache back if it changed

// resolve.c
void sema_declare(Unit *u);
NodeId sema_callable_params(const Unit *u, NodeId decl);

// backend/registry.c: code generation is streamed, one checked function at a time.
struct Backend;
void codegen_start(const struct Backend *backend, void *out_file, const char *path);
void codegen_begin(Unit *u);              // declarations are checked: types, prototypes
void codegen_function(Unit *u, SymId fn); // one function is checked and still in memory
int codegen_finish(void);                 // returns the number of errors the backend reported
int codegen_run(void); // started with no output: run the program in memory (Backend.run)
// Why `fn` could not be generated, if it could not: reported when the program needs it.
typedef struct
{
    Unit *unit;
    Token at;         // where (a copy: the body it came from may be gone)
    const char *what; // what cannot be generated yet
    const char *name; // the function's name, into the source
    int name_len;
} CodegenFailure;
const CodegenFailure *codegen_failure(SymId fn);
void codegen_failure_clear(SymId fn);

// check.c
// What a backend asks about a declaration, beyond the per-node tables.
TypeId sema_fn_ret(Unit *u, SymId fn);                   // declared return type (T_VOID if none)
TypeId sema_param_type(Unit *u, SymId fn, NodeId param); // type of one of its parameters
TypeId sema_value_type(Unit *u, SymId field_or_const);
NodeId sema_fn_params(const Unit *u, SymId fn);      // its parameters after `self`
int sema_is_std_print(const Unit *u, SymId binding); // `x := import("std").io.print`
// Which std function a binding names: `import("std").io.print`, `import("std").sys.put`.
typedef enum
{
    STD_NONE,
    STD_PRINT,
    STD_SYS_PUT, // one byte to the system output
} StdMember;
int sema_std_member(const Unit *u, SymId binding);
// The language's primitives (docs/std.md §2): predeclared names, never keywords. The checker marks
// a call to one in the call node's aux.
enum
{
    PRIM_NONE,
    PRIM_SIZEOF,  // sizeof(T or value)
    PRIM_CONVERT, // u8(x): to the call's type, wrapping
    PRIM_TRAP,    // trap(a, b, ...): its strings to the error output, then stop
};
int sema_primitive(const Unit *u, NodeId call);
// The text of a `site` (AST_SITE at node id): `file:line:col: <the call as written>`.
int sema_site_text(const Unit *u, NodeId site, char *text, size_t cap);
// `sizeof(T)` at node `call`: its value (an unsized constant). 0 if call is not one.
int sema_sizeof(const Unit *u, NodeId call, uint64_t *out);
// `assert(cond)` at node `call`: 1 if call is one.
// `n` naming an unsized module-level constant (`n := 5`, docs/comptime.md §3.1): its exact value as
// 64 bits, and whether it is negative. 0 if id names no such constant.
int sema_unsized_value(const Unit *u, NodeId id, uint64_t *bits, int *negative);
void sema_check(Unit *u);
void sema_kinds_reset(void); // a fresh compilation: no function facts yet
// Compile time (sema/comptime.c, docs/comptime.md step 1): module-level initializers the compiler
// runs. comptime_unit evaluates those of u (after its declarations, before its bodies).
void comptime_reset(void);
void comptime_unit(Unit *u);
int sema_comptime_value(SymId s, uint64_t *out); // the value the compiler computed for s
// A string or struct value the compiler computed: its slots in the target's C layout, and for
// each slot that holds a string, its bytes (str[j] NULL: slot j is plain bytes).
typedef struct
{
    uint32_t n;
    const uint64_t *slots;
    const uint8_t *const *str;
    const uint32_t *len;
} ComptimeAggregate;
const ComptimeAggregate *sema_comptime_aggregate(SymId s); // NULL: none
// Checks fn's body again (quietly) and lowers it for the compiler to run; 0 if that failed.
int sema_lower_for_comptime(SymId fn);
void comptime_capture(Unit *u, SymId fn); // check.c hands it the function it just checked
// A function's kind (docs/comptime.md §2): "both", "run time  (why, where)", "compile time  (...)",
// or "" when its body was not checked. Written into buf when it says why.
const char *sema_fn_kind(SymId fn, char *buf, size_t size);
// Clauses (literal patterns: `u8 fact(u8 0): 1`).
int sema_pattern(Unit *u, NodeId param, SymId owner, uint64_t *value, int loud);
// What a pattern parameter matches: one value, a range (both ends included, in the parameter's
// variant), or a string (its literal token; the bytes from sema_pattern_text).
enum
{
    PATTERN_VALUE = 1,
    PATTERN_RANGE,
    PATTERN_TEXT,
};
typedef struct
{
    int kind;
    uint64_t lo, hi; // PATTERN_VALUE: lo. PATTERN_RANGE: lo .. hi (an open range: hi is the max)
    uint32_t tok;    // PATTERN_TEXT: the string literal
} Pattern;
int sema_pattern_of(Unit *u, NodeId param, SymId owner, Pattern *out, int loud);
int sema_arm_pattern(Unit *u, NodeId pat, TypeId type, Pattern *out); // an arm's (0: a name)
// AST_MATCH aux: MATCH_LAST_FREE: the arms above cover every value but the last arm's, so the
// last needs no test (the compiler proved it).
#define MATCH_LAST_FREE 1
// A string literal's bytes, escapes decoded; returns the length (at most cap are written).
size_t sema_pattern_text(const Unit *u, uint32_t tok, uint8_t *out, size_t cap);
SymId sema_next_clause(Unit *u, SymId fn); // tried when fn's patterns do not match; 0: none
int sema_clause_index(Unit *u, SymId fn);  // 0 for the first clause, which calls go to
TypeId sema_type_of(Unit *u, NodeId type_node, SymId owner); // a written type, quietly
int sema_is_register_block(SymId s);  // a struct whose fields have offsets (`u32 dr @ 0x00`)
uint64_t sema_device_address(SymId s); // `uart0: pl011 @ address`: the address; 0 if not a device
void sema_syntax_only(Unit *u); // for a file that is not checked: parse its bodies for errors //
                                // every name and invocation in a body leads to a declaration

// layout.c
extern const MachineTarget *const targets[]; // NULL-terminated
const MachineTarget *target_find(const char *name); // NULL if unknown
void target_set(const MachineTarget *t);
const MachineTarget *target_get(void);  // the target set, or the host's
Layout type_layout(TypeId t);    // C's layout on the target
uint32_t field_offset_of(SymId field);
uint32_t layout_member_offset(SymId strct, NodeId member); // a field declaration's offset

#endif // MODAL_V2_CX_H
