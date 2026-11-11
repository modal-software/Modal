// backend/c/emit.c (v2): the C backend.
//
// Translates one checked Modal file into one self-contained C99 file.
//
//   name: struct { ... }            typedef struct modal_name { ... } modal_name;
//   f(a: u8): u8 { ... }            static uint8_t modal_f(uint8_t v_a) { ... }
//   m(self: T, a: u8): u8 { ... }   static uint8_t modal_T_m(modal_T *self, uint8_t v_a) { ... }
//   u8 f(a: u8) / u8 f(a, b: u8)    modal_f__1 / modal_f__2      (overloads: one C function each)
//   x := expr                       <type of expr> v_x = expr;
//   value.m(a)                      modal_T_m(&value, a)
//   T.m(a)                          modal_T_m(&(modal_T){0}, a)   (a constructor: fresh zero value)
//   self.field                      self->field
//   main(): u8                      int main(void) { return (int)modal_main(); }
//
// Everything it needs was decided by the front-end: unit->types gives the type of every
// expression and unit->refs the declaration behind every name and call. What the backend
// cannot translate yet is reported as an error at the construct, never silently dropped.
//
// Output is streamed: types, globals and prototypes first (they need only declarations),
// then each function as the checker hands it over, then the entry point.
#include "v2/compiler/backend/backend.h"
#include <string.h>

typedef struct
{
    Unit *u;
    FILE *out;
    int errors;
    int indent;
    size_t len; // bytes waiting in buf
    char buf[1 << 16];
} Emit;

static void emit_expr(Emit *e, NodeId id);

// ---- output helpers --------------------------------------------------------------------------

// Output goes through one 64 KB buffer and is written in large pieces. Almost everything
// emitted is a fixed string or a slice of the source, so it is copied straight in: nothing
// on this path goes through printf.
static void flush(Emit *e)
{
    fwrite(e->buf, 1, e->len, e->out);
    e->len = 0;
}

static inline void put_n(Emit *e, const char *text, size_t n)
{
    if (e->len + n > sizeof(e->buf))
    {
        flush(e);
        if (n > sizeof(e->buf))
        {
            fwrite(text, 1, n, e->out);
            return;
        }
    }
    char *to = e->buf + e->len;
    e->len += n;
    if (n <= 16) // most pieces are a few bytes: cheaper than a call into memcpy
    {
        while (n--)
        {
            *to++ = *text++;
        }
        return;
    }
    memcpy(to, text, n);
}

static void put(Emit *e, const char *text)
{
    put_n(e, text, strlen(text));
}

// A string literal: its length is known at compile time.
#define PUT(e, literal) put_n((e), literal, sizeof(literal) - 1)

static inline void put_c(Emit *e, char ch)
{
    if (e->len == sizeof(e->buf))
    {
        flush(e);
    }
    e->buf[e->len++] = ch;
}

static void out_indent(Emit *e)
{
    static const char spaces[] = "                                                                ";
    size_t n = (size_t)e->indent * 4;
    put_n(e, spaces, n < sizeof(spaces) - 1 ? n : sizeof(spaces) - 1);
}

static void out_tok(Emit *e, uint32_t tok)
{
    const Token *t = TOK(e->u, tok);
    put_n(e, t->start, t->len);
}

static void unsupported(Emit *e, uint32_t tok, const char *what)
{
    diag_at(e->u, tok, "error", "backend c: %s is not supported yet", what);
    e->errors++;
}

// ---- names and types -------------------------------------------------------------------------

static void out_sym_name(Emit *e, SymId s)
{
    int len;
    const char *name = symbol_name(s, &len);
    put_n(e, name, (size_t)len);
}

