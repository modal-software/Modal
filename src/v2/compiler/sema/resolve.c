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

// `face` is SYM_F_EXPOSED for a method written outside its struct's braces, 0 otherwise.
static void declare_callable(Unit *u, Scope *scope, SymId owner, uint32_t tok, NodeId decl,
                             int overloadable, uint8_t face)
{
    const Token *t = TOK(u, tok);
    uint8_t flags = (uint8_t)((overloadable ? SYM_F_MUT : 0) | face);

    SymId first = scope_find_token(scope, t);
    if (!first)
    {
        scope_insert(scope, symbol_new(u, SYM_METHOD, flags, tok, owner, decl));
        return;
    }

    if (SYM(first)->kind == SYM_FIELD && face)
    {
        // An accessor may carry the name of the field it reads: `u8 n1(self: T): self.n1`.
        // From outside only the method can be seen, so the name is never ambiguous there.
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
    // patterns catches everything and must come last.
    NodeId mine = sema_callable_params(u, decl);
    SymId last = first;
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        NodeId theirs = sema_callable_params(u, SYM(s)->decl);
        if (same_signature(u, theirs, mine))
        {
            redeclared(u, tok, s,
                       has_patterns(u, mine) ? " with the same patterns" : " with the same parameter types");
            return;
        }
        if (same_types(u, theirs, mine) && !has_patterns(u, theirs))
        {
            redeclared(u, tok, s,
                       ": that definition takes every value, so this clause would never run. Clauses "
                       "are tried in order: put the ones with patterns first");
            return;
        }
        last = s;
    }
    // Later overloads hang off the first one; only the first sits in the hash bucket.
    SymId added = symbol_new(u, SYM_METHOD, flags, tok, owner, decl);
    SYM(last)->next_overload = added;
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
        declare_callable(u, scope, owner, tok, decl, 0, (top_level && owner) ? SYM_F_EXPOSED : 0);
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

void sema_declare(Unit *u)
{
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
                declare_callable(u, home, owner, n->tok, d, n->flags & AST_F_MUT,
                                 owner ? SYM_F_EXPOSED : 0); // outside the braces: the face
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
        default:
            break;
        }
    }

    // Every group of clauses ends with one that takes every value: a call always finds one.
    for (SymId s = 1; s < cx.syms.size; s++)
    {
        const Symbol *sym = SYM(s);
        if (sym->unit != u->id || sym->kind != SYM_METHOD || !(sym->flags & SYM_F_MUT))
        {
            continue;
        }
        NodeId params = sema_callable_params(u, sym->decl);
        if (!has_patterns(u, params))
        {
            continue;
        }
        int n = arity(u, params), later = 0;
        for (SymId t = sym->next_overload; t && !later; t = SYM(t)->next_overload)
        {
            later = same_types(u, sema_callable_params(u, SYM(t)->decl), params);
        }
        if (!later)
        {
            const Token *name = TOK(u, sym->tok);
            diag_at(u, sym->tok, "error",
                    "'%.*s' with %d argument%s has clauses with patterns but none after them that "
                    "takes every value: add one last, e.g. `T %.*s(T n): ...`",
                    name->len, TSTART(name), n, n == 1 ? "" : "s", name->len, TSTART(name));
        }
    }
}
