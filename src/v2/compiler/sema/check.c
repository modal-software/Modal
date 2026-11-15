// check.c (v2): names, invocations and types inside bodies.
//
// Before anything can be generated, every body is resolved and typed:
//   - a name is a local, a parameter, `self`, a module-level declaration, an import binding,
//     a member reached through implicit self (when the domain enables it), or it is an error
//   - `f(args)` must name a function; the number of arguments selects the definition (any of
//     them, for an overload set) and each argument must fit its parameter
//   - `value.name` and `value.name(args)` are resolved through the type of `value`, which is
//     also where use across domains is recorded: a member another domain uses is public
//   - `T.name(args)` on a struct type calls the method on a fresh zero value (a constructor)
//   - a local takes the type of its initializer; a return must fit the declared type
//
// The results are left in two per-node tables on the unit, which is all a backend reads:
//   unit->types[node]  the type of the expression
//   unit->refs[node]   the declaration a name or an invocation resolved to
#include "v2/compiler/cx.h"
#include "v2/compiler/syntax/parser/parser.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LOCALS 256

// Functions made once per type (see specialize): the specializations and their bound names.
#define SPEC_MAX_BOUND 8
typedef struct
{
    SymId fn, of;  // the specialization and the generic function
    uint32_t n;
    NameId names[SPEC_MAX_BOUND];
    TypeId types[SPEC_MAX_BOUND];  // a type parameter's type (T_NONE for a value)
    uint8_t is_value[SPEC_MAX_BOUND];
    uint64_t values[SPEC_MAX_BOUND]; // a value parameter's value (`i` of field(v, i))
    NodeId texpr[SPEC_MAX_BOUND];  // the signature's types written as values (`T.at(i).of`)
    TypeId ttype[SPEC_MAX_BOUND];  // and the types they stand for, here
    uint32_t nt;
    uint8_t generated, queued;      // checked for the backend / waiting for its turn
    uint8_t failed;                 // a type of its signature could not be computed: reported
} Spec;
#define SPEC_MAX 4096
static Spec specs[SPEC_MAX]; // fixed: a binding points into it while a nested one is made
static uint32_t nspecs;
static const Spec *binding; // the specialization whose names resolve_type is reading under

typedef struct
{
    uint32_t tok;
    TypeId type;
    NameId name; // locals are told apart by interned name: an integer compare (LuaJIT)
    uint8_t mut;     // can be assigned to: type-first parameters and locals (`T x`)
    uint8_t context; // a context parameter (`arena: arena`)
    uint8_t param;   // a parameter, not a local declared in the body
    uint8_t bound;   // bound with `name := value`: immutable, like everywhere else
    uint8_t nonzero; // inside an arm whose values exclude 0: as a divisor it needs no guard
    uint64_t below;  // inside an arm whose values are 0 .. below - 1 (0: not known): an index in range
    uint8_t frame;   // holds a reference into this function's frame (`a: *T = &T{ ... }`)
    NodeId call;     // `a := f(x)` called later as `a()`: the call, its arguments bound (0: none)
    SymId err_of;    // `n, e := f()`: e is f's error (its arms are checked against f's set)
} Local;

typedef struct
{
    Unit *u;
    SymId owner;       // struct that owns the callable being checked, 0 = the module
    int has_self;      // the callable declares `self`
    int implicit_self; // domain config: a bare member name stands for self.name
    int in_callable;
    int pretyped; // check_args: the arguments were typed already (an overload chosen by them)
    int found_nonzero; // find_name: the local found is an arm binding that excludes 0
    NodeId found_call; // find_name: the local found holds a bound call (`a := f(x)`, then `a()`)
    TypeId ret; // declared return type of the callable being checked
    int nlocals;
    int block_base; // first local of the innermost block
    SymId literal;  // the function literal being checked, 0 if none
    int fn_base;    // its first local: a local below it belongs to the function around it
    uint32_t literal_name; // `name := (...) {...}`: the name the next literal is bound to
    SymId fn;              // the function whose body is being checked (its kind's facts)
    int returning_ref;     // checking `return &point{ ... }`: built in self's storage
    int local_ref;         // checking a local's `&point{ ... }`: built in this function's frame
    int fails;             // the function being checked says `, error`: an error may go up
    SymId lits[16];        // the literals being checked, outermost first, and each one's first local:
    int lit_base[16];      // a local below a literal's base is one it captures (context.md §60)
    int nlits;
    NodeId handled;        // the call `n, e := f()` binds: its error is the second name's
    NodeId guard;          // and that call, a boundary: its panics come back to it
    Local locals[MAX_LOCALS];
} Check;

// ---- error sets (guards.md §5, step 6) ----------------------------------------------------------
//
// Noted while a body is checked: the panics it can raise (a guard the checker cannot prove away),
// the errors it returns, and its calls (whether their errors and their panics go up from it).
// The sets are the closure over the calls, computed when asked: `--sig` prints them, an `if e`
// on a call's error is checked against them at the end, and a boundary whose call can raise
// nothing is no boundary.
enum
{
    EE_ERRORS = 1, // the callee's errors go up from the caller
    EE_PANICS = 2, // its panics go up too (the call is not a boundary)
    EE_CALLS = 4,  // a call (every one: what the callee uses, it uses)
};
// Kept small (2 bytes a symbol, 12 a call, 16 a returned error) and outside the arena, which
// a big program fills with its bodies: malloc'd tables, grown in place.
enum
{
    EF_SEEN = 1, // its body was checked
    EF_ANY = 2,  // it returns an error it holds: which one is not known
    EF_GLOBALS = 8, // it uses a mutable module variable (an arena's memory could stay reachable there)
};
typedef struct
{
    SymId from, to;
    uint8_t flags;
} ErrEdge;
typedef struct
{
    SymId fn;
    uint64_t id;
} ErrId;
static uint8_t *ef_panics, *ef_flags; // own: per symbol
static size_t ef_cap;
static ErrEdge *ef_edges;
static size_t ef_nedges, ef_edges_cap;
static ErrId *ef_ids;
static size_t ef_nids, ef_ids_cap;
// the closure, recomputed when facts were added since the last one
static uint8_t *cl_panics, *cl_flags; // EF_ANY, and 4: whole (every function it reaches is checked)
static size_t cl_edges = (size_t)-1, cl_ids = (size_t)-1, cl_seen;
static size_t ef_seen_count;

static void *grow(void *p, size_t *cap, size_t need, size_t size)
{
    if (need <= *cap)
    {
        return p;
    }
    size_t nc = *cap ? *cap : 256;
    while (nc < need)
    {
        nc *= 2;
    }
    p = realloc(p, nc * size);
    if (!p)
    {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    *cap = nc;
    return p;
}

static void ef_fit(SymId fn)
{
    if (fn < ef_cap)
    {
        return;
    }
    size_t old = ef_cap, cap = ef_cap;
    ef_panics = (uint8_t *)grow(ef_panics, &cap, (size_t)fn + 1, 1);
    size_t cap2 = old;
    ef_flags = (uint8_t *)grow(ef_flags, &cap2, (size_t)fn + 1, 1);
    memset(ef_panics + old, 0, cap - old);
    memset(ef_flags + old, 0, cap - old);
    ef_cap = cap;
}

static void note_seen(SymId fn)
{
    ef_fit(fn);
    if (!(ef_flags[fn] & EF_SEEN))
    {
        ef_flags[fn] |= EF_SEEN;
        ef_seen_count++;
    }
}

static void note_panic(Check *c, int kind)
{
    if (c->fn)
    {
        ef_fit(c->fn);
        ef_panics[c->fn] |= (uint8_t)(1u << (kind - 1));
    }
}

static void note_any(Check *c)
{
    if (c->fn)
    {
        ef_fit(c->fn);
        ef_flags[c->fn] |= EF_ANY;
    }
}

static void note_error_id(Check *c, uint64_t id)
{
    if (c->fn)
    {
        ef_ids = (ErrId *)grow(ef_ids, &ef_ids_cap, ef_nids + 1, sizeof(ErrId));
        ef_ids[ef_nids++] = (ErrId){c->fn, id};
    }
}

static void note_edge(Check *c, SymId to, uint8_t flags)
{
    if (!c->fn || !to || !flags)
    {
        return;
    }
    ef_edges = (ErrEdge *)grow(ef_edges, &ef_edges_cap, ef_nedges + 1, sizeof(ErrEdge));
    ef_edges[ef_nedges++] = (ErrEdge){c->fn, to, flags};
}

// The panics, the unknown errors and wholeness over the calls: a fixed point.
static void errors_close(void)
{
    if (cl_edges == ef_nedges && cl_ids == ef_nids && cl_seen == ef_seen_count)
    {
        return; // nothing new since
    }
    ef_fit((SymId)(cx.syms.size ? cx.syms.size - 1 : 0));
    static size_t cl_cap;
    size_t c1 = cl_cap, c2 = cl_cap;
    cl_panics = (uint8_t *)grow(cl_panics, &c1, ef_cap, 1);
    cl_flags = (uint8_t *)grow(cl_flags, &c2, ef_cap, 1);
    cl_cap = c1;
    for (size_t f = 0; f < ef_cap; f++)
    {
        cl_panics[f] = ef_panics[f];
        cl_flags[f] = (uint8_t)((ef_flags[f] & (EF_ANY | EF_GLOBALS)) | (ef_flags[f] & EF_SEEN ? 4 : 0));
    }
    for (int changed = 1; changed;)
    {
        changed = 0;
        for (size_t e = 0; e < ef_nedges; e++)
        {
            SymId x = ef_edges[e].from, y = ef_edges[e].to;
            uint8_t fl = ef_edges[e].flags;
            if (y >= ef_cap)
            {
                continue;
            }
            if ((cl_flags[x] & 4) && !(cl_flags[y] & 4))
            {
                cl_flags[x] &= (uint8_t)~4;
                changed = 1;
            }
            if ((fl & EE_PANICS) && (cl_panics[y] & ~cl_panics[x]))
            {
                cl_panics[x] |= cl_panics[y];
                changed = 1;
            }
            if ((fl & EE_ERRORS) && (cl_flags[y] & EF_ANY) && !(cl_flags[x] & EF_ANY))
            {
                cl_flags[x] |= EF_ANY;
                changed = 1;
            }
            if ((fl & EE_CALLS) && (cl_flags[y] & EF_GLOBALS) && !(cl_flags[x] & EF_GLOBALS))
            {
                cl_flags[x] |= EF_GLOBALS;
                changed = 1;
            }
        }
    }
    cl_edges = ef_nedges;
    cl_ids = ef_nids;
    cl_seen = ef_seen_count;
}

// The errors fn returns, its own and those its error edges bring up (a walk over them: they are
// few). At most cap are written; returns how many.
static uint32_t error_ids_of(SymId fn, uint64_t *out, uint32_t cap)
{
    SymId stack[256], seen[256];
    uint32_t ns = 0, nseen = 0, n = 0;
    stack[ns++] = fn;
    seen[nseen++] = fn;
    while (ns)
    {
        SymId f = stack[--ns];
        for (size_t i = 0; i < ef_nids; i++)
        {
            if (ef_ids[i].fn != f)
            {
                continue;
            }
            int have = 0;
            for (uint32_t k = 0; k < n && !have; k++)
            {
                have = out[k] == ef_ids[i].id;
            }
            if (!have && n < cap)
            {
                out[n++] = ef_ids[i].id;
            }
        }
        for (size_t e = 0; e < ef_nedges; e++)
        {
            if (ef_edges[e].from != f || !(ef_edges[e].flags & EE_ERRORS))
            {
                continue;
            }
            int was = 0;
            for (uint32_t k = 0; k < nseen && !was; k++)
            {
                was = seen[k] == ef_edges[e].to;
            }
            if (!was && nseen < 256 && ns < 256)
            {
                seen[nseen++] = ef_edges[e].to;
                stack[ns++] = ef_edges[e].to;
            }
        }
    }
    return n;
}

// Does fn, or a function it calls, use a mutable module variable? (1 also when not known.)
static int uses_globals(SymId fn)
{
    errors_close();
    return !fn || fn >= ef_cap || !(cl_flags[fn] & 4) || (cl_flags[fn] & EF_GLOBALS);
}

int sema_error_set(SymId fn, uint8_t *panics, const uint64_t **ids, uint32_t *n, int *any)
{
    static uint64_t buf[128];
    *panics = 0;
    *ids = buf;
    *n = 0;
    *any = 0;
    errors_close();
    if (!fn || fn >= ef_cap || !(ef_flags[fn] & EF_SEEN))
    {
        return 0;
    }
    *panics = cl_panics[fn];
    *n = error_ids_of(fn, buf, 128);
    *ids = buf;
    *any = (cl_flags[fn] & EF_ANY) != 0;
    return (cl_flags[fn] & 4) != 0;
}

// Every error a program can name: std's panics and the program's own (`bad_input: error`), for
// print(e). Returns how many (at most cap).
uint32_t sema_errors_list(uint64_t *ids, uint32_t cap)
{
    uint32_t n = 0;
    for (uint64_t k = PANIC_DIVISION; k <= PANIC_STACK && n < cap; k++)
    {
        ids[n++] = k;
    }
    for (SymId s = 1; s < cx.syms.size && n < cap; s++)
    {
        uint64_t v;
        if (SYM(s)->kind == SYM_CONST && !SYM(s)->owner && SYM(s)->decl &&
            NODE(&cx.units[SYM(s)->unit], SYM(s)->decl)->kind == AST_FIELD_DECL &&
            sema_value_type(&cx.units[SYM(s)->unit], s) == T_ERROR && sema_comptime_value(s, &v) &&
            v >= ERROR_FIRST)
        {
            ids[n++] = v;
        }
    }
    return n;
}

// An error's name, for a message or --sig: `bad_input`, or `panic.division`.
int sema_error_name(uint64_t id, char *buf, size_t cap)
{
    static const char *const panics[] = {"division", "index", "assert", "anytype", "unhandled", "raised", "stack"};
    if (id >= PANIC_DIVISION && id <= PANIC_STACK)
    {
        return snprintf(buf, cap, "panic.%s", panics[id - PANIC_DIVISION]);
    }
    if (id >= ERROR_FIRST && id - ERROR_FIRST < cx.syms.size)
    {
        int len;
        const char *name = symbol_name((SymId)(id - ERROR_FIRST), &len);
        return snprintf(buf, cap, "%.*s", len, name);
    }
    return snprintf(buf, cap, "error %llu", (unsigned long long)id);
}

// `if e { ... }` on the error of a call, `n, e := f()`: its arms are checked against f's set
// once every body is checked (sema_errors_finish).
typedef struct
{
    Unit *u;
    Token at; // a copy: the body's tokens and nodes are gone by the end
    SymId callee;
    int value;
    uint32_t ntaken;
    uint64_t taken[32]; // the errors its arms take (nil is 0)
} ErrMatch;
static ErrMatch *err_matches;
static uint32_t nerr_matches, err_matches_cap;

void sema_errors_finish(void)
{
    errors_close();
    for (uint32_t m = 0; m < nerr_matches; m++)
    {
        ErrMatch *em = &err_matches[m];
        Unit *u = em->u;
        uint8_t panics;
        const uint64_t *ids;
        uint32_t n;
        int any;
        int complete = sema_error_set(em->callee, &panics, &ids, &n, &any);
        // what e can hold: nil, the errors, the panics
        uint64_t need[96];
        uint32_t nn = 0;
        need[nn++] = 0;
        for (uint32_t i = 0; i < n && nn < 96; i++)
        {
            need[nn++] = ids[i];
        }
        for (int k = 0; k < 7; k++)
        {
            if ((panics & (1u << k)) && nn < 96)
            {
                need[nn++] = (uint64_t)(PANIC_DIVISION + k);
            }
        }
        char missing[400] = "";
        size_t ml = 0;
        for (uint32_t i = 0; i < nn; i++)
        {
            int taken = 0;
            for (uint32_t t = 0; t < em->ntaken && !taken; t++)
            {
                taken = em->taken[t] == need[i];
            }
            if (!taken && ml + 40 < sizeof(missing))
            {
                char name[64];
                if (need[i])
                {
                    sema_error_name(need[i], name, sizeof(name));
                }
                else
                {
                    snprintf(name, sizeof(name), "nil");
                }
                ml += (size_t)snprintf(missing + ml, sizeof(missing) - ml, "%s%s", ml ? ", " : "", name);
            }
        }
        int len;
        const char *fname = symbol_name(em->callee, &len);
        const char *level = em->value ? "error" : "warning";
        if (!complete || any)
        {
            diag_tok(u, &em->at, !complete && cx.cache ? "warning" : level, // (--cache: a file not checked again)
                    "not every error of '%.*s' is known here (%s): end with `_: ...`", len, fname,
                    any ? "it returns an error it holds" : "it calls a function not checked yet");
        }
        else if (missing[0])
        {
            diag_tok(u, &em->at, level,
                    "'%.*s' can also give %s: add arms for them, or end with `_: ...`", len, fname, missing);
        }
    }
    nerr_matches = 0;
}


// ---- function kinds (docs/comptime.md §2, step 0) --------------------------------------------
//
// What a body does that only the running program can do, noted while it is checked, and the
// functions it calls. A function's kind is then decided over the call graph (sema_fn_kind):
// "compile time" when it is declared in mod.modal, "run time" when it or a function it calls
// prints, touches a device register or reads or writes a mutable module variable, "both"
// otherwise. Pure analysis: nothing generated depends on it yet.
enum
{
    FX_OUTPUT = 1,  // print, write, std.sys.put, an `out`
    FX_DEVICE = 2,  // a register: a field placed with `@`, or a device
    FX_MUTABLE = 4, // a mutable module-level variable, read or written
};

typedef struct
{
    uint8_t fx;      // its own effects
    uint8_t checked; // its body was checked (a cached file's was not)
    uint8_t state;   // closure: 0 not done, 1 in progress, 2 done
    uint8_t kind;    // closure: its effects and those of what it calls
    uint16_t unit;   // its file
    uint32_t at[3];  // where its first effect of each kind is: a byte offset in its source, since
                     // a deferred body's tokens are gone once it is checked
} FnFacts;

static FnFacts *fn_facts;
static size_t fn_facts_cap;
static int capture_mir; // check_callable hands the function to comptime_capture, not the backend
static struct
{
    SymId from, to;
} *fn_edges;
static size_t fn_nedges, fn_edges_cap;

static uint32_t *fn_adj;   // the callees of each function: fn_adj_to[fn_adj[f] .. fn_adj[f + 1])
static SymId *fn_adj_to;
static size_t fn_adj_edges = SIZE_MAX; // the edges it was built from (rebuilt when more came)

void sema_kinds_reset(void)
{
    fn_adj = NULL;
    fn_adj_to = NULL;
    fn_adj_edges = SIZE_MAX;
    fn_facts = NULL;
    fn_facts_cap = 0;
    fn_edges = NULL;
    fn_nedges = fn_edges_cap = 0;
}

static FnFacts *facts_of(SymId fn)
{
    if (fn >= fn_facts_cap)
    {
        size_t cap = cx.syms.cap > fn + 1 ? cx.syms.cap : fn + 1;
        FnFacts *grown = (FnFacts *)arena_alloc(cap * sizeof(FnFacts));
        memset(grown, 0, cap * sizeof(FnFacts));
        if (fn_facts_cap)
        {
            memcpy(grown, fn_facts, fn_facts_cap * sizeof(FnFacts));
        }
        fn_facts = grown;
        fn_facts_cap = cap;
    }
    return &fn_facts[fn];
}

static void note_effect(Check *c, int fx, uint32_t tok)
{
    if (!c->fn || !cx.kinds)
    {
        return;
    }
    FnFacts *f = facts_of(c->fn);
    int bit = fx == FX_OUTPUT ? 0 : fx == FX_DEVICE ? 1 : 2;
    if (!(f->fx & fx))
    {
        f->unit = c->u->id;
        f->at[bit] = (uint32_t)(TSTART(TOK(c->u, tok)) - c->u->src);
    }
    f->fx |= (uint8_t)fx;
}

static void note_call(Check *c, SymId callee)
{
    if (!c->fn || !callee || !cx.kinds)
    {
        return;
    }
    if (fn_nedges == fn_edges_cap)
    {
        size_t cap = fn_edges_cap ? fn_edges_cap * 2 : 256;
        void *grown = arena_alloc(cap * sizeof(*fn_edges));
        if (fn_nedges)
        {
            memcpy(grown, fn_edges, fn_nedges * sizeof(*fn_edges));
        }
        fn_edges = grown;
        fn_edges_cap = cap;
    }
    fn_edges[fn_nedges].from = c->fn;
    fn_edges[fn_nedges++].to = callee;
}

// A name that is a module-level field: a device, or a mutable variable, is an effect.
static void note_global(Check *c, SymId sym, uint32_t tok)
{
    const Symbol *s = SYM(sym);
    if ((s->kind != SYM_FIELD && s->kind != SYM_CONST) || s->owner)
    {
        return;
    }
    const Unit *home = &cx.units[s->unit];
    if (NODE(home, s->decl)->flags & AST_F_PLACED)
    {
        note_effect(c, FX_DEVICE, tok);
    }
    else if (s->flags & SYM_F_MUT)
    {
        note_effect(c, FX_MUTABLE, tok);
        if (c->fn)
        {
            ef_fit(c->fn);
            ef_flags[c->fn] |= EF_GLOBALS;
        }
    }
}

// A field of a struct: a register (placed with `@`) is an effect.
static void note_field(Check *c, SymId field, uint32_t tok)
{
    const Symbol *s = SYM(field);
    if (s->kind == SYM_FIELD && s->owner && (NODE(&cx.units[s->unit], s->decl)->flags & AST_F_PLACED))
    {
        note_effect(c, FX_DEVICE, tok);
    }
}

typedef enum
{
    NAME_MISSING,
    NAME_LOCAL,
    NAME_SELF,
    NAME_BUILTIN,
    NAME_SYMBOL, // module-level declaration or import binding
    NAME_MEMBER, // member of the owning struct, through implicit self
    NAME_WILDCARD,
} NameKind;

static TypeId check_expr(Check *c, NodeId id);
static SymId struct_of(TypeId t);
static SymId field_named(SymId s, const Token *name);
static void note_use(const Check *c, SymId member);
static int known_value(const Unit *u, NodeId e, uint64_t *v);

// ---- small helpers ---------------------------------------------------------------------------

static int text_is(const Token *t, const char *s)
{
    size_t n = strlen(s);
    return t->len == n && memcmp(TSTART(t), s, n) == 0;
}

static int is_self_node(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    return n->kind == AST_IDENT && is_self_token(TSTART(t), (int)t->len);
}

static int count_list(const Unit *u, NodeId id)
{
    int n = 0;
    for (; id; id = NEXT(u, id))
    {
        n++;
    }
    return n;
}

static Scope *struct_scope(SymId s)
{
    return scope_at(SYM(s)->scope_ref);
}

static TypeId set_type(Check *c, NodeId id, TypeId t)
{
    c->u->types[id] = t;
    return t;
}

// Can a value of type `from` be used where `to` is expected?
int sema_is_any(SymId st)
{
    if (!st || SYM(st)->kind != SYM_STRUCT || !cx.std_dir)
    {
        return 0;
    }
    int len;
    const char *name = symbol_name(st, &len);
    return len == 3 && memcmp(name, "any", 3) == 0 && strcmp(cx.domains[cx.units[SYM(st)->unit].domain], cx.std_dir) == 0;
}

static TypeId prim_type(const Token *t);

TypeId sema_named_type(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    while (n->kind == AST_PAREN_GROUP)
    {
        id = n->a;
        n = NODE(u, id);
    }
    if (n->kind != AST_IDENT)
    {
        return T_NONE;
    }
    TypeId prim = prim_type(TOK(u, n->tok));
    if (prim != T_NONE)
    {
        return prim;
    }
    for (uint32_t b = 0; binding && b < binding->n; b++)
    {
        if (!binding->is_value[b] && binding->names[b] == token_name(TOK(u, n->tok)))
        {
            return binding->types[b]; // T in a specialization: the type it was made for
        }
    }
    SymId s = u->refs ? u->refs[id] : 0;
    if (s && SYM(s)->kind == SYM_IMPORT)
    {
        s = module_binding_target(s);
    }
    return s && SYM(s)->kind == SYM_STRUCT ? type_make(T_STRUCT, s) : T_NONE;
}

SymId sema_std_type(void)
{
    // Looked up once per set of loaded files: it can only appear when a file of std loads.
    static SymId found;
    static uint16_t looked_at_units = 0xffff;
    static size_t looked_at_syms;
    if (found && found < cx.syms.size)
    {
        return found;
    }
    if (looked_at_units == cx.nunits && looked_at_syms <= cx.syms.size && !found)
    {
        return 0;
    }
    looked_at_units = cx.nunits;
    looked_at_syms = cx.syms.size;
    found = 0;
    for (SymId s = 1; s < cx.syms.size && cx.std_dir; s++)
    {
        int len;
        const char *name = symbol_name(s, &len);
        if (SYM(s)->kind == SYM_STRUCT && !SYM(s)->owner && len == 4 && memcmp(name, "type", 4) == 0 &&
            strcmp(cx.domains[cx.units[SYM(s)->unit].domain], cx.std_dir) == 0)
        {
            found = s;
            return s;
        }
    }
    return 0;
}

void sema_type_desc(TypeId t, uint64_t d[4])
{
    if (type_kind(t) == T_TYPE)
    {
        t = type_make(T_STRUCT, type_ref(t));
    }
    Layout l = type_layout(t);
    uint64_t n = 0;
    if (type_kind(t) == T_STRUCT)
    {
        SymId st = type_ref(t);
        const Unit *home = &cx.units[SYM(st)->unit];
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            n += NODE(home, m)->kind == AST_FIELD_DECL;
        }
    }
    d[0] = t; // the interned type: one number per type, the same in every backend
    d[1] = l.size;
    d[2] = l.align;
    d[3] = n;
}

int sema_typeof_desc(const Unit *u, NodeId call, uint64_t d[4])
{
    const AstNode *n = NODE(u, call);
    if (n->kind != AST_CALL || n->aux != PRIM_TYPEID || !n->b || !u->types)
    {
        return 0;
    }
    TypeId t = u->types[n->b];
    if (t == T_NONE || t == T_EXTERN || t == T_INT_LIT || sema_is_any_ref(t))
    {
        return 0; // an *any's is read at run time
    }
    sema_type_desc(t, d);
    return 1;
}

int sema_typeof_field(const Unit *u, NodeId sel, uint64_t *out)
{
    const AstNode *n = NODE(u, sel);
    if (n->kind != AST_SELECTOR || !u->refs)
    {
        return 0;
    }
    NodeId base = n->a;
    while (NODE(u, base)->kind == AST_PAREN_GROUP)
    {
        base = NODE(u, base)->a;
    }
    uint64_t d[4];
    SymId field = u->refs[sel];
    if (!field || SYM(field)->kind != SYM_FIELD || !sema_typeof_desc(u, base, d))
    {
        return 0;
    }
    static const char *const names[4] = {"id", "size", "align", "fields"};
    int len;
    const char *name = symbol_name(field, &len);
    for (int i = 0; i < 4; i++)
    {
        if ((size_t)len == strlen(names[i]) && memcmp(name, names[i], (size_t)len) == 0)
        {
            *out = d[i];
            return 1;
        }
    }
    return 0;
}

int sema_is_any_ref(TypeId t)
{
    return type_kind(t) == T_PTR && type_kind(type_ref(t)) == T_STRUCT && sema_is_any(type_ref(type_ref(t)));
}

static int fits(TypeId from, TypeId to)
{
    if (from == to || from == T_NONE || to == T_NONE || from == T_EXTERN || to == T_EXTERN)
    {
        return 1;
    }
    if ((from == T_INT_LIT && type_is_int(to)) || (to == T_INT_LIT && type_is_int(from)))
    {
        return 1;
    }
    if (from == T_NIL && (type_kind(to) == T_PTR || to == T_ERROR))
    {
        return 1; // nil: no reference, or no error (guards.md §5)
    }
    if (sema_is_any_ref(to) && type_kind(from) == T_PTR && !sema_is_any_ref(from))
    {
        return 1; // any reference is an *any: it takes the type of what it points to along
    }
    return 0; // a value is never a reference by itself: `&x` makes one (and `*p` reads one)
}

// How a value is written, for a report: a name, a path of names (`p.x`), or a literal as it
// is in the source; 0 for anything longer (the report then says which argument it is).
static int value_text(const Unit *u, NodeId id, char *buf, size_t cap)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    int k = 0;
    if (n->kind == AST_IDENT || n->kind == AST_NUMBER_LIT || n->kind == AST_STRING_LIT)
    {
        k = snprintf(buf, cap, "%.*s", (int)t->len, TSTART(t));
    }
    else if (n->kind == AST_SELECTOR)
    {
        char base[96];
        if (!value_text(u, n->a, base, sizeof(base)))
        {
            return 0;
        }
        k = snprintf(buf, cap, "%s.%.*s", base, (int)t->len, TSTART(t));
    }
    return k > 0 && (size_t)k < cap ? k : 0;
}

// What a value-for-reference mismatch adds: how to write what is meant, with the value's text.
static void reference_hint(TypeId from, TypeId to, const char *text, char *buf, size_t cap)
{
    buf[0] = 0;
    if (type_kind(to) == T_PTR && type_ref(to) == from)
    {
        snprintf(buf, cap, ": pass a reference to it, `&%s`", *text ? text : "x");
    }
    else if (type_kind(from) == T_PTR && type_ref(from) == to)
    {
        snprintf(buf, cap, ": it is a reference, and `*%s` is the value it points to", *text ? text : "x");
    }
    else if (sema_std_type() && to == type_make(T_STRUCT, sema_std_type()) && from != to)
    {
        snprintf(buf, cap, ": a value where a type is taken; its type is `typeof(%s)`", *text ? text : "x");
    }
}

// ---- constants: exact values and bounds (Go's rule) ---------------------------------------------
//
// An expression of literals is evaluated exactly (128 bits) and must fit the sized type it
// meets: `x: u8 = 300` and `f(256)` for an u8 parameter are errors, never a silent wrap.

typedef __int128 Exact;

static TypeId value_type(Unit *u, SymId s);

// An unsized module-level constant (`n := 5`, `k := n * 3`): its declaration's value node.
static int unsized_const(SymId s, const Unit **home, NodeId *init)
{
    if (!s || SYM(s)->kind != SYM_CONST || SYM(s)->owner)
    {
        return 0;
    }
    Unit *h = &cx.units[SYM(s)->unit];
    const AstNode *decl = NODE(h, SYM(s)->decl);
    if (decl->kind != AST_DEFINE || value_type(h, s) != T_INT_LIT)
    {
        return 0;
    }
    *home = h;
    *init = decl->a;
    return 1;
}

