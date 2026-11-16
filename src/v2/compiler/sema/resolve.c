// resolve.c (v2): declaration pass. Turns the declarations of one unit into symbols and scopes.
//
// Rules enforced here:
//   - a struct is a domain: its fields and methods live in its own scope
//   - a top-level callable whose first parameter is `self: T` belongs to struct T of this
//     module; with a bare `self` (or `self: <module>`) it belongs to the module itself
//   - `name(...): T { }` and `name := (...) T { }` are defined once
//   - `T name(...)` may be defined again with different parameter types (an overload set)
//   - inside / outside: what is declared inside a struct's braces is private to the struct;
//     a method attached from outside with `self: T` is the struct's public face
// Bodies are not resolved yet; that needs types and is the next phase.
#include "v2/compiler/cx.h"
#include <string.h>

static void redeclared(Unit *u, uint32_t tok, SymId previous, const char *hint)
{
    const Token *t = TOK(u, tok);
    int line, col;
    source_pos(u->src, TSTART(TOK(u, SYM(previous)->tok)), &line, &col);
    diag_at(u, tok, "error", "'%.*s' is already declared at line %d%s", t->len, TSTART(t), line,
            hint);
}

// ---- signatures ------------------------------------------------------------------------------

NodeId sema_callable_params(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    if (n->kind == AST_DEFINE)
    {
        n = NODE(u, n->a); // name := (params) T { }
    }
    return n->a;
}

static int type_equal(const Unit *u, NodeId a, NodeId b)
{
    while (a && b)
    {
        const AstNode *x = NODE(u, a);
        const AstNode *y = NODE(u, b);
        if (x->kind != y->kind)
        {
            return 0;
        }
        if (x->kind == AST_TYPE_NAME)
        {
            const Token *xf = TOK(u, x->tok), *xl = TOK(u, x->tok + 2u * x->aux);
            const Token *yf = TOK(u, y->tok), *yl = TOK(u, y->tok + 2u * y->aux);
            size_t xn = (size_t)((TSTART(xl) + xl->len) - TSTART(xf));
            size_t yn = (size_t)((TSTART(yl) + yl->len) - TSTART(yf));
            return xn == yn && memcmp(TSTART(xf), TSTART(yf), xn) == 0;
        }
        if (x->kind == AST_TYPE_STRUCT)
        {
            return a == b;
        }
        a = x->a;
        b = y->a;
    }
    return a == b;
}

static NodeId skip_self(const Unit *u, NodeId param)
{
    if (param)
    {
        const Token *t = TOK(u, NODE(u, param)->tok);
        if (is_self_token(TSTART(t), t->len))
        {
            return NEXT(u, param);
        }
    }
    return param;
}

// Two overloads collide when their parameter types match one by one (self aside).
// Two parameters match the same arguments: both bind a name, or both are the same literal.
static int same_pattern(const Unit *u, NodeId pa, NodeId pb)
{
    const AstNode *a = NODE(u, pa), *b = NODE(u, pb);
    int pat_a = (a->flags & AST_F_PATTERN) != 0, pat_b = (b->flags & AST_F_PATTERN) != 0;
    if (!pat_a || !pat_b)
    {
        return pat_a == pat_b;
    }
    if (a->aux != b->aux)
    {
        return 0;
    }
    // the literal, and for a range its `..` and high end: the same text
    uint32_t span = (a->aux & PAT_RANGE) ? ((a->aux & PAT_OPEN) ? 2 : (a->aux & PAT_NEG_HI) ? 4 : 3) : 1;
    for (uint32_t k = 0; k < span; k++)
    {
        const Token *ta = TOK(u, a->tok + k), *tb = TOK(u, b->tok + k);
        if (ta->len != tb->len || memcmp(TSTART(ta), TSTART(tb), ta->len) != 0)
        {
            return 0;
        }
    }
    return 1;
}

static int has_patterns(const Unit *u, NodeId params)
{
    for (NodeId p = params; p; p = NEXT(u, p))
    {
        if (NODE(u, p)->flags & AST_F_PATTERN)
        {
            return 1;
        }
    }
    return 0;
}