// modal_<struct>_<name>, with __<parameter count> for the overloadable (type-first) form.
static void out_fn_name(Emit *e, SymId fn)
{
    PUT(e, "modal_");
    if (SYM(fn)->owner)
    {
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, "_");
    }
    out_sym_name(e, fn);
    if (SYM(fn)->flags & SYM_F_MUT)
    {
        int arity = 0;
        for (NodeId p = sema_fn_params(e->u, fn); p; p = NODE(e->u, p)->next)
        {
            arity++;
        }
        char digits[12];
        int n = 0;
        do
        {
            digits[n++] = (char)('0' + arity % 10);
            arity /= 10;
        } while (arity);
        PUT(e, "__");
        while (n)
        {
            put_c(e, digits[--n]);
        }
    }
}

static void out_type(Emit *e, TypeId t, uint32_t at)
{
    static const char *const prims[T__PRIM_COUNT] = {
        [T_VOID] = "void",     [T_BOOL] = "bool",    [T_U8] = "uint8_t", [T_U16] = "uint16_t",
        [T_U32] = "uint32_t",  [T_U64] = "uint64_t", [T_I8] = "int8_t",  [T_I16] = "int16_t",
        [T_I32] = "int32_t",   [T_I64] = "int64_t",  [T_INT] = "int",    [T_INT_LIT] = "int",
        [T_STR] = "modal_str",
    };
    TypeKind k = type_kind(t);

    if (k < T__PRIM_COUNT && prims[k])
    {
        put(e, prims[k]);
    }
    else if (k == T_STRUCT)
    {
        PUT(e, "modal_");
        out_sym_name(e, type_ref(t));
    }
    else if (k == T_PTR)
    {
        out_type(e, type_ref(t), at);
        PUT(e, " *");
    }
    else
    {
        char name[64];
        char what[96];
        snprintf(what, sizeof(what), "a value of type %s", type_str(t, name, sizeof(name)));
        unsupported(e, at, what);
        PUT(e, "int");
    }
}

static int is_self_ident(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    return n->kind == AST_IDENT && is_self_token(t->start, (int)t->len);
}

// ---- expressions -----------------------------------------------------------------------------

// Emits `id` where a value of type `want` is expected. `self` is a reference, so a struct
// value may stand where *T is wanted: its address is taken.
static void emit_as(Emit *e, NodeId id, TypeId want)
{
    TypeId have = e->u->types[id];
    if (type_kind(want) == T_PTR && type_kind(have) == T_STRUCT)
    {
        if (is_self_ident(e->u, id))
        {
            PUT(e, "self");
            return;
        }
        PUT(e, "(&");
        emit_expr(e, id);
        PUT(e, ")");
        return;
    }
    emit_expr(e, id);
}

// write(a, b) / print(a, b): one runtime call per argument, chosen by its type. write emits
// its arguments as they are; print puts a space between them and a newline after.
static void emit_output(Emit *e, NodeId args, int newline)
{
    PUT(e, "(");
    for (NodeId a = args; a; a = NODE(e->u, a)->next)
    {
        TypeId t = e->u->types[a];
        if (newline && a != args)
        {
            PUT(e, "modal_write_str(MODAL_STR(\" \")), "); // print separates its arguments
        }
        if (t == T_STR)
        {
            PUT(e, "modal_write_str(");
        }
        else if (t == T_BOOL)
        {
            PUT(e, "modal_write_bool(");
        }
        else if (t == T_U64)
        {
            PUT(e, "modal_write_uint(");
        }
        else if (type_is_int(t))
        {
            PUT(e, "modal_write_int((long long)");
        }
        else
        {
            unsupported(e, NODE(e->u, a)->tok, "printing a value of this type");
            PUT(e, "modal_write_int((long long)");
        }
        emit_expr(e, a);
        PUT(e, "), ");
    }
    put(e, newline ? "modal_write_str(MODAL_STR(\"\\n\")))" : "(void)0)");
}