static int exact_value(const Unit *u, NodeId id, Exact *out)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    switch (n->kind)
    {
    case AST_IDENT:
    case AST_SELECTOR:
    {
        // a name of an unsized constant: its value, exactly (a cycle is reported elsewhere)
        static int depth;
        const Unit *home;
        NodeId init;
        uint64_t field;
        if (n->kind == AST_SELECTOR && sema_typeof_field(u, id, &field)) // `typeof(point).size`
        {
            *out = (Exact)field;
            return 1;
        }
        if (n->kind == AST_IDENT && sema_bound_value(u, id, &field)) // `i`, made known by a call
        {
            *out = (Exact)(int64_t)field;
            return 1;
        }
        if (!u->refs || !unsized_const(u->refs[id], &home, &init) || depth > 64)
        {
            return 0;
        }
        depth++;
        int ok = exact_value(home, init, out);
        depth--;
        return ok;
    }
    case AST_NUMBER_LIT:
    {
        uint64_t v;
        int overflow;
        if (!number_value(TSTART(t), t->len, &v, &overflow))
        {
            return 0;
        }
        *out = overflow ? (Exact)UINT64_MAX + 1 : (Exact)v; // too large for any type
        return 1;
    }
    case AST_PAREN_GROUP:
        return exact_value(u, n->a, out);
    case AST_CALL:
    {
        uint64_t size;
        if (sema_sizeof(u, id, &size))
        {
            *out = (Exact)size;
            return 1;
        }
        return 0;
    }
    case AST_UNARY_OP:
        if (t->kind == TOK_MINUS && exact_value(u, n->a, out))
        {
            *out = -*out;
            return 1;
        }
        return 0;
    case AST_BIN_OP:
    {
        Exact a, b;
        if (u->types[id] != T_INT_LIT || !exact_value(u, n->a, &a) || !exact_value(u, n->b, &b))
        {
            return 0; // an operation of a sized type wraps in it: not a constant here
        }
        if (t->kind == TOK_PLUS)
        {
            return !__builtin_add_overflow(a, b, out);
        }
        if (t->kind == TOK_MINUS)
        {
            return !__builtin_sub_overflow(a, b, out);
        }
        if (t->kind == TOK_STAR)
        {
            return !__builtin_mul_overflow(a, b, out);
        }
        if (t->kind == TOK_SLASH && b != 0)
        {
            *out = a / b;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

// Reports a constant that does not fit `type`. Returns 1 if it fits (or is no constant).
// Does argument `id`, if its value is known now, fit `type`'s range? (No report: choosing among
// overloads, where `u8 f` and `u64 f` both take an unsized literal and its value decides.)
static int in_bounds(const Unit *u, NodeId id, TypeId type)
{
    Exact v;
    int bits = type == T_U8 || type == T_I8 ? 8 : type == T_U16 || type == T_I16 ? 16
             : type == T_U32 || type == T_I32 || type == T_INT ? 32 : 64;
    int is_signed = type == T_I8 || type == T_I16 || type == T_I32 || type == T_INT || type == T_I64;
    if (!type_is_int(type) || type == T_INT_LIT || !exact_value((Unit *)u, id, &v))
    {
        return 1;
    }
    Exact lo = is_signed ? -((Exact)1 << (bits - 1)) : 0;
    Exact hi = is_signed ? ((Exact)1 << (bits - 1)) - 1 : ((Exact)1 << bits) - 1;
    return v >= lo && v <= hi;
}

static int check_bounds(Check *c, NodeId id, TypeId type)
{
    static const struct
    {
        TypeId type;
        int bits;
        int is_signed;
    } sizes[] = {
        {T_U8, 8, 0},  {T_U16, 16, 0}, {T_U32, 32, 0}, {T_U64, 64, 0}, {T_I8, 8, 1},
        {T_I16, 16, 1}, {T_I32, 32, 1}, {T_INT, 32, 1}, {T_I64, 64, 1},
    };
    Exact v;
    if (!id || !exact_value(c->u, id, &v))
    {
        return 1;
    }
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        if (sizes[i].type != type)
        {
            continue;
        }
        int bits = sizes[i].bits;
        Exact lo = sizes[i].is_signed ? -((Exact)1 << (bits - 1)) : 0;
        Exact hi = sizes[i].is_signed ? ((Exact)1 << (bits - 1)) - 1 : ((Exact)1 << bits) - 1;
        if (v >= lo && v <= hi)
        {
            return 1;
        }
        char name[16], value[48], from[48], to[48];
        int k = 47;
        Exact m = v < 0 ? -v : v;
        value[k] = '\0';
        do
        {
            value[--k] = (char)('0' + (int)(m % 10));
            m /= 10;
        } while (m);
        if (v < 0)
        {
            value[--k] = '-';
        }
        snprintf(from, sizeof(from), "%s%llu", sizes[i].is_signed ? "-" : "",
                 sizes[i].is_signed ? (unsigned long long)((uint64_t)1 << (bits - 1)) : 0ull);
        snprintf(to, sizeof(to), "%llu", (unsigned long long)(uint64_t)hi);
        diag_at(c->u, NODE(c->u, id)->tok, "error", "%s does not fit %s (%s to %s, %d bits)",
                value + k, type_str(type, name, sizeof(name)), from, to, bits);
        return 0;
    }
    return 1;
}

// ---- types of declarations -------------------------------------------------------------------

// The built-in type a name spells, or T_NONE. Decided from the length and two characters,
// since this runs for every type written in the program.
static TypeId prim_type(const Token *t)
{
    const char *s = TSTART(t);
    switch (t->len)
    {
    case 2:
        if (s[1] == '8')
        {
            return s[0] == 'u' ? T_U8 : s[0] == 'i' ? T_I8 : T_NONE;
        }
        return T_NONE;
    case 3:
        if (s[0] == 'i' && s[1] == 'n' && s[2] == 't')
        {
            return T_INT;
        }
        if (s[0] == 'n' && s[1] == 'i' && s[2] == 'l')
        {
            return T_VOID; // `nil f(...)`: no result (a type to write first, so it can be overloaded)
        }
        if (s[0] == 'u' || s[0] == 'i')
        {
            int is_unsigned = s[0] == 'u';
            if (s[1] == '1' && s[2] == '6')
            {
                return is_unsigned ? T_U16 : T_I16;
            }
            if (s[1] == '3' && s[2] == '2')
            {
                return is_unsigned ? T_U32 : T_I32;
            }
            if (s[1] == '6' && s[2] == '4')
            {
                return is_unsigned ? T_U64 : T_I64;
            }
        }
        return T_NONE;
    case 4:
        return memcmp(s, "bool", 4) == 0 ? T_BOOL : T_NONE;
    case 5:
        return memcmp(s, "error", 5) == 0 ? T_ERROR : T_NONE;
    default:
        return T_NONE;
    }
}

// Turns a type as written into a TypeId. `owner` gives `self` its meaning. With `loud` the
// errors are reported; signatures are resolved loudly once and quietly afterwards.
static TypeId resolve_type(Unit *u, NodeId tn, SymId owner, int loud)
{
    if (!tn)
    {
        return T_VOID;
    }

    const AstNode *n = NODE(u, tn);
    switch (n->kind)
    {
    case AST_TYPE_SLICE:
    {
        TypeId elem = resolve_type(u, n->a, owner, loud);
        return elem == T_U8 ? T_STR : elem == T_NONE ? T_NONE : type_make(T_SLICE, elem);
    }
    case AST_TYPE_PTR:
    {
        TypeId elem = resolve_type(u, n->a, owner, loud);
        return elem == T_NONE ? T_NONE : type_make(T_PTR, elem);
    }
    case AST_TYPE_FUNC: // `function(T): U`: a function given while compiling (the bound one, in a specialization)
        return type_make(T_FUNC, 0);
    case AST_TYPE_EXPR: // `T.at(i).of`: what the specialization being read computed for it
        for (uint32_t i = 0; binding && i < binding->nt; i++)
        {
            if (binding->texpr[i] == tn)
            {
                return binding->ttype[i];
            }
        }
        return T_NONE;
    case AST_TYPE_ARRAY: // [N]T: N a literal or a constant, T a number or bool (for now)
    {
        TypeId elem = resolve_type(u, n->a, owner, loud);
        Exact len;
        if (elem == T_NONE)
        {
            return T_NONE;
        }
        int known = exact_value(u, n->b, &len);
        if (!known && NODE(u, n->b)->kind == AST_IDENT && u->scope)
        {
            // `[N]u8`: a type is resolved before any body is checked, so the name is looked up
            // here, among the module's immutable constants.
            SymId s = scope_find_token(u->scope, TOK(u, NODE(u, n->b)->tok));
            if (s && SYM(s)->kind == SYM_CONST && !SYM(s)->owner)
            {
                const Unit *h = &cx.units[SYM(s)->unit];
                const AstNode *d = NODE(h, SYM(s)->decl);
                NodeId init = d->kind == AST_DEFINE ? d->a : d->kind == AST_FIELD_DECL && !(d->flags & AST_F_MUT) ? d->b : 0;
                known = init && exact_value(h, init, &len);
            }
        }
        if (!known || len < 1 || len > (1 << 24))
        {
            if (loud)
            {
                diag_at(u, NODE(u, n->b)->tok, "error",
                        "an array's length is a number known now, from 1 to 16777216 (a literal or a "
                        "module-level constant)");
            }
            return T_NONE;
        }
        int struct_elem = type_kind(elem) == T_STRUCT && !sema_is_register_block(type_ref(elem)); // (context.md §62)
        if (!(type_is_int(elem) && elem != T_INT_LIT) && elem != T_BOOL && !struct_elem)
        {
            if (loud)
            {
                char got[64];
                diag_at(u, n->tok, "error", "an array of %s is not supported yet (numbers, bools and structs are)",
                        type_str(elem, got, sizeof(got)));
            }
            return T_NONE;
        }
        return type_array(elem, (uint32_t)len);
    }
    case AST_TYPE_NAME:
    {
        const Token *t = TOK(u, n->tok);
        if (n->aux)
        {
            return T_EXTERN; // a dotted path: a type of another module
        }
        TypeId prim = prim_type(t);
        if (prim != T_NONE)
        {
            return prim;
        }
        for (uint32_t i = 0; binding && i < binding->n; i++)
        {
            if (binding->names[i] == token_name(t) && !binding->is_value[i])
            {
                return binding->types[i]; // `v: T` in a specialization: the type T was given
            }
        }
        if (is_self_token(TSTART(t), (int)t->len))
        {
            return owner ? type_make(T_PTR, type_make(T_STRUCT, owner)) : T_MODULE;
        }
        SymId s = scope_lookup_name(owner ? struct_scope(owner) : u->scope, token_name(t));
        if (s && SYM(s)->kind == SYM_STRUCT)
        {
            return type_make(T_STRUCT, s);
        }
        if (s && SYM(s)->kind == SYM_IMPORT) // `gpoint := import("./geo").shapes.point`: a struct
        {
            SymId target = module_binding_target(s);
            return target && SYM(target)->kind == SYM_STRUCT ? type_make(T_STRUCT, target) : T_EXTERN;
        }
        if (text_is(t, "site"))
        {
            return T_STR; // a call's place, filled by the compiler (docs/std.md §2)
        }
        if (loud)
        {
            diag_at(u, n->tok, "error", "unknown type '%.*s'", t->len, TSTART(t));
        }
        return T_NONE;
    }
    default:
        if (loud)
        {
            diag_at(u, n->tok, "error",
                    "an inline struct type cannot be used here: declare it "
                    "with a name first");
        }
        return T_NONE;
    }
}

// The function node behind a callable declaration: the method itself, or the literal bound
// by `name := (params) T { }`.
static const AstNode *callable_node(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    return n->kind == AST_DEFINE ? NODE(u, n->a) : n;
}

// ---- functions made once per type (docs/comptime.md §3.2) ---------------------------------------
//
// A parameter of std's type that another parameter, or the result, takes as its type
// (`soma(T: type, v: T)`) is known while compiling: the function is made once per type given
// to it, a specialization, a symbol of its own with T bound. No marker says so: the use does.
// The generic function itself is never checked or generated, only its specializations.

static const Spec *spec_of(SymId fn)
{
    for (uint32_t i = 0; fn && i < nspecs; i++)
    {
        if (specs[i].fn == fn)
        {
            return &specs[i];
        }
    }
    return NULL;
}

// The functions a specialization was made for (its `function(...)` parameters), in order.
uint32_t sema_spec_functions(SymId fn, SymId *out, uint32_t cap)
{
    const Spec *sp = fn ? spec_of(fn) : NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; sp && i < sp->n && n < cap; i++)
    {
        if (sp->is_value[i] == 2)
        {
            out[n++] = (SymId)sp->values[i];
        }
    }
    return n;
}

// The name a specialization keeps a capture of literal `lit` under: its own, so no local of the
// specialization meets it.
NameId sema_capture_name(SymId lit, NameId name)
{
    const Name *nm = name_at(name);
    char *text = (char *)arena_alloc(nm->len + 24);
    int n = snprintf(text, nm->len + 24, "m_cap%u_%.*s", (unsigned)lit, (int)nm->len, nm->start);
    return name_intern(text, (size_t)n);
}

TypeId sema_bound_type(SymId fn, NameId name)
{
    const Spec *sp = fn ? spec_of(fn) : NULL;
    for (uint32_t i = 0; sp && i < sp->n; i++)
    {
        if (!sp->is_value[i] && sp->names[i] == name)
        {
            return sp->types[i];
        }
    }
    return T_NONE;
}

int sema_bound_value(const Unit *u, NodeId id, uint64_t *out)
{
    const AstNode *n = NODE(u, id);
    if (!binding || n->kind != AST_IDENT || (u->refs && u->refs[id]))
    {
        return 0;
    }
    NameId name = token_name(TOK(u, n->tok));
    for (uint32_t i = 0; i < binding->n; i++)
    {
        if (binding->is_value[i] && binding->names[i] == name)
        {
            *out = binding->values[i];
            return 1;
        }
    }
    return 0;
}

// The arms a specialization takes while compiling (check_match), for the lowering and C: by
// (specialization, match node), since each specialization parses the body again.
#define TAKEN_MAX 4096
static struct
{
    SymId fn;
    NodeId match, arm;
} taken[TAKEN_MAX];
static uint32_t ntaken;

// Values computed while compiling for nodes of a specialization (a load's offset): by
// (specialization, node), as the taken arms are.
#define KNOWN_MAX 4096
static struct
{
    SymId fn;
    NodeId node;
    uint64_t value;
} known[KNOWN_MAX];
static uint32_t nknown;


static void forget_known(SymId fn)
{
    uint32_t k = 0;
    for (uint32_t i = 0; i < nknown; i++)
    {
        if (known[i].fn != fn)
        {
            known[k++] = known[i];
        }
    }
    nknown = k;
}

static void forget_taken(SymId fn)
{
    forget_known(fn);
    uint32_t k = 0;
    for (uint32_t i = 0; i < ntaken; i++)
    {
        if (taken[i].fn != fn)
        {
            taken[k++] = taken[i];
        }
    }
    ntaken = k;
}

int sema_known(SymId fn, NodeId id, uint64_t *out)
{
    for (uint32_t i = nknown; i > 0 && fn; i--)
    {
        if (known[i - 1].fn == fn && known[i - 1].node == id)
        {
            *out = known[i - 1].value;
            return 1;
        }
    }
    return 0;
}

NodeId sema_taken_arm(SymId fn, NodeId match)
{
    for (uint32_t i = ntaken; i > 0; i--)
    {
        if (taken[i - 1].fn == fn && taken[i - 1].match == match)
        {
            return taken[i - 1].arm;
        }
    }
    return 0;
}

int sema_capturing(void)
{
    return capture_mir;
}

int sema_spec_index(SymId fn)
{
    const Spec *sp = spec_of(fn);
    return sp ? (int)(sp - specs) + 1 : 0;
}

// While reading fn's signature (or body): its bound names, if it is a specialization.
static const Spec *bind_for(SymId fn)
{
    const Spec *old = binding;
    binding = spec_of(fn);
    return old;
}

static TypeId fn_ret_type(Unit *u, SymId fn)
{
    u = &cx.units[SYM(fn)->unit];
    NodeId decl = SYM(fn)->decl;
    NodeId callable = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
    if (NODE(u, callable)->kind == AST_FN_LIT && (NODE(u, callable)->flags & AST_F_ONELINE) &&
        !fn_ret_node(u, callable))
    {
        // `(k: u64): k * limit`: the expression's type, once its body is checked
        NodeId body = fn_body(u, callable);
        TypeId t = u->types && body < u->side_cap ? u->types[body] : T_NONE;
        return t == T_INT_LIT ? T_INT : t == T_NONE ? T_VOID : t;
    }
    const Spec *old = bind_for(fn);
    TypeId t = resolve_type(u, fn_ret_node(u, callable), SYM(fn)->owner, 0);
    binding = old;
    return t;
}

// The parameters a caller supplies: everything after `self`.
static NodeId fn_params(const Unit *u, SymId fn)
{
    u = &cx.units[SYM(fn)->unit]; // a declaration is read in its own file
    NodeId params = callable_node(u, SYM(fn)->decl)->a;
    if (params &&
        is_self_token(TSTART(TOK(u, NODE(u, params)->tok)), (int)TOK(u, NODE(u, params)->tok)->len))
    {
        return NEXT(u, params);
    }
    return params;
}

// The type of a parameter. A context parameter (`arena: arena`) is a reference to the struct,
// as `self` is: the caller's value is shared, never copied.
static TypeId param_type(Unit *u, NodeId param, SymId owner, int loud)
{
    if (!NODE(u, param)->a && (NODE(u, param)->flags & AST_F_PATTERN)) // `(0..10) { ... }`: its first call's
    {
        return u->types && param < u->side_cap ? u->types[param] : T_NONE;
    }
    if (!NODE(u, param)->a && u->types && param < u->side_cap && u->types[param]) // `x: x * 2`: given by the callee
    {
        return u->types[param];
    }
    if (NODE(u, param)->a && NODE(u, NODE(u, param)->a)->kind == AST_TYPE_FUNC)
    {
        NameId name = token_name(TOK(u, NODE(u, param)->tok)); // the function a specialization was made for
        for (uint32_t b = 0; binding && b < binding->n; b++)
        {
            if (binding->names[b] == name && binding->is_value[b] == 2)
            {
                return type_make(T_FUNC, (SymId)binding->values[b]);
            }
        }
        return type_make(T_FUNC, 0);
    }
    TypeId t = resolve_type(u, NODE(u, param)->a, owner, loud);
    if (!(NODE(u, param)->flags & AST_F_CONTEXT))
    {
        return t;
    }
    if (t == T_STR && text_is(TOK(u, NODE(u, param)->tok), "site"))
    {
        return T_STR; // `site: site`: the one context that is not a struct, a string by value
    }
    if (type_kind(t) != T_STRUCT)
    {
        if (loud && t != T_NONE)
        {
            const Token *n = TOK(u, NODE(u, param)->tok);
            diag_at(u, NODE(u, param)->tok, "error",
                    "'%.*s: %.*s' names a parameter after its type, which makes it a context "
                    "parameter, and a context must be a struct",
                    n->len, TSTART(n), n->len, TSTART(n));
        }
        return t;
    }
    return type_make(T_PTR, t);
}

// How many context parameters a definition has: a call may leave all of them out.
static int count_contexts(const Unit *u, SymId fn)
{
    u = &cx.units[SYM(fn)->unit];
    int n = 0;
    for (NodeId p = fn_params(u, fn); p; p = NEXT(u, p))
    {
        n += (NODE(u, p)->flags & AST_F_CONTEXT) != 0;
    }
    return n;
}

// The type of a field or a module-level constant.
static TypeId value_type(Unit *u, SymId s)
{
    u = &cx.units[SYM(s)->unit];
    const AstNode *decl = NODE(u, SYM(s)->decl);
    if (decl->kind == AST_FIELD_DECL)
    {
        TypeId t = resolve_type(u, decl->a, SYM(s)->owner, 0);
        // a device (`uart0: pl011 @ address`) is a reference to the registers at that address
        return (decl->flags & AST_F_PLACED) && !SYM(s)->owner ? type_make(T_PTR, t) : t;
    }
    if (decl->kind == AST_DEFINE)
    {
        const AstNode *v = NODE(u, decl->a);
        const Token *t = TOK(u, v->tok);
        if (v->kind == AST_NUMBER_LIT)
        {
            // unsized until it meets a type, as a literal is (docs/comptime.md §3.1)
            return memchr(TSTART(t), '.', t->len) ? T_INT : T_INT_LIT;
        }
        if (v->kind == AST_STRING_LIT)
        {
            return T_STR;
        }
        if (v->kind == AST_IDENT && (text_is(t, "true") || text_is(t, "false")))
        {
            return T_BOOL;
        }
        // Any other value: the type the checker gave it (`d := area(1) * 2` is an i32, `p :=
        // pt.make(1)` a pt), once
        // the declaration is checked; an unsized literal expression is an int, as a literal is.
        if (v->kind != AST_FN_LIT && !ast_import_root(u, decl->a) && u->types && decl->a < u->side_cap)
        {
            TypeId got = u->types[decl->a];
            if (got == T_INT_LIT)
            {
                return T_INT_LIT; // `k := n * 3 + 1`: still unsized
            }
            if (got == T_VOID && v->kind == AST_CALL)
            {
                return T_VOID; // `a := show(1)`: a call run while compiling for what it does, no value
            }
            if (got && got != T_NONE && got != T_EXTERN && got != T_MODULE &&
                (type_is_int(got) || got == T_BOOL || got == T_STR || type_kind(got) == T_STRUCT))
            {
                return got;
            }
        }
    }
    return T_EXTERN;
}

// `n, e := f(...)` where f takes the `arena` context: does an error give the arena back? Only when
// nothing the call allocates can stay reachable after it: its value holds no reference, no other
// argument is one, and neither f nor a function it calls uses a mutable module variable. Decided
// here, from what the compiler knows of the call; --audit says which, and why not.
static void arena_decision(Check *c, NodeId define, NodeId call, SymId callee, TypeId vt)
{
    Unit *u = c->u;
    NodeId arena = 0;
    const char *why = NULL;
    for (NodeId a = NODE(u, call)->b; a; a = NEXT(u, a))
    {
        const AstNode *an = NODE(u, a);
        TypeId at = u->types[a];
        int context = an->kind == AST_IDENT && type_kind(at) == T_PTR && type_kind(type_ref(at)) == T_STRUCT;
        if (context)
        {
            int sl;
            const char *sn = symbol_name(type_ref(type_ref(at)), &sl);
            const Token *t = TOK(u, an->tok);
            context = (int)t->len == sl && memcmp(TSTART(t), sn, (size_t)sl) == 0; // `arena`, `out`: a context
            if (context && sl == 5 && memcmp(sn, "arena", 5) == 0)
            {
                arena = a;
                continue;
            }
        }
        if (!context && (type_kind(at) == T_PTR || at == T_STR))
        {
            why = "an argument is a reference, which could keep what it allocates";
        }
    }
    if (!arena)
    {
        return;
    }
    int cl;
    const char *cn = symbol_name(callee, &cl);
    char text[300];
    if (!why && !(type_is_int(vt) || vt == T_BOOL || vt == T_ERROR || vt == T_VOID || vt == T_NONE))
    {
        why = "its value could hold a reference to what it allocates";
    }
    if (!why && uses_globals(callee))
    {
        errors_close();
        why = callee < ef_cap && (cl_flags[callee] & 4)
                  ? "it, or a function it calls, uses a mutable module variable, which could keep what it allocates"
                  : "what it does is not known here yet";
    }
    if (!why)
    {
        NODE(u, define)->aux |= DEFINE_ROLLBACK;
        snprintf(text, sizeof(text), "'%.*s': on an error the arena goes back to where it was at the call", cl, cn);
    }
    else
    {
        snprintf(text, sizeof(text), "'%.*s': the arena keeps what it allocated on an error: %s", cl, cn, why);
    }
    audit_boundary(u, NODE(u, call)->tok, text);
}

// ---- captures (context.md §60) -----------------------------------------------------------------
//
// A function literal that reads locals of the function around it takes them as parameters of its
// own, after the ones it declares, and every call passes them (lambda lifting): no closure object.
// It never escapes (a literal is only called), so a call always has them in scope. A capture is
// read, never assigned, and is one slot (a number, a bool, a string, an error, a reference).
static Capture *captures;
static uint32_t ncaptures, captures_cap;

static void note_capture(Check *c, const Token *t, const Local *l)
{
    NameId name = token_name(t);
    for (uint32_t i = 0; i < ncaptures; i++)
    {
        if (captures[i].lit == c->literal && captures[i].name == name)
        {
            return;
        }
    }
    TypeId ty = l->type;
    int one = type_is_int(ty) || ty == T_BOOL || ty == T_STR || ty == T_ERROR || type_kind(ty) == T_PTR ||
              type_kind(ty) == T_STRUCT; // a struct: its value, copied at each call
    if (!one)
    {
        char x[64];
        diag_tok(c->u, t, "error", "a literal reads '%.*s' of the function around it, a %s: a number, a bool, a "
                 "[]u8, an error, a reference or a struct can be read so, an array not yet (pass it as a parameter)",
                 t->len, TSTART(t), type_str(ty, x, sizeof(x)));
        return;
    }
    if (ncaptures == captures_cap)
    {
        captures_cap = captures_cap ? captures_cap * 2 : 16;
        captures = (Capture *)realloc(captures, captures_cap * sizeof(Capture));
    }
    captures[ncaptures++] = (Capture){c->literal, name, ty == T_INT_LIT ? T_INT : ty};
}

uint32_t sema_captures(SymId lit, Capture *out, uint32_t cap)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ncaptures; i++)
    {
        if (captures[i].lit == lit && n < cap)
        {
            out[n++] = captures[i];
        }
    }
    return n;
}

// ---- names -----------------------------------------------------------------------------------

static void push_local(Check *c, uint32_t tok, TypeId type, int mut)
{
    const Token *t = TOK(c->u, tok);
    NameId name = token_name(t);
    for (int i = c->block_base; i < c->nlocals; i++)
    {
        if (c->locals[i].name == name)
        {
            diag_at(c->u, tok, "error", "'%.*s' is already declared in this block", t->len,
                    TSTART(t));
            return;
        }
    }
    if (c->nlocals == MAX_LOCALS)
    {
        diag_at(c->u, tok, "error", "too many locals in one function (limit is %d)", MAX_LOCALS);
        return;
    }
    c->locals[c->nlocals++] = (Local){tok, type, name, (uint8_t)mut, 0, 0, 0, 0, 0, 0, 0, 0};
}

// `a := f(x)` holds the call itself, its arguments bound, when the body calls `a()` after it:
// use decides, as for visibility (std.md §13). Read from the tokens, since the uses come after.
static int called_bare_later(const Unit *u, uint32_t name_tok)
{
    const Token *name = TOK(u, name_tok);
    int depth = 0;
    for (size_t i = name_tok + 1; i + 2 < u->tokens.size; i++)
    {
        const Token *t = TOK(u, i);
        if (t->kind == TOK_EOF)
        {
            return 0;
        }
        if (t->kind == TOK_LBRACE)
        {
            depth++;
        }
        else if (t->kind == TOK_RBRACE && --depth < 0)
        {
            return 0; // the end of the block the binding is in
        }
        else if (t->kind == TOK_IDENTIFIER && t->len == name->len && memcmp(TSTART(t), TSTART(name), t->len) == 0 &&
                 TOK(u, i - 1)->kind != TOK_DOT && TOK(u, i + 1)->kind == TOK_LPAREN &&
                 TOK(u, i + 2)->kind == TOK_RPAREN)
        {
            return 1;
        }
    }
    return 0;
}

// Is the name at name_tok read after it, before the end of its block? (`n, e := f()`: an e no
// line reads is an error no one handles.)
static int mentioned_later(const Unit *u, uint32_t name_tok)
{
    const Token *name = TOK(u, name_tok);
    int depth = 0;
    for (size_t i = name_tok + 1; i < u->tokens.size; i++)
    {
        const Token *t = TOK(u, i);
        if (t->kind == TOK_EOF || (t->kind == TOK_RBRACE && --depth < 0))
        {
            return 0;
        }
        if (t->kind == TOK_LBRACE)
        {
            depth++;
        }
        else if (t->kind == TOK_IDENTIFIER && t->len == name->len && memcmp(TSTART(t), TSTART(name), t->len) == 0 &&
                 TOK(u, i - 1)->kind != TOK_DOT)
        {
            return 1;
        }
    }
    return 0;
}

// The call `a := f(x)` can be bound: a Modal function, no primitive, and no array argument
// (an array is passed by its address, which would not be the value it had at the binding).
// 1: it can; 0: not a call to bind; -1: it cannot, reported.
static int bindable_call(Check *c, NodeId call)
{
    Unit *u = c->u;
    SymId fn = u->refs[call];
    if (NODE(u, call)->kind != AST_CALL || NODE(u, call)->aux != PRIM_NONE || !fn || SYM(fn)->kind != SYM_METHOD ||
        (NODE(u, call)->flags & AST_F_SINK))
    {
        return 0;
    }
    if (NODE(u, NODE(u, call)->a)->kind == AST_SELECTOR &&
        type_kind(u->types[NODE(u, NODE(u, call)->a)->a]) == T_TYPE)
    {
        return 0; // `T.m()`: a fresh value per call, not one bound
    }
    Unit *home = &cx.units[SYM(fn)->unit];
    for (NodeId p = sema_fn_params(home, fn); p; p = NEXT(home, p))
    {
        if (type_kind(sema_param_type(home, fn, p)) == T_ARRAY)
        {
            diag_at(u, NODE(u, call)->tok, "error",
                    "this call is bound to a name and called later, but an array is passed by its address, "
                    "not by its value at the binding: call it where it is used");
            return -1;
        }
    }
    return 1;
}

static Scope *self_scope(const Check *c)
{
    return c->owner ? struct_scope(c->owner) : c->u->scope;
}

static TypeId self_type(const Check *c)
{
    return c->owner ? type_make(T_STRUCT, c->owner) : T_MODULE;
}

static NameKind find_name(Check *c, const Token *t, SymId *sym, TypeId *type)
{
    *sym = 0;
    *type = T_EXTERN;
    c->found_call = 0;
    if (is_self_token(TSTART(t), (int)t->len))
    {
        *type = self_type(c);
        return c->has_self ? NAME_SELF : NAME_MISSING;
    }
    NameId name = token_name(t);
    for (int i = c->nlocals - 1; i >= 0; i--)
    {
        if (c->locals[i].name == name)
        {
            if (c->literal && i < c->fn_base)
            {
                // a closure: every literal between this one and the local's own function reads it
                // (an outer one passes it on to the inner one's calls)
                SymId inner = c->literal;
                for (int j = 0; j < c->nlits; j++)
                {
                    if (i < c->lit_base[j])
                    {
                        c->literal = c->lits[j];
                        SYM(c->lits[j])->flags |= SYM_F_CAPTURES;
                        note_capture(c, t, &c->locals[i]);
                    }
                }
                c->literal = inner;
            }
            *type = c->locals[i].type;
            c->found_nonzero = c->locals[i].nonzero;
            c->found_call = c->locals[i].call;
            return NAME_LOCAL;
        }
    }
    char first = TSTART(t)[0];
    if ((first == 't' && text_is(t, "true")) || (first == 'f' && text_is(t, "false")))
    {
        *type = T_BOOL;
        return NAME_BUILTIN;
    }
    if (first == 'n' && text_is(t, "nil"))
    {
        *type = T_NIL;
        return NAME_BUILTIN;
    }
    if (t->kind == TOK_WRITE)
    {
        return NAME_BUILTIN;
    }
    if ((*sym = scope_find_token(c->u->scope, t)))
    {
        return NAME_SYMBOL;
    }
    if (c->implicit_self && c->has_self && c->owner && (*sym = scope_find_token(self_scope(c), t)))
    {
        return NAME_MEMBER;
    }
    if ((*sym = module_domain_function(c->u, t)))
    {
        return NAME_SYMBOL; // a function of the domain's mod.modal
    }
    // the language's primitives (docs/std.md §2), after every declaration: a program's own wins
    if (text_is(t, "sizeof") || text_is(t, "alignof") || text_is(t, "offsetof") || text_is(t, "fieldsof") ||
        text_is(t, "load") ||
        text_is(t, "typeof") || text_is(t, "trap") || text_is(t, "panic") || type_is_int(prim_type(t)))
    {
        return NAME_BUILTIN;
    }
    return module_wildcard_provides(c->u, TSTART(t), t->len) ? NAME_WILDCARD : NAME_MISSING;
}

// The type a declaration has when it is named as a value.
static TypeId symbol_type(Check *c, SymId s)
{
    switch (SYM(s)->kind)
    {
    case SYM_METHOD:
        return type_make(T_FUNC, s);
    case SYM_STRUCT:
        return type_make(T_TYPE, s);
    case SYM_FIELD:
    case SYM_CONST:
        return value_type(c->u, s);
    case SYM_IMPORT: // `point := import("./geo").shapes.point`: what it names, when that is known
    {
        SymId target = module_binding_target(s);
        return target && SYM(target)->kind != SYM_IMPORT ? symbol_type(c, target) : T_EXTERN;
    }
    default:
        return T_EXTERN;
    }
}

// If some struct of this file has a member with this name, says how to reach it.
static void report_missing(Check *c, uint32_t tok, int is_call)
{
    const Token *t = TOK(c->u, tok);

    if (is_self_token(TSTART(t), (int)t->len))
    {
        diag_at(c->u, tok, "error", "`self` is used in a function that does not declare it");
        return;
    }
    for (SymId id = 1; id < cx.syms.size; id++)
    {
        const Symbol *s = SYM(id);
        int len, tlen;
        const char *name = symbol_name(id, &len);
        if (s->unit != c->u->id || !s->owner || (uint32_t)len != t->len ||
            memcmp(name, TSTART(t), t->len) != 0)
        {
            continue;
        }
        const char *type = symbol_name(s->owner, &tlen);
        if (s->owner == c->owner && c->has_self)
        {
            diag_at(c->u, tok, "error",
                    "'%.*s' is a member of '%.*s': write `self.%.*s` (or set `implicit_self := "
                    "true` in the domain's mod.modal)",
                    len, name, tlen, type, len, name);
        }
        else
        {
            diag_at(c->u, tok, "error",
                    "'%.*s' is a member of '%.*s', not a free name: reach it through a value, "
                    "`value.%.*s`",
                    len, name, tlen, type, len, name);
        }
        return;
    }
    diag_at(c->u, tok, "error",
            is_call ? "call to undeclared function '%.*s'" : "'%.*s' is not declared", t->len,
            TSTART(t));
}

