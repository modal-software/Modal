// check.c (v2): names, invocations and types inside bodies.
//
// Before anything can be generated, every body is resolved and typed:
//   - a name is a local, a parameter, `self`, a module-level declaration, an import binding,
//     a member reached through implicit self (when the domain enables it), or it is an error
//   - `f(args)` must name a function; the number of arguments selects the definition (any of
//     them, for an overload set) and each argument must fit its parameter
//   - `value.name` and `value.name(args)` are resolved through the type of `value`, which is
//     also where the inside / outside rule is enforced: fields and helpers written inside a
//     struct's braces are only reachable from that struct's own methods
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
#include <string.h>

#define MAX_LOCALS 256

typedef struct
{
    uint32_t tok;
    TypeId type;
    NameId name; // locals are told apart by interned name: an integer compare (LuaJIT)
} Local;

typedef struct
{
    Unit *u;
    SymId owner;       // struct that owns the callable being checked, 0 = the module
    int has_self;      // the callable declares `self`
    int implicit_self; // domain config: a bare member name stands for self.name
    int in_callable;
    TypeId ret; // declared return type of the callable being checked
    int nlocals;
    int block_base; // first local of the innermost block
    Local locals[MAX_LOCALS];
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

static TypeId check_expr(Check *c, NodeId id);

// ---- small helpers ---------------------------------------------------------------------------

static int text_is(const Token *t, const char *s)
{
    size_t n = strlen(s);
    return t->len == n && memcmp(t->start, s, n) == 0;
}

static int is_self_node(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    return n->kind == AST_IDENT && is_self_token(t->start, (int)t->len);
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
    if (from == T_NIL && type_kind(to) == T_PTR)
    {
        return 1;
    }
    // `self` is a reference: a struct value can be returned or passed where *T is expected.
    return type_kind(to) == T_PTR && type_ref(to) == from && type_kind(from) == T_STRUCT;
}

// ---- types of declarations -------------------------------------------------------------------

// The built-in type a name spells, or T_NONE. Decided from the length and two characters,
// since this runs for every type written in the program.
static TypeId prim_type(const Token *t)
{
    const char *s = t->start;
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
        if (is_self_token(t->start, (int)t->len))
        {
            return owner ? type_make(T_PTR, type_make(T_STRUCT, owner)) : T_MODULE;
        }
        SymId s = scope_lookup_name(owner ? struct_scope(owner) : u->scope, token_name(t));
        if (s && SYM(s)->kind == SYM_STRUCT)
        {
            return type_make(T_STRUCT, s);
        }
        if (s && SYM(s)->kind == SYM_IMPORT)
        {
            return T_EXTERN;
        }
        if (loud)
        {
            diag_at(u, n->tok, "error", "unknown type '%.*s'", t->len, t->start);
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

static TypeId fn_ret_type(Unit *u, SymId fn)
{
    return resolve_type(u, callable_node(u, SYM(fn)->decl)->b, SYM(fn)->owner, 0);
}

// The parameters a caller supplies: everything after `self`.
static NodeId fn_params(const Unit *u, SymId fn)
{
    NodeId params = callable_node(u, SYM(fn)->decl)->a;
    if (params &&
        is_self_token(TOK(u, NODE(u, params)->tok)->start, (int)TOK(u, NODE(u, params)->tok)->len))
    {
        return NODE(u, params)->next;
    }
    return params;
}

// The type of a field or a module-level constant.
static TypeId value_type(Unit *u, SymId s)
{
    const AstNode *decl = NODE(u, SYM(s)->decl);
    if (decl->kind == AST_FIELD_DECL)
    {
        return resolve_type(u, decl->a, SYM(s)->owner, 0);
    }
    if (decl->kind == AST_DEFINE)
    {
        const AstNode *v = NODE(u, decl->a);
        const Token *t = TOK(u, v->tok);
        if (v->kind == AST_NUMBER_LIT)
        {
            return T_INT;
        }
        if (v->kind == AST_STRING_LIT)
        {
            return T_STR;
        }
        if (v->kind == AST_IDENT && (text_is(t, "true") || text_is(t, "false")))
        {
            return T_BOOL;
        }
    }
    return T_EXTERN;
}

// ---- names -----------------------------------------------------------------------------------

static void push_local(Check *c, uint32_t tok, TypeId type)
{
    const Token *t = TOK(c->u, tok);
    NameId name = token_name(t);
    for (int i = c->block_base; i < c->nlocals; i++)
    {
        if (c->locals[i].name == name)
        {
            diag_at(c->u, tok, "error", "'%.*s' is already declared in this block", t->len,
                    t->start);
            return;
        }
    }
    if (c->nlocals == MAX_LOCALS)
    {
        diag_at(c->u, tok, "error", "too many locals in one function (limit is %d)", MAX_LOCALS);
        return;
    }
    c->locals[c->nlocals++] = (Local){tok, type, name};
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
    if (is_self_token(t->start, (int)t->len))
    {
        *type = self_type(c);
        return c->has_self ? NAME_SELF : NAME_MISSING;
    }
    NameId name = token_name(t);
    for (int i = c->nlocals - 1; i >= 0; i--)
    {
        if (c->locals[i].name == name)
        {
            *type = c->locals[i].type;
            return NAME_LOCAL;
        }
    }
    char first = t->start[0];
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
    if (c->implicit_self && c->has_self && c->owner &&
        (*sym = scope_find_token(self_scope(c), t)))
    {
        return NAME_MEMBER;
    }
    return module_wildcard_provides(c->u, t->start, t->len) ? NAME_WILDCARD : NAME_MISSING;
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
    default:
        return T_EXTERN;
    }
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

// `print := import("std").io.print`: the one std function the compiler knows. It prints its
// arguments and returns nothing.
int sema_is_std_print(const Unit *u, SymId s)
{
    if (!s || SYM(s)->kind != SYM_IMPORT || NODE(u, SYM(s)->decl)->kind != AST_DEFINE)
    {
        return 0;
    }
    static const char *const path[] = {"print", "io"};
    NodeId id = NODE(u, SYM(s)->decl)->a;
    for (int i = 0; i < 2; i++)
    {
        const AstNode *n = NODE(u, id);
        const Token *t = TOK(u, n->tok);
        if (n->kind != AST_SELECTOR || t->len != strlen(path[i]) ||
            memcmp(t->start, path[i], t->len) != 0)
        {
            return 0;
        }
        id = n->a;
    }
    const Token *arg = TOK(u, NODE(u, id)->tok);
    return NODE(u, id)->kind == AST_IMPORT &&
           ((arg->len == 3 && memcmp(arg->start, "std", 3) == 0) ||
            (arg->len == 5 && memcmp(arg->start, "\"std\"", 5) == 0));
}

// ---- invocations -----------------------------------------------------------------------------

// Picks the definition whose parameter count matches (any one, for an overload set).
// Returns 0 after reporting when none does.
static SymId select_overload(Check *c, uint32_t at, SymId first, int argc)
{
    for (SymId s = first; s; s = SYM(s)->next_overload)
    {
        if (count_list(c->u, fn_params(c->u, s)) == argc)
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
        int arity = count_list(c->u, fn_params(c->u, s));
        if (len < sizeof(accepted) - 8)
        {
            len += (size_t)snprintf(accepted + len, sizeof(accepted) - len, "%s%d",
                                    defs ? " or " : "", arity);
        }
        defs++;
    }

    int nlen, line, col;
    const char *name = symbol_name(first, &nlen);
    source_pos(c->u->src, TOK(c->u, SYM(first)->tok)->start, &line, &col);
    diag_at(c->u, at, "error", "'%.*s' takes %s argument%s, but %d %s given (declared at %s:%d)",
            nlen, name, accepted, (defs == 1 && accepted[0] == '1' && !accepted[1]) ? "" : "s",
            argc, argc == 1 ? "is" : "are", c->u->path, line);
    return 0;
}

// Types the arguments and, when the definition is known, checks each against its parameter.
static void check_args(Check *c, NodeId args, SymId fn)
{
    Unit *u = c->u;
    NodeId param = fn ? fn_params(u, fn) : 0;
    int index = 1;

    for (NodeId a = args; a; a = NODE(u, a)->next, index++)
    {
        TypeId at = check_expr(c, a);
        if (at == T_VOID)
        {
            diag_at(u, NODE(u, a)->tok, "error", "argument %d has no value", index);
        }
        if (!param)
        {
            continue;
        }
        TypeId pt = resolve_type(c->u, NODE(u, param)->a, SYM(fn)->owner, 0);
        if (!fits(at, pt))
        {
            char got[64], want[64];
            int nlen;
            const char *name = symbol_name(fn, &nlen);
            const Token *pn = TOK(u, NODE(u, param)->tok);
            diag_at(u, NODE(u, a)->tok, "error",
                    "argument %d of '%.*s' is %s, but parameter '%.*s' is %s", index, nlen, name,
                    type_str(at, got, sizeof(got)), pn->len, pn->start,
                    type_str(pt, want, sizeof(want)));
        }
        param = NODE(u, param)->next;
    }
}

// Calls `first` (a function or an overload set) with the arguments of call node `id`.
static TypeId call_function(Check *c, NodeId id, uint32_t at, SymId first)
{
    NodeId args = NODE(c->u, id)->b;
    SymId fn = select_overload(c, at, first, count_list(c->u, args));
    check_args(c, args, fn);
    c->u->refs[id] = fn;
    return fn ? fn_ret_type(c->u, fn) : T_NONE;
}

// Finds `name` among the members of struct `s`, applying the inside / outside rule: from
// outside the struct's own methods only its face (methods written below its braces) exists.
//
// A field and its accessor may share a name (`u8 n1` and `u8 n1(self: T): self.n1`). Which
// one is meant follows from the use: `x.n1(...)` is the method, `x.n1` is the field when the
// field can be seen from here, and the accessor otherwise.
static SymId find_member(Check *c, SymId s, uint32_t name_tok, int is_call)
{
    const Token *name = TOK(c->u, name_tok);
    int tlen;
    const char *type = symbol_name(s, &tlen);
    SymId member = scope_find_token(struct_scope(s), name);

    if (member && !is_call && SYM(member)->kind == SYM_METHOD && c->owner == s)
    {
        for (SymId other = SYM(member)->next; other; other = SYM(other)->next)
        {
            int olen;
            const char *oname = symbol_name(other, &olen);
            if (SYM(other)->kind == SYM_FIELD && (uint32_t)olen == name->len &&
                memcmp(oname, name->start, name->len) == 0)
            {
                member = other;
                break;
            }
        }
    }

    if (!member)
    {
        diag_at(c->u, name_tok, "error", "'%.*s' has no member '%.*s'", tlen, type, name->len,
                name->start);
        return 0;
    }
    if (c->owner == s)
    {
        return member; // a struct's own methods see all of it
    }
    if (SYM(member)->kind == SYM_FIELD)
    {
        diag_at(c->u, name_tok, "error",
                "'%.*s' is a field of '%.*s', and fields are never visible outside their "
                "struct. Add an accessor below it: `T %.*s(self: %.*s): self.%.*s`",
                name->len, name->start, tlen, type, name->len, name->start, tlen, type, name->len,
                name->start);
        return 0;
    }
    if (!(SYM(member)->flags & SYM_F_EXPOSED))
    {
        diag_at(c->u, name_tok, "error",
                "'%.*s' is inside '%.*s'. To use it here, move it below the closing brace of "
                "the struct",
                name->len, name->start, tlen, type);
        return 0;
    }
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
                        t->len, t->start, t->len, t->start);
                check_args(c, args, 0);
                return T_NONE;
            }
            if (SYM(sym)->kind != SYM_IMPORT && value_type(c->u, sym) != T_EXTERN)
            {
                diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, t->start);
                check_args(c, args, 0);
                return T_NONE;
            }
        }
        else if (kind == NAME_LOCAL && type_kind(type) == T_FUNC)
        {
            return call_function(c, id, cn->tok, type_ref(type)); // f := double; f(1)
        }
        else if (kind == NAME_LOCAL && type != T_EXTERN && type != T_NONE)
        {
            diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, t->start);
        }
        set_type(c, callee, T_EXTERN);
        check_args(c, args, 0);
        // write(...) and std's print return nothing; anything else here is external
        return (kind == NAME_BUILTIN || sema_is_std_print(u, sym)) ? T_VOID : T_EXTERN;
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
                        name->start);
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
                        name->len, name->start);
            }
            else if (SYM(member)->kind == SYM_METHOD)
            {
                u->refs[callee] = member;
                return call_function(c, id, cn->tok, member);
            }
        }
        else if (base != T_EXTERN && base != T_NONE)
        {
            char got[64];
            diag_at(u, cn->tok, "error", "a value of type %s has no method '%.*s'",
                    type_str(base, got, sizeof(got)), name->len, name->start);
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

    int ok = logic     ? (l == T_BOOL && r == T_BOOL)
             : compare ? (fits(l, r) || fits(r, l))
                       : (type_is_int(l) && type_is_int(r) && fits(l, r));
    if (!ok)
    {
        char a[64], b[64];
        const Token *t = TOK(c->u, n->tok);
        diag_at(c->u, n->tok, "error", "'%.*s' cannot be applied to %s and %s", t->len, t->start,
                type_str(l, a, sizeof(a)), type_str(r, b, sizeof(b)));
        return T_NONE;
    }
    if (compare || logic)
    {
        return T_BOOL;
    }
    return l == T_INT_LIT ? r : l;
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
        return symbol_type(c, member);
    }
    if (base == T_MODULE)
    {
        SymId member = scope_find_token(u->scope, name);
        if (!member)
        {
            diag_at(u, n->tok, "error", "'%.*s' has no member '%.*s'", u->name_len, u->name,
                    name->len, name->start);
            return T_NONE;
        }
        u->refs[id] = member;
        return symbol_type(c, member);
    }
    if (base != T_EXTERN && base != T_NONE)
    {
        char got[64];
        diag_at(u, n->tok, "error", "a value of type %s has no member '%.*s'",
                type_str(base, got, sizeof(got)), name->len, name->start);
        return T_NONE;
    }
    return base;
}