static int arity(const Unit *u, NodeId params)
{
    int n = 0;
    for (NodeId p = skip_self(u, params); p; p = NEXT(u, p))
    {
        n++;
    }
    return n;
}

static int same_signature(const Unit *u, NodeId pa, NodeId pb)
{
    pa = skip_self(u, pa);
    pb = skip_self(u, pb);
    while (pa && pb)
    {
        if (!type_equal(u, NODE(u, pa)->a, NODE(u, pb)->a) || !same_pattern(u, pa, pb))
        {
            return 0;
        }
        pa = NEXT(u, pa);
        pb = NEXT(u, pb);
    }
    return pa == pb;
}

// The same parameter types in the same order, patterns aside: definitions of one clause group.
// Overloads with other types are other functions (chosen at the call by the argument types).
static int same_types(const Unit *u, NodeId pa, NodeId pb)
{
    pa = skip_self(u, pa);
    pb = skip_self(u, pb);
    while (pa && pb)
    {
        if (!type_equal(u, NODE(u, pa)->a, NODE(u, pb)->a))
        {
            return 0;
        }
        pa = NEXT(u, pa);
        pb = NEXT(u, pb);
    }
    return pa == pb;
}

// ---- the clause index ---------------------------------------------------------------------------
//
// One open-addressing table per unit, built while its declarations are read, and only for names
// defined more than once. Each group of definitions (the first one's SymId) has a tail entry
// (where the next definition hangs), one entry per signature (types and patterns) and one per
// catch-all (types, no patterns). A hash match is confirmed with the exact comparison.

enum
{
    CLAUSE_TAIL = 1,
    CLAUSE_SIG,
    CLAUSE_CATCHALL,
};

typedef struct
{
    SymId first; // 0: empty
    uint32_t key;
    SymId sym;
    uint32_t kind;
} ClauseEntry;

static ClauseEntry *clauses;
static uint32_t clauses_cap, clauses_used;