// `print := import("std").io.print`: the one std function the compiler knows. It prints its
// arguments and returns nothing.
static int is_std_path(const Unit *u, SymId s, const char *member, const char *module)
{
    if (!s || SYM(s)->kind != SYM_IMPORT || NODE(u, SYM(s)->decl)->kind != AST_DEFINE)
    {
        return 0;
    }
    const char *const path[] = {member, module};
    NodeId id = NODE(u, SYM(s)->decl)->a;
    for (int i = 0; i < 2; i++)
    {
        const AstNode *n = NODE(u, id);
        const Token *t = TOK(u, n->tok);
        if (n->kind != AST_SELECTOR || t->len != strlen(path[i]) ||
            memcmp(TSTART(t), path[i], t->len) != 0)
        {
            return 0;
        }
        id = n->a;
    }
    const Token *arg = TOK(u, NODE(u, id)->tok);
    return NODE(u, id)->kind == AST_IMPORT &&
           ((arg->len == 3 && memcmp(TSTART(arg), "std", 3) == 0) ||
            (arg->len == 5 && memcmp(TSTART(arg), "\"std\"", 5) == 0));
}

int sema_std_member(const Unit *u, SymId s)
{
    return is_std_path(u, s, "print", "io")       ? STD_PRINT
           : is_std_path(u, s, "put", "sys")      ? STD_SYS_PUT
                                                  : STD_NONE;
}


int sema_unsized_value(const Unit *u, NodeId id, uint64_t *bits, int *negative)
{
    const Unit *home;
    NodeId init;
    Exact v;
    if (!u->refs || !unsized_const(u->refs[id], &home, &init) || !exact_value(home, init, &v) ||
        v < -(Exact)0x8000000000000000ull || v > (Exact)UINT64_MAX)
    {
        return 0;
    }
    *negative = v < 0;
    *bits = (uint64_t)v;
    return 1;
}


int sema_site_text(const Unit *u, NodeId site, char *text, size_t cap)
{
    const Token *callee = TOK(u, NODE(u, site)->tok), *open = callee + 1;
    int line, col;
    source_pos(u->src, TSTART(callee), &line, &col);
    const char *from = TSTART(callee);
    const char *to = open->kind == TOK_LPAREN && open->match ? TSTART(TOK(u, open->match)) + 1 : from + callee->len;
    int k = snprintf(text, cap, "%s:%d:%d: %.*s", u->path, line, col, (int)(to - from), from);
    return k < 0 ? 0 : k >= (int)cap ? (int)cap - 1 : k;
}

int sema_primitive(const Unit *u, NodeId call)
{
    return NODE(u, call)->kind == AST_CALL ? NODE(u, call)->aux : PRIM_NONE;
}

static int is_type_query(int prim)
{
    return prim == PRIM_SIZEOF || prim == PRIM_ALIGNOF || prim == PRIM_OFFSETOF || prim == PRIM_FIELDSOF ||
           prim == PRIM_TYPEID;
}

int sema_sizeof(const Unit *u, NodeId call, uint64_t *out)
{
    int prim = sema_primitive(u, call);
    NodeId arg = NODE(u, call)->kind == AST_CALL ? NODE(u, call)->b : 0;
    if (!is_type_query(prim) || !arg || !u->types)
    {
        return 0;
    }
    if (prim == PRIM_OFFSETOF)
    {
        SymId field = u->refs ? u->refs[arg] : 0; // check_type_query put the field there
        if (!field || SYM(field)->kind != SYM_FIELD)
        {
            return 0;
        }
        *out = field_offset_of(field);
        return 1;
    }
    TypeId t = u->types[arg]; // what it asks about (check_type_query put it there)
    if (type_kind(t) == T_TYPE)
    {
        t = type_make(T_STRUCT, type_ref(t));
    }
    if (t == T_NONE || t == T_EXTERN || t == T_INT_LIT)
    {
        return 0;
    }
    if (prim == PRIM_TYPEID)
    {
        return 0; // a `type` value, not a number (sema_typeof_desc)
    }
    if (prim == PRIM_FIELDSOF)
    {
        uint64_t n = 0;
        if (type_kind(t) == T_STRUCT)
        {
            SymId s = type_ref(t);
            const Unit *home = &cx.units[SYM(s)->unit];
            for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
            {
                n += NODE(home, m)->kind == AST_FIELD_DECL;
            }
        }
        *out = n;
        return 1;
    }
    Layout l = type_layout(t);
    if (!l.size)
    {
        return 0;
    }
    *out = prim == PRIM_ALIGNOF ? l.align : l.size;
    return 1;
}

// `sizeof(u32)`, `alignof(header)`, `fieldsof(p)`, `typeof(point)`, `offsetof(point.y)`: what is
// asked about is a type or a value (its type), or for offsetof a field named through its
// struct. The result is an unsized constant: nothing is left for run time, and no name is.
static TypeId check_type_query(Check *c, NodeId id, uint32_t at, int prim)
{
    static const char *const names[] = {[PRIM_SIZEOF] = "sizeof", [PRIM_ALIGNOF] = "alignof",
                                        [PRIM_OFFSETOF] = "offsetof", [PRIM_FIELDSOF] = "fieldsof",
                                        [PRIM_TYPEID] = "typeof"};
    Unit *u = c->u;
    NodeId arg = NODE(u, id)->b;
    const char *what = names[prim];
    if (!arg || NEXT(u, arg))
    {
        diag_at(u, at, "error", prim == PRIM_OFFSETOF ? "offsetof takes one field: `offsetof(T.field)`"
                                                      : "%s takes one type or value: `%s(T)`",
                what, what);
        for (NodeId a = arg; a; a = NEXT(u, a))
        {
            check_expr(c, a);
        }
        return T_NONE;
    }
    const AstNode *a = NODE(u, arg);
    if (prim == PRIM_OFFSETOF)
    {
        SymId s = a->kind == AST_SELECTOR ? struct_of(check_expr(c, a->a)) : 0;
        SymId field = s ? field_named(s, TOK(u, a->tok)) : 0;
        if (!field)
        {
            diag_at(u, a->tok, "error", "offsetof takes a field of a struct: `offsetof(T.field)`");
            return T_NONE;
        }
        u->refs[arg] = field;
        set_type(c, arg, value_type(u, field));
        return T_INT_LIT;
    }
    TypeId t = a->kind == AST_IDENT ? prim_type(TOK(u, a->tok)) : T_NONE;
    for (uint32_t b = 0; binding && a->kind == AST_IDENT && t == T_NONE && b < binding->n; b++)
    {
        if (!binding->is_value[b] && binding->names[b] == token_name(TOK(u, a->tok)))
        {
            t = binding->types[b]; // `fieldsof(T)` in a specialization: the type T was given
        }
    }
    if (t == T_NONE)
    {
        t = check_expr(c, arg);
    }
    set_type(c, arg, t);
    uint64_t v;
    if (t == T_NONE)
    {
        return T_NONE;
    }
    if (prim == PRIM_TYPEID)
    {
        SymId st = sema_std_type();
        if (!st)
        {
            diag_at(u, at, "error", "typeof gives std's type: bind it first, `type := import(\"std\").type`");
            return T_NONE;
        }
        if (t == T_INT_LIT)
        {
            diag_at(u, a->tok, "error", "typeof: an unsized constant has no type yet: give it one (`u8 x = 3`)");
            return T_NONE;
        }
        return type_make(T_STRUCT, st); // read from a's descriptor at run time when a is an *any
    }
    if (!sema_sizeof(u, id, &v))
    {
        char name[64];
        diag_at(u, a->tok, "error",
                t == T_INT_LIT ? "%s: an unsized constant has no type yet: give it one (`u8 x = 3`)"
                               : "%s: %s has no layout in memory",
                what, type_str(t, name, sizeof(name)));
        return T_NONE;
    }
    return T_INT_LIT;
}


// A type written as a value known while compiling (`T.at(i).of`: std's type, or a reference to
// one): the type it stands for, or T_NONE.
static TypeId sema_load_type(Check *c, NodeId e)
{
    Unit *u = c->u;
    TypeId vt = u->types[e];
    SymId st = sema_std_type();
    if (!st || !(vt == type_make(T_STRUCT, st) || vt == type_make(T_PTR, type_make(T_STRUCT, st))))
    {
        return T_NONE;
    }
    TypeId t;
    return comptime_type_value(u, e, &t) ? t : T_NONE;
}

// `load(T, p, offset)`: the T at p + offset bytes, the one primitive that reads memory as a type
// (std's reflection is written over it). T is known while compiling: a type's name, or a type a
// specialization was made for. The offset may be known when the program runs.
static TypeId check_load(Check *c, NodeId id, uint32_t at)
{
    Unit *u = c->u;
    NodeId tn = NODE(u, id)->b, p = tn ? NEXT(u, tn) : 0, off = p ? NEXT(u, p) : 0;
    if (!tn || !p || !off || NEXT(u, off))
    {
        diag_at(u, at, "error", "load takes a type, a reference and a byte offset: `load(T, p, offset)`");
        for (NodeId a = tn; a; a = NEXT(u, a))
        {
            check_expr(c, a);
        }
        return T_NONE;
    }
    check_expr(c, tn); // (a struct's name resolves here)
    TypeId t = sema_named_type(u, tn);
    if (t == T_NONE)
    {
        t = sema_load_type(c, tn);
    }
    if (t == T_NONE)
    {
        diag_at(u, NODE(u, tn)->tok, "error", "load's type is known while compiling: a type's name (`u32`, `point`)");
        check_expr(c, p);
        check_expr(c, off);
        return T_NONE;
    }
    set_type(c, tn, t);
    TypeId pt = check_expr(c, p);
    if (type_kind(pt) != T_PTR)
    {
        char name[64];
        diag_at(u, NODE(u, p)->tok, "error", "load reads through a reference, not %s",
                type_str(pt, name, sizeof(name)));
        return T_NONE;
    }
    TypeId ot = check_expr(c, off);
    if (!type_is_int(ot) && ot != T_NONE)
    {
        diag_at(u, NODE(u, off)->tok, "error", "load's offset is a number of bytes");
    }
    uint64_t k;
    if (ot != T_NONE && !known_value(u, off, &k) && c->fn && spec_of(c->fn) && nknown < KNOWN_MAX &&
        comptime_value_u64(u, off, &k)) // `T.at(i).offset` in a specialization: a constant
    {
        known[nknown].fn = c->fn;
        known[nknown].node = off;
        known[nknown++].value = k;
    }
    return t;
}

// `u8(x)`: an integer (or bool) converted to the type named, wrapping as a C cast does.
static TypeId check_convert(Check *c, NodeId id, uint32_t at, TypeId to)
{
    Unit *u = c->u;
    NodeId arg = NODE(u, id)->b;
    char name[64];
    if (!arg || NEXT(u, arg))
    {
        diag_at(u, at, "error", "a conversion takes one value: `%s(x)`", type_str(to, name, sizeof(name)));
        for (NodeId a = arg; a; a = NEXT(u, a))
        {
            check_expr(c, a);
        }
        return to;
    }
    TypeId t = check_expr(c, arg);
    if (!type_is_int(t) && t != T_BOOL && t != T_NONE && t != T_EXTERN)
    {
        char got[64];
        diag_at(u, NODE(u, arg)->tok, "error", "%s(x) converts an integer, but this is %s",
                type_str(to, name, sizeof(name)), type_str(t, got, sizeof(got)));
    }
    return to;
}

// `trap(a, b, ...)`: strings to the error output, after the program's own output, then stop.
static TypeId check_trap(Check *c, NodeId id, uint32_t at)
{
    Unit *u = c->u;
    {
        int al = 0;
        const char *an = c->fn ? symbol_name(c->fn, &al) : "";
        note_panic(c, al == 6 && memcmp(an, "assert", 6) == 0 && strstr(cx.units[SYM(c->fn)->unit].path, "std/")
                          ? PANIC_ASSERT
                          : PANIC_RAISED);
    }
    if (!NODE(u, id)->b)
    {
        diag_at(u, at, "error", "trap takes the message to stop with: `trap(\"why\")`");
    }
    for (NodeId a = NODE(u, id)->b; a; a = NEXT(u, a))
    {
        TypeId t = check_expr(c, a);
        if (t != T_STR && t != T_NONE && t != T_EXTERN)
        {
            char got[64];
            diag_at(u, NODE(u, a)->tok, "error", "trap writes strings ([]u8), but this is %s",
                    type_str(t, got, sizeof(got)));
        }
    }
    return T_VOID;
}

int sema_is_std_print(const Unit *u, SymId s)
{
    return sema_std_member(u, s) == STD_PRINT;
}

// ---- invocations -----------------------------------------------------------------------------

// How many parameters a definition takes (after `self`), read in its own file.
static int arity_of(SymId fn)
{
    const Unit *home = &cx.units[SYM(fn)->unit];
    return count_list(home, fn_params(home, fn));
}

// Picks the definition whose parameter count matches (any one, for an overload set).
// Returns 0 after reporting when none does.
static SymId select_overload(Check *c, uint32_t at, SymId first, int argc)
{
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        int arity = arity_of(s), contexts = count_contexts(c->u, s);
        if (arity == argc || (contexts && arity - contexts == argc))
        {
            return s;
        }
    }

    // None fits: only now is the list of accepted counts written out.
    char accepted[96];
    size_t len = 0;
    int defs = 0;
    accepted[0] = '\0';
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        int arity = arity_of(s);
        if (len < sizeof(accepted) - 8)
        {
            len += (size_t)snprintf(accepted + len, sizeof(accepted) - len, "%s%d",
                                    defs ? " or " : "", arity);
        }
        defs++;
    }

    int nlen, line, col;
    const char *name = symbol_name(first, &nlen);
    const Unit *home = &cx.units[SYM(first)->unit]; // where it is declared
    source_pos(home->src, TSTART(TOK(home, SYM(first)->tok)), &line, &col);
    diag_at(c->u, at, "error", "'%.*s' takes %s argument%s, but %d %s given (declared at %s:%d)",
            nlen, name, accepted, (defs == 1 && accepted[0] == '1' && !accepted[1]) ? "" : "s",
            argc, argc == 1 ? "is" : "are", home->path, line);
    return 0;
}

// Types the arguments and, when the definition is known, checks each against its parameter.
static void check_args(Check *c, NodeId args, SymId fn)
{
    Unit *u = c->u;
    Unit *home = fn ? &cx.units[SYM(fn)->unit] : u; // where the parameters are declared
    NodeId param = fn ? fn_params(u, fn) : 0;
    int index = 1;

    int pretyped = c->pretyped;
    c->pretyped = 0;
    for (NodeId a = args; a; a = NEXT(u, a), index++)
    {
        TypeId at = pretyped ? u->types[a] : check_expr(c, a);
        if (at == T_VOID)
        {
            diag_at(u, NODE(u, a)->tok, "error", "argument %d has no value", index);
        }
        if (!param)
        {
            continue;
        }
        const Spec *old_binding = bind_for(fn);
        TypeId pt = param_type(&cx.units[SYM(fn)->unit], param, SYM(fn)->owner, 0);
        binding = old_binding;
        TypeId named = sema_named_type(u, a);
        if (named != T_NONE && sema_std_type() && pt == type_make(T_STRUCT, sema_std_type()))
        {
            // `describe(point)`, `describe(u32)`: a type where std's type is taken is its
            // descriptor, a value the compiler makes (as typeof(point) is)
            set_type(c, a, pt);
            param = NEXT(home, param);
            continue;
        }
        if (fits(at, pt) && !check_bounds(c, a, pt))
        {
            param = NEXT(home, param);
            continue;
        }
        if (!fits(at, pt))
        {
            char got[64], want[64];
            int nlen;
            const char *name = symbol_name(fn, &nlen);
            const Token *pn = TOK(home, NODE(home, param)->tok);
            if (NODE(home, param)->flags & AST_F_PATTERN) // a pattern has no name: say the type
            {
                diag_at(u, NODE(u, a)->tok, "error", "argument %d of '%.*s' is %s, but it takes %s", index, nlen,
                        name, type_str(at, got, sizeof(got)), type_str(pt, want, sizeof(want)));
                param = NEXT(home, param);
                continue;
            }
            char text[128], hint[192], what[160];
            int named = value_text(u, a, text, sizeof(text));
            reference_hint(at, pt, named ? text : "", hint, sizeof(hint));
            if (named)
            {
                snprintf(what, sizeof(what), "argument %d of '%.*s', ", index, nlen, name);
                diag_at(u, NODE(u, a)->tok, "error", "`%s` should be %s, but it is %s%s (%sparameter '%.*s')", text,
                        type_str(pt, want, sizeof(want)), type_str(at, got, sizeof(got)), hint, what, pn->len,
                        TSTART(pn));
            }
            else
            {
                diag_at(u, NODE(u, a)->tok, "error", "argument %d of '%.*s' should be %s, but it is %s%s (parameter '%.*s')",
                        index, nlen, name, type_str(pt, want, sizeof(want)), type_str(at, got, sizeof(got)), hint,
                        pn->len, TSTART(pn));
            }
        }
        param = NEXT(home, param);
    }
}

// Calls `first` (a function or an overload set) with the arguments of call node `id`.
static void side_tables(Unit *u, NodeId from);

// A call that leaves its context arguments out gets them here: each is the caller's own value
// of that name (a local or a parameter), as a plain name, so every backend sees a full call.
// The whole argument list is laid out again as consecutive nodes (the old ones are left inert).
static void fill_contexts(Check *c, NodeId id, SymId fn, uint32_t at)
{
    Unit *u = c->u;
    const Unit *home = &cx.units[SYM(fn)->unit];
    NodeId explicit_arg = NODE(u, id)->b;
    AstNode args[256];
    int n = 0;
    for (NodeId p = fn_params(home, fn); p && n < 256; p = NEXT(home, p))
    {
        if (!(NODE(home, p)->flags & AST_F_CONTEXT))
        {
            if (explicit_arg)
            {
                NodeId next = NEXT(u, explicit_arg);
                args[n++] = *NODE(u, explicit_arg);
                NODE(u, explicit_arg)->kind = AST_NONE;
                explicit_arg = next;
            }
            continue;
        }
        const Token *name = TOK(home, NODE(home, p)->tok);
        NameId want = token_name(name);
        const Local *found = NULL;
        for (int i = c->nlocals - 1; i >= 0 && !found; i--)
        {
            found = c->locals[i].name == want ? &c->locals[i] : NULL;
        }
        if (!found && text_is(name, "site") && resolve_type((Unit *)home, NODE(home, p)->a, 0, 0) == T_STR)
        {
            args[n++] = (AstNode){.kind = AST_SITE, .tok = at}; // the compiler knows where this call is
            continue;
        }
        if (!found)
        {
            int nlen;
            const char *fname = symbol_name(fn, &nlen);
            diag_at(u, at, "error",
                    "'%.*s' needs a context '%.*s': declare `%.*s := ...` here, take "
                    "`%.*s: %.*s` as a parameter, or pass one",
                    nlen, fname, name->len, TSTART(name), name->len, TSTART(name), name->len,
                    TSTART(name), name->len, TSTART(name));
            return;
        }
        args[n++] = (AstNode){.kind = AST_IDENT, .tok = found->tok};
    }
    NodeId first = (NodeId)u->nodes.size;
    for (int i = 0; i < n; i++)
    {
        args[i].flags = (uint8_t)((args[i].flags & ~AST_F_LAST) | (i + 1 == n ? AST_F_LAST : 0));
        node_append(u, args[i]);
    }
    NODE(u, id)->b = n ? first : 0;
    side_tables(u, first); // the new nodes get their entries
}

// ---- output: print and write go to the `out` in scope --------------------------------------------
//
// The runtime is made of context parameters. A program may declare its own output: a struct
// named `out` with `put(self: out, b: u8)`. print and write then send every byte, in order, to
// the `out` in scope (a local, or the context parameter `out: out`, which every call fills in
// from its caller); a file that declares an `out` and prints where none is in scope is told so.
// Without one, bytes go to the target's system output, if it has one (TCAP_SYSOUT): on a board
// without, `out.put` is the way out (a UART). The call is rewritten with that `out` as its first
// argument and marked AST_F_SINK, and its ref is the `put` method: a backend sees an ordinary
// value and an ordinary method call.

// The `put(self: out, b: u8)` of type t (a struct `out` or a reference to one), or 0.
static SymId out_put_method(TypeId t)
{
    TypeId s = type_kind(t) == T_PTR ? type_ref(t) : t;
    if (type_kind(s) != T_STRUCT)
    {
        return 0;
    }
    SymId st = type_ref(s);
    int len;
    const char *name = symbol_name(st, &len);
    if (len != 3 || memcmp(name, "out", 3) != 0)
    {
        return 0;
    }
    for (SymId m = scope_find(scope_at(SYM(st)->scope_ref), "put", 3); m;
         m = SYM(m)->kind == SYM_METHOD ? SYM(m)->next_overload : 0)
    {
        if (SYM(m)->kind == SYM_METHOD && arity_of(m) == 1)
        {
            return m;
        }
    }
    return 0;
}

static void route_output(Check *c, NodeId id, uint32_t at)
{
    Unit *u = c->u;
    note_effect(c, FX_OUTPUT, at);
    NameId want = name_intern("out", 3);
    const Local *found = NULL;
    for (int i = c->nlocals - 1; i >= 0 && !found; i--)
    {
        found = c->locals[i].name == want ? &c->locals[i] : NULL;
    }
    SymId declared = scope_find(u->scope, "out", 3);
    int file_has_out = declared && SYM(declared)->kind == SYM_STRUCT;
    if (!found)
    {
        if (file_has_out)
        {
            diag_at(u, at, "error",
                    "this file declares its own `out`, and output goes to the `out` in scope: "
                    "there is none here. Take the context parameter `out: out`, or declare "
                    "`out := out.new()`");
        }
        else if (!(target_get()->caps & TCAP_SYSOUT))
        {
            diag_at(u, at, "error",
                    "target %s has no system output: declare `out: struct` with "
                    "`put(self: out, b: u8)` and print through it",
                    target_get()->name);
        }
        return;
    }
    SymId put = out_put_method(found->type);
    if (!put)
    {
        TypeId s = type_kind(found->type) == T_PTR ? type_ref(found->type) : found->type;
        if (file_has_out && type_kind(s) == T_STRUCT && type_ref(s) == declared)
        {
            diag_at(u, at, "error", "`out` needs a method `put(self: out, b: u8)` to be printed to");
        }
        return; // a local that happens to be called `out`: the system output
    }
    // [out, args...], laid out again as consecutive nodes (the old ones are left inert)
    AstNode args[257];
    int n = 0;
    args[n++] = (AstNode){.kind = AST_IDENT, .tok = found->tok};
    for (NodeId a = NODE(u, id)->b; a && n < 257;)
    {
        NodeId next = NEXT(u, a);
        args[n++] = *NODE(u, a);
        NODE(u, a)->kind = AST_NONE;
        a = next;
    }
    NodeId first = (NodeId)u->nodes.size;
    for (int i = 0; i < n; i++)
    {
        args[i].flags = (uint8_t)((args[i].flags & ~AST_F_LAST) | (i + 1 == n ? AST_F_LAST : 0));
        node_append(u, args[i]);
    }
    NODE(u, id)->b = first;
    NODE(u, id)->flags |= AST_F_SINK;
    side_tables(u, first);
    u->refs[id] = put;
}

// put(b): one byte, a u8, to the system output.
static void check_sys_put(Check *c, NodeId id, uint32_t at)
{
    Unit *u = c->u;
    note_effect(c, FX_OUTPUT, at);
    NodeId args = NODE(u, id)->b;
    if (!(target_get()->caps & TCAP_SYSOUT))
    {
        diag_at(u, at, "error", "target %s has no system output for std.sys.put",
                target_get()->name);
    }
    if (!args || NEXT(u, args))
    {
        diag_at(u, at, "error", "std.sys.put takes one byte: put(b: u8)");
        check_args(c, args, 0);
        return;
    }
    TypeId t = check_expr(c, args);
    if (!fits(t, T_U8))
    {
        char buf[64];
        diag_at(u, NODE(u, args)->tok, "error", "std.sys.put takes a u8, not %s",
                type_str(t, buf, sizeof(buf)));
        return;
    }
    check_bounds(c, args, T_U8);
}

static int same_group(SymId a, SymId b);

// Do two definitions of the set take this many arguments, of other types? Then the argument
// types choose (docs/std.md §7): the first definition, in the order written, that every argument
// fits, an unsized literal by its value too (`write(300)` skips `u8 write` for `u64 write`).
static int typed_overloads(SymId first, int argc)
{
    SymId seen = 0;
    for (SymId s = first; s; s = SYM(s)->kind == SYM_METHOD ? SYM(s)->next_overload : 0)
    {
        if (SYM(s)->kind != SYM_METHOD || arity_of(s) != argc)
        {
            continue;
        }
        if (!seen)
        {
            seen = s;
        }
        else if (!same_group(seen, s))
        {
            return 1;
        }
    }
    return 0;
}

static int group_head(SymId first, SymId s) // the first clause of its group: calls go to it
{
    for (SymId t = first; t && t != s; t = SYM(t)->next_overload)
    {
        if (SYM(t)->kind == SYM_METHOD && same_group(t, s))
        {
            return 0;
        }
    }
    return 1;
}

static void param_list_str(SymId fn, char *out, size_t cap) // "(u64, []u8)"
{
    const Unit *home = &cx.units[SYM(fn)->unit];
    size_t len = (size_t)snprintf(out, cap, "(");
    for (NodeId p = fn_params(home, fn); p && len < cap; p = NEXT(home, p))
    {
        char t[64];
        type_str(param_type((Unit *)home, p, SYM(fn)->owner, 0), t, sizeof(t));
        len += (size_t)snprintf(out + len, cap - len, "%s%s", p == fn_params(home, fn) ? "" : ", ", t);
    }
    if (len < cap)
    {
        snprintf(out + len, cap - len, ")");
    }
}

static SymId select_by_types(Check *c, uint32_t at, NodeId args, SymId first, int argc)
{
    Unit *u = c->u;
    for (NodeId a = args; a; a = NEXT(u, a))
    {
        check_expr(c, a);
    }
    c->pretyped = 1;
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        if (SYM(s)->kind != SYM_METHOD || arity_of(s) != argc || !group_head(first, s))
        {
            continue;
        }
        const Unit *home = &cx.units[SYM(s)->unit];
        int ok = 1;
        NodeId p = fn_params(home, s);
        for (NodeId a = args; a && p && ok; a = NEXT(u, a), p = NEXT(home, p))
        {
            TypeId pt = param_type((Unit *)home, p, SYM(s)->owner, 0);
            ok = fits(u->types[a], pt) && in_bounds(u, a, pt);
        }
        if (ok)
        {
            return s;
        }
    }
    char got[160], takes[400];
    size_t len = (size_t)snprintf(got, sizeof(got), "(");
    for (NodeId a = args; a && len < sizeof(got); a = NEXT(u, a))
    {
        char t[64];
        len += (size_t)snprintf(got + len, sizeof(got) - len, "%s%s", a == args ? "" : ", ",
                                type_str(u->types[a], t, sizeof(t)));
    }
    if (len < sizeof(got))
    {
        snprintf(got + len, sizeof(got) - len, ")");
    }
    len = 0;
    takes[0] = 0;
    for (SymId s = first; s && len < sizeof(takes); s = SYM(s)->next_overload)
    {
        if (SYM(s)->kind == SYM_METHOD && arity_of(s) == argc && group_head(first, s))
        {
            char one[160];
            param_list_str(s, one, sizeof(one));
            len += (size_t)snprintf(takes + len, sizeof(takes) - len, "%s%s", len ? ", " : "", one);
        }
    }
    int nlen;
    const char *name = symbol_name(first, &nlen);
    diag_at(u, at, "error", "no definition of '%.*s' takes %s: it takes %s", nlen, name, got, takes);
    c->pretyped = 0;
    return 0;
}

// Does this type (a node of u) name `name` anywhere (`T`, `*T`, `[4]T`)?
static int type_names(const Unit *u, NodeId t, NameId name)
{
    while (t)
    {
        const AstNode *n = NODE(u, t);
        if (n->kind == AST_TYPE_NAME)
        {
            return !n->aux && token_name(TOK(u, n->tok)) == name;
        }
        if (n->kind != AST_TYPE_PTR && n->kind != AST_TYPE_ARRAY && n->kind != AST_TYPE_SLICE)
        {
            return 0;
        }
        t = n->a;
    }
    return 0;
}

// Does an expression (a type written as a value) name `name`?
static int expr_names(const Unit *u, NodeId id, NameId name, int list, int depth)
{
    for (; id && depth < 128; id = list ? NEXT(u, id) : 0)
    {
        const AstNode *n = NODE(u, id);
        if (n->kind == AST_IDENT && token_name(TOK(u, n->tok)) == name)
        {
            return 1;
        }
        uint8_t mask = n->kind < AST__COUNT ? ast_child_mask[n->kind] : 0;
        if (((mask & 1) && n->a && expr_names(u, n->a, name, 1, depth + 1)) ||
            ((mask & 2) && n->b && expr_names(u, n->b, name, 1, depth + 1)))
        {
            return 1;
        }
    }
    return 0;
}

// The types fn's signature writes as values (AST_TYPE_EXPR nodes: of its parameters, its result).
static int sig_type_exprs(SymId fn, NodeId *out, int cap)
{
    Unit *u = &cx.units[SYM(fn)->unit];
    NodeId decl = SYM(fn)->decl;
    NodeId node = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
    int k = 0;
    NodeId ret = fn_ret_node(u, node);
    for (NodeId t = ret; t && k < cap; t = NODE(u, t)->kind == AST_TYPE_PTR ? NODE(u, t)->a : 0)
    {
        if (NODE(u, t)->kind == AST_TYPE_EXPR)
        {
            out[k++] = t;
        }
    }
    for (NodeId p = fn_params(u, fn); p && k < cap; p = NEXT(u, p))
    {
        for (NodeId t = NODE(u, p)->a; t && k < cap; t = NODE(u, t)->kind == AST_TYPE_PTR ? NODE(u, t)->a : 0)
        {
            if (NODE(u, t)->kind == AST_TYPE_EXPR)
            {
                out[k++] = t;
            }
        }
    }
    return k;
}

static int sig_exprs_name(SymId fn, NameId name)
{
    NodeId te[SPEC_MAX_BOUND];
    int n = sig_type_exprs(fn, te, SPEC_MAX_BOUND);
    const Unit *u = &cx.units[SYM(fn)->unit];
    for (int i = 0; i < n; i++)
    {
        if (expr_names(u, NODE(u, te[i])->a, name, 0, 0))
        {
            return 1;
        }
    }
    return 0;
}

// The parameters of fn that its signature takes as types (index into its parameter list, after
// self), and their names. 0: fn is not made once per type.
static int type_params(SymId fn, int *index, NameId *names)
{
    SymId st = sema_std_type();
    if (!st || SYM(fn)->kind != SYM_METHOD || spec_of(fn))
    {
        return 0;
    }
    Unit *u = &cx.units[SYM(fn)->unit];
    TypeId std_type = type_make(T_STRUCT, st);
    NodeId decl = SYM(fn)->decl;
    NodeId ret = fn_ret_node(u, NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl);
    int k = 0, i = 0;
    for (NodeId p = fn_params(u, fn); p && k < SPEC_MAX_BOUND; p = NEXT(u, p), i++)
    {
        const AstNode *pt = NODE(u, p)->a ? NODE(u, NODE(u, p)->a) : NULL;
        if (!pt || pt->kind != AST_TYPE_NAME || pt->aux || TOK(u, pt->tok)->len != 4 ||
            resolve_type(u, NODE(u, p)->a, SYM(fn)->owner, 0) != std_type)
        {
            continue; // (the name is read first: most parameters are not of std's type)
        }
        NameId name = token_name(TOK(u, NODE(u, p)->tok));
        int used = (ret && type_names(u, ret, name)) || sig_exprs_name(fn, name);
        for (NodeId q = fn_params(u, fn); q && !used; q = NEXT(u, q))
        {
            used = q != p && NODE(u, q)->a && type_names(u, NODE(u, q)->a, name);
        }
        if (used)
        {
            index[k] = i;
            names[k++] = name;
        }
    }
    return k;
}

// The names of the functions whose signature makes them generic (a parameter of std's type, or a
// type written as a value): a body that names none of them gets no parameter known while
// compiling from them. Found once per set of loaded files.
static SymId *generic_names;
static uint32_t ngeneric_names, cgeneric_names;
static uint16_t generic_names_units = 0xffff;

static int sig_generic(SymId fn)
{
    if (SYM(fn)->kind != SYM_METHOD || !SYM(fn)->decl)
    {
        return 0;
    }
    Unit *u = &cx.units[SYM(fn)->unit];
    NodeId te[1];
    if (sig_type_exprs(fn, te, 1))
    {
        return 1;
    }
    for (NodeId p = fn_params(u, fn); p; p = NEXT(u, p))
    {
        const AstNode *pt = NODE(u, p)->a ? NODE(u, NODE(u, p)->a) : NULL;
        if (pt && pt->kind == AST_TYPE_NAME && !pt->aux && TOK(u, pt->tok)->len == 4 &&
            memcmp(TSTART(TOK(u, pt->tok)), "type", 4) == 0)
        {
            return 1;
        }
        if (pt && pt->kind == AST_TYPE_FUNC)
        {
            return 1; // a function given while compiling
        }
    }
    return 0;
}

