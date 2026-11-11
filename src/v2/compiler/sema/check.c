// check.c (v2): name and invocation resolution inside bodies.
//
// Before anything can be executed or generated, every name a body uses must lead to a
// declaration, and every invocation must fit one:
//   - a name is a local, a parameter, `self`, a module-level declaration, an import binding,
//     a member reached through implicit self (when the domain enables it), or it is an error
//   - `f(args)` must name something declared, and when that is a function, the number of
//     arguments must match one of its definitions (all of them, for an overload set)
//   - `self.name` and `self.name(args)` must name a member of the struct that owns the method
// Types are not inferred yet, so `value.method(args)` on a local is not checked, and
// overloads are told apart by argument count only.
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

#define MAX_LOCALS 256

typedef struct
{
    Unit *u;
    SymId owner;                 // struct that owns the callable being checked, 0 = the module
    int has_self;                // the callable declares `self`
    int implicit_self;           // domain config: a bare member name stands for self.name
    uint32_t locals[MAX_LOCALS]; // name tokens of the parameters and locals in scope
    int nlocals;
} Check;

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

static void check_expr(Check *c, NodeId id);

static int text_is(const Token *t, const char *s)
{
    size_t n = strlen(s);
    return t->len == n && memcmp(t->start, s, n) == 0;
}

static void push_local(Check *c, uint32_t tok)
{
    if (c->nlocals == MAX_LOCALS)
    {
        diag_at(c->u, tok, "error", "too many locals in one function (limit is %d)", MAX_LOCALS);
        return;
    }
    c->locals[c->nlocals++] = tok;
}

static Scope *self_scope(const Check *c)
{
    return c->owner ? scope_at(SYM(c->owner)->scope_ref) : c->u->scope;
}

static NameKind find_name(Check *c, const Token *t, SymId *sym)
{
    *sym = 0;
    if (is_self_token(t->start, (int)t->len))
    {
        return c->has_self ? NAME_SELF : NAME_MISSING;
    }
    for (int i = c->nlocals - 1; i >= 0; i--)
    {
        const Token *l = TOK(c->u, c->locals[i]);
        if (l->len == t->len && memcmp(l->start, t->start, t->len) == 0)
        {
            return NAME_LOCAL;
        }
    }
    if (t->kind == TOK_WRITE || text_is(t, "true") || text_is(t, "false") || text_is(t, "nil"))
    {
        return NAME_BUILTIN;
    }
    if ((*sym = scope_find(c->u->scope, t->start, t->len)))
    {
        return NAME_SYMBOL;
    }
    if (c->implicit_self && c->has_self && c->owner &&
        (*sym = scope_find(self_scope(c), t->start, t->len)))
    {
        return NAME_MEMBER;
    }
    return module_wildcard_provides(c->u, t->start, t->len) ? NAME_WILDCARD : NAME_MISSING;
}

// If some struct of this file has a member with this name, says how to reach it.
static void report_missing(Check *c, uint32_t tok, int is_call)
{
    const Token *t = TOK(c->u, tok);

    if (is_self_token(t->start, (int)t->len))
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
            memcmp(name, t->start, t->len) != 0)
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
            t->start);
}

static int count_list(const Unit *u, NodeId id)
{
    int n = 0;
    for (; id; id = NODE(u, id)->next)
    {
        n++;
    }
    return n;
}

// Parameters a caller supplies: everything but `self`.
static int arity_of(const Unit *u, NodeId decl)
{
    NodeId params = sema_callable_params(u, decl);
    int n = count_list(u, params);
    if (params)
    {
        const Token *first = TOK(u, NODE(u, params)->tok);
        if (is_self_token(first->start, (int)first->len))
        {
            n--;
        }
    }
    return n;
}