static uint32_t mix32(uint32_t h, uint32_t v)
{
    h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

// Follows type_equal: what it calls equal hashes equal.
static uint32_t type_hash(const Unit *u, NodeId a)
{
    uint32_t h = 0x811c9dc5u;
    while (a)
    {
        const AstNode *x = NODE(u, a);
        h = mix32(h, x->kind);
        if (x->kind == AST_TYPE_NAME)
        {
            const Token *f = TOK(u, x->tok), *l = TOK(u, x->tok + 2u * x->aux);
            size_t n = (size_t)((TSTART(l) + l->len) - TSTART(f));
            return mix32(h, (uint32_t)hash_bytes(TSTART(f), n, 0));
        }
        if (x->kind == AST_TYPE_STRUCT)
        {
            return mix32(h, a);
        }
        a = x->a;
    }
    return h;
}

static NodeId skip_self(const Unit *u, NodeId param);

// The parameter types (self aside) and, with `patterns`, what same_pattern compares.
static uint32_t signature_hash(const Unit *u, NodeId params, int patterns)
{
    uint32_t h = 0;
    for (NodeId p = skip_self(u, params); p; p = NEXT(u, p))
    {
        const AstNode *n = NODE(u, p);
        h = mix32(h, type_hash(u, n->a));
        if (!patterns)
        {
            continue;
        }
        if (!(n->flags & AST_F_PATTERN))
        {
            h = mix32(h, 1);
            continue;
        }
        h = mix32(h, n->aux);
        uint32_t span = (n->aux & PAT_RANGE) ? ((n->aux & PAT_OPEN) ? 2 : (n->aux & PAT_NEG_HI) ? 4 : 3) : 1;
        for (uint32_t k = 0; k < span; k++)
        {
            const Token *t = TOK(u, n->tok + k);
            h = mix32(h, (uint32_t)hash_bytes(TSTART(t), t->len, 0));
        }
    }
    return h;
}

static uint32_t clause_slot(SymId first, uint32_t key, uint32_t kind)
{
    return mix32(mix32(first * 0x9e3779b1u, key), kind) & (clauses_cap - 1);
}

static void clause_put(SymId first, uint32_t key, uint32_t kind, SymId sym);

static void clauses_grow(void)
{
    ClauseEntry *old = clauses;
    uint32_t old_cap = clauses_cap;
    clauses_cap = old_cap ? old_cap * 2 : 256;
    clauses = (ClauseEntry *)arena_alloc((size_t)clauses_cap * sizeof(ClauseEntry));
    memset(clauses, 0, (size_t)clauses_cap * sizeof(ClauseEntry));
    clauses_used = 0;
    for (uint32_t i = 0; i < old_cap; i++)
    {
        if (old[i].first)
        {
            clause_put(old[i].first, old[i].key, old[i].kind, old[i].sym);
        }
    }
}

static void clause_put(SymId first, uint32_t key, uint32_t kind, SymId sym)
{
    if ((clauses_used + 1) * 2 > clauses_cap)
    {
        clauses_grow();
    }
    uint32_t i = clause_slot(first, key, kind);
    while (clauses[i].first)
    {
        i = (i + 1) & (clauses_cap - 1);
    }
    clauses[i] = (ClauseEntry){first, key, sym, kind};
    clauses_used++;
}

static int same_signature(const Unit *u, NodeId pa, NodeId pb);
static int same_types(const Unit *u, NodeId pa, NodeId pb);
static int has_patterns(const Unit *u, NodeId params);

// The definition of group `first` with this signature (CLAUSE_SIG) or this catch-all
// (CLAUSE_CATCHALL), or 0.
static SymId clause_find(const Unit *u, uint32_t kind, SymId first, uint32_t key, NodeId params)
{
    for (uint32_t i = clause_slot(first, key, kind); clauses[i].first; i = (i + 1) & (clauses_cap - 1))
    {
        const ClauseEntry *e = &clauses[i];
        if (e->first != first || e->key != key || e->kind != kind)
        {
            continue;
        }
        NodeId theirs = sema_callable_params(u, SYM(e->sym)->decl);
        if (kind == CLAUSE_SIG ? same_signature(u, theirs, params) : same_types(u, theirs, params))
        {
            return e->sym;
        }
    }
    return 0;
}

static SymId *clause_tail(SymId first)
{
    for (uint32_t i = clause_slot(first, first, CLAUSE_TAIL); clauses[i].first; i = (i + 1) & (clauses_cap - 1))
    {
        if (clauses[i].first == first && clauses[i].kind == CLAUSE_TAIL)
        {
            return &clauses[i].sym;
        }
    }
    return NULL; // clause_index_first put it there
}

static void clause_index_add(const Unit *u, SymId first, SymId sym, NodeId params)
{
    clause_put(first, signature_hash(u, params, 1), CLAUSE_SIG, sym);
    if (!has_patterns(u, params))
    {
        clause_put(first, signature_hash(u, params, 0), CLAUSE_CATCHALL, sym);
    }
}

// A group enters the index when its second definition is read.
static void clause_index_first(const Unit *u, SymId first)
{
    if (clauses_cap && clause_tail(first))
    {
        return;
    }
    clause_put(first, first, CLAUSE_TAIL, first);
    clause_index_add(u, first, first, sema_callable_params(u, SYM(first)->decl));
}

// ---- declarations ----------------------------------------------------------------------------

static void declare_value(Unit *u, Scope *scope, SymId owner, SymKind kind, uint8_t flags,
                          uint32_t tok, NodeId decl)
{
    const Token *t = TOK(u, tok);
    SymId previous = scope_find_token(scope, t);
    if (previous)
    {
        redeclared(u, tok, previous, "");
        return;
    }
    scope_insert(scope, symbol_new(u, kind, flags, tok, owner, decl));
}

// `outside` is 1 for a method written outside its struct's braces, 0 otherwise.
static void declare_callable(Unit *u, Scope *scope, SymId owner, uint32_t tok, NodeId decl,
                             int overloadable, int outside)
{
    const Token *t = TOK(u, tok);
    uint8_t flags = (uint8_t)(overloadable ? SYM_F_MUT : 0);

    SymId first = scope_find_token(scope, t);
    if (!first)
    {
        scope_insert(scope, symbol_new(u, SYM_METHOD, flags, tok, owner, decl));
        return;
    }

    if (SYM(first)->kind == SYM_FIELD && outside)
    {
        // An accessor may carry the name of the field it reads: `u8 n1(self: T): self.n1`.
        // `x.n1(...)` is the method and `x.n1` the field, so the name is never ambiguous.
        // It is inserted in front of the field, which is what a lookup then finds.
        scope_insert(scope, symbol_new(u, SYM_METHOD, flags, tok, owner, decl));
        return;
    }
    if (SYM(first)->kind != SYM_METHOD)
    {
        redeclared(u, tok, first, "");
        return;
    }
    if (!overloadable || !(SYM(first)->flags & SYM_F_MUT))
    {
        redeclared(u, tok, first,
                   " (only the type-first form `T name(...)` can be overloaded; "
                   "`name(...): T` and `name := (...)` are defined once)");
        return;
    }

    // Clauses: definitions with as many parameters are tried in order, so one without
    // patterns catches everything and must come last. The group's index answers "the same
    // signature?" and "a catch-all of these types?" by hash, so n clauses cost n, not n².
    NodeId mine = sema_callable_params(u, decl);
    clause_index_first(u, first);
    uint32_t sig = signature_hash(u, mine, 1), types = signature_hash(u, mine, 0);
    SymId s = clause_find(u, CLAUSE_SIG, first, sig, mine);
    if (s)
    {
        redeclared(u, tok, s,
                   has_patterns(u, mine) ? " with the same patterns" : " with the same parameter types");
        return;
    }
    s = clause_find(u, CLAUSE_CATCHALL, first, types, mine);
    if (s)
    {
        redeclared(u, tok, s,
                   ": that definition takes every value, so this clause would never run. Clauses "
                   "are tried in order: put the ones with patterns first");
        return;
    }
    // Later overloads hang off the first one; only the first sits in the hash bucket.
    SymId added = symbol_new(u, SYM_METHOD, flags, tok, owner, decl);
    SymId *tail = clause_tail(first);
    SYM(*tail)->next_overload = added;
    *tail = added;
    clause_index_add(u, first, added, mine);
}

// Where does a top-level callable live? Returns the scope and sets *owner, or NULL on error.
static Scope *callable_home(Unit *u, NodeId params, SymId *owner)
{
    *owner = 0;
    if (!params)
    {
        return u->scope;
    }

    const AstNode *first = NODE(u, params);
    const Token *name = TOK(u, first->tok);
    if (!is_self_token(TSTART(name), name->len) || !first->a)
    {
        return u->scope; // no self, or a bare `self`: the module owns it
    }

    const AstNode *type = NODE(u, first->a);
    if (type->kind == AST_TYPE_NAME && type->aux == 0)
    {
        const Token *t = TOK(u, type->tok);
        const char *domain = domain_name(u->domain);

        // A struct wins over the module when they share a name (counter.modal declaring
        // `counter: struct`): the file exists to hold that type.
        SymId target = scope_find_token(u->scope, t);
        if (target && SYM(target)->kind == SYM_STRUCT)
        {
            *owner = target;
            return scope_at(SYM(target)->scope_ref);
        }

        if (is_self_token(TSTART(t), t->len) ||
            (t->len == u->name_len && memcmp(TSTART(t), u->name, (size_t)t->len) == 0) ||
            (strlen(domain) == (size_t)t->len && memcmp(TSTART(t), domain, (size_t)t->len) == 0))
        {
            return u->scope; // `self: <module>` spelled out
        }
    }

    diag_at(u, type->tok, "error",
            "the type of `self` must be this module or a struct declared in this file: a "
            "method can only be attached to a type of its own domain");
    return NULL;
}

// Inside a struct body, `self` may be left bare or typed with the struct's own name.
static void check_member_self(Unit *u, SymId owner, NodeId params)
{
    if (!params || !NODE(u, params)->a)
    {
        return;
    }
    const AstNode *first = NODE(u, params);
    const Token *name = TOK(u, first->tok);
    if (!is_self_token(TSTART(name), name->len))
    {
        return;
    }

    const AstNode *type = NODE(u, first->a);
    const Token *t = TOK(u, type->tok);
    int olen;
    const char *oname = symbol_name(owner, &olen);
    if (type->kind == AST_TYPE_NAME && type->aux == 0 &&
        (is_self_token(TSTART(t), t->len) ||
         (t->len == olen && memcmp(TSTART(t), oname, (size_t)olen) == 0)))
    {
        return;
    }
    diag_at(u, type->tok, "error", "inside '%.*s', `self` can only be typed as '%.*s'", olen, oname,
            olen, oname);
}

static void declare_define(Unit *u, Scope *scope, SymId owner, NodeId decl, int top_level)
{
    const AstNode *n = NODE(u, decl);
    uint32_t tok = n->tok;
    NodeId value = n->a;

    if (NODE(u, value)->kind == AST_FN_LIT)
    {
        NodeId params = NODE(u, value)->a;
        if (top_level)
        {
            scope = callable_home(u, params, &owner);
            if (!scope)
            {
                return;
            }
        }
        else
        {
            check_member_self(u, owner, params);
        }
        declare_callable(u, scope, owner, tok, decl, 0, top_level && owner);
        return;
    }

    SymKind kind = ast_import_root(u, value) ? SYM_IMPORT : SYM_CONST;
    declare_value(u, scope, owner, kind, 0, tok, decl);
}

static void declare_import_items(Unit *u, NodeId item)
{
    for (; item; item = NEXT(u, item))
    {
        const AstNode *n = NODE(u, item);
        declare_value(u, u->scope, 0, SYM_IMPORT, 0, n->b ? n->b : n->tok, item);
    }
}

// A scope's hash table is sized once, from the number of declarations it will hold, so
// lookups stay O(1) in a file with thousands of declarations and small scopes stay small.
static uint32_t bucket_count(const Unit *u, NodeId first, uint32_t minimum)
{
    uint32_t n = 0, buckets = minimum;
    for (NodeId d = first; d; d = NEXT(u, d))
    {
        n++;
    }
    while (buckets < n)
    {
        buckets *= 2;
    }
    return buckets;
}

// Nesting is bounded by the parser's depth limit, so this recursion is too.
static void declare_struct(Unit *u, Scope *scope, SymId owner, NodeId decl)
{
    uint32_t tok = NODE(u, decl)->tok;
    const Token *t = TOK(u, tok);
    SymId previous = scope_find_token(scope, t);
    if (previous)
    {
        redeclared(u, tok, previous, "");
        return;
    }

    SymId self = symbol_new(u, SYM_STRUCT, 0, tok, owner, decl);
    Scope *members =
        scope_new(scope, self, bucket_count(u, NODE(u, decl)->a, SCOPE_STRUCT_BUCKETS));
    SYM(self)->scope_ref = scope_ref(members);
    scope_insert(scope, self);

    for (NodeId m = NODE(u, decl)->a; m; m = NEXT(u, m))
    {
        const AstNode *n = NODE(u, m);
        switch (n->kind)
        {
        case AST_STRUCT_DECL:
            declare_struct(u, members, self, m);
            break;
        case AST_FIELD_DECL:
            declare_value(u, members, self, SYM_FIELD, (n->flags & AST_F_MUT) ? SYM_F_MUT : 0,
                          n->tok, m);
            break;
        case AST_METHOD_DECL:
            check_member_self(u, self, n->a);
            declare_callable(u, members, self, n->tok, m, n->flags & AST_F_MUT, 0); // inside
            break;
        case AST_DEFINE:
            declare_define(u, members, self, m, 0);
            break;
        default:
            break;
        }
    }
}

typedef struct
{
    uint32_t key; // the parameter types' hash; then, for a span, its column
    uint32_t pos; // order of definition in the chain
} ClauseKey;

typedef struct
{
    uint64_t lo, hi;  // order keys (sema_pattern_span); a string: its bytes' hash in lo, length in hi
    uint32_t col;     // the parameter holding the clause's only pattern; strings: | 1u << 31
    uint32_t pos;
    uint32_t tok;     // a string's literal
} ClauseSpan;

typedef struct
{
    uint32_t later, earlier; // positions
} ClausePair;

static SymId *chain_syms;
static size_t chain_cap;
static ClauseKey *chain_keys;
static size_t keys_cap;
static ClauseSpan *chain_spans;
static size_t spans_cap;
static ClausePair *chain_pairs;
static size_t pairs_cap, pairs_n;

static void clause_last(Unit *u, SymId s);
static void clause_chain(Unit *u, SymId head, uint8_t *seen, SymId first_sym);

void sema_declare(Unit *u)
{
    // the clause index and the chain buffers live in the arena, which a new compilation empties
    clauses = NULL;
    clauses_cap = clauses_used = 0;
    chain_syms = NULL;
    chain_keys = NULL;
    chain_spans = NULL;
    chain_pairs = NULL;
    chain_cap = keys_cap = spans_cap = pairs_cap = 0;
    SymId first_sym = (SymId)cx.syms.size;
    u->scope = scope_new(NULL, 0, bucket_count(u, NODE(u, u->root)->a, SCOPE_MODULE_BUCKETS));

    // Pass 1: structs, so that `name(self: T)` can attach to a T declared further down.
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            declare_struct(u, u->scope, 0, d);
        }
    }

    // Pass 2: everything else at the top level.
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        const AstNode *n = NODE(u, d);
        switch (n->kind)
        {
        case AST_FIELD_DECL:
            declare_value(u, u->scope, 0, SYM_CONST, (n->flags & AST_F_MUT) ? SYM_F_MUT : 0, n->tok,
                          d);
            break;
        case AST_METHOD_DECL:
        {
            SymId owner;
            Scope *home = callable_home(u, n->a, &owner);
            if (home)
            {
                declare_callable(u, home, owner, n->tok, d, n->flags & AST_F_MUT, owner != 0);
            }
            break;
        }
        case AST_DEFINE:
            declare_define(u, u->scope, 0, d, 1);
            break;
        case AST_VAL_DECL:
            declare_import_items(u, n->a);
            break;
        case AST_IMPORT_PICK:
            declare_import_items(u, n->b); // bare import(x).(a, b as c)
            break;
        case AST_STRUCT_LIT: // the face line of before: visibility follows use now
            diag_at(u, n->tok, "error",
                    "a struct needs no face line: its members are public as soon as another domain uses "
                    "them (std.md §10). Remove this line");
            break;
        default:
            break;
        }
    }

    // Every group of clauses ends with one that takes every value: a call always finds one.
    // Each chain of definitions is read once (its first one has the lowest SymId), so the
    // check costs what the clauses cost, with n log n sorts in place of pairs.
    uint32_t count = (uint32_t)cx.syms.size - first_sym;
    uint8_t *seen = NULL;
    for (SymId s = first_sym; s < cx.syms.size; s++)
    {
        const Symbol *sym = SYM(s);
        if (sym->unit != u->id || sym->kind != SYM_METHOD || !(sym->flags & SYM_F_MUT))
        {
            continue;
        }
        if (!sym->next_overload)
        {
            if (seen && (seen[(s - first_sym) >> 3] >> ((s - first_sym) & 7) & 1))
            {
                continue; // the last of a chain read already
            }
            clause_last(u, s); // a definition alone
            continue;
        }
        if (seen && (seen[(s - first_sym) >> 3] >> ((s - first_sym) & 7) & 1))
        {
            continue;
        }
        if (!seen)
        {
            seen = (uint8_t *)arena_alloc((count + 7) / 8);
            memset(seen, 0, (count + 7) / 8);
        }
        clause_chain(u, s, seen, first_sym);
    }
}