static int names_a_generic(const char *text, size_t len)
{
    if (generic_names_units != cx.nunits)
    {
        generic_names_units = cx.nunits;
        ngeneric_names = 0;
        for (SymId s = 1; s < cx.syms.size; s++)
        {
            if (SYM(s)->kind == SYM_METHOD && SYM(s)->decl && !(SYM(s)->flags & SYM_F_LITERAL) && sig_generic(s))
            {
                if (ngeneric_names == cgeneric_names)
                {
                    cgeneric_names = cgeneric_names ? cgeneric_names * 2 : 32;
                    generic_names = realloc(generic_names, cgeneric_names * sizeof(SymId));
                }
                generic_names[ngeneric_names++] = s;
            }
        }
    }
    for (uint32_t i = 0; i < ngeneric_names; i++)
    {
        int nlen;
        const char *name = symbol_name(generic_names[i], &nlen);
        if (name && nlen > 0 && memmem(text, len, name, (size_t)nlen))
        {
            return 1;
        }
    }
    return 0;
}

// A body's tokens (of a function whose body is not parsed yet), or 0 entries.
static int body_tokens(SymId fn, TokenVec *out)
{
    Unit *u = &cx.units[SYM(fn)->unit];
    NodeId decl = SYM(fn)->decl;
    NodeId node = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
    memset(out, 0, sizeof(*out));
    if (!(NODE(u, node)->flags & AST_F_LAZY))
    {
        return 0;
    }
    const Token *body = TOK(u, fn_body(u, node));
    if (body->kind != TOK_BODY || !names_a_generic(TSTART(body), body->len))
    {
        return 0; // (a body that names no generic function passes nothing known while compiling)
    }
    scan_quiet = 1;
    int errors = scan_span(u->path, u->src, TSTART(body), body->len, out);
    scan_quiet = 0;
    if (errors)
    {
        free(out->data);
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return 1;
}

static int compile_params(SymId fn, int *index, NameId *names, int *types_count);

// A function a body calls by this name (in fn's file, or through an import binding), or 0.
static SymId called_function(const Unit *u, SymId fn, const Token *name)
{
    SymId s = scope_find_token(u->scope, name);
    if (!s && SYM(fn)->owner)
    {
        s = scope_find_token(struct_scope(SYM(fn)->owner), name);
    }
    if (s && SYM(s)->kind == SYM_IMPORT)
    {
        s = module_binding_function(s);
    }
    return s && SYM(s)->kind == SYM_METHOD && s != fn ? s : 0;
}

// `g(a0, a1, ...)` in fn's body (tokens from `args` to `close`): is the name `p` alone at a
// place that g, made once per type and value, takes while compiling?
static int passes_compile_time(const Unit *u, SymId fn, const Token *callee, const Token *args,
                               const Token *close, NameId p)
{
    SymId g = called_function(u, fn, callee);
    int index[SPEC_MAX_BOUND], ntypes;
    NameId names[SPEC_MAX_BOUND];
    if (!g || !sema_is_generic(g))
    {
        return 0;
    }
    int k = compile_params(g, index, names, &ntypes);
    int arg = 0, depth = 0;
    const Token *start = args;
    for (const Token *t = args; t <= close; t++)
    {
        if (t == close || (depth == 0 && t->kind == TOK_COMMA))
        {
            if (t - start == 1 && start->kind == TOK_IDENTIFIER && start->match == p)
            {
                for (int j = ntypes; j < k; j++)
                {
                    if (index[j] == arg)
                    {
                        return 1;
                    }
                }
            }
            arg++;
            start = t + 1;
            continue;
        }
        depth += t->kind == TOK_LPAREN || t->kind == TOK_LBRACKET || t->kind == TOK_LBRACE;
        depth -= t->kind == TOK_RPAREN || t->kind == TOK_RBRACKET || t->kind == TOK_RBRACE;
    }
    return 0;
}

// The number parameters of fn its body passes to a place known while compiling: known while
// compiling, so fn is made once per value. Appended after the type parameters; their count.
// Does a parsed body pass `name` to a place known while compiling? (A body of arms is parsed
// with its declaration.)
static SymId tree_fn; // the function whose body is walked
static int tree_indexes(const Unit *u, NodeId id, NameId name, int list, int depth)
{
    for (; id && depth < 256; id = list ? NEXT(u, id) : 0)
    {
        const AstNode *n = NODE(u, id);
        if (n->kind == AST_CALL && NODE(u, n->a)->kind == AST_IDENT && tree_fn)
        {
            SymId g = called_function(u, tree_fn, TOK(u, NODE(u, n->a)->tok));
            int index[SPEC_MAX_BOUND], ntypes;
            NameId names[SPEC_MAX_BOUND];
            int k = g && sema_is_generic(g) ? compile_params(g, index, names, &ntypes) : 0;
            int arg = 0;
            for (NodeId a = n->b; a && k; a = NEXT(u, a), arg++)
            {
                if (NODE(u, a)->kind != AST_IDENT || token_name(TOK(u, NODE(u, a)->tok)) != name)
                {
                    continue;
                }
                for (int j = ntypes; j < k; j++)
                {
                    if (index[j] == arg)
                    {
                        return 1;
                    }
                }
            }
        }
        uint8_t mask = n->kind < AST__COUNT ? ast_child_mask[n->kind] : 0;
        if (((mask & 1) && n->a && tree_indexes(u, n->a, name, 1, depth + 1)) ||
            ((mask & 2) && n->b && tree_indexes(u, n->b, name, 1, depth + 1)))
        {
            return 1;
        }
    }
    return 0;
}

static int index_params(SymId fn, int *index, NameId *names, int k)
{
    TokenVec tv;
    Unit *u = &cx.units[SYM(fn)->unit];
    {
        // a number a type written as a value reads (`i` of `T.at(i).of`) is known while compiling
        int added = 0, i = 0;
        NodeId te[SPEC_MAX_BOUND];
        if (sig_type_exprs(fn, te, SPEC_MAX_BOUND))
        {
            for (NodeId p = fn_params(u, fn); p && k + added < SPEC_MAX_BOUND; p = NEXT(u, p), i++)
            {
                NameId name = token_name(TOK(u, NODE(u, p)->tok));
                TypeId pt = NODE(u, p)->a ? resolve_type(u, NODE(u, p)->a, SYM(fn)->owner, 0) : T_NONE;
                if (type_is_int(pt) && sig_exprs_name(fn, name))
                {
                    index[k + added] = i;
                    names[k + added] = name;
                    added++;
                }
            }
            if (added)
            {
                return added;
            }
        }
    }
    if (!body_tokens(fn, &tv))
    {
        NodeId decl = SYM(fn)->decl;
        NodeId node = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
        NodeId body = (NODE(u, node)->flags & AST_F_LAZY) ? 0 : fn_body(u, node);
        int added = 0, i = 0;
        for (NodeId p = fn_params(u, fn); body && p && k + added < SPEC_MAX_BOUND; p = NEXT(u, p), i++)
        {
            NameId name = token_name(TOK(u, NODE(u, p)->tok));
            tree_fn = fn;
            int hit = tree_indexes(u, body, name, 0, 0);
            tree_fn = 0;
            if (hit)
            {
                index[k + added] = i;
                names[k + added] = name;
                added++;
            }
        }
        return added;
    }
    int added = 0, i = 0;
    for (NodeId p = fn_params(u, fn); p && k + added < SPEC_MAX_BOUND; p = NEXT(u, p), i++)
    {
        NameId name = token_name(TOK(u, NODE(u, p)->tok));
        const Token *t = tv.data;
        int used = 0;
        for (size_t j = 0; j + 3 < tv.size && !used; j++)
        {
            // g ( a0 , a1 , ... ): p alone at a place g takes while compiling
            if (t[j].kind == TOK_IDENTIFIER && t[j + 1].kind == TOK_LPAREN && t[j + 1].match > j + 1 &&
                t[j + 1].match < tv.size)
            {
                used = passes_compile_time(u, fn, &t[j], t + j + 2, t + t[j + 1].match, name);
            }
        }
        if (used)
        {
            index[k + added] = i;
            names[k + added] = name;
            added++;
        }
    }
    free(tv.data);
    return added;
}

// Per symbol: 0 not known yet, 1 + the number of parameters known while compiling.
static uint8_t *generic_cache;
static size_t generic_cap;

// Every parameter of fn known while compiling (types first, then field numbers): their count.
// Found once per function, from its body as written (later its body may be parsed in place).
#define GENERIC_MAX 1024
static struct
{
    SymId fn;
    int k, ntypes;
    int index[SPEC_MAX_BOUND];
    NameId names[SPEC_MAX_BOUND];
    uint32_t body_tok; // its body as written (a deferred token), 0 if it was parsed with it
} generics[GENERIC_MAX];
static uint32_t ngenerics;

// The parameters typed `function(...)` (context.md §61): known while compiling, each a function
// given by name or as a literal; the function is made once per set of them.
static int function_params(SymId fn, int *index, NameId *names, int k)
{
    Unit *u = &cx.units[SYM(fn)->unit];
    int added = 0, i = 0;
    for (NodeId p = fn_params(u, fn); p && k + added < SPEC_MAX_BOUND; p = NEXT(u, p), i++)
    {
        if (NODE(u, p)->a && NODE(u, NODE(u, p)->a)->kind == AST_TYPE_FUNC)
        {
            index[k + added] = i;
            names[k + added] = token_name(TOK(u, NODE(u, p)->tok));
            added++;
        }
    }
    return added;
}

static int is_function_param(SymId fn, int i)
{
    Unit *u = &cx.units[SYM(fn)->unit];
    NodeId p = fn_params(u, fn);
    for (int j = 0; p && j < i; j++)
    {
        p = NEXT(u, p);
    }
    return p && NODE(u, p)->a && NODE(u, NODE(u, p)->a)->kind == AST_TYPE_FUNC;
}

static int compile_params(SymId fn, int *index, NameId *names, int *types_count)
{
    for (uint32_t g = 0; g < ngenerics; g++)
    {
        if (generics[g].fn == fn)
        {
            memcpy(index, generics[g].index, sizeof(generics[g].index));
            memcpy(names, generics[g].names, sizeof(generics[g].names));
            *types_count = generics[g].ntypes;
            return generics[g].k;
        }
    }
    int k = type_params(fn, index, names);
    *types_count = k;
    if (SYM(fn)->kind == SYM_METHOD && !spec_of(fn))
    {
        k += index_params(fn, index, names, k);
        k += function_params(fn, index, names, k);
    }
    if (k && ngenerics < GENERIC_MAX)
    {
        generics[ngenerics].fn = fn;
        generics[ngenerics].k = k;
        generics[ngenerics].ntypes = *types_count;
        memcpy(generics[ngenerics].index, index, sizeof(generics[ngenerics].index));
        memcpy(generics[ngenerics].names, names, sizeof(generics[ngenerics].names));
        Unit *u = &cx.units[SYM(fn)->unit];
        NodeId decl = SYM(fn)->decl;
        NodeId node = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
        generics[ngenerics].body_tok = (NODE(u, node)->flags & AST_F_LAZY) ? fn_body(u, node) : 0;
        ngenerics++;
    }
    return k;
}

static uint32_t generic_body_token(SymId fn)
{
    for (uint32_t g = 0; g < ngenerics; g++)
    {
        if (generics[g].fn == fn)
        {
            return generics[g].body_tok;
        }
    }
    return 0;
}

static void check_spec(Spec *sp);

// Is a specialization made for a function literal waiting? (Its literal lives in the body that
// made it: it must be checked before that body is released.)
static int literal_specs_waiting(void)
{
    for (uint32_t i = 0; i < nspecs; i++)
    {
        if (!specs[i].queued || specs[i].generated || specs[i].failed)
        {
            continue;
        }
        for (uint32_t k = 0; k < specs[i].n; k++)
        {
            if (specs[i].is_value[k] == 2 && (SYM((SymId)specs[i].values[k])->flags & SYM_F_LITERAL))
            {
                return 1;
            }
        }
    }
    return 0;
}

// The specializations made while checking, each checked (and generated) in its turn.
void sema_drain_specs(void)
{
    for (int more = 1; more;)
    {
        more = 0;
        for (uint32_t i = 0; i < nspecs; i++)
        {
            if (specs[i].queued && !specs[i].generated && !specs[i].failed)
            {
                specs[i].generated = 1;
                specs[i].queued = 0;
                check_spec(&specs[i]);
                more = 1;
            }
        }
    }
}

int sema_is_generic(SymId fn)
{
    if (fn >= generic_cap)
    {
        size_t cap = generic_cap ? generic_cap : 1024;
        while (cap <= fn)
        {
            cap *= 2;
        }
        generic_cache = realloc(generic_cache, cap);
        memset(generic_cache + generic_cap, 0, cap - generic_cap);
        generic_cap = cap;
    }
    if (!generic_cache[fn])
    {
        int index[SPEC_MAX_BOUND], nt;
        NameId names[SPEC_MAX_BOUND];
        generic_cache[fn] = 1; // (while it is found: a call back to it says "not generic")
        generic_cache[fn] = (uint8_t)(1 + (spec_of(fn) ? 0 : compile_params(fn, index, names, &nt)));
    }
    return generic_cache[fn] > 1;
}

static void check_callable(Check *c, NodeId fn, SymId owner, SymId sym);
static void check_decl(Check *c, NodeId decl, Scope *scope, SymId enclosing, int functions);

// Checks (and so generates, or captures for the compiler) a specialization, in its own file.
static void check_spec(Spec *sp)
{
    Unit *u = &cx.units[SYM(sp->fn)->unit];
    Check c;
    memset(&c, 0, offsetof(Check, locals));
    c.u = u;
    c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);
    if (!u->side_cap || !u->types)
    {
        side_tables(u, 0);
        for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, d, u->scope, 0, 0);
        }
        memset(&c, 0, offsetof(Check, locals));
        c.u = u;
        c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);
    }
    NodeId decl = SYM(sp->fn)->decl;
    check_callable(&c, NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl, SYM(sp->fn)->owner, sp->fn);
}

static int const_value(const Unit *u, NodeId e, TypeId type, uint64_t *v);

// A value an argument has while compiling, in 64 bits as the program computes it (wrapping):
// literals, constants, a specialization's bound values, and + - * / of them.
static int known_value(const Unit *u, NodeId e, uint64_t *v)
{
    const AstNode *n = NODE(u, e);
    Exact x;
    if (n->kind == AST_PAREN_GROUP)
    {
        return known_value(u, n->a, v);
    }
    if (n->kind == AST_IDENT && sema_bound_value(u, e, v))
    {
        return 1;
    }
    if (exact_value(u, e, &x))
    {
        *v = (uint64_t)x;
        return 1;
    }
    if (n->kind == AST_IDENT)
    {
        return const_value(u, e, T_U64, v);
    }
    uint64_t a, b;
    if (n->kind == AST_UNARY_OP && TOK(u, n->tok)->kind == TOK_MINUS && known_value(u, n->a, &a))
    {
        *v = 0 - a;
        return 1;
    }
    if (n->kind != AST_BIN_OP || !known_value(u, n->a, &a) || !known_value(u, n->b, &b))
    {
        return 0;
    }
    switch (TOK(u, n->tok)->kind)
    {
    case TOK_PLUS:
        *v = a + b;
        return 1;
    case TOK_MINUS:
        *v = a - b;
        return 1;
    case TOK_STAR:
        *v = a * b;
        return 1;
    case TOK_SLASH:
        if (!b)
        {
            return 0;
        }
        *v = a / b;
        return 1;
    default:
        return 0;
    }
}

// A new specialization's types written as values (`T.at(i).of`), computed now with its bound
// names: its parameters are locals of a check of their own, and the interpreter runs each.
static void spec_type_exprs(Check *caller, Spec *sp, uint32_t at)
{
    NodeId te[SPEC_MAX_BOUND];
    int n = sig_type_exprs(sp->of, te, SPEC_MAX_BOUND);
    if (!n)
    {
        return;
    }
    Unit *u = &cx.units[SYM(sp->of)->unit];
    Check c;
    memset(&c, 0, offsetof(Check, locals));
    c.u = u;
    if (!u->side_cap || !u->types) // a file whose turn has not come: its declarations first
    {
        side_tables(u, 0);
        for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, d, u->scope, 0, 0);
        }
        memset(&c, 0, offsetof(Check, locals));
        c.u = u;
    }
    c.owner = SYM(sp->of)->owner;
    c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);
    const Spec *old = binding;
    binding = sp;
    for (NodeId p = fn_params(u, sp->of); p; p = NEXT(u, p))
    {
        const AstNode *pt = NODE(u, p)->a ? NODE(u, NODE(u, p)->a) : NULL;
        TypeId t = pt && pt->kind != AST_TYPE_EXPR ? param_type(u, p, c.owner, 0) : T_NONE;
        push_local(&c, NODE(u, p)->tok, t, 0);
        c.locals[c.nlocals - 1].param = 1;
    }
    for (int i = 0; i < n; i++)
    {
        NodeId e = NODE(u, te[i])->a;
        check_expr(&c, e);
        TypeId t;
        if (!comptime_type_value(u, e, &t))
        {
            int len;
            const char *name = symbol_name(sp->of, &len);
            diag_at(caller->u, at, "error",
                    "'%.*s' writes a type as a value that the compiler cannot compute for this call: %s", len, name,
                    comptime_type_value_why()[0] ? comptime_type_value_why() : "it cannot run while compiling");
            t = T_NONE;
            sp->failed = 1;
        }
        if (sp->nt < SPEC_MAX_BOUND)
        {
            sp->texpr[sp->nt] = te[i];
            sp->ttype[sp->nt++] = t;
        }
    }
    binding = old;
}

// A call of a function made once per type: the specialization for the types its arguments
// name (made, and checked, the first time).
// The function given to parameter `pi` of fn (`function(T): U`): a function's name, or a literal.
// A literal with a parameter of no type (`x: x * 2`) takes the signature's types, the type
// parameters bound as this call binds them, and is checked now, where it is written.
static SymId function_argument(Check *c, SymId fn, int pi, NodeId a, const NameId *names, const TypeId *types,
                               int ntypes)
{
    Unit *u = c->u;
    Unit *home = &cx.units[SYM(fn)->unit];
    NodeId p = fn_params(home, fn);
    for (int j = 0; p && j < pi; j++)
    {
        p = NEXT(home, p);
    }
    const AstNode *ft = NODE(home, NODE(home, p)->a);
    Spec tmp;
    memset(&tmp, 0, sizeof(tmp)); // the type parameters as this call binds them
    tmp.n = (uint32_t)ntypes;
    memcpy(tmp.names, names, (size_t)ntypes * sizeof(NameId));
    memcpy(tmp.types, types, (size_t)ntypes * sizeof(TypeId));
    const Spec *old = binding;
    binding = &tmp;
    TypeId want[16];
    int nwant = 0;
    for (NodeId t = ft->a; t && nwant < 16; t = NEXT(home, t))
    {
        want[nwant++] = resolve_type(home, t, SYM(fn)->owner, 0);
    }
    TypeId want_ret = ft->b ? resolve_type(home, ft->b, SYM(fn)->owner, 0) : T_VOID;
    binding = old;
    int len;
    const char *name = symbol_name(fn, &len);
    const AstNode *an = NODE(u, a);
    SymId given = 0;
    if (an->kind == AST_FN_LIT)
    {
        int i = 0;
        for (NodeId q = an->a; q; q = NEXT(u, q), i++)
        {
            if (!NODE(u, q)->a && !(NODE(u, q)->flags & AST_F_PATTERN) && i < nwant)
            {
                u->types[q] = want[i]; // `x: ...`: x is what the signature says
            }
        }
        TypeId t = check_expr(c, a);
        given = type_kind(t) == T_FUNC ? type_ref(t) : 0;
        // (a literal that reads locals of the caller: the specialization takes them, see
        // sema_spec_functions; every call to it passes them)
    }
    else if (an->kind == AST_IDENT)
    {
        TypeId t = check_expr(c, a);
        given = type_kind(t) == T_FUNC ? type_ref(t) : 0;
        if (!given && u->refs[a] && SYM(u->refs[a])->kind == SYM_METHOD)
        {
            given = u->refs[a];
        }
        if (!given && u->refs[a] && SYM(u->refs[a])->kind == SYM_IMPORT)
        {
            given = module_binding_function(u->refs[a]);
        }
    }
    if (!given)
    {
        diag_at(u, an->tok, "error",
                "'%.*s' takes a function here (`function(...)`): name one, or write one, `x: x * 2`", len, name);
        return 0;
    }
    // its parameters and result must be the signature's
    Unit *gh = &cx.units[SYM(given)->unit];
    int i = 0;
    NodeId gp = sema_fn_params(gh, given);
    for (; gp && i < nwant; gp = NEXT(gh, gp), i++)
    {
        TypeId pt = sema_param_type(gh, given, gp);
        if (pt != want[i] && !(NODE(gh, gp)->flags & AST_F_PATTERN))
        {
            char x[64], y[64];
            diag_at(u, an->tok, "error", "the function given to '%.*s' takes %s, but its signature gives it %s",
                    len, name, type_str(pt, x, sizeof(x)), type_str(want[i], y, sizeof(y)));
            return 0;
        }
    }
    if (gp || i != nwant)
    {
        diag_at(u, an->tok, "error", "the function given to '%.*s' takes %d parameters, its signature gives %d",
                len, name, i + (gp ? 1 : 0), nwant);
        return 0;
    }
    TypeId gr = fn_ret_type(gh, given);
    if (want_ret != T_VOID && !fits(gr, want_ret))
    {
        char x[64], y[64];
        diag_at(u, an->tok, "error", "the function given to '%.*s' gives %s, but its signature wants %s", len,
                name, type_str(gr, x, sizeof(x)), type_str(want_ret, y, sizeof(y)));
        return 0;
    }
    return given;
}

static SymId specialize(Check *c, SymId fn, NodeId args, uint32_t at)
{
    int index[SPEC_MAX_BOUND], ntypes;
    NameId names[SPEC_MAX_BOUND];
    int k = compile_params(fn, index, names, &ntypes);
    TypeId types[SPEC_MAX_BOUND];
    uint64_t values[SPEC_MAX_BOUND];
    memset(values, 0, sizeof(values));
    for (int j = ntypes; j < k; j++) // a field number: a value known while compiling
    {
        NodeId a = args;
        for (int i = 0; a && i < index[j]; i++)
        {
            a = NEXT(c->u, a);
        }
        types[j] = T_NONE;
        if (is_function_param(fn, index[j]))
        {
            continue; // a function: after the types (its literal is typed by them)
        }
        uint64_t v;
        if (a)
        {
            check_expr(c, a);
        }
        if (!a || !known_value(c->u, a, &v))
        {
            int len;
            const char *name = symbol_name(fn, &len);
            diag_at(c->u, a ? NODE(c->u, a)->tok : at, "error",
                    "'%.*s' takes this argument while compiling (its body passes it where a value known while "
                    "compiling is taken): a constant, or a value its caller knows while compiling",
                    len, name);
            return 0;
        }
        // in the parameter's width, as the program would pass it
        NodeId p = fn_params(&cx.units[SYM(fn)->unit], fn);
        for (int i = 0; p && i < index[j]; i++)
        {
            p = NEXT(&cx.units[SYM(fn)->unit], p);
        }
        TypeId pt = p ? param_type(&cx.units[SYM(fn)->unit], p, SYM(fn)->owner, 0) : T_U64;
        uint32_t w = type_layout(pt).size;
        if (w && w < 8)
        {
            v &= (1ull << (8 * w)) - 1;
        }
        values[j] = v;
    }
    for (int j = 0; j < ntypes; j++)
    {
        NodeId a = args;
        for (int i = 0; a && i < index[j]; i++)
        {
            a = NEXT(c->u, a);
        }
        types[j] = T_NONE;
        if (a)
        {
            check_expr(c, a);
            types[j] = sema_named_type(c->u, a);
        }
        if (types[j] == T_NONE)
        {
            int len;
            const char *name = symbol_name(fn, &len);
            diag_at(c->u, a ? NODE(c->u, a)->tok : at, "error",
                    "'%.*s' takes this argument as a type (its signature types a parameter by it): name one, "
                    "`point` or `u32`",
                    len, name);
            return 0;
        }
    }
    for (int j = ntypes; j < k; j++) // a function: a name, or a literal typed by the signature
    {
        if (!is_function_param(fn, index[j]))
        {
            continue;
        }
        NodeId a = args;
        for (int i = 0; a && i < index[j]; i++)
        {
            a = NEXT(c->u, a);
        }
        SymId given = a ? function_argument(c, fn, index[j], a, names, types, ntypes) : 0;
        if (!given)
        {
            return 0;
        }
        values[j] = given;
    }
    Spec *sp = NULL;
    for (uint32_t i = 0; i < nspecs && !sp; i++)
    {
        sp = specs[i].of == fn && specs[i].n == (uint32_t)k &&
                     memcmp(specs[i].types, types, (size_t)k * sizeof(TypeId)) == 0 &&
                     memcmp(specs[i].values, values, (size_t)k * sizeof(uint64_t)) == 0
                 ? &specs[i]
                 : NULL;
    }
    uint32_t made = 0;
    for (uint32_t i = 0; i < nspecs && !sp; i++)
    {
        made += specs[i].of == fn;
    }
    if (!sp && made >= 256)
    {
        int len;
        const char *name = symbol_name(fn, &len);
        diag_at(c->u, at, "error",
                "'%.*s' is made for more than 256 sets of arguments known while compiling: its arms on them "
                "never end the recursion",
                len, name);
        return 0;
    }
    if (!sp)
    {
        if (nspecs == SPEC_MAX)
        {
            diag_at(c->u, at, "error", "more than %d specializations of functions made once per type", SPEC_MAX);
            return 0;
        }
        sp = &specs[nspecs++];
        memset(sp, 0, sizeof(*sp));
        sp->of = fn;
        sp->n = (uint32_t)k;
        memcpy(sp->names, names, (size_t)k * sizeof(NameId));
        memcpy(sp->types, types, (size_t)k * sizeof(TypeId));
        memcpy(sp->values, values, (size_t)k * sizeof(uint64_t));
        for (int j = ntypes; j < k; j++)
        {
            sp->is_value[j] = is_function_param(fn, index[j]) ? 2 : 1; // 2: a function (its symbol)
        }
        spec_type_exprs(c, sp, at);
        Unit *home = &cx.units[SYM(fn)->unit];
        sp->fn = symbol_new(home, SYM_METHOD, SYM(fn)->flags, SYM(fn)->tok, SYM(fn)->owner, SYM(fn)->decl);
    }
    uint32_t at_index = (uint32_t)(sp - specs);
    if (!specs[at_index].generated && !specs[at_index].queued && !capture_mir)
    {
        // checked in its turn, when no body is being checked (sema_drain_specs): a body checked
        // inside another would need room for both. The backend declares it now (C: a prototype).
        // (One made while a body is checked again for the compiler is for the compiler only: it
        // lowers it when it runs it, and the program has the one made in the body's own turn.)
        specs[at_index].queued = 1;
        codegen_declare(specs[at_index].fn);
    }
    return specs[at_index].fn;
}

// A literal whose parameter is a pattern alone, `f := (0..10) { ... }`, waits for its first call:
// that call's argument gives the parameter its type (use decides), and the body is checked then,
// seeing the locals that were there where it is written.
typedef struct
{
    SymId lit;
    NodeId node;
    int nlocals; // the locals it can see
} PendingLit;
static PendingLit pending_lits[64];
static uint32_t npending_lits;

static void check_callable(Check *c, NodeId fn, SymId owner, SymId sym);
static void check_waiting_literal(Check *c, SymId lit, NodeId args)
{
    Unit *u = c->u;
    for (uint32_t i = 0; i < npending_lits; i++)
    {
        if (pending_lits[i].lit != lit)
        {
            continue;
        }
        PendingLit w = pending_lits[i];
        pending_lits[i] = pending_lits[--npending_lits];
        NodeId a = args;
        for (NodeId p = NODE(u, w.node)->a; p; p = NEXT(u, p), a = a ? NEXT(u, a) : 0)
        {
            if (NODE(u, p)->a || !(NODE(u, p)->flags & AST_F_PATTERN))
            {
                continue;
            }
            TypeId at = a ? check_expr(c, a) : T_NONE;
            u->types[p] = at == T_INT_LIT ? T_INT : at; // the argument's type is the parameter's
        }
        int saved = c->nlocals;
        c->nlocals = w.nlocals < saved ? w.nlocals : saved;
        check_callable(c, w.node, c->owner, lit);
        c->nlocals = saved;
        return;
    }
}

static void error_goes_up(Check *c, uint32_t at, SymId fn);
static TypeId call_function(Check *c, NodeId id, uint32_t at, SymId first)
{
    NodeId args = NODE(c->u, id)->b;
    int argc = count_list(c->u, args);
    SymId fn = typed_overloads(first, argc) ? select_by_types(c, at, args, first, argc)
                                            : select_overload(c, at, first, argc);
    if (!fn && c->pretyped == 0 && typed_overloads(first, argc))
    {
        c->u->refs[id] = 0;
        return T_NONE; // reported, and its arguments are typed
    }
    if (fn && c->fn && cx.units[SYM(fn)->unit].is_manifest && !cx.units[SYM(c->fn)->unit].is_manifest)
    {
        // compile time only (docs/comptime.md §2.2): a body runs in the program
        const Token *t = TOK(c->u, at);
        diag_at(c->u, at, "error",
                "'%.*s' is declared in mod.modal, so it runs only in the compiler. To use its value here, "
                "bind it at module level: `name := %.*s(...)`",
                t->len, TSTART(t), t->len, TSTART(t));
    }
    if (fn && sema_is_generic(fn))
    {
        fn = specialize(c, fn, args, at); // `soma(point, p)`: soma made for point
        if (!fn)
        {
            check_args(c, args, 0);
            return T_NONE;
        }
    }
    if (fn && count_contexts(c->u, fn) && argc < arity_of(fn))
    {
        fill_contexts(c, id, fn, at);
        args = NODE(c->u, id)->b;
    }
    check_args(c, args, fn);
    c->u->refs[id] = fn;
    note_call(c, fn);
    if (fn && sema_fn_fails(fn) && c->handled != id)
    {
        error_goes_up(c, at, fn); // `n := parse(s)`: an error of parse goes up
    }
    note_edge(c, fn, (uint8_t)((sema_fn_fails(fn) && c->handled != id ? EE_ERRORS : 0) | (c->guard != id ? EE_PANICS : 0) |
                               EE_CALLS));
    if (fn && c->fn && cx.sym_names[fn] == cx.sym_names[c->fn] && SYM(fn)->owner == SYM(c->fn)->owner)
    {
        SYM(c->fn)->flags |= SYM_F_LOOPS; // the lowering inlines larger bodies into it
    }
    return fn ? fn_ret_type(c->u, fn) : T_NONE;
}

// Finds `name` among the members of struct `s`. Every member is reachable; a use from another
// domain makes the member public (note_use).
//
// A field and its accessor may share a name (`u8 n1` and `u8 n1(self: T): self.n1`). Which
// one is meant follows from the use: `x.n1(...)` is the method, `x.n1` is the field.
// A member used from another domain is public: visibility follows use (std.md §10). The
// record is what --sig prints and what the layout may rely on.
static void note_use(const Check *c, SymId member)
{
    if (cx.units[SYM(member)->unit].domain != c->u->domain)
    {
        SYM(member)->flags |= SYM_F_PUBLIC;
        audit_use(member, c->u->domain);
    }
}

static SymId find_member(Check *c, SymId s, uint32_t name_tok, int is_call)
{
    const Token *name = TOK(c->u, name_tok);
    int tlen;
    const char *type = symbol_name(s, &tlen);
    SymId member = scope_find_token(struct_scope(s), name);

    if (member && !is_call && SYM(member)->kind == SYM_METHOD) // `x.n1`: the field, not its accessor
    {
        for (SymId other = SYM(member)->next; other; other = SYM(other)->next)
        {
            int olen;
            const char *oname = symbol_name(other, &olen);
            if (SYM(other)->kind == SYM_FIELD && (uint32_t)olen == name->len &&
                memcmp(oname, TSTART(name), name->len) == 0)
            {
                member = other;
                break;
            }
        }
    }
    if (!member)
    {
        diag_at(c->u, name_tok, "error", "'%.*s' has no member '%.*s'", tlen, type, name->len,
                TSTART(name));
        return 0;
    }
    note_use(c, member);
    return member;
}