static void emit_call(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    const AstNode *callee = NODE(u, n->a);
    SymId fn = u->refs[id];

    if (callee->kind == AST_IDENT && TOK(u, callee->tok)->kind == TOK_WRITE)
    {
        emit_output(e, n->b, 0);
        return;
    }
    if (callee->kind == AST_IDENT && sema_is_std_print(u, u->refs[n->a]))
    {
        emit_output(e, n->b, 1);
        return;
    }
    if (!fn)
    {
        unsupported(e, callee->tok, "calling something that is not a function of this file");
        PUT(e, "0");
        return;
    }

    out_fn_name(e, fn);
    PUT(e, "(");
    int first = 1;

    if (SYM(fn)->owner) // a method: the receiver is the first C argument
    {
        if (callee->kind == AST_SELECTOR)
        {
            TypeId base = u->types[callee->a];
            if (type_kind(base) == T_TYPE) // T.m(...): a fresh zero value
            {
                PUT(e, "&(modal_");
                out_sym_name(e, type_ref(base));
                PUT(e, "){0}");
            }
            else
            {
                emit_as(e, callee->a, type_make(T_PTR, type_make(T_STRUCT, SYM(fn)->owner)));
            }
        }
        else
        {
            PUT(e, "self"); // a bare member name, through implicit self
        }
        first = 0;
    }

    NodeId param = sema_fn_params(u, fn);
    for (NodeId a = n->b; a && param; a = NODE(u, a)->next, param = NODE(u, param)->next)
    {
        put(e, first ? "" : ", ");
        emit_as(e, a, sema_param_type(u, fn, param));
        first = 0;
    }
    PUT(e, ")");
}

static void emit_expr(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    const Token *t = TOK(u, n->tok);
    TypeId type = u->types[id];

    switch (n->kind)
    {
    case AST_NUMBER_LIT:
        if (memchr(t->start, '.', t->len))
        {
            unsupported(e, n->tok, "a fractional number");
        }
        out_tok(e, n->tok);
        break;
    case AST_STRING_LIT:
        PUT(e, "MODAL_STR(");
        for (uint32_t i = 0; i < t->len; i++) // a Modal string may span lines; a C one may not
        {
            char ch = t->start[i];
            if (ch == '\n')
            {
                PUT(e, "\\n");
            }
            else if (ch == '\r')
            {
                PUT(e, "\\r");
            }
            else
            {
                put_c(e, ch);
            }
        }
        PUT(e, ")");
        break;
    case AST_IDENT:
    {
        SymId s = u->refs[id];
        if (is_self_ident(u, id))
        {
            PUT(e, "(*self)");
        }
        else if (s && SYM(s)->kind == SYM_FIELD)
        {
            PUT(e, "self->"); // a bare member name, through implicit self
            out_tok(e, n->tok);
        }
        else if (s && SYM(s)->kind == SYM_CONST)
        {
            PUT(e, "g_");
            out_tok(e, n->tok);
        }
        else if (s)
        {
            unsupported(e, n->tok, "using a function, a type or an import as a value");
            PUT(e, "0");
        }
        else if (type == T_BOOL && (t->len == 4 || t->len == 5) &&
                 (!memcmp(t->start, "true", 4) || !memcmp(t->start, "false", 5)))
        {
            out_tok(e, n->tok);
        }
        else if (type == T_NIL)
        {
            PUT(e, "NULL");
        }
        else
        {
            PUT(e, "v_");
            out_tok(e, n->tok);
        }
        break;
    }
    case AST_PAREN_GROUP:
        PUT(e, "(");
        emit_expr(e, n->a);
        PUT(e, ")");
        break;
    case AST_UNARY_OP:
        if (t->kind == TOK_AMP && is_self_ident(u, n->a))
        {
            PUT(e, "self"); // &self: the reference itself
            break;
        }
        put_c(e, '(');
        put_c(e, *t->start);
        emit_expr(e, n->a);
        PUT(e, ")");
        break;
    case AST_BIN_OP:
    {
        const char *op = t->kind == TOK_AND ? "&&" : t->kind == TOK_OR ? "||" : NULL;
        if (u->types[n->a] == T_STR || u->types[n->b] == T_STR)
        {
            unsupported(e, n->tok, "an operator on strings");
        }
        // Arithmetic is done in the Modal type: u8 wraps at 256 at every step, as declared.
        if (type >= T_U8 && type <= T_INT)
        {
            PUT(e, "((");
            out_type(e, type, n->tok);
            PUT(e, ")");
        }
        else
        {
            PUT(e, "(");
        }
        PUT(e, "(");
        emit_expr(e, n->a);
        put_c(e, ' ');
        if (op)
        {
            put(e, op);
        }
        else
        {
            put_n(e, t->start, t->len);
        }
        put_c(e, ' ');
        emit_expr(e, n->b);
        PUT(e, "))");
        break;
    }
    case AST_SELECTOR:
    {
        SymId member = u->refs[id];
        if (!member || SYM(member)->kind != SYM_FIELD)
        {
            unsupported(e, n->tok, "using a method as a value");
            PUT(e, "0");
            break;
        }
        if (is_self_ident(u, n->a))
        {
            PUT(e, "self->");
        }
        else
        {
            PUT(e, "(");
            emit_expr(e, n->a);
            put(e, type_kind(u->types[n->a]) == T_PTR ? ")->" : ").");
        }
        out_tok(e, n->tok);
        break;
    }
    case AST_CALL:
        emit_call(e, id);
        break;
    case AST_ASSIGN:
        emit_expr(e, n->a);
        PUT(e, " = ");
        emit_as(e, n->b, u->types[n->a]);
        break;
    case AST_FN_LIT:
        unsupported(e, n->tok, "a function literal inside another function");
        PUT(e, "0");
        break;
    case AST_INDEX:
        unsupported(e, n->tok, "indexing");
        PUT(e, "0");
        break;
    default:
        unsupported(e, n->tok, "this expression");
        PUT(e, "0");
        break;
    }
}