// ---- the clause groups ---------------------------------------------------------------------------

// The last definition of a group: with patterns, no clause after it takes the other values.
static void clause_last(Unit *u, SymId s)
{
    const Symbol *sym = SYM(s);
    NodeId params = sema_callable_params(u, sym->decl);
    if (!has_patterns(u, params))
    {
        return;
    }
    int n = arity(u, params);
    // A clause set with no result may leave values to nobody (the call does nothing for them):
    // a warning, as for arms. One that gives a value takes them all.
    NodeId ret = fn_ret_node(u, sym->decl);
    const Token *rt = ret ? TOK(u, NODE(u, ret)->tok) : NULL;
    int nil = !ret || (rt->len == 3 && memcmp(TSTART(rt), "nil", 3) == 0);
    const Token *name = TOK(u, sym->tok);
    if (nil)
    {
        diag_at(u, sym->tok, "warning",
                "'%.*s' with %d argument%s: no clause takes the other values, so a call with one does "
                "nothing (`nil %.*s(T n) {}` last says so)",
                name->len, TSTART(name), n, n == 1 ? "" : "s", name->len, TSTART(name));
    }
    else
    {
        diag_at(u, sym->tok, "error",
                "'%.*s' with %d argument%s has clauses with patterns but none after them that "
                "takes every value: add one last, e.g. `T %.*s(T n): ...`",
                name->len, TSTART(name), n, n == 1 ? "" : "s", name->len, TSTART(name));
    }
}