// The struct a value of this type gives access to, or 0.
static SymId struct_of(TypeId t)
{
    TypeKind k = type_kind(t);
    if (k == T_PTR)
    {
        t = type_ref(t);
        k = type_kind(t);
    }
    return (k == T_STRUCT || k == T_TYPE) ? type_ref(t) : 0;
}

static TypeId check_call(Check *c, NodeId id)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    NodeId callee = n->a;
    NodeId args = n->b;
    const AstNode *cn = NODE(u, callee);

    cx.calls_resolved++;

    if (cn->kind == AST_IDENT)
    {
        SymId sym;
        TypeId type;
        const Token *t = TOK(u, cn->tok);
        NameKind kind = find_name(c, t, &sym, &type);

        if (kind == NAME_MISSING)
        {
            report_missing(c, cn->tok, 1);
            check_args(c, args, 0);
            return T_NONE;
        }
        if (kind == NAME_SYMBOL || kind == NAME_MEMBER)
        {
            u->refs[callee] = sym;
            SymId as = SYM(sym)->kind == SYM_STRUCT ? sym
                       : SYM(sym)->kind == SYM_IMPORT && module_binding_target(sym) &&
                               SYM(module_binding_target(sym))->kind == SYM_STRUCT
                           ? module_binding_target(sym)
                           : 0;
            if (as && args && !NEXT(u, args))
            {
                TypeId at = check_expr(c, args);
                if (sema_is_any_ref(at)) // `point(a)`: what a points to, as a point
                {
                    NODE(u, id)->aux = PRIM_UNANY;
                    note_panic(c, PANIC_ANYTYPE);
                    u->refs[id] = as;
                    set_type(c, callee, T_EXTERN);
                    return type_make(T_PTR, type_make(T_STRUCT, as));
                }
                int tl;
                const char *tn = symbol_name(as, &tl);
                diag_at(u, cn->tok, "error",
                        "'%.*s' is a type, not a function. `%.*s(a)` takes an *any and gives its *%.*s back; to "
                        "create one, write a literal `%.*s{ ... }` or call a constructor `%.*s.new()`",
                        tl, tn, tl, tn, tl, tn, tl, tn, tl, tn);
                return T_NONE;
            }
            if (SYM(sym)->kind == SYM_METHOD)
            {
                set_type(c, callee, type_make(T_FUNC, sym));
                return call_function(c, id, cn->tok, sym);
            }
            if (SYM(sym)->kind == SYM_STRUCT)
            {
                diag_at(u, cn->tok, "error",
                        "'%.*s' is a type, not a function. To create one, call a constructor "
                        "on it: `%.*s.new()`",
                        t->len, TSTART(t), t->len, TSTART(t));
                check_args(c, args, 0);
                return T_NONE;
            }
            if (SYM(sym)->kind != SYM_IMPORT && value_type(c->u, sym) != T_EXTERN)
            {
                diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, TSTART(t));
                check_args(c, args, 0);
                return T_NONE;
            }
            if (text_is(t, "panic") && SYM(sym)->kind == SYM_IMPORT && !module_binding_function(sym))
            {
                NODE(u, id)->aux = PRIM_TRAP; // `panic := import("std").panic`: a module, and `panic("why")`
                set_type(c, callee, T_EXTERN);
                return check_trap(c, id, cn->tok);
            }
            // `assert := import("std").assert`: a binding that names a function is that function
            SymId bound = SYM(sym)->kind == SYM_IMPORT && sema_std_member(u, sym) == STD_NONE
                              ? module_binding_function(sym)
                              : 0;
            if (bound)
            {
                set_type(c, callee, type_make(T_FUNC, bound));
                return call_function(c, id, cn->tok, bound);
            }
        }
        else if (kind == NAME_LOCAL && type_kind(type) == T_FUNC && c->found_call)
        {
            SymId fn = type_ref(type); // `a := loop(u64, 1)`; `a()`: that call, with those arguments
            if (args)
            {
                diag_at(u, cn->tok, "error",
                        "'%.*s' holds a call with its arguments already given: call it with none, `%.*s()`",
                        t->len, TSTART(t), t->len, TSTART(t));
                check_args(c, args, 0);
            }
            set_type(c, callee, type);
            u->refs[id] = fn;
            NODE(u, id)->aux = PRIM_BOUND;
            note_call(c, fn);
            return fn_ret_type(u, fn);
        }
        else if (kind == NAME_LOCAL && type_kind(type) == T_FUNC)
        {
            check_waiting_literal(c, type_ref(type), args); // `(0..10) { ... }`: typed by this call
            return call_function(c, id, cn->tok, type_ref(type)); // f := double; f(1)
        }
        else if (kind == NAME_LOCAL && type != T_EXTERN && type != T_NONE)
        {
            diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, TSTART(t));
        }
        set_type(c, callee, T_EXTERN);
        int member = kind == NAME_BUILTIN ? STD_NONE : sema_std_member(u, sym);
        NODE(u, id)->aux = PRIM_NONE;
        if (kind == NAME_BUILTIN && t->kind != TOK_WRITE)
        {
            int query = text_is(t, "sizeof")     ? PRIM_SIZEOF
                        : text_is(t, "alignof")  ? PRIM_ALIGNOF
                        : text_is(t, "offsetof") ? PRIM_OFFSETOF
                        : text_is(t, "fieldsof") ? PRIM_FIELDSOF
                        : text_is(t, "typeof")   ? PRIM_TYPEID
                                                 : PRIM_NONE;
            if (query != PRIM_NONE)
            {
                NODE(u, id)->aux = (uint8_t)query;
                return check_type_query(c, id, cn->tok, query);
            }
            if (text_is(t, "load"))
            {
                NODE(u, id)->aux = PRIM_LOAD;
                return check_load(c, id, cn->tok);
            }
            if (text_is(t, "trap") || text_is(t, "panic"))
            {
                NODE(u, id)->aux = PRIM_TRAP; // `panic("why")`: panic.raised (guards.md §5)
                return check_trap(c, id, cn->tok);
            }
            TypeId to = prim_type(t);
            if (type_is_int(to))
            {
                NODE(u, id)->aux = PRIM_CONVERT;
                return check_convert(c, id, cn->tok, to);
            }
        }
        if (member == STD_SYS_PUT)
        {
            check_sys_put(c, id, cn->tok);
            return T_VOID;
        }
        if (kind == NAME_BUILTIN || member == STD_PRINT) // write(...) and std's print
        {
            route_output(c, id, cn->tok);
            check_args(c, NODE(u, id)->b, 0);
            return T_VOID;
        }
        check_args(c, args, 0);
        return T_EXTERN;
    }

    if (cn->kind == AST_SELECTOR)
    {
        TypeId base = (is_self_node(u, cn->a) && c->has_self) ? set_type(c, cn->a, self_type(c))
                                                              : check_expr(c, cn->a);
        SymId s = struct_of(base);
        const Token *name = TOK(u, cn->tok);

        if (s)
        {
            SymId member = find_member(c, s, cn->tok, 1);
            if (member && SYM(member)->kind != SYM_METHOD)
            {
                diag_at(u, cn->tok, "error", "'%.*s' is a field, not a method", name->len,
                        TSTART(name));
                member = 0;
            }
            if (!member)
            {
                check_args(c, args, 0);
                return T_NONE;
            }
            u->refs[callee] = member;
            set_type(c, callee, type_make(T_FUNC, member));
            return call_function(c, id, cn->tok, member);
        }
        if (base == T_MODULE) // `self.f()` in a module-level function: a name of the module
        {
            SymId member = scope_find_token(u->scope, name);
            if (!member)
            {
                diag_at(u, cn->tok, "error", "'%.*s' has no member '%.*s'", u->name_len, u->name,
                        name->len, TSTART(name));
            }
            else if (SYM(member)->kind == SYM_METHOD)
            {
                u->refs[callee] = member;
                return call_function(c, id, cn->tok, member);
            }
        }
        else if (base == T_EXTERN)
        {
            // `util.twice(2)`: a function of an imported module, checked like a local one.
            SymId member = module_member(u, cn->a, cn->tok);
            if (member && SYM(member)->kind == SYM_METHOD)
            {
                u->refs[callee] = member;
                set_type(c, callee, type_make(T_FUNC, member));
                return call_function(c, id, cn->tok, member);
            }
        }
        else if (base != T_NONE)
        {
            char got[64];
            diag_at(u, cn->tok, "error", "a value of type %s has no method '%.*s'",
                    type_str(base, got, sizeof(got)), name->len, TSTART(name));
        }
        check_args(c, args, 0);
        return base == T_NONE ? T_NONE : T_EXTERN;
    }

    check_expr(c, callee);
    check_args(c, args, 0);
    return T_EXTERN;
}

// ---- expressions and statements --------------------------------------------------------------

static TypeId check_binary(Check *c, const AstNode *n)
{
    TypeId l = check_expr(c, n->a);
    TypeId r = check_expr(c, n->b);
    TokenKind op = (TokenKind)TOK(c->u, n->tok)->kind;
    int compare = op == TOK_CMP || op == TOK_NEQ || op == TOK_LT || op == TOK_GT || op == TOK_LE ||
                  op == TOK_GE;
    int logic = op == TOK_AND || op == TOK_OR;

    if (l == T_NONE || r == T_NONE)
    {
        return T_NONE;
    }
    if (l == T_EXTERN || r == T_EXTERN)
    {
        return (compare || logic) ? T_BOOL : T_EXTERN;
    }

    // `<` `>` `<=` `>=` order numbers; bools, strings and structs have no order.
    int order = op == TOK_LT || op == TOK_GT || op == TOK_LE || op == TOK_GE;
    int ok = logic     ? (l == T_BOOL && r == T_BOOL)
             : order   ? (type_is_int(l) && type_is_int(r) && (fits(l, r) || fits(r, l)))
             : compare ? (fits(l, r) || fits(r, l))
                       : (type_is_int(l) && type_is_int(r) && fits(l, r));
    if (!ok)
    {
        char a[64], b[64];
        const Token *t = TOK(c->u, n->tok);
        diag_at(c->u, n->tok, "error", "'%.*s' cannot be applied to %s and %s", t->len, TSTART(t),
                type_str(l, a, sizeof(a)), type_str(r, b, sizeof(b)));
        return T_NONE;
    }
    Exact divisor;
    if (op == TOK_SLASH && exact_value(c->u, n->b, &divisor) && divisor == 0)
    {
        diag_at(c->u, NODE(c->u, n->b)->tok, "error", "division by zero");
        return T_NONE;
    }
    if (op == TOK_SLASH && !(exact_value(c->u, n->b, &divisor) && divisor != 0) &&
        !(NODE(c->u, n->b)->kind == AST_IDENT && (NODE(c->u, n->b)->aux & IDENT_NONZERO)))
    {
        note_panic(c, PANIC_DIVISION); // a divisor the compiler cannot prove not zero
    }
    if (l == T_INT_LIT && r != T_INT_LIT)
    {
        check_bounds(c, n->a, r);
    }
    else if (r == T_INT_LIT && l != T_INT_LIT)
    {
        check_bounds(c, n->b, l);
    }
    if (compare || logic)
    {
        return T_BOOL;
    }
    return l == T_INT_LIT ? r : l;
}

// The domain's configuration (`max_ram`) and the target's facts (`target.os`) are compile-time
// values (docs/comptime.md §2.2): a module-level position reads them; a body cannot, since a
// function that did would be compile time only. The error says how to use one there.
static void compile_time_value(Check *c, SymId s, uint32_t tok)
{
    const Unit *home = &cx.units[SYM(s)->unit];
    if (!c->fn || SYM(s)->kind != SYM_CONST || (!home->provided && !home->is_manifest) ||
        cx.units[SYM(c->fn)->unit].is_manifest)
    {
        return;
    }
    const Token *t = TOK(c->u, tok);
    diag_at(c->u, tok, "error",
            "'%.*s' is %s, a compile-time value: a function body cannot read it. Give its value a "
            "name at module level (`n := ... %.*s ...`) and use that name here",
            t->len, TSTART(t), home->provided ? "a fact of the target" : "configuration of the domain",
            t->len, TSTART(t));
}

static TypeId check_selector(Check *c, NodeId id)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    const Token *name = TOK(u, n->tok);
    TypeId base = (is_self_node(u, n->a) && c->has_self) ? set_type(c, n->a, self_type(c))
                                                         : check_expr(c, n->a);
    SymId s = struct_of(base);

    if (s)
    {
        SymId member = find_member(c, s, n->tok, 0);
        if (!member)
        {
            return T_NONE;
        }
        u->refs[id] = member;
        if (SYM(member)->kind == SYM_METHOD)
        {
            diag_at(u, n->tok, "error", "'%.*s' is a method of '%.*s': call it, `x.%.*s(...)`", name->len,
                    TSTART(name), (int)TOK(&cx.units[SYM(s)->unit], SYM(s)->tok)->len,
                    TSTART(TOK(&cx.units[SYM(s)->unit], SYM(s)->tok)), name->len, TSTART(name));
            return T_NONE;
        }
        note_field(c, member, n->tok);
        return symbol_type(c, member);
    }
    if (base == T_MODULE)
    {
        SymId member = scope_find_token(u->scope, name);
        if (!member)
        {
            diag_at(u, n->tok, "error", "'%.*s' has no member '%.*s'", u->name_len, u->name,
                    name->len, TSTART(name));
            return T_NONE;
        }
        u->refs[id] = member;
        note_global(c, member, n->tok);
        return symbol_type(c, member);
    }
    if (base == T_EXTERN)
    {
        // `util.limit`: a constant of an imported module has its own type.
        SymId member = module_member(u, n->a, n->tok);
        if (member && SYM(member)->kind == SYM_CONST)
        {
            u->refs[id] = member;
            compile_time_value(c, member, n->tok);
            return symbol_type(c, member);
        }
    }
    if (base != T_EXTERN && base != T_NONE)
    {
        char got[64];
        diag_at(u, n->tok, "error", "a value of type %s has no member '%.*s'",
                type_str(base, got, sizeof(got)), name->len, TSTART(name));
        return T_NONE;
    }
    return base;
}

static Local *local_named(Check *c, NodeId id)
{
    const AstNode *n = NODE(c->u, id);
    if (n->kind != AST_IDENT)
    {
        return NULL;
    }
    NameId name = token_name(TOK(c->u, n->tok));
    for (int i = c->nlocals - 1; i >= 0; i--)
    {
        if (c->locals[i].name == name)
        {
            return &c->locals[i];
        }
    }
    return NULL;
}

// A reference into this function's frame: 1 for `&T{ ... }` (a literal given storage here),
// 2 for a local that holds one (`a`, after `a: *T = &T{ ... }`), 0 otherwise.
static int frame_reference(Check *c, NodeId id)
{
    const AstNode *n = NODE(c->u, id);
    while (n->kind == AST_PAREN_GROUP)
    {
        id = n->a;
        n = NODE(c->u, id);
    }
    if (n->kind == AST_UNARY_OP && TOK(c->u, n->tok)->kind == TOK_AMP && NODE(c->u, n->a)->kind == AST_STRUCT_LIT)
    {
        return 1;
    }
    Local *l = local_named(c, id);
    return l && l->frame ? 2 : 0;
}

// Does fn say `, error` in its result (guards.md §5)?
int sema_fn_fails(SymId fn)
{
    if (!fn || SYM(fn)->kind != SYM_METHOD || !SYM(fn)->decl)
    {
        return 0;
    }
    const Unit *u = &cx.units[SYM(fn)->unit];
    NodeId decl = SYM(fn)->decl;
    NodeId callable = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
    NodeId sig = NODE(u, callable)->b;
    return sig && NODE(u, callable)->kind != AST_DEFINE && (NODE(u, sig)->aux & SIG_FAILS);
}

// An error that is not handled here goes up: the function must say so.
static void error_goes_up(Check *c, uint32_t at, SymId fn)
{
    if (c->fails || c->ret == T_ERROR || !c->in_callable)
    {
        return;
    }
    int len, ml = 0;
    const char *name = symbol_name(fn, &len);
    const char *mine = c->fn ? symbol_name(c->fn, &ml) : "";
    diag_at(c->u, at, "error",
            "'%.*s' can fail, and its error is not handled here: bind it, `n, e := %.*s(...)`, or say that "
            "'%.*s' can fail too (`, error` in its result)",
            len, name, len, name, ml, mine);
}

static void check_return(Check *c, NodeId id, NodeId value)
{
    const AstNode *v = value ? NODE(c->u, value) : NULL;
    c->returning_ref = v && v->kind == AST_UNARY_OP && TOK(c->u, v->tok)->kind == TOK_AMP &&
                       NODE(c->u, v->a)->kind == AST_STRUCT_LIT;
    TypeId got = value ? check_expr(c, value) : T_VOID;
    c->returning_ref = 0;
    if (value && got == T_ERROR && (c->fails || c->ret == T_ERROR))
    {
        uint64_t eid;
        if (const_value(c->u, value, T_ERROR, &eid))
        {
            if (eid)
            {
                note_error_id(c, eid);
            }
        }
        else if (c->fn)
        {
            note_any(c); // an error it holds: which one is not known here
        }
    }
    if (c->fails && got == T_ERROR && c->ret != T_ERROR)
    {
        return; // `return bad_input` in `parse(...): u64, error` (guards.md §5)
    }
    if (value && sema_is_any_ref(c->ret) && type_kind(got) == T_PTR && !sema_is_any_ref(got))
    {
        diag_at(c->u, v->tok, "error",
                "an *any made here is a pair in this function's frame, which ends when it returns: return "
                "the reference itself (its type), or take an *any as a parameter and return that");
    }
    Local *made = value ? local_named(c, value) : NULL;
    if (made && made->frame && sema_is_any_ref(made->type))
    {
        const Token *t = TOK(c->u, v->tok);
        diag_at(c->u, v->tok, "error",
                "`%.*s` is an *any made in this function: its pair (address, type) ends when it returns. "
                "Return the reference itself, or take the *any as a parameter",
                (int)t->len, TSTART(t));
    }
    else if (value && frame_reference(c, value) == 2)
    {
        const Token *t = TOK(c->u, v->tok);
        diag_at(c->u, v->tok, "error",
                "`%.*s` points to a literal of this function, which ends when it returns: return the value, "
                "`*%.*s`, or build it in a constructor, `return &T{ ... }` in a method with `self: T`",
                (int)t->len, TSTART(t), (int)t->len, TSTART(t));
    }
    char a[64], b[64];

    if (!c->in_callable || c->ret == T_NONE)
    {
        return;
    }
    if (c->ret == T_VOID && value && got != T_VOID)
    {
        diag_at(c->u, NODE(c->u, id)->tok, "error",
                "this function declares no return type, but returns %s",
                type_str(got, a, sizeof(a)));
    }
    else if (c->ret != T_VOID && !fits(got, c->ret))
    {
        char text[128], hint[192];
        int named = value && value_text(c->u, value, text, sizeof(text));
        reference_hint(got, c->ret, named ? text : "", hint, sizeof(hint));
        if (named)
        {
            diag_at(c->u, NODE(c->u, id)->tok, "error", "`%s` should be %s, as the function declares, but it is %s%s",
                    text, type_str(c->ret, b, sizeof(b)), type_str(got, a, sizeof(a)), hint);
        }
        else
        {
            diag_at(c->u, NODE(c->u, id)->tok, "error", "the result should be %s, as the function declares, but it is %s%s",
                    type_str(c->ret, b, sizeof(b)), type_str(got, a, sizeof(a)), hint);
        }
    }
    else if (c->ret != T_VOID)
    {
        check_bounds(c, value, c->ret);
    }
}

static int signed_type(TypeId t);
static uint64_t type_max(TypeId t);
static int pattern_from(Unit *u, const AstNode *n, TypeId type, Pattern *out, int loud);

// ---- arms (docs/std.md §8) -----------------------------------------------------------------------
//
// What the arms above take, as sorted disjoint ranges of one subject's values (an integer or a
// bool, in the type's own order: signed types as signed), or the strings taken so far. It says
// whether an arm can still run, and whether every value is taken.
#define MAX_COVER 256
typedef struct
{
    TypeId type;
    int all;     // a name took every value
    int n;
    Exact lo[MAX_COVER], hi[MAX_COVER];
    int nstr;
    uint32_t str[MAX_COVER]; // string literal tokens
    int overflow;
} Cover;

static Exact cover_value(TypeId t, uint64_t v)
{
    return signed_type(t) ? (Exact)(int64_t)v : (Exact)v;
}

static Exact cover_min(TypeId t)
{
    return t == T_BOOL ? 0 : signed_type(t) ? -(Exact)type_max(t) - 1 : 0;
}

static Exact cover_max(TypeId t)
{
    return t == T_BOOL ? 1 : (Exact)type_max(t);
}

static int cover_has(const Cover *cv, Exact lo, Exact hi) // is [lo, hi] taken already?
{
    if (cv->all)
    {
        return 1;
    }
    for (int i = 0; i < cv->n && lo <= hi; i++)
    {
        if (cv->lo[i] <= lo && lo <= cv->hi[i])
        {
            lo = cv->hi[i] + 1;
        }
    }
    return lo > hi;
}

static void cover_add(Cover *cv, Exact lo, Exact hi)
{
    if (cv->n == MAX_COVER)
    {
        cv->overflow = 1;
        return;
    }
    int at = 0;
    while (at < cv->n && cv->lo[at] < lo)
    {
        at++;
    }
    memmove(&cv->lo[at + 1], &cv->lo[at], (size_t)(cv->n - at) * sizeof(Exact));
    memmove(&cv->hi[at + 1], &cv->hi[at], (size_t)(cv->n - at) * sizeof(Exact));
    cv->lo[at] = lo;
    cv->hi[at] = hi;
    cv->n++;
    int k = 0; // merge what touches
    for (int i = 1; i < cv->n; i++)
    {
        if (cv->lo[i] <= cv->hi[k] + 1)
        {
            cv->hi[k] = cv->hi[i] > cv->hi[k] ? cv->hi[i] : cv->hi[k];
        }
        else
        {
            k++;
            cv->lo[k] = cv->lo[i];
            cv->hi[k] = cv->hi[i];
        }
    }
    cv->n = k + 1;
}

static int same_text(const Unit *u, uint32_t a, uint32_t b)
{
    const Token *x = TOK(u, a), *y = TOK(u, b);
    return x->len == y->len && memcmp(TSTART(x), TSTART(y), x->len) == 0;
}

// The first part of [lo, hi] an arm above takes, when only part of it is taken.
static int cover_overlap(const Cover *cv, Exact lo, Exact hi, Exact *olo, Exact *ohi)
{
    for (int i = 0; i < cv->n; i++)
    {
        if (cv->lo[i] <= hi && lo <= cv->hi[i])
        {
            *olo = cv->lo[i] > lo ? cv->lo[i] : lo;
            *ohi = cv->hi[i] < hi ? cv->hi[i] : hi;
            return 1;
        }
    }
    return 0;
}

static void exact_text(Exact v, char *out, size_t cap)
{
    snprintf(out, cap, v < 0 ? "%lld" : "%llu", v < 0 ? (long long)v : (unsigned long long)v);
}

// Takes one arm's pattern into the cover. 0: it adds nothing (the arms above took all of it).
// 2: part of it is taken already (arms take disjoint values: written to `overlap`).
static int cover_take(Unit *u, Cover *cv, NodeId pat, char *overlap, size_t cap)
{
    if (cv->all)
    {
        return 0;
    }
    if (NODE(u, pat)->aux & PAT_ANY)
    {
        int full = cv->type != T_STR && cover_has(cv, cover_min(cv->type), cover_max(cv->type));
        cv->all = 1;
        return !full;
    }
    Pattern pt;
    if (!sema_arm_pattern(u, pat, cv->type, &pt))
    {
        return 1; // an error, reported
    }
    if (pt.kind == PATTERN_TEXT)
    {
        for (int i = 0; i < cv->nstr; i++)
        {
            if (same_text(u, cv->str[i], pt.tok))
            {
                return 0;
            }
        }
        if (cv->nstr < MAX_COVER)
        {
            cv->str[cv->nstr++] = pt.tok;
        }
        return 1;
    }
    Exact lo = cover_value(cv->type, pt.lo), hi = pt.kind == PATTERN_RANGE ? cover_value(cv->type, pt.hi) : lo;
    if (cover_has(cv, lo, hi))
    {
        return 0;
    }
    Exact olo, ohi;
    if (cover_overlap(cv, lo, hi, &olo, &ohi))
    {
        char a[48], b[48];
        exact_text(olo, a, sizeof(a));
        exact_text(ohi, b, sizeof(b));
        if (olo == ohi)
        {
            snprintf(overlap, cap, "%s", cv->type == T_BOOL ? (olo ? "true" : "false") : a);
        }
        else
        {
            snprintf(overlap, cap, "%s..%s", a, b);
        }
        cover_add(cv, lo, hi);
        return 2;
    }
    cover_add(cv, lo, hi);
    return 1;
}

static int cover_full(const Cover *cv)
{
    return cv->all || (cv->type != T_STR && !cv->overflow &&
                       cover_has(cv, cover_min(cv->type), cover_max(cv->type)));
}

// "10..255, 300" : the values no arm takes (the first few gaps).
static void cover_gaps(const Cover *cv, char *out, size_t cap)
{
    size_t len = 0;
    int shown = 0;
    Exact at = cover_min(cv->type), end = cover_max(cv->type);
    out[0] = 0;
    for (int i = 0; i <= cv->n && at <= end && len < cap; i++)
    {
        Exact stop = i < cv->n ? cv->lo[i] - 1 : end;
        if (stop >= at)
        {
            if (shown == 3)
            {
                len += (size_t)snprintf(out + len, cap - len, ", ...");
                return;
            }
            const char *sep = shown ? ", " : "";
            if (cv->type == T_BOOL)
            {
                len += (size_t)snprintf(out + len, cap - len, "%s%s", sep, at ? "true" : "false");
            }
            else if (stop == at)
            {
                len += (size_t)snprintf(out + len, cap - len, "%s%lld", sep, (long long)at);
            }
            else
            {
                char a[48], b[48];
                snprintf(a, sizeof(a), at < 0 ? "%lld" : "%llu", at < 0 ? (long long)at : (unsigned long long)at);
                snprintf(b, sizeof(b), stop < 0 ? "%lld" : "%llu", stop < 0 ? (long long)stop : (unsigned long long)stop);
                len += (size_t)snprintf(out + len, cap - len, "%s%s..%s", sep, a, b);
            }
            shown++;
        }
        if (i < cv->n)
        {
            at = cv->hi[i] + 1;
        }
    }
}

// The subjects a match tests: its value, or (a = 0) the parameters of the function whose body
// it is, `self` and context parameters aside. Returns how many, with their types (and, for
// parameters, their nodes).
static int match_subjects(Check *c, NodeId id, TypeId *types, NodeId *params, int cap)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    if (n->a) // the values written after `if`, one column each
    {
        int k = 0;
        for (NodeId v = n->a; v && k < cap; v = NEXT(u, v), k++)
        {
            types[k] = check_expr(c, v);
            types[k] = types[k] == T_INT_LIT ? T_I64 : types[k]; // an unsized value: matched as an i64
            params[k] = 0;
        }
        return k;
    }
    if (!c->fn || !c->in_callable)
    {
        diag_at(u, n->tok, "error", "arms without a value before them match a function's parameters: "
                                    "they are a whole function body");
        return 0;
    }
    int k = 0;
    const Unit *home = &cx.units[SYM(c->fn)->unit];
    for (NodeId p = fn_params(home, c->fn); p && k < cap; p = NEXT(home, p))
    {
        const AstNode *pn = NODE(home, p);
        const Token *name = TOK(home, pn->tok);
        if ((pn->flags & AST_F_CONTEXT) || is_self_token(TSTART(name), (int)name->len))
        {
            continue;
        }
        types[k] = param_type((Unit *)home, p, SYM(c->fn)->owner, 0);
        params[k++] = p;
    }
    return k;
}

static int is_statement(const Unit *u, NodeId result)
{
    int k = NODE(u, result)->kind;
    return k == AST_BLOCK || k == AST_RETURN || k == AST_ASSIGN || k == AST_FIELD_DECL || k == AST_IF;
}

// Does code `id` assign to the local `name` (`name = ...` anywhere in it)? Then an arm's range
// fact about it does not hold for the whole arm.
static int assigns_to(const Unit *u, NodeId id, NameId name)
{
    if (!id)
    {
        return 0;
    }
    const AstNode *n = NODE(u, id);
    switch (n->kind)
    {
    case AST_ASSIGN:
        if (NODE(u, n->a)->kind == AST_IDENT && token_name(TOK(u, NODE(u, n->a)->tok)) == name)
        {
            return 1;
        }
        return assigns_to(u, n->a, name) || assigns_to(u, n->b, name);
    case AST_BLOCK:
        for (NodeId s = n->a; s; s = NEXT(u, s))
        {
            if (assigns_to(u, s, name))
            {
                return 1;
            }
        }
        return 0;
    case AST_CALL:
        for (NodeId s = n->b; s; s = NEXT(u, s))
        {
            if (assigns_to(u, s, name))
            {
                return 1;
            }
        }
        return 0;
    case AST_MATCH:
        for (NodeId v = n->a; v; v = NEXT(u, v))
        {
            if (assigns_to(u, v, name))
            {
                return 1;
            }
        }
        for (NodeId s = n->b; s; s = NEXT(u, s))
        {
            if (assigns_to(u, NODE(u, s)->b, name))
            {
                return 1;
            }
        }
        return 0;
    case AST_IF:
        return assigns_to(u, n->a, name) || assigns_to(u, NODE(u, n->b)->a, name) ||
               assigns_to(u, NODE(u, n->b)->b, name);
    case AST_BIN_OP:
    case AST_INDEX:
        return assigns_to(u, n->a, name) || assigns_to(u, n->b, name);
    case AST_UNARY_OP:
    case AST_RETURN:
    case AST_DEFINE:
    case AST_SELECTOR:
    case AST_PAREN_GROUP:
        return assigns_to(u, n->a, name);
    case AST_FIELD_DECL:
        return assigns_to(u, n->b, name);
    default:
        return 0;
    }
}

static int name_pattern_value(const Unit *u, const AstNode *pn, TypeId type, uint64_t *v);
static int ends_pattern(Unit *u, const AstNode *pn, TypeId type, Pattern *out, int loud);

// A name used as a pattern: it must exist, fit the value matched, and not be that value. 1 when
// the compiler knows its value (a constant: it counts like a literal), 0 when the program does.
static int name_pattern(Check *c, const AstNode *pn, TypeId type, NameId subject)
{
    Unit *u = c->u;
    const Token *t = TOK(u, pn->tok);
    TypeId nt = check_expr(c, pn->a);
    char x[64], y[64];
    if (subject && token_name(t) == subject)
    {
        diag_at(u, pn->tok, "error", "'%.*s' is the value being matched, so this pattern takes every value: "
                                     "write `_`", t->len, TSTART(t));
        return 0;
    }
    if (nt == T_NONE || nt == T_EXTERN)
    {
        return 0;
    }
    if (type == T_STR)
    {
        diag_at(u, pn->tok, "error", "a name as a pattern on a []u8 is not supported yet: write the string");
        return 0;
    }
    if (!fits(nt, type))
    {
        diag_at(u, pn->tok, "error", "the pattern '%.*s' is %s, but the value matched is %s", t->len, TSTART(t),
                type_str(nt, x, sizeof(x)), type_str(type, y, sizeof(y)));
        return 0;
    }
    uint64_t v;
    if (!name_pattern_value(u, pn, type, &v))
    {
        return 0;
    }
    if (nt == T_INT_LIT && type_is_int(type)) // an unsized constant: its value must fit
    {
        int negative;
        sema_unsized_value(u, pn->a, &v, &negative);
        Exact val = negative ? (Exact)(int64_t)v : (Exact)v;
        int ok = val >= cover_min(type) && val <= cover_max(type);
        if (!ok)
        {
            diag_at(u, pn->tok, "error", "the pattern '%.*s' does not fit %s", t->len, TSTART(t),
                    type_str(type, y, sizeof(y)));
            return 0;
        }
    }
    return 1;
}