// ---- statements ------------------------------------------------------------------------------

static void emit_stmt(Emit *e, NodeId id, TypeId ret)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);

    switch (n->kind)
    {
    case AST_BLOCK:
        out_indent(e);
        PUT(e, "{\n");
        e->indent++;
        for (NodeId s = n->a; s; s = NODE(u, s)->next)
        {
            emit_stmt(e, s, ret);
        }
        e->indent--;
        out_indent(e);
        PUT(e, "}\n");
        return;
    case AST_DEFINE:
        out_indent(e);
        if (NODE(u, n->a)->kind == AST_FN_LIT)
        {
            unsupported(e, n->tok, "a function literal inside another function");
            PUT(e, ";\n");
            return;
        }
        out_type(e, u->types[id], n->tok);
        PUT(e, " v_");
        out_tok(e, n->tok);
        PUT(e, " = ");
        emit_as(e, n->a, u->types[id]);
        PUT(e, ";\n");
        return;
    case AST_RETURN:
        out_indent(e);
        PUT(e, "return");
        if (n->a)
        {
            PUT(e, " ");
            emit_as(e, n->a, ret);
        }
        PUT(e, ";\n");
        return;
    default:
        out_indent(e);
        emit_expr(e, id);
        PUT(e, ";\n");
        return;
    }
}

// ---- declarations ----------------------------------------------------------------------------

static const AstNode *fn_node(const Unit *u, SymId fn)
{
    const AstNode *n = NODE(u, SYM(fn)->decl);
    return n->kind == AST_DEFINE ? NODE(u, n->a) : n;
}

// `static <ret> modal_name(<params>)`
static void emit_signature(Emit *e, SymId fn)
{
    Unit *u = e->u;
    int first = 1;

    PUT(e, "static ");
    out_type(e, sema_fn_ret(u, fn), SYM(fn)->tok);
    PUT(e, " ");
    out_fn_name(e, fn);
    PUT(e, "(");
    if (SYM(fn)->owner)
    {
        PUT(e, "modal_");
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, " *self");
        first = 0;
    }
    for (NodeId p = sema_fn_params(u, fn); p; p = NODE(u, p)->next)
    {
        put(e, first ? "" : ", ");
        out_type(e, sema_param_type(u, fn, p), NODE(u, p)->tok);
        PUT(e, " v_");
        out_tok(e, NODE(u, p)->tok);
        first = 0;
    }
    put(e, first ? "void)" : ")");
}