static int key_order(const void *x, const void *y)
{
    const ClauseKey *a = x, *b = y;
    return a->key != b->key ? (a->key < b->key ? -1 : 1) : (a->pos > b->pos) - (a->pos < b->pos);
}

static int span_order(const void *x, const void *y)
{
    const ClauseSpan *a = x, *b = y;
    if (a->col != b->col)
    {
        return a->col < b->col ? -1 : 1;
    }
    if (a->lo != b->lo)
    {
        return a->lo < b->lo ? -1 : 1;
    }
    if (a->hi != b->hi && (a->col >> 31))
    {
        return a->hi < b->hi ? -1 : 1;
    }
    return (a->pos > b->pos) - (a->pos < b->pos);
}

static int pair_order(const void *x, const void *y)
{
    const ClausePair *a = x, *b = y;
    return a->later != b->later ? (a->later < b->later ? -1 : 1)
                                : (a->earlier > b->earlier) - (a->earlier < b->earlier);
}

static void pair_add(uint32_t p, uint32_t q)
{
    if (pairs_n == pairs_cap)
    {
        chain_pairs = (ClausePair *)arena_grow(chain_pairs, sizeof(ClausePair), &pairs_cap);
    }
    chain_pairs[pairs_n++] = p > q ? (ClausePair){p, q} : (ClausePair){q, p};
}