static void check_return(Check *c, NodeId id, NodeId value)
{
    TypeId got = value ? check_expr(c, value) : T_VOID;
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
        diag_at(c->u, NODE(c->u, id)->tok, "error", "returns %s, but the function declares %s",
                type_str(got, a, sizeof(a)), type_str(c->ret, b, sizeof(b)));
    }
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
static void check_callable(Check *c, NodeId fn, SymId owner, SymId sym)
{
    Unit *u = c->u;
    // Copies, not a pointer: parsing the body may move the node array.
    uint8_t flags = NODE(u, fn)->flags;
    uint8_t kind = NODE(u, fn)->kind;
    NodeId params = NODE(u, fn)->a;
    NodeId ret_node = NODE(u, fn)->b;
    NodeId body = NODE(u, fn)->c;
    uint32_t deferred_tok = body;
    size_t mark = u->nodes.size;
    size_t tok_mark = u->tokens.size;

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
            return;
        }
        NODE(u, fn)->c = body;
        NODE(u, fn)->flags = (uint8_t)(flags & ~AST_F_LAZY);
        side_tables(u, (NodeId)mark);
        module_body_nodes(u, (NodeId)mark);
    }

    // Only the scalars are saved: the locals stay where they are, which is also what lets a
    // nested function literal see the enclosing ones.
    SymId saved_owner = c->owner;
    int saved_self = c->has_self, saved_in = c->in_callable, saved_base = c->block_base;
    int saved_locals = c->nlocals;
    TypeId saved_ret = c->ret;

    c->owner = owner;
    c->has_self = (flags & AST_F_SELF) ? 1 : (kind == AST_FN_LIT && saved_self);
    c->in_callable = 1;
    c->ret = resolve_type(u, ret_node, owner, 1);
    c->block_base = c->nlocals;

    for (NodeId p = params; p; p = NODE(u, p)->next)
    {
        const Token *name = TOK(u, NODE(u, p)->tok);
        if (!is_self_token(name->start, (int)name->len))
        {
            push_local(c, NODE(u, p)->tok, resolve_type(u, NODE(u, p)->a, owner, 1));
        }
    }

    if (flags & AST_F_ONELINE)
    {
        check_return(c, body, body); // `T name(params): expr` returns the expression
    }
    else
    {
        check_expr(c, body);
    }

    c->owner = saved_owner;
    c->has_self = saved_self;
    c->in_callable = saved_in;
    c->block_base = saved_base;
    c->nlocals = saved_locals;
    c->ret = saved_ret;

    if (sym)
    {
        codegen_function(u, sym);
    }
    if (flags & AST_F_LAZY)
    {
        NODE(u, fn)->c = deferred_tok; // back to "not parsed": its tokens and nodes are gone
        NODE(u, fn)->flags = flags;
        u->nodes.size = mark;
        u->tokens.size = tok_mark;
    }
}

