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
    uint8_t mut;     // can be assigned to: type-first parameters and locals (`T x`)
    uint8_t context; // a context parameter (`arena: arena`)
    uint8_t param;   // a parameter, not a local declared in the body
    uint8_t bound;   // bound with `name := value`: immutable, like everywhere else
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
    SymId literal;  // the function literal being checked, 0 if none
    int fn_base;    // its first local: a local below it belongs to the function around it
    uint32_t literal_name; // `name := (...) {...}`: the name the next literal is bound to
    SymId fn;              // the function whose body is being checked (its kind's facts)
    Local locals[MAX_LOCALS];
} Check;

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
        if (is_self_token(TSTART(t), (int)t->len))
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

static TypeId fn_ret_type(Unit *u, SymId fn)
{
    u = &cx.units[SYM(fn)->unit];
    NodeId decl = SYM(fn)->decl;
    NodeId callable = NODE(u, decl)->kind == AST_DEFINE ? NODE(u, decl)->a : decl;
    return resolve_type(u, fn_ret_node(u, callable), SYM(fn)->owner, 0);
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
    TypeId t = resolve_type(u, NODE(u, param)->a, owner, loud);
    if (!(NODE(u, param)->flags & AST_F_CONTEXT))
    {
        return t;
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
            if (got && got != T_NONE && got != T_EXTERN && got != T_MODULE &&
                (type_is_int(got) || got == T_BOOL || got == T_STR || type_kind(got) == T_STRUCT))
            {
                return got;
            }
        }
    }
    return T_EXTERN;
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
    c->locals[c->nlocals++] = (Local){tok, type, name, (uint8_t)mut, 0, 0, 0};
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
                SYM(c->literal)->flags |= SYM_F_CAPTURES; // a closure
            }
            *type = c->locals[i].type;
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
           : is_std_path(u, s, "assert", "debug") ? STD_ASSERT
           : is_std_path(u, s, "sizeof", "mem")   ? STD_SIZEOF
                                                  : STD_NONE;
}