static int same_string(const Unit *u, uint32_t a, uint32_t b)
{
    static uint8_t x[4096], y[4096];
    size_t nx = sema_pattern_text(u, a, x, sizeof(x)), ny = sema_pattern_text(u, b, y, sizeof(y));
    return nx == ny && memcmp(x, y, nx < sizeof(x) ? nx : sizeof(x)) == 0;
}

// Clauses take disjoint values, as arms do (std.md §8): the order decides nothing but where the
// catch-all goes, so a value two clauses with patterns both take is an error. As for arms, the
// check is on one column: both clauses have their only pattern in the same parameter. With
// patterns in several places the first that matches wins. `group` holds positions in the chain.
static void clause_group(Unit *u, const ClauseKey *group, uint32_t n)
{
    size_t spans = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const Symbol *sym = SYM(chain_syms[group[i].pos]);
        NodeId params = sema_callable_params(u, sym->decl);
        uint32_t col = 0, k = 0, patterns = 0;
        NodeId only = 0;
        for (NodeId x = params; x; x = NEXT(u, x), k++)
        {
            if (NODE(u, x)->flags & AST_F_PATTERN)
            {
                patterns++;
                col = k;
                only = x;
            }
        }
        if (patterns != 1)
        {
            continue;
        }
        Pattern pat;
        uint64_t lo, hi;
        int kind = sema_pattern_span(u, only, SYM(chain_syms[group[0].pos])->owner, &pat, &lo, &hi);
        if (!kind)
        {
            continue; // does not fit its type: the check of the clause says so
        }
        if (kind == 2)
        {
            static uint8_t text[4096];
            size_t len = sema_pattern_text(u, pat.tok, text, sizeof(text));
            lo = hash_bytes(text, len < sizeof(text) ? len : sizeof(text), 0);
            hi = len;
            col |= 1u << 31;
        }
        if (spans == spans_cap)
        {
            chain_spans = (ClauseSpan *)arena_grow(chain_spans, sizeof(ClauseSpan), &spans_cap);
        }
        chain_spans[spans++] = (ClauseSpan){lo, hi, col, group[i].pos, pat.tok};
    }
    if (spans < 2)
    {
        return;
    }
    qsort(chain_spans, spans, sizeof(ClauseSpan), span_order);
    pairs_n = 0;
    for (size_t i = 0; i < spans;)
    {
        size_t j = i + 1;
        if (chain_spans[i].col >> 31) // strings: the same bytes, within one run of equal hashes
        {
            while (j < spans && chain_spans[j].col == chain_spans[i].col && chain_spans[j].lo == chain_spans[i].lo &&
                   chain_spans[j].hi == chain_spans[i].hi)
            {
                j++;
            }
            for (size_t a = i + 1; a < j; a++)
            {
                for (size_t b = i; b < a; b++)
                {
                    if (same_string(u, chain_spans[a].tok, chain_spans[b].tok))
                    {
                        pair_add(chain_spans[a].pos, chain_spans[b].pos);
                        break;
                    }
                }
            }
            i = j;
            continue;
        }
        // numbers: sorted by low end, an interval overlaps one before it when it starts at or
        // before the highest end seen so far
        size_t top = i;
        while (j < spans && chain_spans[j].col == chain_spans[i].col)
        {
            if (chain_spans[j].lo <= chain_spans[top].hi)
            {
                pair_add(chain_spans[j].pos, chain_spans[top].pos);
            }
            if (chain_spans[j].hi > chain_spans[top].hi)
            {
                top = j;
            }
            j++;
        }
        i = j;
    }
    qsort(chain_pairs, pairs_n, sizeof(ClausePair), pair_order);
    for (size_t i = 0; i < pairs_n; i++)
    {
        int line, col;
        source_pos(u->src, TSTART(TOK(u, SYM(chain_syms[chain_pairs[i].earlier])->tok)), &line, &col);
        diag_at(u, SYM(chain_syms[chain_pairs[i].later])->tok, "error",
                "this clause takes values the clause at line %d takes too: clauses take disjoint "
                "values, so what each takes is what it says", line);
    }
}