// Depth is bounded by the parser's nesting limit, so this recursion is too.
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
    case AST_STRING_LIT:
    {
        const Token *t = TOK(u, n->tok);
        for (uint32_t i = 1; i + 1 < t->len; i++) // the escapes a string may contain
        {
            if (t->start[i] == '\\')
            {
                char esc = t->start[++i];
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
        NameKind kind = find_name(c, TOK(u, n->tok), &sym, &type);
        cx.names_resolved++;
        if (kind == NAME_MISSING)
        {
            report_missing(c, n->tok, 0);
            type = T_NONE;
        }
        else if (kind == NAME_SYMBOL || kind == NAME_MEMBER)
        {
            u->refs[id] = sym;
            type = symbol_type(c, sym);
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
    case AST_INDEX:
        check_expr(c, n->a);
        check_expr(c, n->b);
        type = T_EXTERN;
        break;
    case AST_ASSIGN:
    {
        TypeId target = check_expr(c, n->a);
        TypeId value = check_expr(c, n->b);
        const AstNode *t = NODE(u, n->a);
        SymId field = t->kind == AST_SELECTOR ? u->refs[n->a] : 0;

        if (t->kind != AST_IDENT && t->kind != AST_SELECTOR)
        {
            diag_at(u, n->tok, "error", "only a local or a field can be assigned to");
        }
        else if (field && SYM(field)->kind == SYM_FIELD && !(SYM(field)->flags & SYM_F_MUT))
        {
            const Token *name = TOK(u, t->tok);
            diag_at(u, n->tok, "error",
                    "'%.*s' is immutable (declared `%.*s: T`). Declare it type-first, "
                    "`T %.*s`, to assign to it",
                    name->len, name->start, name->len, name->start, name->len, name->start);
        }
        else if (!fits(value, target))
        {
            char a[64], b[64];
            diag_at(u, n->tok, "error", "cannot assign %s to %s", type_str(value, a, sizeof(a)),
                    type_str(target, b, sizeof(b)));
        }
        type = T_VOID;
        break;
    }
    case AST_DEFINE:
    {
        const Token *name = TOK(u, n->tok);
        type = check_expr(c, n->a);
        if (type == T_VOID || type == T_NIL)
        {
            diag_at(u, n->tok, "error", "'%.*s' cannot be bound: the expression has no value",
                    name->len, name->start);
            type = T_NONE;
        }
        else if (type == T_INT_LIT)
        {
            type = T_INT; // an integer literal on its own is an int
        }
        push_local(c, n->tok, type); // visible after its own initializer
        break;
    }
    case AST_RETURN:
        check_return(c, id, n->a);
        break;
    case AST_BLOCK:
    {
        int saved_count = c->nlocals, saved_base = c->block_base;
        c->block_base = c->nlocals;
        for (NodeId s = n->a; s; s = NODE(u, s)->next)
        {
            check_expr(c, s);
        }
        c->nlocals = saved_count; // a block's locals end with the block
        c->block_base = saved_base;
        break;
    }
    case AST_FN_LIT:
        check_callable(c, id, c->owner, 0);
        type = T_EXTERN; // a function value: callable, not otherwise typed yet
        break;
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
    if (!is_self_token(name->start, (int)name->len) || type->kind != AST_TYPE_NAME || type->aux)
    {
        return 0;
    }
    const Token *t = TOK(u, type->tok);
    SymId s = scope_find_token(u->scope, t);
    return (s && SYM(s)->kind == SYM_STRUCT) ? s : 0;
}

static void check_field(Check *c, NodeId decl, SymId owner)
{
    const AstNode *n = NODE(c->u, decl);
    TypeId declared = resolve_type(c->u, n->a, owner, 1);
    if (n->b)
    {
        TypeId init = check_expr(c, n->b);
        if (!fits(init, declared))
        {
            char a[64], b[64];
            diag_at(c->u, NODE(c->u, n->b)->tok, "error", "cannot initialize %s with %s",
                    type_str(declared, b, sizeof(b)), type_str(init, a, sizeof(a)));
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
            if ((uint32_t)len == name->len && memcmp(other, name->start, name->len) == 0)
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
            for (NodeId m = a; m; m = NODE(u, m)->next)
            {
                check_decl(c, m, struct_scope(self), self, functions);
            }
        }
        break;
    }
    case AST_VAL_DECL:
        break;
    default:
        if (!functions)
        {
            check_expr(c, decl); // a top-level statement such as write("...")
        }
        break;
    }
}

TypeId sema_fn_ret(Unit *u, SymId fn)
{
    return fn_ret_type(u, fn);
}

TypeId sema_param_type(Unit *u, SymId fn, NodeId param)
{
    return resolve_type(u, NODE(u, param)->a, SYM(fn)->owner, 0);
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
        if (NODE(u, id)->flags & AST_F_LAZY)
        {
            parse_function_body(u, NODE(u, id)->c);
            u->nodes.size = decls;
            u->tokens.size = toks;
        }
    }
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
            codegen_begin(u); // everything a signature can mention is checked by now
        }
        for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, d, u->scope, 0, functions);
        }
    }
}