static int std_call(const Unit *u, NodeId call, int member)
{
    const AstNode *n = NODE(u, call);
    return n->kind == AST_CALL && NODE(u, n->a)->kind == AST_IDENT && u->refs &&
           sema_std_member(u, u->refs[n->a]) == member;
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

int sema_is_assert(const Unit *u, NodeId call)
{
    return std_call(u, call, STD_ASSERT);
}

int sema_sizeof(const Unit *u, NodeId call, uint64_t *out)
{
    if (!std_call(u, call, STD_SIZEOF) || !NODE(u, call)->b)
    {
        return 0;
    }
    TypeId t = u->types[NODE(u, call)->b]; // what it measures (check_sizeof put it there)
    if (type_kind(t) == T_TYPE)
    {
        t = type_make(T_STRUCT, type_ref(t));
    }
    Layout l = type_layout(t);
    if (!l.size)
    {
        return 0;
    }
    *out = l.size;
    return 1;
}

// `sizeof(u32)`, `sizeof(header)`, `sizeof(value)`: the argument is a type or a value; its type
// is what is measured, in the target's layout. The result is an unsized constant.
static TypeId check_sizeof(Check *c, NodeId id, uint32_t at)
{
    Unit *u = c->u;
    NodeId arg = NODE(u, id)->b;
    if (!arg || NEXT(u, arg))
    {
        diag_at(u, at, "error", "sizeof takes one type or value: `sizeof(T)`");
        for (NodeId a = arg; a; a = NEXT(u, a))
        {
            check_expr(c, a);
        }
        return T_NONE;
    }
    const AstNode *a = NODE(u, arg);
    TypeId t = a->kind == AST_IDENT ? prim_type(TOK(u, a->tok)) : T_NONE;
    if (t == T_NONE)
    {
        t = check_expr(c, arg);
    }
    set_type(c, arg, t);
    uint64_t size;
    if (t == T_NONE)
    {
        return T_NONE;
    }
    if (!sema_sizeof(u, id, &size))
    {
        char name[64];
        diag_at(u, a->tok, "error", "sizeof: %s has no size in memory", type_str(t, name, sizeof(name)));
        return T_NONE;
    }
    return T_INT_LIT;
}

static TypeId check_assert(Check *c, NodeId id, uint32_t at)
{
    Unit *u = c->u;
    NodeId arg = NODE(u, id)->b;
    if (!arg || NEXT(u, arg))
    {
        diag_at(u, at, "error", "assert takes one condition: `assert(a == b)`");
        for (NodeId a = arg; a; a = NEXT(u, a))
        {
            check_expr(c, a);
        }
        return T_VOID;
    }
    TypeId t = check_expr(c, arg);
    if (t != T_BOOL && t != T_NONE && t != T_EXTERN)
    {
        char name[64];
        diag_at(u, NODE(u, arg)->tok, "error", "assert needs a bool, but this is %s",
                type_str(t, name, sizeof(name)));
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

    for (NodeId a = args; a; a = NEXT(u, a), index++)
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
        TypeId pt = param_type(&cx.units[SYM(fn)->unit], param, SYM(fn)->owner, 0);
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
            diag_at(u, NODE(u, a)->tok, "error",
                    "argument %d of '%.*s' is %s, but parameter '%.*s' is %s", index, nlen, name,
                    type_str(at, got, sizeof(got)), pn->len, TSTART(pn),
                    type_str(pt, want, sizeof(want)));
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

static TypeId call_function(Check *c, NodeId id, uint32_t at, SymId first)
{
    NodeId args = NODE(c->u, id)->b;
    int argc = count_list(c->u, args);
    SymId fn = select_overload(c, at, first, argc);
    if (fn && c->fn && cx.units[SYM(fn)->unit].is_manifest && !cx.units[SYM(c->fn)->unit].is_manifest)
    {
        // compile time only (docs/comptime.md §2.2): a body runs in the program
        const Token *t = TOK(c->u, at);
        diag_at(c->u, at, "error",
                "'%.*s' is declared in mod.modal, so it runs only in the compiler. To use its value here, "
                "bind it at module level: `name := %.*s(...)`",
                t->len, TSTART(t), t->len, TSTART(t));
    }
    if (fn && count_contexts(c->u, fn) && argc < arity_of(fn))
    {
        fill_contexts(c, id, fn, at);
        args = NODE(c->u, id)->b;
    }
    check_args(c, args, fn);
    c->u->refs[id] = fn;
    note_call(c, fn);
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
    if (c->owner == s)
    {
        return member; // a struct's own methods see all of it
    }
    if (SYM(member)->kind == SYM_FIELD)
    {
        diag_at(c->u, name_tok, "error",
                "'%.*s' is a field of '%.*s', and fields are never visible outside their "
                "struct. Add an accessor below it: `T %.*s(self: %.*s): self.%.*s`",
                name->len, TSTART(name), tlen, type, name->len, TSTART(name), tlen, type, name->len,
                TSTART(name));
        return 0;
    }
    if (!(SYM(member)->flags & SYM_F_EXPOSED))
    {
        diag_at(c->u, name_tok, "error",
                "'%.*s' is inside '%.*s'. To use it here, move it below the closing brace of "
                "the struct",
                name->len, TSTART(name), tlen, type);
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
        }
        else if (kind == NAME_LOCAL && type_kind(type) == T_FUNC)
        {
            return call_function(c, id, cn->tok, type_ref(type)); // f := double; f(1)
        }
        else if (kind == NAME_LOCAL && type != T_EXTERN && type != T_NONE)
        {
            diag_at(u, cn->tok, "error", "'%.*s' is not a function", t->len, TSTART(t));
        }
        set_type(c, callee, T_EXTERN);
        int member = kind == NAME_BUILTIN ? STD_NONE : sema_std_member(u, sym);
        if (member == STD_SIZEOF)
        {
            return check_sizeof(c, id, cn->tok);
        }
        if (member == STD_ASSERT)
        {
            return check_assert(c, id, cn->tok);
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
    else if (c->ret != T_VOID)
    {
        check_bounds(c, value, c->ret);
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
// Does every way through `block` end in a `return`? Without conditionals or loops there is one
// way: a `return` among its statements, or in a block among them.
static int always_returns(const Unit *u, NodeId block)
{
    const AstNode *n = NODE(u, block);
    if (n->kind == AST_RETURN)
    {
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
    // Copies, not a pointer: parsing the body may move the node array.
    uint8_t flags = NODE(u, fn)->flags;
    uint8_t kind = NODE(u, fn)->kind;
    NodeId params = NODE(u, fn)->a;
    NodeId ret_node = fn_ret_node(u, fn);
    NodeId body = fn_body(u, fn);
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

    c->owner = owner;
    c->has_self = (flags & AST_F_SELF) ? 1 : (kind == AST_FN_LIT && saved_self);
    c->in_callable = 1;
    c->ret = resolve_type(u, ret_node, owner, 1);
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

    if (flags & AST_F_ONELINE)
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
    c->fn = saved_fn;
    c->fn_base = saved_fn_base;
    c->owner = saved_owner;
    c->has_self = saved_self;
    c->in_callable = saved_in;
    c->block_base = saved_base;
    c->nlocals = saved_locals;
    c->ret = saved_ret;

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
    if (flags & AST_F_LAZY)
    {
        fn_set_body(u, fn, deferred_tok); // back to "not parsed": its tokens and nodes are gone
        NODE(u, fn)->flags = flags;
        NODE(u, NODE(u, fn)->b)->flags |= AST_F_LAZY;
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

        const Local *local = NULL;
        if (t->kind == AST_IDENT)
        {
            NameId name = token_name(TOK(u, t->tok));
            for (int i = c->nlocals - 1; i >= 0 && !local; i--)
            {
                local = c->locals[i].name == name ? &c->locals[i] : NULL;
            }
        }
        if (t->kind != AST_IDENT && t->kind != AST_SELECTOR)
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
        type = check_expr(c, n->a);
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
        if (n->b)
        {
            TypeId init = check_expr(c, n->b);
            if (!fits(init, declared))
            {
                char a[64];
                char b[64];
                diag_at(u, NODE(u, n->b)->tok, "error", "cannot initialize %s with %s",
                        type_str(declared, b, sizeof(b)), type_str(init, a, sizeof(a)));
            }
            else
            {
                check_bounds(c, n->b, declared);
            }
        }
        push_local(c, n->tok, declared, (n->flags & AST_F_MUT) != 0); // after its own value
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
            check_expr(c, s);
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
        SymId lit = symbol_new(u, SYM_METHOD, SYM_F_LITERAL, name_tok, 0, id);
        u->refs[id] = lit;
        check_callable(c, id, c->owner, lit);
        type = type_make(T_FUNC, lit);
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
int sema_pattern(Unit *u, NodeId param, SymId owner, uint64_t *value, int loud)
{
    const AstNode *n = NODE(u, param);
    if (!(n->flags & AST_F_PATTERN))
    {
        return 0;
    }
    const Token *t = TOK(u, n->tok);
    TypeId type = resolve_type(u, n->a, owner, 0);
    if (t->kind == TOK_IDENTIFIER) // true / false
    {
        if (type != T_BOOL)
        {
            if (loud)
            {
                diag_at(u, n->tok, "error", "the pattern '%.*s' is a bool, but the parameter is not",
                        t->len, TSTART(t));
            }
            return 0;
        }
        *value = t->len == 4;
        return 1;
    }
    uint64_t v = 0;
    int overflow = 0;
    if (!number_value(TSTART(t), t->len, &v, &overflow))
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
        if (overflow || (n->aux ? v > ranges[i].min : v > ranges[i].max))
        {
            if (loud)
            {
                char name[32];
                diag_at(u, n->tok, "error", "the pattern %s%.*s does not fit %s", n->aux ? "-" : "",
                        t->len, TSTART(t), type_str(type, name, sizeof(name)));
            }
            return 0;
        }
        *value = n->aux ? 0 - v : v;
        return 1;
    }
    if (loud && type != T_NONE)
    {
        diag_at(u, n->tok, "error", "a number pattern needs an integer parameter");
    }
    return 0;
}

static int clause_arity(const Unit *u, SymId fn)
{
    (void)u;
    return arity_of(fn);
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
    int n = clause_arity(u, fn);
    for (SymId s = SYM(fn)->next_overload; s; s = SYM(s)->next_overload)
    {
        if (clause_arity(u, s) == n)
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
    return param_type(u, param, SYM(fn)->owner, 0);
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
        if (NODE(u, id)->flags & AST_F_LAZY)
        {
            parse_function_body(u, fn_body(u, id));
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
            if (!u->errors)
            {
                comptime_unit(u); // its declarations are checked: the compiler computes what it must
            }
            codegen_begin(u); // everything a signature can mention is checked by now
        }
        for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
        {
            c.nlocals = 0;
            c.block_base = 0;
            check_decl(&c, d, u->scope, 0, functions);
        }
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