// The invocation must fit one definition of the function (any one, for an overload set).
static void check_arity(Check *c, uint32_t at, SymId first, int argc)
{
    char accepted[96];
    size_t len = 0;
    int defs = 0;

    accepted[0] = '\0';
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        int arity = arity_of(&cx.units[SYM(s)->unit], SYM(s)->decl);
        if (arity == argc)
        {
            return;
        }
        if (len < sizeof(accepted) - 8)
        {
            len += (size_t)snprintf(accepted + len, sizeof(accepted) - len, "%s%d",
                                    defs ? " or " : "", arity);
        }
        defs++;
    }

    int nlen;
    const char *name = symbol_name(first, &nlen);
    int line, col;
    const Unit *owner = &cx.units[SYM(first)->unit];
    source_pos(owner->src, TOK(owner, SYM(first)->tok)->start, &line, &col);
    diag_at(c->u, at, "error", "'%.*s' takes %s argument%s, but %d %s given (declared at %s:%d)",
            nlen, name, accepted, (defs == 1 && accepted[0] == '1' && !accepted[1]) ? "" : "s",
            argc, argc == 1 ? "is" : "are", owner->path, line);
}

// `self.name`: the member must exist in the struct (or module) that owns the callable.
// Returns the member, or 0 after reporting.
static SymId check_self_member(Check *c, NodeId selector)
{
    const Token *name = TOK(c->u, NODE(c->u, selector)->tok);
    SymId member = scope_find(self_scope(c), name->start, name->len);
    if (!member)
    {
        int olen = c->u->name_len;
        const char *owner = c->u->name;
        if (c->owner)
        {
            owner = symbol_name(c->owner, &olen);
        }
        diag_at(c->u, NODE(c->u, selector)->tok, "error", "'%.*s' has no member '%.*s'", olen,
                owner, name->len, name->start);
    }
    return member;
}

static int is_self_selector(const Check *c, NodeId id)
{
    const AstNode *n = NODE(c->u, id);
    if (n->kind != AST_SELECTOR || NODE(c->u, n->a)->kind != AST_IDENT || !c->has_self)
    {
        return 0;
    }
    const Token *base = TOK(c->u, NODE(c->u, n->a)->tok);
    return is_self_token(base->start, (int)base->len);
}

static void check_call(Check *c, NodeId id)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);
    NodeId callee = n->a;
    NodeId args = n->b;
    int argc = count_list(u, args);
    const AstNode *cn = NODE(u, callee);

    if (cn->kind == AST_IDENT)
    {
        SymId sym;
        NameKind kind = find_name(c, TOK(u, cn->tok), &sym);
        if (kind == NAME_MISSING)
        {
            report_missing(c, cn->tok, 1);
        }
        else if ((kind == NAME_SYMBOL || kind == NAME_MEMBER) && SYM(sym)->kind == SYM_METHOD)
        {
            check_arity(c, cn->tok, sym, argc);
        }
    }
    else if (is_self_selector(c, callee))
    {
        SymId member = check_self_member(c, callee);
        if (member && SYM(member)->kind == SYM_METHOD)
        {
            check_arity(c, cn->tok, member, argc);
        }
    }
    else
    {
        check_expr(c, callee);
    }

    for (NodeId a = args; a; a = NODE(u, a)->next)
    {
        check_expr(c, a);
    }
}

static void check_block(Check *c, NodeId block)
{
    int saved = c->nlocals; // a block's locals end with the block
    for (NodeId s = NODE(c->u, block)->a; s; s = NODE(c->u, s)->next)
    {
        check_expr(c, s);
    }
    c->nlocals = saved;
}

static void check_callable(Check *c, NodeId params, NodeId body, SymId owner, int has_self)
{
    SymId saved_owner = c->owner;
    int saved_self = c->has_self;
    int saved_locals = c->nlocals; // a nested function literal still sees the enclosing locals

    c->owner = owner;
    c->has_self = has_self;
    for (NodeId p = params; p; p = NODE(c->u, p)->next)
    {
        const Token *name = TOK(c->u, NODE(c->u, p)->tok);
        if (!is_self_token(name->start, (int)name->len))
        {
            push_local(c, NODE(c->u, p)->tok);
        }
    }
    check_expr(c, body);

    c->owner = saved_owner;
    c->has_self = saved_self;
    c->nlocals = saved_locals;
}