static void emit_function(Emit *e, SymId fn)
{
    Unit *u = e->u;
    const AstNode *n = fn_node(u, fn);
    TypeId ret = sema_fn_ret(u, fn);

    emit_signature(e, fn);
    PUT(e, "\n{\n");
    e->indent = 1;
    if (SYM(fn)->owner)
    {
        PUT(e, "    (void)self;\n");
    }
    if (n->flags & AST_F_ONELINE)
    {
        out_indent(e);
        PUT(e, "return ");
        emit_as(e, n->c, ret);
        PUT(e, ";\n");
    }
    else
    {
        for (NodeId s = NODE(u, n->c)->a; s; s = NODE(u, s)->next)
        {
            emit_stmt(e, s, ret);
        }
    }
    e->indent = 0;
    PUT(e, "}\n\n");
}

// Is this a function the backend can emit? Module-level `self` functions are not, yet.
static int emittable(Emit *e, SymId fn, int report)
{
    const AstNode *n = fn_node(e->u, fn);
    if (!SYM(fn)->owner && (n->flags & AST_F_SELF))
    {
        if (report)
        {
            unsupported(e, SYM(fn)->tok, "a module-level function that takes `self`");
        }
        return 0;
    }
    return 1;
}

static void emit_struct(Emit *e, NodeId decl)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, decl);
    const Token *name = TOK(u, n->tok);
    SymId self = scope_find_token(u->scope, name);
    int fields = 0;

    PUT(e, "struct modal_");
    put_n(e, name->start, name->len);
    PUT(e, "\n{\n");
    for (NodeId m = n->a; m; m = NODE(u, m)->next)
    {
        const AstNode *member = NODE(u, m);
        if (member->kind == AST_STRUCT_DECL)
        {
            unsupported(e, member->tok, "a struct declared inside another struct");
        }
        if (member->kind != AST_FIELD_DECL)
        {
            continue;
        }
        const Token *field = TOK(u, member->tok);
        SymId s = scope_find(scope_at(SYM(self)->scope_ref), field->start, field->len);
        while (s && SYM(s)->kind != SYM_FIELD)
        {
            s = SYM(s)->next; // skip an accessor that shares the field's name
        }
        PUT(e, "    ");
        out_type(e, s ? sema_value_type(u, s) : T_NONE, member->tok);
        put_c(e, ' ');
        put_n(e, field->start, field->len);
        PUT(e, ";\n");
        fields++;
    }
    if (!fields)
    {
        PUT(e, "    char modal_empty;\n"); // C has no empty structs
    }
    PUT(e, "};\n\n");
}

static const char *const prelude =
    "// Generated by the Modal compiler (backend: c). Do not edit.\n"
    "#include <stdbool.h>\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "#include <stdio.h>\n"
    "\n"
    "typedef struct\n"
    "{\n"
    "    const uint8_t *ptr;\n"
    "    size_t len;\n"
    "} modal_str;\n"
    "\n"
    "#define MODAL_STR(s) ((modal_str){(const uint8_t *)(s), sizeof(s) - 1})\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#define MODAL_RT static inline __attribute__((unused))\n"
    "#else\n"
    "#define MODAL_RT static inline\n"
    "#endif\n"
    "\n"
    "MODAL_RT void modal_write_str(modal_str s) { fwrite(s.ptr, 1, s.len, stdout); }\n"
    "MODAL_RT void modal_write_int(long long n) { printf(\"%lld\", n); }\n"
    "MODAL_RT void modal_write_uint(unsigned long long n) { printf(\"%llu\", n); }\n"
    "MODAL_RT void modal_write_bool(bool b) { fputs(b ? \"true\" : \"false\", stdout); }\n"
    "\n";