// In a specialization, the arm a match takes while compiling: every subject an arm tests is known
// (a value parameter, or a constant), so the first arm that matches is the only one there is.
// 0 when some arm cannot be decided.
static NodeId static_arm(Check *c, NodeId id, const TypeId *types, const NodeId *params, int ns)
{
    Unit *u = c->u;
    const AstNode *m = NODE(u, id);
    uint64_t known[16];
    int is_known[16];
    NodeId value = m->a;
    for (int k = 0; k < ns; k++, value = value ? NEXT(u, value) : 0)
    {
        is_known[k] = 0;
        Exact e;
        if (!m->a && params[k])
        {
            NameId name = token_name(TOK(u, NODE(u, params[k])->tok));
            for (uint32_t b = 0; binding && b < binding->n; b++)
            {
                if (binding->is_value[b] && binding->names[b] == name)
                {
                    known[k] = binding->values[b];
                    is_known[k] = 1;
                }
            }
        }
        else if (value && exact_value(u, value, &e))
        {
            known[k] = (uint64_t)e;
            is_known[k] = 1;
        }
    }
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm))
    {
        const AstNode *a = NODE(u, arm);
        int matches = 1, k = 0;
        for (NodeId pat = a->a; pat && k < ns && matches; pat = NEXT(u, pat), k++)
        {
            const AstNode *pn = NODE(u, pat);
            if (pn->aux & PAT_ANY)
            {
                continue;
            }
            if (!is_known[k] || types[k] == T_STR)
            {
                return 0;
            }
            if ((pn->aux & (PAT_NAME | PAT_ENDS)) && pn->a)
            {
                check_expr(c, pn->a);
            }
            if ((pn->aux & PAT_ENDS) && pn->b)
            {
                check_expr(c, pn->b);
            }
            Pattern pt;
            if (!sema_arm_pattern(u, pat, types[k], &pt) || pt.kind == PATTERN_TEXT)
            {
                return 0;
            }
            Exact v = cover_value(types[k], known[k]), lo = cover_value(types[k], pt.lo);
            Exact hi = pt.kind == PATTERN_RANGE ? cover_value(types[k], pt.hi) : lo;
            matches = v >= lo && v <= hi;
        }
        if (matches)
        {
            return arm;
        }
    }
    return 0;
}

// Is `call` (not checked yet) a call to a function that says `, error`? Its callee is looked up
// by name, as check_call will.
static int call_fails(Check *c, NodeId call)
{
    const AstNode *cn = NODE(c->u, NODE(c->u, call)->a);
    if (cn->kind != AST_IDENT)
    {
        return 0;
    }
    SymId sym;
    TypeId type;
    NameKind k = find_name(c, TOK(c->u, cn->tok), &sym, &type);
    if (k == NAME_SYMBOL && SYM(sym)->kind == SYM_IMPORT)
    {
        sym = module_binding_function(sym);
    }
    for (SymId f = (k == NAME_SYMBOL || k == NAME_MEMBER) ? sym : 0; f; f = SYM(f)->next_overload)
    {
        if (SYM(f)->kind == SYM_METHOD && sema_fn_fails(f))
        {
            return 1;
        }
    }
    return 0;
}

// What an arm of a stage on a call that can fail tests (guards.md §5): a type as a pattern says
// which side, an error's name one error, anything else the value.
int sema_error_arm(const Unit *u, NodeId pat, TypeId value_type, uint64_t *id)
{
    const AstNode *p = NODE(u, pat);
    *id = 0;
    if (p->aux & PAT_ANY)
    {
        return EARM_ANY;
    }
    if (!(p->aux & PAT_NAME) || !p->a || (NODE(u, p->a)->kind != AST_IDENT && NODE(u, p->a)->kind != AST_SELECTOR))
    {
        return EARM_VALUE;
    }
    const Token *t = TOK(u, NODE(u, p->a)->tok);
    if (NODE(u, p->a)->kind == AST_SELECTOR) // `panic.division`: a module's error
    {
        SymId s = u->refs ? u->refs[p->a] : 0;
        uint64_t v;
        if (s && SYM(s)->kind == SYM_CONST && sema_comptime_value(s, &v))
        {
            *id = v;
            return EARM_NAME;
        }
        return EARM_VALUE;
    }
    if (text_is(t, "error"))
    {
        return EARM_ERROR;
    }
    if (text_is(t, "nil"))
    {
        return EARM_NAME; // no error: 0
    }
    TypeId prim = prim_type(t);
    if (prim != T_NONE && prim != T_VOID)
    {
        (void)value_type;
        return EARM_VALUE_TYPE;
    }
    SymId s = u->refs ? u->refs[p->a] : 0;
    uint64_t v;
    if (s && SYM(s)->kind == SYM_CONST && sema_value_type((Unit *)&cx.units[SYM(s)->unit], s) == T_ERROR &&
        sema_comptime_value(s, &v))
    {
        *id = v;
        return EARM_NAME;
    }
    return EARM_VALUE;
}

// A stage on a call that can fail: its value and its error, each arm on one side. What no arm
// takes goes on: a value passes (it must fit the arms), an error goes up (or to `n, e :=`).
static TypeId check_error_match(Check *c, NodeId id)
{
    Unit *u = c->u;
    AstNode *m = NODE(u, id);
    NodeId subject = m->a;
    NodeId saved = c->handled;
    c->handled = subject; // the stage takes the call's error
    TypeId vt = check_expr(c, subject);
    c->handled = saved;
    SymId fn = u->refs[subject];
    int errors_taken = 0, values_taken = 0, value_arms = 1;
    TypeId result = T_NONE;
    char x[64], y[64];
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm))
    {
        const AstNode *a = NODE(u, arm);
        NodeId pat = a->a;
        if (NEXT(u, pat))
        {
            diag_at(u, a->tok, "error", "an arm on a call that can fail takes one pattern");
            continue;
        }
        const AstNode *pn = NODE(u, pat);
        if ((pn->aux & PAT_NAME) && pn->a && NODE(u, pn->a)->kind == AST_SELECTOR)
        {
            check_expr(c, pn->a); // `panic.division`
        }
        if ((pn->aux & PAT_NAME) && pn->a && NODE(u, pn->a)->kind == AST_IDENT)
        {
            const Token *t = TOK(u, NODE(u, pn->a)->tok);
            if (!text_is(t, "error") && !text_is(t, "nil") && prim_type(t) == T_NONE)
            {
                check_expr(c, pn->a); // a name: it must exist (an error, or a value)
            }
        }
        uint64_t eid;
        int cls = sema_error_arm(u, pat, vt, &eid);
        Pattern pt;
        if (cls == EARM_ANY)
        {
            errors_taken = values_taken = 1;
        }
        else if (cls == EARM_ERROR)
        {
            errors_taken = 1;
        }
        else if (cls == EARM_VALUE_TYPE)
        {
            const Token *t = TOK(u, NODE(u, pn->a)->tok);
            if (prim_type(t) != vt)
            {
                diag_at(u, pn->tok, "error", "'%.*s' is not the value's type, %s", t->len, TSTART(t),
                        type_str(vt, x, sizeof(x)));
            }
            values_taken = 1;
        }
        else if (cls == EARM_VALUE && !sema_arm_pattern(u, pat, vt, &pt) && !sema_arm_name(u, pat, vt))
        {
            diag_at(u, pn->tok, "error", "this pattern does not fit the value, %s, nor is it an error",
                    type_str(vt, x, sizeof(x)));
        }
        NodeId r = a->b;
        TypeId t = check_expr(c, r);
        if (type_kind(t) == T_FUNC && NODE(u, r)->kind == AST_IDENT) // `u64: double`: given the value
        {
            SymId f = type_ref(t);
            NodeId p = sema_fn_params(u, f);
            if (!p || NEXT(&cx.units[SYM(f)->unit], p) ||
                !fits(vt, sema_param_type(&cx.units[SYM(f)->unit], f, p)))
            {
                diag_at(u, NODE(u, r)->tok, "error", "a function named in an arm is given the value, %s: it "
                        "takes one parameter of that type", type_str(vt, x, sizeof(x)));
            }
            note_call(c, f);
            t = fn_ret_type(u, f);
        }
        if (is_statement(u, r) || t == T_VOID)
        {
            value_arms = 0;
            continue;
        }
        if (result == T_NONE || result == T_INT_LIT)
        {
            result = t == T_NONE ? result : t;
        }
        else if (!fits(t, result) && t != T_NONE)
        {
            diag_at(u, a->tok, "error", "this arm gives %s, but the arms above give %s",
                    type_str(t, x, sizeof(x)), type_str(result, y, sizeof(y)));
        }
    }
    m->aux |= MATCH_ERRORS;
    if (!errors_taken && c->handled != id && fn)
    {
        error_goes_up(c, m->tok, fn);
        note_edge(c, fn, EE_ERRORS);
    }
    if (!value_arms || (m->aux & MATCH_STATEMENT))
    {
        return T_VOID;
    }
    if (result == T_INT_LIT)
    {
        result = vt;
    }
    if (!values_taken)
    {
        if (!fits(vt, result))
        {
            diag_at(u, m->tok, "error", "a value no arm takes goes on, but it is %s and the arms give %s: add "
                    "`%s: ...` or `_: ...`", type_str(vt, x, sizeof(x)), type_str(result, y, sizeof(y)),
                    type_str(vt, x, sizeof(x)));
        }
        m->aux |= MATCH_PASS;
    }
    return result;
}

static TypeId check_match(Check *c, NodeId id)
{
    Unit *u = c->u;
    AstNode *m = NODE(u, id);
    TypeId types[16];
    NodeId params[16];
    int ns = match_subjects(c, id, types, params, 16);
    int on_params = m->a == 0;
    NodeId only = ns && binding && c->fn && spec_of(c->fn) ? static_arm(c, id, types, params, ns) : 0;
    if (only)
    {
        // known while compiling: the other arms are not part of this specialization (not checked,
        // not generated), which is what ends a recursion over a field number
        if (ntaken < TAKEN_MAX)
        {
            taken[ntaken++].fn = c->fn;
            taken[ntaken - 1].match = id;
            taken[ntaken - 1].arm = only;
        }
        NodeId r = NODE(u, only)->b;
        int saved = c->nlocals, saved_base = c->block_base;
        c->block_base = c->nlocals;
        TypeId t;
        if (on_params && !is_statement(u, r) && c->ret != T_VOID)
        {
            check_return(c, only, r);
            t = T_VOID;
        }
        else
        {
            t = check_expr(c, r);
        }
        c->nlocals = saved;
        c->block_base = saved_base;
        return on_params || (m->aux & MATCH_STATEMENT) ? T_VOID : t;
    }
    if (!ns)
    {
        for (NodeId arm = m->b; arm; arm = NEXT(u, arm))
        {
            check_expr(c, NODE(u, arm)->b);
        }
        return T_NONE;
    }
    // One cover per subject; an arm adds to the subject it tests when the others are names
    // (one column decides). With several literal columns only the last arm is followed.
    static Cover covers[16];
    for (int s = 0; s < ns; s++)
    {
        covers[s] = (Cover){.type = types[s]};
    }
    // The local each subject is (a name matched, or a parameter): its range fact in an arm.
    int subj_local[16];
    NameId subj_name[16];
    for (int k = 0; k < ns; k++)
    {
        subj_local[k] = -1;
        subj_name[k] = 0;
        NodeId value = m->a;
        for (int j = 0; value && j < k; j++)
        {
            value = NEXT(u, value);
        }
        uint32_t name_tok = on_params ? (params[k] && SYM(c->fn)->unit == u->id ? NODE(u, params[k])->tok : 0)
                          : value && NODE(u, value)->kind == AST_IDENT ? NODE(u, value)->tok : 0;
        if (!name_tok)
        {
            continue;
        }
        NameId want = token_name(TOK(u, name_tok));
        subj_name[k] = want;
        for (int i = c->nlocals - 1; i >= 0 && subj_local[k] < 0; i--)
        {
            subj_local[k] = c->locals[i].name == want ? i : -1;
        }
    }
    TypeId result = T_NONE;
    int value_arms = 1, index = 0, all_names_last = 0;
    NodeId last_arm = 0;
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm), index++)
    {
        last_arm = arm;
        const AstNode *a = NODE(u, arm);
        int saved = c->nlocals, saved_base = c->block_base;
        c->block_base = c->nlocals;
        int np = 0, literal_col = -1, literals = 0, runtime = 0;
        int lone_any = !a->aux && NODE(u, a->a)->kind == AST_PATTERN && (NODE(u, a->a)->aux & PAT_ANY) &&
                       !NEXT(u, a->a);
        for (NodeId pat = a->a; pat; pat = NEXT(u, pat), np++)
        {
            const AstNode *pn = NODE(u, pat);
            if (np >= ns || (pn->aux & PAT_ANY))
            {
                continue;
            }
            if (pn->aux & PAT_NAME)
            {
                if (!name_pattern(c, pn, types[np], subj_name[np]))
                {
                    runtime++; // compared when the program runs: it adds nothing the compiler can count
                    continue;
                }
            }
            else if (pn->aux & PAT_ENDS)
            {
                for (NodeId e = pn->a; e; e = e == pn->a ? pn->b : 0)
                {
                    check_expr(c, e);
                }
                Pattern pt;
                if (!ends_pattern(u, pn, types[np], &pt, 1))
                {
                    literals = 2; // reported: no cover from this arm
                }
            }
            else
            {
                Pattern pt;
                pattern_from(u, pn, types[np], &pt, 1);
            }
            literals++;
            literal_col = np;
        }
        if (lone_any)
        {
            np = ns; // `_` alone: every value of every parameter
        }
        else if (ns > 1 && !a->aux)
        {
            diag_at(u, a->tok, "error", "this match has %d values: write one pattern for each, in parentheses "
                                        "(`(0, _): ...`), or `_` alone for all of them",
                    ns);
            np = ns;
            literals = 2; // reported: no cover
        }
        if (np != ns)
        {
            diag_at(u, a->tok, "error", "this arm has %d pattern%s, but the match has %d value%s (%s)", np,
                    np == 1 ? "" : "s", ns, ns == 1 ? "" : "s",
                    on_params ? "one per parameter" : "one per value after `if`");
        }
        else if (all_names_last)
        {
            diag_at(u, a->tok, "error", "this arm never runs: an arm above takes every value (`_` in each "
                                        "place). Move it above that arm, or remove it");
        }
        else if (runtime)
        {
            // A value known only at run time: the arm is tested in its place, first wins, and the
            // compiler counts nothing from it (neither coverage nor overlap).
        }
        else if (literals <= 1)
        {
            int col = literals ? literal_col : 0;
            NodeId pat = a->a;
            for (int k = 0; k < col; k++)
            {
                pat = NEXT(u, pat);
            }
            if (literals == 0)
            {
                all_names_last = 1;
                int full = ns == 1 && cover_full(&covers[0]);
                if (full)
                {
                    diag_at(u, a->tok, "error", "this arm never runs: the arms above already take every "
                                                "value. Remove it");
                }
            }
            else
            {
                char overlap[100];
                int took = cover_take(u, &covers[col], pat, overlap, sizeof(overlap));
                if (!took)
                {
                    diag_at(u, a->tok, "error", "this arm never runs: the arms above already take every "
                                                "value it takes. Move it above them, or remove it");
                }
                else if (took == 2)
                {
                    diag_at(u, a->tok, "error", "this arm overlaps an arm above on %s: arms take disjoint "
                                                "values, so what each takes is what it says. Leave %s out of "
                                                "this one", overlap, overlap);
                }
            }
        }
        // Inside the arm, the value keeps its name and is known to be in the arm's values: when
        // those exclude 0 and the arm does not assign to it, a division by it needs no guard.
        NodeId r = a->b;
        int fact = -1, fact_below = -1;
        if (literals == 1 && literal_col >= 0 && subj_local[literal_col] >= 0)
        {
            NodeId pat = a->a;
            for (int k = 0; k < literal_col; k++)
            {
                pat = NEXT(u, pat);
            }
            Pattern pt;
            if (sema_arm_pattern(u, pat, types[literal_col], &pt) && pt.kind != PATTERN_TEXT &&
                types[literal_col] != T_BOOL)
            {
                Exact lo = cover_value(types[literal_col], pt.lo);
                Exact hi = pt.kind == PATTERN_RANGE ? cover_value(types[literal_col], pt.hi) : lo;
                const Local *lc = &c->locals[subj_local[literal_col]];
                int unchanged = !assigns_to(u, r, lc->name);
                if ((lo > 0 || hi < 0) && !lc->nonzero && unchanged)
                {
                    fact = subj_local[literal_col];
                    c->locals[fact].nonzero = 1;
                }
                if (lo >= 0 && !lc->below && unchanged) // an index below hi + 1 needs no bounds check
                {
                    fact_below = subj_local[literal_col];
                    c->locals[fact_below].below = (uint64_t)hi + 1;
                }
            }
        }
        TypeId t;
        if (on_params && !is_statement(u, r) && c->ret != T_VOID)
        {
            check_return(c, arm, r);
            t = c->ret;
        }
        else
        {
            t = check_expr(c, r);
            if (is_statement(u, r) || t == T_VOID)
            {
                value_arms = 0;
            }
        }
        if (value_arms && !on_params)
        {
            char x[64], y[64];
            if (result == T_NONE || result == T_INT_LIT)
            {
                if (result == T_INT_LIT && !fits(T_INT_LIT, t) && t != T_NONE)
                {
                    diag_at(u, a->tok, "error", "this arm gives %s, but the arms above give a number",
                            type_str(t, x, sizeof(x)));
                }
                result = t == T_NONE ? result : t;
            }
            else if (!fits(t, result) && t != T_NONE)
            {
                diag_at(u, a->tok, "error", "this arm gives %s, but the arms above give %s: every arm of a "
                                            "match used as a value gives the same type",
                        type_str(t, x, sizeof(x)), type_str(result, y, sizeof(y)));
            }
        }
        if (fact >= 0)
        {
            c->locals[fact].nonzero = 0;
        }
        if (fact_below >= 0)
        {
            c->locals[fact_below].below = 0;
        }
        c->nlocals = saved;
        c->block_base = saved_base;
    }
    // Every value must be taken: `_` last, or literals that cover the whole type.
    int full = all_names_last;
    for (int s = 0; s < ns && !full; s++)
    {
        full = ns == 1 && cover_full(&covers[s]);
    }
    // A statement may leave values untaken (it does nothing for them): a warning. A value, or a
    // body that returns one, takes them all.
    const char *level = (m->aux & MATCH_STATEMENT) || (on_params && c->ret == T_VOID) ? "warning" : "error";
    SymId err_callee = 0; // `if e { ... }` with `n, e := f()`: checked against f's set, at the end
    if (!full && ns == 1 && types[0] == T_ERROR && m->a && NODE(u, m->a)->kind == AST_IDENT &&
        !(m->aux & MATCH_PARTIAL))
    {
        NameId en = token_name(TOK(u, NODE(u, m->a)->tok));
        for (int i = c->nlocals - 1; i >= 0; i--)
        {
            if (c->locals[i].name == en)
            {
                err_callee = c->locals[i].err_of;
                break;
            }
        }
    }
    if (err_callee)
    {
        if (nerr_matches == err_matches_cap)
        {
            uint32_t nc = err_matches_cap ? err_matches_cap * 2 : 16;
            ErrMatch *grown = (ErrMatch *)arena_alloc(nc * sizeof(ErrMatch));
            if (nerr_matches)
            {
                memcpy(grown, err_matches, nerr_matches * sizeof(ErrMatch));
            }
            err_matches = grown;
            err_matches_cap = nc;
        }
        ErrMatch *em = &err_matches[nerr_matches++];
        *em = (ErrMatch){u, *TOK(u, m->tok), err_callee, !(m->aux & MATCH_STATEMENT) && value_arms, 0, {0}};
        for (NodeId arm = m->b; arm && em->ntaken < 32; arm = NEXT(u, arm))
        {
            Pattern pt;
            if (sema_arm_pattern(u, NODE(u, arm)->a, T_ERROR, &pt) && pt.kind == PATTERN_VALUE)
            {
                em->taken[em->ntaken++] = pt.lo;
            }
        }
    }
    else if (!full && (m->aux & MATCH_PARTIAL)) // a pipe stage: what no arm takes goes on (std.md §14)
    {
        if (value_arms && result != T_NONE && !(m->aux & MATCH_STATEMENT))
        {
            char x[64], y[64];
            if (ns != 1 || !(fits(types[0], result) || (types[0] == T_INT_LIT && fits(T_INT_LIT, result))))
            {
                diag_at(u, m->tok, "error",
                        "the values no arm takes go on as the stage's value, but they are %s and the arms give "
                        "%s: end with `_: ...`",
                        type_str(types[0], x, sizeof(x)), type_str(result, y, sizeof(y)));
            }
            m->aux |= MATCH_PASS;
        }
    }
    else if (!full)
    {
        char gaps[160] = "";
        if (ns == 1 && types[0] != T_STR && type_is_int(types[0]) | (types[0] == T_BOOL))
        {
            cover_gaps(&covers[0], gaps, sizeof(gaps));
        }
        if (gaps[0])
        {
            diag_at(u, m->tok, level, "not every value is taken: no arm takes %s. Add arms for them, or end "
                                      "with `_: ...`, which takes the rest", gaps);
        }
        else
        {
            diag_at(u, m->tok, level, "not every value is taken: end with `_: ...`, which takes the rest");
        }
    }
    else if (!all_names_last && last_arm)
    {
        m->aux |= MATCH_LAST_FREE; // literals that cover the type: the last one needs no test
    }
    if (on_params)
    {
        return T_VOID;
    }
    return value_arms ? (result == T_NONE ? T_NONE : result) : T_VOID;
}

// Makes sure the per-node tables cover every node, and clears the entries of [from, size).
static void side_tables(Unit *u, NodeId from)
{
    if (u->nodes.size > u->side_cap)
    {
        // Sized like the node array, which already has room for the largest body.
        size_t cap = u->nodes.cap > u->nodes.size ? u->nodes.cap : u->nodes.size;
        TypeId *types = (TypeId *)arena_alloc(cap * sizeof(TypeId));
        SymId *refs = (SymId *)arena_alloc(cap * sizeof(SymId));
        if (u->side_cap)
        {
            memcpy(types, u->types, u->side_cap * sizeof(TypeId));
            memcpy(refs, u->refs, u->side_cap * sizeof(SymId));
        }
        u->types = types;
        u->refs = refs;
        u->side_cap = cap;
    }
    memset(u->types + from, 0, (u->nodes.size - from) * sizeof(TypeId));
    memset(u->refs + from, 0, (u->nodes.size - from) * sizeof(SymId));
}

// Checks one function. `fn` is a method declaration or a function literal; `sym` is its
// symbol when it is a declared function (0 for a literal inside another body).
//
// This is where memory is kept small. A deferred body is tokenized and parsed here, into
// the tails of the token and node arrays; its module paths and types are checked; the
// backend, if one is running, emits the function while the body is in memory; then both
// tails are cut off again. At any moment the compiler holds the declarations of the file
// plus the tokens and tree of one function.
// Does every way through `block` end in a `return`? Without conditionals or loops there is one
// way: a `return` among its statements, or in a block among them.
static int always_returns(const Unit *u, NodeId block)
{
    const AstNode *n = NODE(u, block);
    if (n->kind == AST_RETURN)
    {
        return 1;
    }
    if (n->kind == AST_IF) // both branches return
    {
        const AstNode *br = NODE(u, n->b);
        return br->b && always_returns(u, br->a) && always_returns(u, br->b);
    }
    if (n->kind == AST_MATCH) // every arm returns: on parameters, a value is the return
    {
        for (NodeId arm = n->b; arm; arm = NEXT(u, arm))
        {
            NodeId r = NODE(u, arm)->b;
            if (!(n->a == 0 && !is_statement(u, r)) && !always_returns(u, r))
            {
                return 0;
            }
        }
        return 1;
    }
    if (n->kind != AST_BLOCK)
    {
        return 0;
    }
    for (NodeId s = n->a; s; s = NEXT(u, s))
    {
        if (always_returns(u, s))
        {
            return 1;
        }
    }
    return 0;
}

static void check_callable(Check *c, NodeId fn, SymId owner, SymId sym)
{
    Unit *u = c->u;
    if (sym && SYM(sym)->kind == SYM_METHOD && !spec_of(sym) && sema_is_generic(sym))
    {
        return; // made once per type at its calls (specialize): never on its own
    }
    // A specialization reads its bound names (T, i) in its signature and body; any other function
    // reads none (a function literal keeps the binding of the body it is in).
    const Spec *saved_binding = binding;
    if (sym && spec_of(sym))
    {
        binding = spec_of(sym);
        forget_taken(sym);
    }
    else if (sym && SYM(sym)->kind == SYM_METHOD)
    {
        binding = NULL;
    }
    // Copies, not a pointer: parsing the body may move the node array.
    uint8_t flags = NODE(u, fn)->flags;
    uint8_t kind = NODE(u, fn)->kind;
    NodeId params = NODE(u, fn)->a;
    NodeId ret_node = fn_ret_node(u, fn);
    NodeId body = fn_body(u, fn);
    uint32_t deferred_tok = body;
    size_t mark = u->nodes.size;
    size_t tok_mark = u->tokens.size;
    // A specialization checked while another of the same function is (soma for i + 1 inside soma
    // for i): that one's body is parsed in the declaration, so this one parses its own copy, and
    // gives the declaration back as it found it.
    int nested = 0;
    NodeId outer_body = 0;
    uint8_t outer_flags = 0, outer_sig_flags = 0;
    if (sym && spec_of(sym) && !(flags & AST_F_LAZY) && generic_body_token(spec_of(sym)->of))
    {
        nested = 1;
        outer_body = body;
        outer_flags = flags;
        outer_sig_flags = NODE(u, NODE(u, fn)->b)->flags;
        deferred_tok = body = generic_body_token(spec_of(sym)->of);
        flags |= AST_F_LAZY;
    }

    if (flags & AST_F_LAZY)
    {
        uint64_t t = cx.timing ? clock_ns() : 0;
        body = parse_function_body(u, deferred_tok);
        if (cx.timing)
        {
            phase_end(PHASE_PARSE, t);
        }
        if (!body)
        {
            u->nodes.size = mark; // errors inside, already reported
            u->tokens.size = tok_mark;
            if (nested)
            {
                fn_set_body(u, fn, outer_body);
                NODE(u, fn)->flags = outer_flags;
                NODE(u, NODE(u, fn)->b)->flags = outer_sig_flags;
            }
            binding = saved_binding;
            return;
        }
        fn_set_body(u, fn, body);
        NODE(u, fn)->flags = (uint8_t)(flags & ~AST_F_LAZY);
        NODE(u, NODE(u, fn)->b)->flags &= (uint8_t)~AST_F_LAZY;
        side_tables(u, (NodeId)mark);
        module_body_nodes(u, (NodeId)mark);
    }

    // Only the scalars are saved: the locals stay where they are, which is also what lets a
    // nested function literal see the enclosing ones.
    SymId saved_owner = c->owner;
    int saved_self = c->has_self, saved_in = c->in_callable, saved_base = c->block_base;
    int saved_locals = c->nlocals;
    TypeId saved_ret = c->ret;
    SymId saved_literal = c->literal;
    SymId saved_fn = c->fn;
    int saved_fn_base = c->fn_base;
    c->fn = sym;
    if (sym)
    {
        note_seen(sym); // its body: what it can raise is noted as it is checked
    }
    if (sym && cx.kinds)
    {
        facts_of(sym)->checked = 1;
        if (SYM(sym)->kind == SYM_METHOD)
        {
            note_call(c, sema_next_clause(u, sym)); // a pattern that does not match goes on to the next
        }
    }
    c->literal = kind == AST_FN_LIT ? sym : 0;
    c->fn_base = c->nlocals;
    int pushed_lit = kind == AST_FN_LIT && c->nlits < 16;
    if (pushed_lit)
    {
        c->lits[c->nlits] = sym;
        c->lit_base[c->nlits++] = c->nlocals;
    }

    c->owner = owner;
    c->has_self = (flags & AST_F_SELF) ? 1 : (kind == AST_FN_LIT && saved_self);
    c->in_callable = 1;
    c->ret = resolve_type(u, ret_node, owner, 1);
    int saved_fails = c->fails;
    {
        NodeId sig = NODE(u, fn)->b;
        c->fails = sig && (NODE(u, sig)->aux & SIG_FAILS);
    }
    c->block_base = c->nlocals;

    for (NodeId p = params; p; p = NEXT(u, p))
    {
        const Token *name = TOK(u, NODE(u, p)->tok);
        if (NODE(u, p)->flags & AST_F_PATTERN)
        {
            uint64_t value;
            sema_pattern(u, p, owner, &value, 1); // a pattern binds no name: only its fit is checked
            continue;
        }
        if (!is_self_token(TSTART(name), (int)name->len))
        {
            push_local(c, NODE(u, p)->tok, param_type(u, p, owner, 1),
                       (NODE(u, p)->flags & AST_F_MUT) != 0);
            c->locals[c->nlocals - 1].param = 1;
            if (NODE(u, p)->flags & AST_F_CONTEXT)
            {
                c->locals[c->nlocals - 1].context = 1;
            }
        }
    }

    if ((flags & AST_F_ONELINE) && NODE(u, fn)->kind == AST_FN_LIT && !ret_node)
    {
        check_expr(c, body); // `(k: u64): k * limit`: its result's type is the expression's
    }
    else if (flags & AST_F_ONELINE)
    {
        check_return(c, body, body); // `T name(params): expr` returns the expression
    }
    else
    {
        check_expr(c, body);
        if (c->ret != T_VOID && c->ret != T_NONE && !always_returns(u, body))
        {
            const Token *at = TOK(u, NODE(u, fn)->tok);
            int len = (int)at->len;
            const char *name = TSTART(at);
            if (sym) // a literal is named by its binding, not by its '('
            {
                name = symbol_name(sym, &len);
            }
            char t[64];
            diag_at(u, NODE(u, fn)->tok, "error",
                    "'%.*s' returns %s, but its body can reach its end without `return`", len,
                    name, type_str(c->ret, t, sizeof(t)));
        }
    }

    c->literal = saved_literal;
    if (pushed_lit)
    {
        c->nlits--;
    }
    c->fn = saved_fn;
    c->fn_base = saved_fn_base;
    c->owner = saved_owner;
    c->has_self = saved_self;
    c->in_callable = saved_in;
    c->block_base = saved_base;
    c->nlocals = saved_locals;
    c->ret = saved_ret;
    c->fails = saved_fails;

    if (sym)
    {
        if (capture_mir)
        {
            comptime_capture(u, sym); // the compiler is going to run it
        }
        else if (!u->is_manifest) // a function of mod.modal runs only in the compiler: never generated
        {
            codegen_function(u, sym);
        }
    }
    if (kind != AST_FN_LIT && !capture_mir && literal_specs_waiting())
    {
        // a specialization made for a literal of this body (`apply(u64, 5, x: x * limit)`) is
        // checked now, while the literal's nodes are here: once the body is released they are gone
        sema_drain_specs();
    }
    if (flags & AST_F_LAZY)
    {
        fn_set_body(u, fn, deferred_tok); // back to "not parsed": its tokens and nodes are gone
        NODE(u, fn)->flags = flags;
        NODE(u, NODE(u, fn)->b)->flags |= AST_F_LAZY;
        u->nodes.size = mark;
        u->tokens.size = tok_mark;
    }
    if (nested) // the outer specialization's body, as it was
    {
        fn_set_body(u, fn, outer_body);
        NODE(u, fn)->flags = outer_flags;
        NODE(u, NODE(u, fn)->b)->flags = outer_sig_flags;
    }
    binding = saved_binding;
}

// Depth is bounded by the parser's nesting limit, so this recursion is too.
// The field `name` of struct s (a field and its accessor may share a name), or 0.
static SymId field_named(SymId s, const Token *name)
{
    for (SymId m = scope_find_token(struct_scope(s), name); m; m = SYM(m)->next)
    {
        int len;
        const char *text = symbol_name(m, &len);
        if (SYM(m)->kind == SYM_FIELD && SYM(m)->owner == s && (uint32_t)len == name->len &&
            memcmp(text, TSTART(name), name->len) == 0)
        {
            return m;
        }
    }
    return 0;
}