// Depth is bounded by the parser's nesting limit, so this recursion is too.
static void check_expr(Check *c, NodeId id)
{
    if (!id)
    {
        return;
    }
    Unit *u = c->u;
    const AstNode *n = NODE(u, id);

    switch (n->kind)
    {
    case AST_IDENT:
    {
        SymId sym;
        if (find_name(c, TOK(u, n->tok), &sym) == NAME_MISSING)
        {
            report_missing(c, n->tok, 0);
        }
        break;
    }
    case AST_CALL:
        check_call(c, id);
        break;
    case AST_SELECTOR:
        if (is_self_selector(c, id))
        {
            check_self_member(c, id);
        }
        else
        {
            check_expr(c, n->a); // the member needs the type of the value: not checked yet
        }
        break;
    case AST_BLOCK:
        check_block(c, id);
        break;
    case AST_DEFINE:
        check_expr(c, n->a);
        push_local(c, n->tok); // visible after its own initializer
        break;
    case AST_FN_LIT:
        check_callable(c, n->a, n->c, c->owner, (n->flags & AST_F_SELF) ? 1 : c->has_self);
        break;
    case AST_ASSIGN:
    case AST_BIN_OP:
    case AST_INDEX:
        check_expr(c, n->a);
        check_expr(c, n->b);
        break;
    case AST_RETURN:
    case AST_UNARY_OP:
    case AST_PAREN_GROUP:
        check_expr(c, n->a);
        break;
    default: // literals and import chains: nothing to resolve here
        break;
    }
}

// The struct a top-level callable attaches to through `self: T`, or 0 for the module.
static SymId attached_owner(const Unit *u, NodeId params)
{
    if (!params || !NODE(u, params)->a)
    {
        return 0;
    }
    const Token *name = TOK(u, NODE(u, params)->tok);
    const AstNode *type = NODE(u, NODE(u, params)->a);
    if (!is_self_token(name->start, (int)name->len) || type->kind != AST_TYPE_NAME || type->aux)
    {
        return 0;
    }
    const Token *t = TOK(u, type->tok);
    SymId s = scope_find(u->scope, t->start, t->len);
    return (s && SYM(s)->kind == SYM_STRUCT) ? s : 0;
}

static void check_decl(Check *c, NodeId decl, Scope *scope, SymId enclosing)
{
    Unit *u = c->u;
    const AstNode *n = NODE(u, decl);

    switch (n->kind)
    {
    case AST_METHOD_DECL:
        check_callable(c, n->a, n->c, enclosing ? enclosing : attached_owner(u, n->a),
                       (n->flags & AST_F_SELF) != 0);
        break;
    case AST_DEFINE:
        if (NODE(u, n->a)->kind == AST_FN_LIT)
        {
            const AstNode *fn = NODE(u, n->a);
            check_callable(c, fn->a, fn->c, enclosing ? enclosing : attached_owner(u, fn->a),
                           (fn->flags & AST_F_SELF) != 0);
        }
        else
        {
            check_expr(c, n->a);
        }
        break;
    case AST_FIELD_DECL:
        check_expr(c, n->b);
        break;
    case AST_STRUCT_DECL:
    {
        const Token *name = TOK(u, n->tok);
        SymId self = scope_find(scope, name->start, name->len);
        if (self && SYM(self)->kind == SYM_STRUCT)
        {
            Scope *members = scope_at(SYM(self)->scope_ref);
            for (NodeId m = n->a; m; m = NODE(u, m)->next)
            {
                check_decl(c, m, members, self);
            }
        }
        break;
    }
    case AST_VAL_DECL:
        break;
    default:
        check_expr(c, decl); // a top-level statement such as write("...")
        break;
    }
}

void sema_check(Unit *u)
{
    // The checker state is a few hundred bytes of name tokens: it lives on the C stack.
    Check c = {.u = u, .implicit_self = domain_config_bool(u->domain, "implicit_self", 0)};

    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        c.nlocals = 0;
        check_decl(&c, d, u->scope, 0);
    }
}