// One chain of definitions of a name: its groups are the definitions with the same parameter
// types (clauses), found by sorting on the types' hash.
static void clause_chain(Unit *u, SymId head, uint8_t *seen, SymId first_sym)
{
    uint32_t m = 0;
    for (SymId t = head; t; t = SYM(t)->next_overload, m++)
    {
        if (m == chain_cap)
        {
            chain_syms = (SymId *)arena_grow(chain_syms, sizeof(SymId), &chain_cap);
        }
        chain_syms[m] = t;
        seen[(t - first_sym) >> 3] |= (uint8_t)(1u << ((t - first_sym) & 7));
    }
    while (keys_cap < m)
    {
        chain_keys = (ClauseKey *)arena_grow(chain_keys, sizeof(ClauseKey), &keys_cap);
    }
    for (uint32_t i = 0; i < m; i++)
    {
        chain_keys[i] = (ClauseKey){signature_hash(u, sema_callable_params(u, SYM(chain_syms[i])->decl), 0), i};
    }
    qsort(chain_keys, m, sizeof(ClauseKey), key_order);
    for (uint32_t i = 0; i < m;)
    {
        uint32_t j = i + 1;
        while (j < m && chain_keys[j].key == chain_keys[i].key)
        {
            j++;
        }
        // One run of equal hashes: almost always one group. A collision splits it by comparing.
        while (i < j)
        {
            NodeId rep = sema_callable_params(u, SYM(chain_syms[chain_keys[i].pos])->decl);
            uint32_t end = i + 1;
            for (uint32_t k = i + 1; k < j; k++)
            {
                if (same_types(u, sema_callable_params(u, SYM(chain_syms[chain_keys[k].pos])->decl), rep))
                {
                    ClauseKey keep = chain_keys[k];
                    memmove(&chain_keys[end + 1], &chain_keys[end], (k - end) * sizeof(ClauseKey));
                    chain_keys[end++] = keep;
                }
            }
            clause_group(u, &chain_keys[i], end - i);
            clause_last(u, chain_syms[chain_keys[end - 1].pos]);
            i = end;
        }
    }
}