// One Emit for the whole job: it holds the output buffer between the three calls.
static Emit emit_state;

static void backend_c_begin(const BackendJob *job)
{
    Unit *u = job->entry;
    emit_state = (Emit){.u = u, .out = job->out};
    Emit *e = &emit_state;

    // This backend translates one file. An import of another Modal file needs the linker's
    // module graph on the C side as well, which is the next step.
    for (NodeId id = 1; id < u->nodes.size; id++)
    {
        if (NODE(u, id)->kind == AST_IMPORT && NODE(u, id)->aux)
        {
            unsupported(e, NODE(u, id)->tok, "importing another Modal file");
        }
    }

    put(e, prelude);

    // 1. Types: every struct is declared before any is defined, so they can refer to each
    //    other through pointers in any order.
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            const Token *name = TOK(u, NODE(u, d)->tok);
            PUT(e, "typedef struct modal_");
            put_n(e, name->start, name->len);
            PUT(e, " modal_");
            put_n(e, name->start, name->len);
            PUT(e, ";\n");
        }
    }
    PUT(e, "\n");
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            emit_struct(e, d);
        }
    }

    // 2. Module-level values.
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        const AstNode *n = NODE(u, d);
        const Token *name = TOK(u, n->tok);
        int is_const =
            n->kind == AST_DEFINE && NODE(u, n->a)->kind != AST_FN_LIT && !ast_import_root(u, n->a);
        if (n->kind != AST_FIELD_DECL && !is_const)
        {
            continue;
        }
        SymId s = scope_find_token(u->scope, name);
        NodeId init = n->kind == AST_FIELD_DECL ? n->b : n->a;
        TypeId type = s ? sema_value_type(u, s) : T_NONE;

        put(e, (n->kind == AST_DEFINE || !(n->flags & AST_F_MUT)) ? "static const " : "static ");
        out_type(e, type, n->tok);
        PUT(e, " g_");
        put_n(e, name->start, name->len);
        if (init)
        {
            PUT(e, " = ");
            emit_expr(e, init);
        }
        PUT(e, ";\n");
    }
    PUT(e, "\n");

    // 3. A prototype for every function, so the bodies that follow can call each other in
    //    any order. Signatures need only the declarations.
    for (SymId fn = 1; fn < cx.syms.size; fn++)
    {
        if (SYM(fn)->unit == u->id && SYM(fn)->kind == SYM_METHOD && emittable(e, fn, 1))
        {
            emit_signature(e, fn);
            PUT(e, ";\n");
        }
    }
    PUT(e, "\n");
}

// Called once per function, right after it is checked, while its body is still in memory.
static void backend_c_function(const BackendJob *job, SymId fn)
{
    (void)job;
    if (emittable(&emit_state, fn, 0))
    {
        emit_function(&emit_state, fn);
    }
}

static int backend_c_end(const BackendJob *job)
{
    Unit *u = job->entry;
    Emit *e = &emit_state;
    SymId entry = scope_find(u->scope, "main", 4);

    if (!entry || SYM(entry)->kind != SYM_METHOD || sema_fn_params(u, entry) ||
        SYM(entry)->next_overload)
    {
        diag_at(u, entry ? SYM(entry)->tok : 0, "error",
                "backend c: the program needs one `main()` without parameters to start from");
        e->errors++;
    }
    else if (sema_fn_ret(u, entry) == T_VOID)
    {
        PUT(e, "int main(void)\n{\n    ");
        out_fn_name(e, entry);
        PUT(e, "();\n    return 0;\n}\n");
    }
    else
    {
        PUT(e, "int main(void)\n{\n    return (int)");
        out_fn_name(e, entry);
        PUT(e, "();\n}\n");
    }

    flush(e);
    return e->errors;
}

void backend_c_init(Backend *b)
{
    b->summary = "portable C99 source, one file per program";
    b->extension = ".c";
    b->toolchain = "cc";
    b->begin = backend_c_begin;
    b->function = backend_c_function;
    b->end = backend_c_end;
}