// `point{ x, y: e }`. Every field is given (no field starts at a value nobody wrote), each once.
// It is built where point's fields are seen: in its methods, or anywhere when its face shows
// every field.
static TypeId check_struct_lit(Check *c, NodeId id)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    const Token *tname = TOK(u, n->tok);
    SymId s;
    TypeId found;
    NameKind kind = find_name(c, tname, &s, &found);
    if (kind == NAME_MISSING)
    {
        report_missing(c, n->tok, 0);
        return T_NONE;
    }
    if (s && SYM(s)->kind == SYM_IMPORT) // `point := import("./geo").shapes.point`
    {
        s = module_binding_target(s);
    }
    if (!s || SYM(s)->kind != SYM_STRUCT)
    {
        diag_at(u, n->tok, "error", "'%.*s' is not a struct: a literal `name{ ... }` builds a struct", tname->len,
                TSTART(tname));
        return T_NONE;
    }
    const Unit *home = &cx.units[SYM(s)->unit];
    for (NodeId f = n->a; f; f = NEXT(u, f))
    {
        const AstNode *fn = NODE(u, f);
        const Token *fname = TOK(u, fn->tok);
        SymId field = field_named(s, fname);
        TypeId vt = check_expr(c, fn->a);
        if (!field)
        {
            diag_at(u, fn->tok, "error", "'%.*s' has no field '%.*s'", tname->len, TSTART(tname), fname->len,
                    TSTART(fname));
            continue;
        }
        for (NodeId g = n->a; g != f; g = NEXT(u, g))
        {
            if (u->refs[g] == field)
            {
                diag_at(u, fn->tok, "error", "'%.*s' is given twice", fname->len, TSTART(fname));
            }
        }
        u->refs[f] = field;
        note_use(c, field);
        TypeId ft = symbol_type(c, field);
        char a[64], b[64];
        if (vt != T_NONE && vt != T_EXTERN && !fits(vt, ft))
        {
            diag_at(u, fn->tok, "error", "'%.*s' is %s, but this value is %s", fname->len, TSTART(fname),
                    type_str(ft, a, sizeof(a)), type_str(vt, b, sizeof(b)));
        }
        else if (vt == T_INT_LIT)
        {
            check_bounds(c, fn->a, ft); // `point{ x: 300 }` with a u8 x
        }
    }
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL)
        {
            continue;
        }
        SymId field = field_named(s, TOK(home, NODE(home, m)->tok));
        int given = 0;
        for (NodeId f = n->a; f && !given; f = NEXT(u, f))
        {
            given = u->refs[f] == field;
        }
        if (!given)
        {
            const Token *missing = TOK(home, NODE(home, m)->tok);
            diag_at(u, n->tok, "error", "'%.*s' is not given: a literal gives every field (`%.*s{ ..., %.*s: 0 }`)",
                    missing->len, TSTART(missing), tname->len, TSTART(tname), missing->len, TSTART(missing));
        }
    }
    return type_make(T_STRUCT, s);
}

static TypeId check_expr(Check *c, NodeId id)
{
    if (!id)
    {
        return T_VOID;
    }
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    TypeId type = T_VOID;

    switch (n->kind)
    {
    case AST_NUMBER_LIT:
        type = T_INT_LIT;
        break;
    case AST_SITE:
        type = T_STR;
        break;
    case AST_STRING_LIT:
    {
        const Token *t = TOK(u, n->tok);
        for (uint32_t i = 1; i + 1 < t->len; i++) // the escapes a string may contain
        {
            if (TSTART(t)[i] == '\\')
            {
                char esc = TSTART(t)[++i];
                if (!strchr("ntr0\\\"", esc) || esc == '\0')
                {
                    diag_at(u, n->tok, "error",
                            "unknown escape '\\%c' in a string (known: \\n \\t \\r \\0 \\\\ \\\")",
                            esc);
                    break;
                }
            }
        }
        type = T_STR;
        break;
    }
    case AST_IDENT:
    {
        SymId sym;
        c->found_nonzero = 0;
        NameKind kind = find_name(c, TOK(u, n->tok), &sym, &type);
        cx.names_resolved++;
        if (kind == NAME_LOCAL && c->found_nonzero)
        {
            NODE(u, id)->aux |= IDENT_NONZERO; // the arm's test proved it: no divisor guard
        }
        if (kind == NAME_MISSING)
        {
            report_missing(c, n->tok, 0);
            type = T_NONE;
        }
        else if (kind == NAME_SYMBOL || kind == NAME_MEMBER)
        {
            u->refs[id] = sym;
            type = symbol_type(c, sym);
            compile_time_value(c, sym, n->tok);
            note_global(c, sym, n->tok);
            note_field(c, sym, n->tok); // a bare member name (implicit_self)
        }
        else if (TOK(u, n->tok)->kind == TOK_WRITE)
        {
            diag_at(u, n->tok, "error", "`write` can only be called: write(...)");
            type = T_NONE;
        }
        break;
    }
    case AST_CALL:
        type = check_call(c, id);
        break;
    case AST_SELECTOR:
        type = check_selector(c, id);
        break;
    case AST_BIN_OP:
        type = check_binary(c, n);
        break;
    case AST_UNARY_OP:
    {
        TypeId operand = check_expr(c, n->a);

        TokenKind op = (TokenKind)TOK(u, n->tok)->kind;
        // printf("debug: aa %s\noutput: ", TSTART(u->tokens.data));
        if (operand == T_VOID)
        {
            diag_at(u, n->tok, "error", "the operand has no value");
            type = T_NONE;
        }
        else if (operand == T_NONE || operand == T_EXTERN)
        {
            type = operand;
        }
        else if (op == TOK_AMP)
        {
            type = type_make(T_PTR, operand);
            if (NODE(u, n->a)->kind == AST_STRUCT_LIT &&
                !(c->returning_ref && c->has_self && operand == type_make(T_STRUCT, c->owner)) && !c->local_ref)
            {
                diag_at(u, n->tok, "error",
                        "a reference to a literal is a local's value, `p: *%.*s = &%.*s{ ... }` (it lives in "
                        "this function's frame), or a constructor's result, `return &%.*s{ ... }` in a method "
                        "with `self: %.*s` (built in self's storage)",
                        TOK(u, NODE(u, n->a)->tok)->len, TSTART(TOK(u, NODE(u, n->a)->tok)),
                        TOK(u, NODE(u, n->a)->tok)->len, TSTART(TOK(u, NODE(u, n->a)->tok)),
                        TOK(u, NODE(u, n->a)->tok)->len, TSTART(TOK(u, NODE(u, n->a)->tok)),
                        TOK(u, NODE(u, n->a)->tok)->len, TSTART(TOK(u, NODE(u, n->a)->tok)));
            }
        }
        else if (op == TOK_STAR)
        {
            type = type_kind(operand) == T_PTR ? type_ref(operand) : T_NONE;
            if (type == T_NONE)
            {
                diag_at(u, n->tok, "error", "only a pointer can be dereferenced");
            }
        }
        else if ((op == TOK_MINUS && !type_is_int(operand)) ||
                 (op == TOK_BANG && operand != T_BOOL))
        {
            char got[64];
            diag_at(u, n->tok, "error", "'%c' cannot be applied to %s", op == TOK_MINUS ? '-' : '!',
                    type_str(operand, got, sizeof(got)));
            type = T_NONE;
        }
        else
        {
            type = operand;
        }
        break;
    }
    case AST_PAREN_GROUP:
        type = check_expr(c, n->a);
        break;
    case AST_STRUCT_LIT:
        type = check_struct_lit(c, id);
        break;
    case AST_MATCH:
        type = (n->aux & MATCH_STAGE) && n->a && !NEXT(u, n->a) && NODE(u, n->a)->kind == AST_CALL &&
                       call_fails(c, n->a)
                   ? check_error_match(c, id)
                   : check_match(c, id);
        break;
    case AST_IF: // if cond then [else other]: a statement
    {
        TypeId cond = check_expr(c, n->a);
        if (cond != T_BOOL && cond != T_NONE && cond != T_EXTERN)
        {
            char got[64];
            diag_at(u, NODE(u, n->a)->tok, "error", "an if's condition is a bool, not %s",
                    type_str(cond, got, sizeof(got)));
        }
        const AstNode *br = NODE(u, n->b);
        check_expr(c, br->a);
        if (br->b)
        {
            check_expr(c, br->b);
        }
        type = T_VOID;
        break;
    }
    case AST_INDEX: // a[i]: an array's element, or a []u8's byte (read only)
    {
        TypeId base = check_expr(c, n->a);
        TypeId index = check_expr(c, n->b);
        if (base == T_NONE || base == T_EXTERN)
        {
            type = base;
            break;
        }
        if (!type_is_int(index))
        {
            char got[64];
            diag_at(u, NODE(u, n->b)->tok, "error", "an index is an integer, not %s",
                    type_str(index, got, sizeof(got)));
            type = T_NONE;
            break;
        }
        if (base == T_STR)
        {
            note_panic(c, PANIC_INDEX);
            type = T_U8;
            break;
        }
        if (type_kind(base) == T_PTR && type_kind(type_ref(base)) == T_ARRAY)
        {
            base = type_ref(base); // `p: *[24]u8`: p[i] is the array's element, in place
        }
        if (type_kind(base) != T_ARRAY)
        {
            char got[64];
            diag_at(u, n->tok, "error", "only an array or a []u8 can be indexed, not %s",
                    type_str(base, got, sizeof(got)));
            type = T_NONE;
            break;
        }
        const AstNode *ix = NODE(u, n->b);
        if (ix->kind == AST_IDENT && !u->refs[n->b])
        {
            NameId name = token_name(TOK(u, ix->tok));
            for (int i = c->nlocals - 1; i >= 0; i--)
            {
                if (c->locals[i].name == name)
                {
                    if (c->locals[i].below && c->locals[i].below <= type_len(base))
                    {
                        NODE(u, id)->aux |= INDEX_SAFE; // an arm took it in range: no bounds check
                    }
                    break;
                }
            }
        }
        Exact k;
        if (exact_value(u, n->b, &k) && (k < 0 || k >= (Exact)type_len(base)))
        {
            char got[64];
            diag_at(u, NODE(u, n->b)->tok, "error", "index %lld is out of range for %s (0 to %u)",
                    (long long)k, type_str(base, got, sizeof(got)), type_len(base) - 1);
        }
        if (!(NODE(u, id)->aux & INDEX_SAFE) && !(exact_value(u, n->b, &k) && k >= 0 && k < (Exact)type_len(base)))
        {
            note_panic(c, PANIC_INDEX);
        }
        type = type_ref(base);
        break;
    }
    case AST_ASSIGN:
    {
        TypeId target = check_expr(c, n->a);
        TypeId value = check_expr(c, n->b);
        const AstNode *t = NODE(u, n->a);
        SymId field = t->kind == AST_SELECTOR ? u->refs[n->a] : 0;

        const Local *local = NULL;
        // a[i] = v: the element of an array named by a local or a parameter, which must be
        // mutable (type-first). A []u8 is read only.
        const AstNode *named = t->kind == AST_INDEX ? NODE(u, t->a) : t;
        if (named->kind == AST_IDENT)
        {
            NameId name = token_name(TOK(u, named->tok));
            for (int i = c->nlocals - 1; i >= 0 && !local; i--)
            {
                local = c->locals[i].name == name ? &c->locals[i] : NULL;
            }
            if (local && c->literal && local - c->locals < c->fn_base)
            {
                const Token *nt = TOK(u, named->tok);
                diag_at(u, n->tok, "error",
                        "'%.*s' is a local of the function around this literal: the literal reads it, and "
                        "cannot assign to it (give it as a parameter, or return the new value)",
                        nt->len, TSTART(nt));
            }
        }
        TypeId through = t->kind == AST_INDEX ? u->types[t->a] : T_NONE;
        int by_pointer = type_kind(through) == T_PTR && type_kind(type_ref(through)) == T_ARRAY;
        if (by_pointer)
        {
            // `p[i] = v` with p: *[N]T: the array p points at is written, as the caller asked by
            // passing `&a` (the pointer itself is not changed)
            if (!fits(value, target))
            {
                char a[64], b[64];
                diag_at(u, n->tok, "error", "cannot assign %s to %s", type_str(value, a, sizeof(a)),
                        type_str(target, b, sizeof(b)));
            }
        }
        else if (t->kind == AST_INDEX && (named->kind != AST_IDENT || !local ||
                                     type_kind(u->types[t->a]) != T_ARRAY))
        {
            diag_at(u, n->tok, "error",
                    u->types[t->a] == T_STR ? "a []u8 is read only: its bytes cannot be assigned"
                                            : "only the elements of an array local or parameter can be assigned");
        }
        else if (t->kind == AST_INDEX)
        {
            if (!local->mut)
            {
                const Token *name = TOK(u, named->tok);
                diag_at(u, n->tok, "error",
                        "%s '%.*s' is immutable (declared `%.*s: T`). Declare it type-first, `T %.*s`, "
                        "to assign to its elements",
                        local->param ? "parameter" : "local", name->len, TSTART(name), name->len,
                        TSTART(name), name->len, TSTART(name));
            }
            else if (!fits(value, target))
            {
                char a[64], b[64];
                diag_at(u, n->tok, "error", "cannot assign %s to %s", type_str(value, a, sizeof(a)),
                        type_str(target, b, sizeof(b)));
            }
            else
            {
                check_bounds(c, n->b, target);
            }
        }
        else if (type_kind(target) == T_ARRAY)
        {
            diag_at(u, n->tok, "error", "an array is not assigned whole: assign its elements (`a[i] = v`)");
        }
        else if (t->kind != AST_IDENT && t->kind != AST_SELECTOR)
        {
            diag_at(u, n->tok, "error", "only a local or a field can be assigned to");
        }
        else if (!local && t->kind == AST_IDENT && u->refs[n->a] && SYM(u->refs[n->a])->kind == SYM_CONST &&
                 !(SYM(u->refs[n->a])->flags & SYM_F_MUT))
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "'%.*s' is immutable (declared `%.*s: T`). Declare it type-first, "
                    "`T %.*s`, to assign to it",
                    name->len, TSTART(name), name->len, TSTART(name), name->len, TSTART(name));
        }
        else if (local && local->context)
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "'%.*s' is a context parameter: it cannot be reassigned. To use another one "
                    "from here on, declare `%.*s := ...` in a block, or pass it to the call",
                    name->len, TSTART(name), name->len, TSTART(name));
        }
        else if (local && local->bound)
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "'%.*s' is immutable (bound with `%.*s := ...`). Declare it type-first, "
                    "`T %.*s = ...`, to assign to it",
                    name->len, TSTART(name), name->len, TSTART(name), name->len, TSTART(name));
        }
        else if (local && !local->mut)
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "%s '%.*s' is immutable (declared `%.*s: T`). Declare it type-first, "
                    "`T %.*s`, to assign to it",
                    local->param ? "parameter" : "local", name->len, TSTART(name), name->len,
                    TSTART(name), name->len, TSTART(name));
        }
        else if (field && SYM(field)->kind == SYM_FIELD && !(SYM(field)->flags & SYM_F_MUT))
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "'%.*s' is immutable (declared `%.*s: T`). Declare it type-first, "
                    "`T %.*s`, to assign to it",
                    name->len, TSTART(name), name->len, TSTART(name), name->len, TSTART(name));
        }
        else if (!fits(value, target))
        {
            char a[64];
            char b[64];
            diag_at(u, n->tok, "error", "cannot assign %s to %s", type_str(value, a, sizeof(a)),
                    type_str(target, b, sizeof(b)));
        }
        else
        {
            check_bounds(c, n->b, target);
        }
        type = T_VOID;
        break;
    }
    case AST_DEFINE:
    {
        const Token *name = TOK(u, n->tok);
        c->literal_name = NODE(u, n->a)->kind == AST_FN_LIT ? n->tok : 0;
        int frame = frame_reference(c, n->a);
        c->local_ref = frame == 1;
        NodeId saved_handled = c->handled;
        NodeId saved_guard = c->guard;
        if (n->b && (NODE(u, n->a)->kind == AST_CALL || NODE(u, n->a)->kind == AST_MATCH))
        {
            c->handled = n->a; // `n, e := f()`: f's error is e's (also through a stage on f)
            c->guard = NODE(u, n->a)->kind == AST_CALL ? n->a : 0;
        }
        type = check_expr(c, n->a);
        c->handled = saved_handled;
        c->guard = saved_guard;
        c->local_ref = 0;
        if (n->b) // the value, then the error (guards.md §5)
        {
            const Token *second = TOK(u, n->b);
            SymId callee = NODE(u, n->a)->kind == AST_CALL ? u->refs[n->a]
                           : NODE(u, n->a)->kind == AST_MATCH && (NODE(u, n->a)->aux & MATCH_ERRORS)
                               ? u->refs[NODE(u, n->a)->a]
                               : 0;
            if (!callee || SYM(callee)->kind != SYM_METHOD)
            {
                diag_at(u, n->b, "error", "'%.*s' would be the error of a call: `n, e := f(...)`", second->len,
                        TSTART(second));
            }
            else if (type == T_ERROR && !sema_fn_fails(callee))
            {
                diag_at(u, n->b, "error", "this call gives only an error: bind it alone, `%.*s := ...`",
                        second->len, TSTART(second));
            }
            if (type == T_INT_LIT)
            {
                type = T_INT;
            }
            push_local(c, n->tok, type == T_VOID ? T_NONE : type, 0);
            c->locals[c->nlocals - 1].bound = 1;
            push_local(c, n->b, T_ERROR, 0);
            c->locals[c->nlocals - 1].bound = 1;
            c->locals[c->nlocals - 1].err_of = NODE(u, n->a)->kind == AST_CALL ? callee : 0;
            if (!mentioned_later(u, n->b))
            {
                NODE(u, id)->aux |= DEFINE_UNREAD; // an error that reaches it stops the program (panic.unhandled)
            }
            if (callee && NODE(u, n->a)->kind == AST_CALL)
            {
                arena_decision(c, id, n->a, callee, type);
            }
            int nothing = 0; // what the call can raise, when it is known already
            if (callee && NODE(u, n->a)->kind == AST_CALL && !sema_fn_fails(callee))
            {
                uint8_t pn;
                const uint64_t *ids;
                uint32_t ni;
                int any;
                nothing = sema_error_set(callee, &pn, &ids, &ni, &any) == 1 && !pn && !ni && !any;
            }
            if (nothing)
            {
                int cl;
                const char *cn = symbol_name(callee, &cl);
                diag_at(u, n->b, "warning", "'%.*s' is always nil: '%.*s' can neither fail nor panic", second->len,
                        TSTART(second), cl, cn);
            }
            else if (NODE(u, id)->aux & DEFINE_UNREAD)
            {
                note_panic(c, PANIC_UNHANDLED);
            }
            break;
        }
        NodeId bound_call = 0;
        int bindable = type != T_NONE && NODE(u, n->a)->kind == AST_CALL && called_bare_later(u, n->tok)
                           ? bindable_call(c, n->a)
                           : 0;
        if (bindable == 1)
        {
            bound_call = n->a; // `a := loop(u64, 1)`, then `a()`: the call, its arguments computed now
            type = type_make(T_FUNC, u->refs[n->a]);
        }
        else if (bindable < 0)
        {
            type = T_NONE;
        }
        if (type == T_VOID || type == T_NIL)
        {
            diag_at(u, n->tok, "error", "'%.*s' cannot be bound: the expression has no value",
                    name->len, TSTART(name));
            type = T_NONE;
        }
        else if (type == T_INT_LIT)
        {
            type = T_INT; // an integer literal on its own is an int
        }
        check_bounds(c, n->a, type); // `x := 3000000000` does not fit the int it becomes
        push_local(c, n->tok, type, 0); // immutable, as at module level; visible after its value
        c->locals[c->nlocals - 1].bound = 1;
        c->locals[c->nlocals - 1].frame = (uint8_t)(frame != 0);
        c->locals[c->nlocals - 1].call = bound_call;
        break;
    }
    case AST_FIELD_DECL: // a local with its type: `name: T = v`, `T name [= v]` (parse_local)
    {
        TypeId declared = resolve_type(u, n->a, c->owner, 1);
        if (declared == T_VOID || declared == T_NIL)
        {
            const Token *name = TOK(u, n->tok);
            diag_at(u, n->tok, "error", "'%.*s' cannot be declared with a type that has no value",
                    name->len, TSTART(name));
            declared = T_NONE;
        }
        int frame = n->b ? frame_reference(c, n->b) : 0;
        if (n->b)
        {
            c->local_ref = frame == 1;
            TypeId init = check_expr(c, n->b);
            c->local_ref = 0;
            if (!fits(init, declared))
            {
                char a[64];
                char b[64];
                char text[128], hint[192];
                int named = value_text(u, n->b, text, sizeof(text));
                TypeId wanted = sema_is_any_ref(declared) && type_kind(init) == T_STRUCT
                                    ? type_make(T_PTR, init) // `c: *any = p`: the fix is &p as for *T
                                    : declared;
                reference_hint(init, wanted, named ? text : "", hint, sizeof(hint));
                diag_at(u, NODE(u, n->b)->tok, "error", "cannot initialize %s with %s%s",
                        type_str(declared, b, sizeof(b)), type_str(init, a, sizeof(a)), hint);
            }
            else
            {
                check_bounds(c, n->b, declared);
            }
        }
        push_local(c, n->tok, declared, (n->flags & AST_F_MUT) != 0); // after its own value
        // an *any made here (`a: *any = &p`) is a pair in this frame, as a literal's reference is
        int made_any = n->b && sema_is_any_ref(declared) && !sema_is_any_ref(u->types[n->b]);
        c->locals[c->nlocals - 1].frame = (uint8_t)(frame != 0 || made_any);
        type = declared;
        break;
    }
    case AST_RETURN:
        check_return(c, id, n->a);
        break;
    case AST_BLOCK:
    {
        int saved_count = c->nlocals, saved_base = c->block_base;
        c->block_base = c->nlocals;
        for (NodeId s = n->a; s; s = NEXT(u, s))
        {
            TypeId st = check_expr(c, s);
            if (st == T_ERROR && NODE(u, s)->kind == AST_CALL && u->refs[s] && !sema_fn_fails(u->refs[s]))
            {
                error_goes_up(c, NODE(u, s)->tok, u->refs[s]); // `run()` alone: its error goes up
                note_edge(c, u->refs[s], EE_ERRORS);
            }
        }
        c->nlocals = saved_count; // a block's locals end with the block
        c->block_base = saved_base;
        break;
    }
    case AST_FN_LIT:
    {
        // A literal is a function of its own: it gets a symbol, so calls to it are checked like
        // any other, and it is generated now, while the body around it is in memory.
        uint32_t name_tok = c->literal_name ? c->literal_name : n->tok; // named by its binding
        c->literal_name = 0;
        if (u->refs[id] && SYM(u->refs[id])->kind == SYM_METHOD && (SYM(u->refs[id])->flags & SYM_F_LITERAL))
        {
            type = type_make(T_FUNC, u->refs[id]); // checked already (an argument checked again)
            break;
        }
        SymId lit = symbol_new(u, SYM_METHOD, SYM_F_LITERAL, name_tok, 0, id);
        u->refs[id] = lit;
        type = type_make(T_FUNC, lit);
        int bare = 0; // a pattern alone as a parameter: typed by the first call, checked then
        for (NodeId p = n->a; p && !bare; p = NEXT(u, p))
        {
            bare = !NODE(u, p)->a && (NODE(u, p)->flags & AST_F_PATTERN);
            if (!NODE(u, p)->a && !(NODE(u, p)->flags & AST_F_PATTERN) && !u->types[p])
            {
                diag_at(u, NODE(u, p)->tok, "error", "'%.*s' needs a type: `(%.*s: u64): ...`, or give the literal to "
                        "a parameter whose `function(...)` types it", TOK(u, NODE(u, p)->tok)->len,
                        TSTART(TOK(u, NODE(u, p)->tok)), TOK(u, NODE(u, p)->tok)->len, TSTART(TOK(u, NODE(u, p)->tok)));
            }
        }
        if (bare && npending_lits < 64)
        {
            pending_lits[npending_lits++] = (PendingLit){lit, id, c->nlocals};
            break;
        }
        check_callable(c, id, c->owner, lit);
        break;
    }
    default: // import chains: checked by the module linker
        type = T_EXTERN;
        break;
    }

    return set_type(c, id, type);
}

// ---- declarations ----------------------------------------------------------------------------

// The struct a top-level callable attaches to through `self: T`, or 0 for the module.
static SymId attached_owner(const Unit *u, NodeId params)
{
    if (!params || !NODE(u, params)->a)
    {
        return 0;
    }
    const Token *name = TOK(u, NODE(u, params)->tok);
    const AstNode *type = NODE(u, NODE(u, params)->a);
    if (!is_self_token(TSTART(name), (int)name->len) || type->kind != AST_TYPE_NAME || type->aux)
    {
        return 0;
    }
    const Token *t = TOK(u, type->tok);
    SymId s = scope_find_token(u->scope, t);
    return (s && SYM(s)->kind == SYM_STRUCT) ? s : 0;
}

// ---- placement: register blocks and devices ------------------------------------------------------
//
// `u32 dr @ 0x00` gives a field its offset; a struct whose fields all have one is a register
// block: its layout is exactly what it says, whatever the ABI, and every access to one of its
// fields is a real load or store of that field's width (volatile). `uart0: pl011 @ 0x0900_0000`
// places a register block at an address: a device. Mutability is the usual rule, so a read-only
// register is a type-last field (`fr: u32 @ 0x18`) and cannot be assigned.

static int place_value(const Unit *u, NodeId n, uint64_t *v)
{
    const Token *t = TOK(u, NODE(u, n)->tok);
    int overflow = 0;
    return number_value(TSTART(t), t->len, v, &overflow) && !overflow;
}

static void check_register(Check *c, NodeId decl, SymId owner, TypeId declared)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, decl);
    char buf[64];
    // all or none: the fields of a register block each say where they are
    NodeId body = NODE(&cx.units[SYM(owner)->unit], SYM(owner)->decl)->a;
    int placed = 0, plain = 0;
    for (NodeId m = body; m; m = NEXT(u, m))
    {
        if (NODE(u, m)->kind == AST_FIELD_DECL)
        {
            placed += (NODE(u, m)->flags & AST_F_PLACED) != 0;
            plain += !(NODE(u, m)->flags & AST_F_PLACED);
        }
    }
    if (!(n->flags & AST_F_PLACED))
    {
        if (placed)
        {
            const Token *fname = TOK(u, n->tok);
            diag_at(u, n->tok, "error",
                    "the other fields of this struct have offsets: give '%.*s' one too "
                    "(`%.*s: T @ offset`)",
                    fname->len, TSTART(fname), fname->len, TSTART(fname));
        }
        return;
    }
    if (!(type_is_int(declared) && declared != T_INT_LIT && declared != T_INT) && declared != T_BOOL)
    {
        diag_at(u, n->tok, "error", "a field with an offset is a register: a sized number or bool, not %s",
                type_str(declared, buf, sizeof(buf)));
        return;
    }
    (void)plain;
    uint64_t off;
    if (!place_value(u, n->b, &off) || off > 0xffffffffu)
    {
        diag_at(u, NODE(u, n->b)->tok, "error", "an offset is a whole number below 4 GB");
        return;
    }
    Layout l = type_layout(declared);
    if (off % l.align)
    {
        diag_at(u, NODE(u, n->b)->tok, "error", "offset %llu is not a multiple of %u, the alignment of %s",
                (unsigned long long)off, l.align, type_str(declared, buf, sizeof(buf)));
        return;
    }
    // in order, without overlap: where the field before this one ends
    uint64_t end = 0;
    for (NodeId m = body; m && m != decl; m = NEXT(u, m))
    {
        const AstNode *f = NODE(u, m);
        uint64_t at;
        if (f->kind == AST_FIELD_DECL && (f->flags & AST_F_PLACED) && place_value(u, f->b, &at))
        {
            Layout fl = type_layout(resolve_type(u, f->a, owner, 0));
            end = at + fl.size;
        }
    }
    if (off < end)
    {
        diag_at(u, NODE(u, n->b)->tok, "error",
                "offset %llu overlaps the register before it, which ends at %llu (registers are "
                "listed in the order of their offsets)",
                (unsigned long long)off, (unsigned long long)end);
    }
}

static void check_device(Check *c, NodeId decl, TypeId declared)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, decl);
    const Token *name = TOK(u, n->tok);
    char buf[64];
    if (n->flags & AST_F_MUT)
    {
        diag_at(u, n->tok, "error",
                "a device stays at its address: declare it type-last, `%.*s: T @ address`",
                name->len, TSTART(name));
        return;
    }
    if (type_kind(declared) != T_STRUCT)
    {
        diag_at(u, n->tok, "error",
                "only a register block can be placed at an address: a struct whose fields have "
                "offsets, not %s",
                type_str(declared, buf, sizeof(buf)));
        return;
    }
    if (!sema_is_register_block(type_ref(declared)))
    {
        diag_at(u, n->tok, "error",
                "'%s' is placed at an address, so it is a device: give each field of %s its "
                "offset (`u32 dr @ 0x00`)",
                type_str(declared, buf, sizeof(buf)), type_str(declared, buf, sizeof(buf)));
        return;
    }
    uint64_t at;
    Layout l = type_layout(declared);
    if (!place_value(u, n->b, &at) || at == 0)
    {
        diag_at(u, NODE(u, n->b)->tok, "error", "a device's address is a whole number other than 0");
    }
    else if (at % l.align)
    {
        diag_at(u, NODE(u, n->b)->tok, "error", "address 0x%llx is not a multiple of %u, the alignment of %s",
                (unsigned long long)at, l.align, type_str(declared, buf, sizeof(buf)));
    }
    else if (target_get()->ptr_size < 8 && at >> (8 * target_get()->ptr_size))
    {
        diag_at(u, NODE(u, n->b)->tok, "error", "address 0x%llx is beyond the %u-bit address space of %s",
                (unsigned long long)at, 8 * target_get()->ptr_size, target_get()->name);
    }
}

int sema_is_register_block(SymId s)
{
    const Unit *home = &cx.units[SYM(s)->unit];
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind == AST_FIELD_DECL && (NODE(home, m)->flags & AST_F_PLACED))
        {
            return 1;
        }
    }
    return 0;
}

uint64_t sema_device_address(SymId s)
{
    const Unit *home = &cx.units[SYM(s)->unit];
    const AstNode *decl = NODE(home, SYM(s)->decl);
    uint64_t at = 0;
    if (decl->kind == AST_FIELD_DECL && (decl->flags & AST_F_PLACED) && !SYM(s)->owner)
    {
        place_value(home, decl->b, &at);
    }
    return at;
}

static void check_field(Check *c, NodeId decl, SymId owner)
{
    const AstNode *n = NODE(c->u, decl);
    TypeId declared = resolve_type(c->u, n->a, owner, 1);
    if (owner)
    {
        check_register(c, decl, owner, declared);
    }
    if (n->flags & AST_F_PLACED)
    {
        if (!owner)
        {
            check_device(c, decl, declared);
        }
        return;
    }
    if (n->b)
    {
        TypeId init = check_expr(c, n->b);
        if (!fits(init, declared))
        {
            char a[64];
            char b[64];
            diag_at(c->u, NODE(c->u, n->b)->tok, "error", "cannot initialize %s with %s",
                    type_str(declared, b, sizeof(b)), type_str(init, a, sizeof(a)));
        }
        else
        {
            check_bounds(c, n->b, declared);
        }
    }
}

// The symbol a callable declaration created: looked up by name in the scope that owns it,
// then matched by declaration among the overloads and any field sharing the name.
static SymId decl_symbol(const Unit *u, SymId owner, NodeId decl, uint32_t name_tok)
{
    const Token *name = TOK(u, name_tok);
    const Scope *scope = owner ? struct_scope(owner) : u->scope;
    SymId first = scope_find_token(scope, name);

    while (first)
    {
        for (SymId o = first; o; o = SYM(o)->kind == SYM_METHOD ? SYM(o)->next_overload : 0)
        {
            if (SYM(o)->decl == decl)
            {
                return o;
            }
        }
        // the same name further down the bucket: an accessor sharing its field's name
        SymId s = SYM(first)->next;
        for (; s; s = SYM(s)->next)
        {
            int len;
            const char *other = symbol_name(s, &len);
            if ((uint32_t)len == name->len && memcmp(other, TSTART(name), name->len) == 0)
            {
                break;
            }
        }
        first = s;
    }
    return 0;
}

// Declarations are checked in two sweeps. First everything that is not a function (fields,
// constants, module-level statements), so the backend can emit types, globals and
// prototypes; then the functions, one at a time.
static void check_decl(Check *c, NodeId decl, Scope *scope, SymId enclosing, int functions)
{
    Unit *u = c->u;
    uint8_t kind = NODE(u, decl)->kind;
    uint32_t tok = NODE(u, decl)->tok;
    NodeId a = NODE(u, decl)->a;

    switch (kind)
    {
    case AST_METHOD_DECL:
        if (functions)
        {
            SymId owner = enclosing ? enclosing : attached_owner(u, a);
            check_callable(c, decl, owner, decl_symbol(u, owner, decl, tok));
        }
        break;
    case AST_DEFINE:
        if (NODE(u, a)->kind == AST_FN_LIT)
        {
            if (functions)
            {
                SymId owner = enclosing ? enclosing : attached_owner(u, NODE(u, a)->a);
                check_callable(c, a, owner, decl_symbol(u, owner, decl, tok));
            }
        }
        else if (!functions)
        {
            check_expr(c, a);
            Exact v;
            if (!enclosing && u->types[a] == T_INT_LIT && !u->is_manifest &&
                (!exact_value(u, a, &v) || v < -(Exact)0x8000000000000000ull || v > (Exact)UINT64_MAX))
            {
                const Token *name = TOK(u, tok);
                diag_at(u, tok, "error",
                        "'%.*s' is too large for any integer type (from -2^63 to 2^64 - 1)", name->len,
                        TSTART(name));
            }
        }
        break;
    case AST_FIELD_DECL:
        if (!functions)
        {
            check_field(c, decl, enclosing);
        }
        break;
    case AST_STRUCT_DECL:
    {
        const Token *name = TOK(u, tok);
        SymId self = scope_find_token(scope, name);
        if (self && SYM(self)->kind == SYM_STRUCT)
        {
            for (NodeId m = a; m; m = NEXT(u, m))
            {
                check_decl(c, m, struct_scope(self), self, functions);
            }
        }
        break;
    }
    case AST_VAL_DECL:
    case AST_STRUCT_LIT: // `point{ x, y }` alone: the struct's face (sema_declare took it)
        break;
    default:
        if (!functions)
        {
            check_expr(c, decl); // a top-level statement such as write("...")
        }
        break;
    }
}

// ---- clauses (literal patterns) ----------------------------------------------------------------

// The value a pattern parameter matches, as its type holds it. `loud`: report a literal that
// does not fit the type. Returns 0 if `param` is not a pattern (or does not fit).
// One number of a pattern, at token `tok` (negative: a `-` before it), checked against the
// parameter's type. 0 after reporting (loud) when it does not fit.
static int pattern_number(Unit *u, uint32_t tok, int negative, TypeId type, uint64_t *value, int loud)
{
    const Token *t = TOK(u, tok);
    uint64_t v = 0;
    int overflow = 0;
    if (t->kind != TOK_NUMBER || !number_value(TSTART(t), t->len, &v, &overflow))
    {
        overflow = 1;
    }
    static const struct
    {
        TypeId type;
        uint64_t max; // the largest magnitude a positive value may have
        uint64_t min; // the largest magnitude a negative value may have (0: unsigned)
    } ranges[] = {
        {T_U8, 0xff, 0},        {T_U16, 0xffff, 0},           {T_U32, 0xffffffffu, 0},
        {T_U64, UINT64_MAX, 0}, {T_I8, 0x7f, 0x80},           {T_I16, 0x7fff, 0x8000},
        {T_I32, 0x7fffffff, 0x80000000u}, {T_INT, 0x7fffffff, 0x80000000u},
        {T_I64, 0x7fffffffffffffffull, 0x8000000000000000ull},
    };
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
    {
        if (ranges[i].type != type)
        {
            continue;
        }
        if (overflow || (negative ? v > ranges[i].min : v > ranges[i].max))
        {
            if (loud)
            {
                char name[32];
                diag_at(u, tok, "error", "the pattern %s%.*s does not fit %s", negative ? "-" : "",
                        t->len, TSTART(t), type_str(type, name, sizeof(name)));
            }
            return 0;
        }
        *value = negative ? 0 - v : v;
        return 1;
    }
    if (loud && type != T_NONE)
    {
        char name[32];
        diag_at(u, tok, "error", "a number pattern needs an integer value to match, not %s",
                type_str(type, name, sizeof(name)));
    }
    return 0;
}

static int signed_type(TypeId t)
{
    return t == T_I8 || t == T_I16 || t == T_I32 || t == T_INT || t == T_I64;
}

static uint64_t type_max(TypeId t)
{
    return t == T_U8 ? 0xff : t == T_U16 ? 0xffff : t == T_U32 ? 0xffffffffu : t == T_U64 ? UINT64_MAX
         : t == T_I8 ? 0x7f : t == T_I16 ? 0x7fff : t == T_I32 || t == T_INT ? 0x7fffffff
                                                   : 0x7fffffffffffffffull;
}

static int pattern_from(Unit *u, const AstNode *n, TypeId type, Pattern *out, int loud);

int sema_pattern_of(Unit *u, NodeId param, SymId owner, Pattern *out, int loud)
{
    const AstNode *n = NODE(u, param);
    if (!(n->flags & AST_F_PATTERN))
    {
        return 0;
    }
    TypeId t = n->a ? resolve_type(u, n->a, owner, 0) : u->types && param < u->side_cap ? u->types[param] : T_NONE;
    return t == T_NONE ? 0 : pattern_from(u, n, t, out, loud);
}

static int const_value(const Unit *u, NodeId e, TypeId type, uint64_t *v);
static int ends_pattern(Unit *u, const AstNode *pn, TypeId type, Pattern *out, int loud);

// A range with a name at an end: both ends must be known now and fit the type, low <= high.
static int ends_pattern(Unit *u, const AstNode *pn, TypeId type, Pattern *out, int loud)
{
    *out = (Pattern){.kind = PATTERN_RANGE, .tok = pn->tok};
    char y[32];
    if (!type_is_int(type) || type == T_INT_LIT)
    {
        if (loud)
        {
            diag_at(u, pn->tok, "error", "a range pattern needs an integer value to match, not %s",
                    type_str(type, y, sizeof(y)));
        }
        return 0;
    }
    NodeId ends[2] = {pn->a, pn->b};
    for (int e = 0; e < 2; e++)
    {
        uint64_t v;
        if (!ends[e])
        {
            out->hi = cover_max(type) < 0 ? 0 : (uint64_t)cover_max(type); // open: the type's last value
            continue;
        }
        if (!const_value(u, ends[e], type, &v))
        {
            if (loud)
            {
                const Token *t = TOK(u, NODE(u, ends[e])->tok);
                diag_at(u, NODE(u, ends[e])->tok, "error", "'%.*s' is not known while compiling: a range's ends are "
                        "literals or constants", t->len, TSTART(t));
            }
            return 0;
        }
        Exact x = signed_type(type) ? (Exact)(int64_t)v : (Exact)v; // (a negative end on an unsigned value: too large)
        if (x < cover_min(type) || x > cover_max(type))
        {
            if (loud)
            {
                diag_at(u, NODE(u, ends[e])->tok, "error", "this end of the range does not fit %s",
                        type_str(type, y, sizeof(y)));
            }
            return 0;
        }
        if (e == 0)
        {
            out->lo = (uint64_t)x;
        }
        else
        {
            out->hi = (uint64_t)x;
        }
    }
    int empty = signed_type(type) ? (int64_t)out->hi < (int64_t)out->lo : out->hi < out->lo;
    if (empty)
    {
        if (loud)
        {
            diag_at(u, pn->tok, "error", "this range takes no value: its high end is below its low end");
        }
        return 0;
    }
    return 1;
}

// A name pattern's value when the compiler knows it (a constant, sized or not).
static int name_pattern_value(const Unit *u, const AstNode *pn, TypeId type, uint64_t *v)
{
    return const_value(u, pn->a, type, v);
}

// The value of a pattern's expression the compiler knows: a literal, a negative one, or a
// constant's name.
static int const_value(const Unit *u, NodeId e, TypeId type, uint64_t *v)
{
    int negative;
    Exact x;
    if (type == T_STR)
    {
        return 0;
    }
    if (NODE(u, e)->kind == AST_UNARY_OP && NODE(u, NODE(u, e)->a)->kind == AST_IDENT) // `-limit`
    {
        if (!const_value(u, NODE(u, e)->a, type, v))
        {
            return 0;
        }
        *v = 0 - *v;
        return 1;
    }
    if (NODE(u, e)->kind == AST_SELECTOR && u->refs && u->refs[e] && SYM(u->refs[e])->kind == SYM_CONST)
    {
        return sema_comptime_value(u->refs[e], v); // `panic.division`: a module's constant
    }
    if (NODE(u, e)->kind != AST_IDENT)
    {
        if (!exact_value(u, e, &x))
        {
            return 0;
        }
        *v = (uint64_t)x;
        return 1;
    }
    if (sema_unsized_value(u, e, v, &negative))
    {
        return 1;
    }
    if (type == T_ERROR && text_is(TOK(u, NODE(u, e)->tok), "nil") && !(u->refs && u->refs[e]))
    {
        *v = 0; // no error
        return 1;
    }
    SymId s = u->refs ? u->refs[e] : 0;
    if (!s || SYM(s)->owner || (NODE(&cx.units[SYM(s)->unit], SYM(s)->decl)->flags & AST_F_MUT) ||
        SYM(s)->kind == SYM_METHOD)
    {
        return 0; // a local, a parameter, a field, a mutable module variable: known when it runs
    }
    if (SYM(s)->kind == SYM_CONST) // `limit: u8 = 50`: its value, exactly
    {
        const Unit *h = &cx.units[SYM(s)->unit];
        const AstNode *d = NODE(h, SYM(s)->decl);
        NodeId init = d->kind == AST_DEFINE ? d->a : d->kind == AST_FIELD_DECL ? d->b : 0;
        Exact e;
        if (init && exact_value(h, init, &e))
        {
            *v = (uint64_t)e;
            return 1;
        }
    }
    return s && sema_comptime_value(s, v); // one the compiler computed (`fact(5)`)
}

int sema_params_overlap(Unit *u, NodeId a, NodeId b, SymId owner)
{
    Pattern pa, pb;
    if (!sema_pattern_of(u, a, owner, &pa, 0) || !sema_pattern_of(u, b, owner, &pb, 0))
    {
        return 1; // a parameter without a pattern takes every value
    }
    if (pa.kind == PATTERN_TEXT || pb.kind == PATTERN_TEXT)
    {
        if (pa.kind != pb.kind)
        {
            return 0;
        }
        static uint8_t x[4096], y[4096];
        size_t nx = sema_pattern_text(u, pa.tok, x, sizeof(x)), ny = sema_pattern_text(u, pb.tok, y, sizeof(y));
        return nx == ny && memcmp(x, y, nx < sizeof(x) ? nx : sizeof(x)) == 0;
    }
    TypeId t = resolve_type(u, NODE(u, a)->a, owner, 0);
    Exact alo = cover_value(t, pa.lo), ahi = pa.kind == PATTERN_RANGE ? cover_value(t, pa.hi) : alo;
    Exact blo = cover_value(t, pb.lo), bhi = pb.kind == PATTERN_RANGE ? cover_value(t, pb.hi) : blo;
    return alo <= bhi && blo <= ahi;
}

// An arm's pattern known while compiling (a literal, or a name of a constant), for a subject of
// `type`. 0 for `_`, a name known only when the program runs, or an error.
int sema_arm_pattern(Unit *u, NodeId pat, TypeId type, Pattern *out)
{
    const AstNode *pn = NODE(u, pat);
    if (pn->kind != AST_PATTERN || (pn->aux & PAT_ANY))
    {
        return 0;
    }
    if (pn->aux & PAT_NAME)
    {
        *out = (Pattern){.kind = PATTERN_VALUE, .tok = pn->tok};
        return name_pattern_value(u, pn, type, &out->lo);
    }
    if (pn->aux & PAT_ENDS)
    {
        return ends_pattern(u, pn, type, out, 0);
    }
    return pattern_from(u, pn, type, out, 0);
}

NodeId sema_arm_name(Unit *u, NodeId pat, TypeId type)
{
    const AstNode *pn = NODE(u, pat);
    uint64_t v;
    return pn->kind == AST_PATTERN && (pn->aux & PAT_NAME) && !name_pattern_value(u, pn, type, &v) ? pn->a : 0;
}

// What a literal pattern takes (a parameter's or an arm's: the literal token and PAT_* in aux).
static int pattern_from(Unit *u, const AstNode *n, TypeId type, Pattern *out, int loud)
{
    const Token *t = TOK(u, n->tok);
    *out = (Pattern){.kind = PATTERN_VALUE, .tok = n->tok};
    if (n->aux & PAT_TEXT)
    {
        if (type != T_STR)
        {
            if (loud)
            {
                diag_at(u, n->tok, "error", "a string pattern needs a []u8 value to match");
            }
            return 0;
        }
        out->kind = PATTERN_TEXT;
        return 1;
    }
    if (t->kind == TOK_IDENTIFIER) // true / false
    {
        if (type != T_BOOL)
        {
            if (loud)
            {
                diag_at(u, n->tok, "error", "the pattern '%.*s' is a bool, but the value matched is not",
                        t->len, TSTART(t));
            }
            return 0;
        }
        out->lo = t->len == 4;
        return 1;
    }
    if (!pattern_number(u, n->tok, (n->aux & PAT_NEG) != 0, type, &out->lo, loud))
    {
        return 0;
    }
    if (!(n->aux & PAT_RANGE))
    {
        return 1;
    }
    out->kind = PATTERN_RANGE;
    out->hi = signed_type(type) ? type_max(type) : type_max(type);
    if (!(n->aux & PAT_OPEN))
    {
        uint32_t hi_tok = n->tok + 2 + ((n->aux & PAT_NEG_HI) ? 1 : 0);
        if (!pattern_number(u, hi_tok, (n->aux & PAT_NEG_HI) != 0, type, &out->hi, loud))
        {
            return 0;
        }
    }
    int empty = signed_type(type) ? (int64_t)out->hi < (int64_t)out->lo : out->hi < out->lo;
    if (empty)
    {
        if (loud)
        {
            diag_at(u, n->tok, "error", "this range takes no value: its high end is below its low end");
        }
        return 0;
    }
    return 1;
}

// The value of a single-value pattern (a range: its low end; a string: 0).
int sema_pattern(Unit *u, NodeId param, SymId owner, uint64_t *value, int loud)
{
    Pattern pat;
    if (!sema_pattern_of(u, param, owner, &pat, loud))
    {
        return 0;
    }
    *value = pat.lo;
    return 1;
}

size_t sema_pattern_text(const Unit *u, uint32_t tok, uint8_t *out, size_t cap)
{
    const Token *t = TOK(u, tok);
    size_t n = 0;
    for (uint32_t i = 1; i + 1 < t->len; i++, n++)
    {
        char ch = TSTART(t)[i];
        if (ch == '\\' && i + 2 < t->len)
        {
            char e = TSTART(t)[++i];
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == '0' ? '\0' : e;
        }
        if (n < cap)
        {
            out[n] = (uint8_t)ch;
        }
    }
    return n;
}

static int clause_arity(const Unit *u, SymId fn)
{
    (void)u;
    return arity_of(fn);
}

// One clause group: as many parameters, of the same types (patterns aside).
static int same_group(SymId a, SymId b)
{
    const Unit *ua = &cx.units[SYM(a)->unit], *ub = &cx.units[SYM(b)->unit];
    NodeId pa = fn_params(ua, a), pb = fn_params(ub, b);
    for (; pa && pb; pa = NEXT(ua, pa), pb = NEXT(ub, pb))
    {
        if (param_type((Unit *)ua, pa, SYM(a)->owner, 0) != param_type((Unit *)ub, pb, SYM(b)->owner, 0))
        {
            return 0;
        }
    }
    return pa == pb;
}

// The clause tried after `fn` when its patterns do not match: the next definition with as many
// parameters. 0 if `fn` has no patterns (it takes every value).
SymId sema_next_clause(Unit *u, SymId fn)
{
    int patterns = 0;
    for (NodeId p = fn_params(u, fn); p; p = NEXT(u, p))
    {
        patterns |= (NODE(u, p)->flags & AST_F_PATTERN) != 0;
    }
    if (!patterns)
    {
        return 0;
    }
    for (SymId s = SYM(fn)->next_overload; s; s = SYM(s)->next_overload)
    {
        if (SYM(s)->kind == SYM_METHOD && same_group(fn, s))
        {
            return s;
        }
    }
    return 0;
}

// Its place among the clauses with as many parameters: 0 for the first, which calls go to.
int sema_clause_index(Unit *u, SymId fn)
{
    SymId first = scope_find_token(SYM(fn)->owner ? struct_scope(SYM(fn)->owner) : u->scope,
                                   TOK(u, SYM(fn)->tok));
    int n = clause_arity(u, fn), index = 0;
    for (SymId s = first; s && s != fn; s = SYM(s)->next_overload)
    {
        index += SYM(s)->kind == SYM_METHOD && clause_arity(u, s) == n;
    }
    return index;
}

TypeId sema_fn_ret(Unit *u, SymId fn)
{
    return fn_ret_type(u, fn);
}

TypeId sema_param_type(Unit *u, SymId fn, NodeId param)
{
    u = &cx.units[SYM(fn)->unit];
    const Spec *old = bind_for(fn);
    TypeId t = param_type(u, param, SYM(fn)->owner, 0);
    binding = old;
    return t;
}

TypeId sema_type_of(Unit *u, NodeId type_node, SymId owner)
{
    return resolve_type(u, type_node, owner, 0);
}

TypeId sema_value_type(Unit *u, SymId field_or_const)
{
    return value_type(u, field_or_const);
}

NodeId sema_fn_params(const Unit *u, SymId fn)
{
    return fn_params(u, fn);
}

// A file whose declarations did not parse is not checked, but the syntax errors inside its
// function bodies are still worth reporting in the same run.
void sema_syntax_only(Unit *u)
{
    size_t decls = u->nodes.size, toks = u->tokens.size;
    for (NodeId id = 1; id < decls; id++)
    {
        // (a function's signature node carries AST_F_LAZY too; its body is reached from the function)
        int kind = NODE(u, id)->kind;
        if ((NODE(u, id)->flags & AST_F_LAZY) && (kind == AST_METHOD_DECL || kind == AST_FN_LIT))
        {
            parse_function_body(u, fn_body(u, id));
            u->nodes.size = decls;
            u->tokens.size = toks;
        }
    }
}

// ---- the order bodies are checked in (context.md §58) -------------------------------------------
//
// Callees first: a function's body is checked, and generated, after the bodies of the functions of
// its file that it calls. When its turn comes, what they can raise is known (a boundary on a call
// that can raise nothing is no boundary) and they are lowered (a small one is inlined), whatever
// order they are written in. A cycle (recursion) keeps the order it is written in. The calls are
// read from the bodies' text before they are parsed: a name followed by `(`.
typedef struct
{
    uint32_t hash; // of the declaration's name, as text
    uint32_t idx;  // its index + 1 (0: empty)
} OrderSlot;
typedef struct
{
    Unit *u;
    NodeId *decls;    // the file's declarations, as written
    uint32_t n;
    uint8_t *state;   // 0 not visited, 1 in progress, 2 placed
    NodeId *out;
    uint32_t nout;
    OrderSlot *slots; // the functions by the hash of their names' text (one probe per call)
    uint32_t mask;
} Order;

static int is_function_decl(const Unit *u, NodeId d)
{
    const AstNode *n = NODE(u, d);
    return n->kind == AST_METHOD_DECL || (n->kind == AST_DEFINE && NODE(u, n->a)->kind == AST_FN_LIT);
}

static uint32_t text_hash(const char *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
    {
        h = (h ^ (uint8_t)p[i]) * 16777619u;
    }
    return h | 1u; // never 0
}

static void order_visit(Order *o, uint32_t i);

// The functions named by the text w[0..n) (overloads and clauses share a name): each is visited.
static void order_calls_text(Order *o, const char *w, size_t n)
{
    uint32_t h = text_hash(w, n);
    for (uint32_t k = h & o->mask;; k = (k + 1) & o->mask)
    {
        const OrderSlot *sl = &o->slots[k];
        if (!sl->idx)
        {
            return;
        }
        if (sl->hash != h || o->state[sl->idx - 1])
        {
            continue;
        }
        const Token *t = TOK(o->u, NODE(o->u, o->decls[sl->idx - 1])->tok);
        if (t->len == n && memcmp(TSTART(t), w, n) == 0)
        {
            order_visit(o, sl->idx - 1);
        }
    }
}

static int ident_byte(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

// The names a body not tokenized yet calls: the identifier before each `(`. Each `(` is found
// with memchr (most of a body is not calls); one in a string or a comment only adds an edge, which
// moves a function earlier, never changes what it means.
static void order_scan_text(Order *o, const char *p, size_t len)
{
    const char *start = p, *end = p + len;
    while (p < end && (p = memchr(p, '(', (size_t)(end - p))) != NULL)
    {
        const char *w = p;
        while (w > start && (w[-1] == ' ' || w[-1] == '\t'))
        {
            w--;
        }
        const char *e = w;
        while (w > start && ident_byte(w[-1]))
        {
            w--;
        }
        if (e > w && !(*w >= '0' && *w <= '9'))
        {
            order_calls_text(o, w, (size_t)(e - w));
        }
        p++;
    }
}

static void order_visit(Order *o, uint32_t i)
{
    Unit *u = o->u;
    o->state[i] = 1;
    NodeId d = o->decls[i];
    if (is_function_decl(u, d))
    {
        // its tokens: up to the next declaration's first (a body not parsed yet is one token)
        uint32_t from = NODE(u, d)->tok, to = i + 1 < o->n ? NODE(u, o->decls[i + 1])->tok : (uint32_t)u->tokens.size;
        for (uint32_t t = from + 1; t < to && t < u->tokens.size; t++)
        {
            const Token *k = TOK(u, t);
            if (k->kind == TOK_EOF)
            {
                break;
            }
            if (k->kind == TOK_BODY)
            {
                order_scan_text(o, TSTART(k), k->len);
            }
            else if (k->kind == TOK_IDENTIFIER && t + 1 < to && TOK(u, t + 1)->kind == TOK_LPAREN)
            {
                order_calls_text(o, TSTART(k), k->len);
            }
        }
    }
    o->state[i] = 2;
    o->out[o->nout++] = d;
}

// The file's declarations in checking order (out holds n): callees first.
static uint32_t check_order(Unit *u, NodeId **out)
{
    Order o = {u, NULL, 0, NULL, NULL, 0, NULL, 0};
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        o.n++;
    }
    uint32_t cap = 16;
    while (cap < o.n * 2 + 2)
    {
        cap *= 2;
    }
    o.mask = cap - 1;
    o.decls = (NodeId *)malloc((o.n + 1) * sizeof(NodeId));
    o.out = (NodeId *)malloc((o.n + 1) * sizeof(NodeId));
    o.state = (uint8_t *)calloc(o.n + 1, 1);
    o.slots = (OrderSlot *)calloc(cap, sizeof(OrderSlot));
    if (!o.decls || !o.out || !o.state || !o.slots)
    {
        fprintf(stderr, "fatal: out of memory\n");
        exit(2);
    }
    uint32_t i = 0;
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d), i++)
    {
        o.decls[i] = d;
        if (is_function_decl(u, d))
        {
            const Token *t = TOK(u, NODE(u, d)->tok);
            uint32_t h = text_hash(TSTART(t), t->len), k = h & o.mask;
            while (o.slots[k].idx)
            {
                k = (k + 1) & o.mask;
            }
            o.slots[k] = (OrderSlot){h, i + 1};
        }
    }
    static int off = -1;
    if (off < 0)
    {
        const char *env = getenv("MODAL_ORDER"); // 0: as written (for measuring)
        off = env && env[0] == '0';
    }
    for (i = 0; i < o.n; i++)
    {
        if (off)
        {
            o.out[o.nout++] = o.decls[i];
        }
        else if (o.state[i] == 0)
        {
            order_visit(&o, i);
        }
    }
    free(o.decls);
    free(o.state);
    free(o.slots);
    *out = o.out;
    return o.nout;
}

void sema_check(Unit *u)
{
    // One type and one resolved declaration per node (8 bytes), which is all a backend
    // reads. They cover the declarations now and grow to fit the largest body later.
    side_tables(u, 0);

    // The checker state is a few KB of locals: it lives on the C stack.
    Check c;
    memset(&c, 0, offsetof(Check, locals)); // the locals array needs no clearing
    c.u = u;
    c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);

    for (int functions = 0; functions < 2; functions++)
    {
        if (functions)
        {
            if (!u->errors)
            {
                comptime_unit(u); // its declarations are checked: the compiler computes what it must
            }
            codegen_begin(u); // everything a signature can mention is checked by now
        }
        if (!functions)
        {
            for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
            {
                c.nlocals = 0;
                c.block_base = 0;
                check_decl(&c, d, u->scope, 0, functions);
            }
            continue;
        }
        NodeId *order;
        uint32_t n = check_order(u, &order); // the bodies: callees first
        for (uint32_t i = 0; i < n; i++)
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, order[i], u->scope, 0, functions);
            sema_drain_specs(); // the specializations its body made, now that it is done
        }
        free(order);
    }
}

// The compiler is about to run fn: its body is checked again, without reports (it is checked
// and reported in its own turn), and lowered for the interpreter instead of the backend.
int sema_lower_for_comptime(SymId fn)
{
    Unit *u = &cx.units[SYM(fn)->unit];
    if (SYM(fn)->kind != SYM_METHOD || !SYM(fn)->decl)
    {
        return 0;
    }
    NodeId decl = SYM(fn)->decl;
    NodeId node = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
    Check c;
    memset(&c, 0, offsetof(Check, locals));
    c.u = u;
    c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);
    int saved = capture_mir, errors = cx.quiet_errors;
    uint32_t names = cx.names_resolved, calls = cx.calls_resolved;
    cx.quiet++;
    if (!u->side_cap || !u->types)
    {
        // A file whose turn has not come (a mod.modal, a module further on): its declarations
        // are checked first, quietly, so the body can be (they are checked again in its turn).
        side_tables(u, 0);
        for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, d, u->scope, 0, 0);
        }
        memset(&c, 0, offsetof(Check, locals));
        c.u = u;
        c.implicit_self = domain_config_bool(u->domain, "implicit_self", 0);
        errors = cx.quiet_errors; // what the declarations say is reported in their turn
    }
    capture_mir = 1;
    check_callable(&c, node, SYM(fn)->owner, fn);
    cx.quiet--;
    capture_mir = saved;
    cx.names_resolved = names; // (counted when it is checked in its turn)
    cx.calls_resolved = calls;
    return cx.quiet_errors == errors;
}

// ---- function kinds: over the call graph ------------------------------------------------------

static uint8_t kind_closure(SymId fn, int depth);

// The call graph as lists of callees (counting sort by caller), once per set of edges; the
// kinds are computed again from it.
static void build_adjacency(void)
{
    if (fn_adj_edges == fn_nedges)
    {
        return;
    }
    facts_of((SymId)(cx.syms.size ? cx.syms.size - 1 : 0)); // the table covers every symbol
    size_t n = fn_facts_cap;
    fn_adj = (uint32_t *)arena_alloc((n + 1) * sizeof(uint32_t));
    fn_adj_to = (SymId *)arena_alloc((fn_nedges + 1) * sizeof(SymId));
    memset(fn_adj, 0, (n + 1) * sizeof(uint32_t));
    for (size_t e = 0; e < fn_nedges; e++)
    {
        fn_adj[fn_edges[e].from + 1]++;
    }
    for (size_t i = 0; i < n; i++)
    {
        fn_adj[i + 1] += fn_adj[i];
    }
    uint32_t *fill = (uint32_t *)arena_alloc((n + 1) * sizeof(uint32_t));
    memcpy(fill, fn_adj, (n + 1) * sizeof(uint32_t));
    for (size_t e = 0; e < fn_nedges; e++)
    {
        fn_adj_to[fill[fn_edges[e].from]++] = fn_edges[e].to;
    }
    for (size_t i = 0; i < n; i++)
    {
        fn_facts[i].state = 0;
    }
    fn_adj_edges = fn_nedges;
    // Every function's kind: exact by the walk where there is no cycle; a cycle then settles by
    // passing kinds back along the edges until nothing changes.
    for (size_t i = 1; i < n; i++)
    {
        kind_closure((SymId)i, 0);
    }
    for (int changed = 1, rounds = 0; changed && rounds < 10000; rounds++)
    {
        changed = 0;
        for (size_t e = 0; e < fn_nedges; e++)
        {
            FnFacts *from = &fn_facts[fn_edges[e].from], *to = &fn_facts[fn_edges[e].to];
            if (to->kind & ~from->kind)
            {
                from->kind |= to->kind;
                changed = 1;
            }
        }
    }
}

// A function's effects with those of everything it calls. In a cycle, a function sees what is
// known of the others so far; the cycle's members are then done again from the top.
static uint8_t kind_closure(SymId fn, int depth)
{
    FnFacts *f = &fn_facts[fn];
    if (f->state == 2 || f->state == 1 || depth > 10000)
    {
        return f->state == 2 ? f->kind : f->fx;
    }
    f->state = 1;
    uint8_t k = f->fx;
    for (uint32_t e = fn_adj[fn]; e < fn_adj[fn + 1]; e++)
    {
        k |= kind_closure(fn_adj_to[e], depth + 1);
    }
    f = &fn_facts[fn];
    f->kind = k;
    f->state = 2;
    return k;
}

// The kind of a function, as `--sig` prints it: "both", "run time  (prints, at util.modal:8)",
// "compile time  (declared in mod.modal)", or "" when its body was not checked here.
const char *sema_fn_kind(SymId fn, char *buf, size_t size)
{
    const Symbol *s = SYM(fn);
    if (s->kind != SYM_METHOD)
    {
        return "";
    }
    if (cx.units[s->unit].is_manifest)
    {
        return "compile time  (declared in mod.modal)";
    }
    if (fn >= fn_facts_cap || !fn_facts[fn].checked)
    {
        return "";
    }
    build_adjacency();
    uint8_t k = kind_closure(fn, 0);
    if (!k)
    {
        return "both";
    }
    // The reason given: output first, then a device, then a variable; found in the function
    // itself, or along the chain of callees through which it has it.
    int fx = k & FX_OUTPUT ? FX_OUTPUT : k & FX_DEVICE ? FX_DEVICE : FX_MUTABLE;
    int bit = fx == FX_OUTPUT ? 0 : fx == FX_DEVICE ? 1 : 2;
    // The nearest function with the effect itself (breadth first over the calls), and the
    // callee of `fn` that leads to it.
    SymId at = fn, through = 0;
    if (!(fn_facts[fn].fx & fx))
    {
        size_t n = fn_facts_cap;
        SymId *queue = (SymId *)arena_alloc(n * sizeof(SymId));
        SymId *first = (SymId *)arena_alloc(n * sizeof(SymId)); // the callee of fn it was reached by
        uint8_t *seen = (uint8_t *)arena_alloc(n);
        memset(seen, 0, n);
        size_t head = 0, tail = 0;
        seen[fn] = 1;
        queue[tail++] = fn;
        first[fn] = 0;
        while (head < tail && at == fn)
        {
            SymId g = queue[head++];
            for (uint32_t e = fn_adj[g]; e < fn_adj[g + 1]; e++)
            {
                SymId h = fn_adj_to[e];
                if (seen[h])
                {
                    continue;
                }
                seen[h] = 1;
                first[h] = g == fn ? h : first[g];
                if (fn_facts[h].fx & fx)
                {
                    at = h;
                    through = first[h];
                    break;
                }
                queue[tail++] = h;
            }
        }
    }
    const FnFacts *f = facts_of(at);
    const char *what = fx == FX_OUTPUT ? "prints" : fx == FX_DEVICE ? "a device register" : "a mutable module variable";
    const Unit *u = &cx.units[f->unit];
    int line = 1;
    for (const char *p = u->src; p < u->src + f->at[bit] && p < u->src + u->len; p++)
    {
        line += *p == '\n';
    }
    const char *base = strrchr(u->path, '/');
    int len;
    const char *name = symbol_name(through ? through : at, &len);
    if (at == fn)
    {
        snprintf(buf, size, "run time  (%s, %s:%d)", what, base ? base + 1 : u->path, line);
    }
    else
    {
        snprintf(buf, size, "run time  (%s, through '%.*s', %s:%d)", what, len, name, base ? base + 1 : u->path,
                 line);
    }
    return buf;
}

// What print shows for a function value (std.md §13): its name and its signature as made,
// `function loop(type, u64): u64`. The name is in the binary only because the program printed it.
size_t sema_fn_text(Unit *u, SymId fn, char *buf, size_t cap)
{
    (void)u;
    Unit *home = &cx.units[SYM(fn)->unit];
    int len;
    const char *name = symbol_name(fn, &len);
    size_t n = 0;
#define ADD(...)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        int k_ = snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__);                            \
        n += k_ > 0 ? (size_t)k_ : 0;                                                              \
    } while (0)
    char t[128];
    ADD("function ");
    if (sema_is_generic(fn)) // not made yet: as declared, `T loop(T: type, n: T)`
    {
        char sig[256];
        ADD("%s", ast.sig_str(home, SYM(fn)->decl, sig, sizeof(sig)));
#undef ADD
        return n < cap ? n : cap ? cap - 1 : 0;
    }
#define ADD(...)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        int k_ = snprintf(buf + n, n < cap ? cap - n : 0, __VA_ARGS__);                            \
        n += k_ > 0 ? (size_t)k_ : 0;                                                              \
    } while (0)
    if (SYM(fn)->owner)
    {
        int ol;
        const char *on = symbol_name(SYM(fn)->owner, &ol);
        ADD("%.*s.", ol, on);
    }
    ADD("%.*s(", len, name);
    int first = 1;
    for (NodeId p = sema_fn_params(home, fn); p; p = NEXT(home, p))
    {
        ADD("%s%s", first ? "" : ", ", type_str(sema_param_type(home, fn, p), t, sizeof(t)));
        first = 0;
    }
    ADD(")");
    TypeId ret = sema_fn_ret(home, fn);
    if (ret != T_VOID)
    {
        ADD(": %s", type_str(ret, t, sizeof(t)));
    }
#undef ADD
    return n < cap ? n : cap ? cap - 1 : 0;
}
