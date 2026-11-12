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
#include <stdio.h>
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
    put_n(e, TSTART(t), t->len);
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
    if (SYM(fn)->flags & SYM_F_LITERAL) // anonymous: named by its symbol
    {
        char name[32];
        snprintf(name, sizeof(name), "m__lit%u", (unsigned)fn);
        put(e, name);
        return;
    }
    PUT(e, "m_");
    if (SYM(fn)->owner)
    {
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, "_");
    }
    out_sym_name(e, fn);
    if (SYM(fn)->flags & SYM_F_MUT)
    {
        int arity = 0;
        for (NodeId p = sema_fn_params(e->u, fn); p; p = NEXT(e->u, p))
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
        // a later clause of the same arity: _2, _3, ... (calls go to the first)
        int clause = sema_clause_index(e->u, fn);
        if (clause > 0)
        {
            put_c(e, '_');
            clause++;
            n = 0;
            do
            {
                digits[n++] = (char)('0' + clause % 10);
                clause /= 10;
            } while (clause);
            while (n)
            {
                put_c(e, digits[--n]);
            }
        }
    }
}

// A parameter's C name: v_<name>, or v__<index> for a pattern, which has no name.
static void out_param_name(Emit *e, NodeId param, int index)
{
    if (NODE(e->u, param)->flags & AST_F_PATTERN)
    {
        PUT(e, "v__");
        put_c(e, (char)('0' + index / 10 % 10));
        put_c(e, (char)('0' + index % 10));
        return;
    }
    PUT(e, "v_");
    out_tok(e, NODE(e->u, param)->tok);
}

static void out_type(Emit *e, TypeId t, uint32_t at)
{
    static const char *const prims[T__PRIM_COUNT] = {
        [T_VOID] = "void",    [T_BOOL] = "bool",    [T_U8] = "uint8_t", [T_U16] = "uint16_t",
        [T_U32] = "uint32_t", [T_U64] = "uint64_t", [T_I8] = "int8_t",  [T_I16] = "int16_t",
        [T_I32] = "int32_t",  [T_I64] = "int64_t",  [T_INT] = "int",    [T_INT_LIT] = "int",
        [T_STR] = "m_str",
    };
    TypeKind k = type_kind(t);

    if (k < T__PRIM_COUNT && prims[k])
    {
        put(e, prims[k]);
    }
    else if (k == T_STRUCT)
    {
        PUT(e, "m_");
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
    return n->kind == AST_IDENT && is_self_token(TSTART(t), (int)t->len);
}

// ---- expressions -----------------------------------------------------------------------------

static int literal_above_i64(const Unit *u, NodeId id);

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
        const AstNode *n = NODE(e->u, id);
        while (n->kind == AST_PAREN_GROUP)
        {
            n = NODE(e->u, n->a);
        }
        if (n->kind == AST_CALL) // a struct a call returned: kept in a one-element array
        {
            PUT(e, "((");
            out_type(e, have, NODE(e->u, id)->tok);
            PUT(e, "[1]){");
            emit_expr(e, id);
            PUT(e, "})");
            return;
        }
        PUT(e, "(&");
        emit_expr(e, id);
        PUT(e, ")");
        return;
    }
    emit_expr(e, id);
}

// An integer literal too large for a long long is C's unsigned long long: printed unsigned.
// An unsized constant (`n := 5`) is written as the literal of its value, so the same holds.
static int literal_above_i64(const Unit *u, NodeId id)
{
    const AstNode *n = NODE(u, id);
    while (n->kind == AST_PAREN_GROUP)
    {
        id = n->a;
        n = NODE(u, n->a);
    }
    uint64_t bits;
    int neg;
    if ((n->kind == AST_IDENT || n->kind == AST_SELECTOR) && sema_unsized_value(u, id, &bits, &neg))
    {
        return !neg && bits > 0x7fffffffffffffffull;
    }
    if (n->kind != AST_NUMBER_LIT)
    {
        return 0;
    }
    const Token *t = TOK(u, n->tok);
    uint64_t v = 0;
    return number_value(TSTART(t), t->len, &v, NULL) && v > 0x7fffffffffffffffull;
}

// The adapter that lets the sink helpers call `put` (one per put method, see backend_c_begin).
static void out_sink_name(Emit *e, SymId method)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "m_sink_%u", (unsigned)method);
    put(e, buf);
}

// write(a, b) / print(a, b): one runtime call per argument, chosen by its type. write emits
// its arguments as they are; print puts a space between them and a newline after. With a
// `put` (AST_F_SINK), the first argument is the `out` every byte goes to.
static void emit_output(Emit *e, NodeId args, int newline, SymId sink)
{
    NodeId out = 0;
    if (sink)
    {
        out = args;
        args = NEXT(e->u, args);
    }
#define OPEN(fn)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (sink)                                                                                  \
        {                                                                                          \
            PUT(e, "m_sink_" fn "(");                                                              \
            emit_as(e, out, type_make(T_PTR, type_make(T_STRUCT, SYM(sink)->owner)));             \
            PUT(e, ", ");                                                                          \
            out_sink_name(e, sink);                                                                \
            PUT(e, ", ");                                                                          \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            PUT(e, "m_write_" fn "(");                                                             \
        }                                                                                          \
    } while (0)
    PUT(e, "(");
    for (NodeId a = args; a; a = NEXT(e->u, a))
    {
        TypeId t = e->u->types[a];
        if (newline && a != args)
        {
            OPEN("str"); // print separates its arguments
            PUT(e, "M_STR(\" \")), ");
        }
        if (t == T_STR)
        {
            OPEN("str");
        }
        else if (t == T_BOOL)
        {
            OPEN("bool");
        }
        else if (t == T_U64 || (t == T_INT_LIT && literal_above_i64(e->u, a)))
        {
            OPEN("uint");
        }
        else if (type_is_int(t))
        {
            OPEN("int");
            PUT(e, "(long long)");
        }
        else
        {
            unsupported(e, NODE(e->u, a)->tok, "printing a value of this type");
            OPEN("int");
        }
        emit_expr(e, a);
        PUT(e, "), ");
    }
    if (newline)
    {
        OPEN("str");
        PUT(e, "M_STR(\"\\n\")))");
    }
    else
    {
        PUT(e, "(void)0)");
    }
#undef OPEN
}

// Bytes as a C string literal: anything but letters, digits and spaces as an octal escape.
static void put_c_bytes(Emit *e, const uint8_t *b, size_t n)
{
    char buf[8];
    put_c(e, '"');
    for (size_t i = 0; i < n; i++)
    {
        if ((b[i] >= 'a' && b[i] <= 'z') || (b[i] >= 'A' && b[i] <= 'Z') || (b[i] >= '0' && b[i] <= '9') ||
            b[i] == ' ')
        {
            put_c(e, (char)b[i]);
        }
        else
        {
            snprintf(buf, sizeof(buf), "\\%03o", b[i]);
            put(e, buf);
        }
    }
    put_c(e, '"');
}

// An unsized constant (docs/comptime.md §3.1) where it is used: the literal of its value, so C
// gives it the type it gives that literal.
static int emit_unsized(Emit *e, NodeId id)
{
    uint64_t bits;
    int neg;
    if (!sema_unsized_value(e->u, id, &bits, &neg))
    {
        return 0;
    }
    char num[48];
    if (neg)
    {
        snprintf(num, sizeof(num), "(-%lldll - 1)", (long long)~bits); // -2^63 too
    }
    else
    {
        snprintf(num, sizeof(num), bits > 0x7fffffffffffffffull ? "%lluu" : "%llu", (unsigned long long)bits);
    }
    put(e, num);
    return 1;
}

static void emit_call(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    const AstNode *callee = NODE(u, n->a);
    SymId fn = u->refs[id];
    uint64_t size;
    if (sema_sizeof(u, id, &size))
    {
        char num[32];
        snprintf(num, sizeof(num), "%llu", (unsigned long long)size);
        put(e, num);
        return;
    }
    if (sema_is_assert(u, id))
    {
        char text[600];
        int k = mir_assert_message(u, id, text, sizeof(text));
        PUT(e, "((");
        emit_expr(e, n->b);
        PUT(e, ") ? (void)0 : m_trap(");
        put_c_bytes(e, (const uint8_t *)text, (size_t)k);
        PUT(e, "))");
        return;
    }

    SymId sink = (n->flags & AST_F_SINK) ? fn : 0;
    if (callee->kind == AST_IDENT && TOK(u, callee->tok)->kind == TOK_WRITE)
    {
        emit_output(e, n->b, 0, sink);
        return;
    }
    if (callee->kind == AST_IDENT && sema_is_std_print(u, u->refs[n->a]))
    {
        emit_output(e, n->b, 1, sink);
        return;
    }
    if (callee->kind == AST_IDENT && sema_std_member(u, u->refs[n->a]) == STD_SYS_PUT)
    {
        PUT(e, "m_write_byte(");
        emit_expr(e, n->b);
        PUT(e, ")");
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
                PUT(e, "&(m_");
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
    for (NodeId a = n->b; a && param; a = NEXT(u, a), param = NEXT(u, param))
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
        if (memchr(TSTART(t), '.', t->len))
        {
            unsupported(e, n->tok, "a fractional number");
        }
        {
            // Written in decimal, whatever its spelling: C gives a hex literal another type
            // (0xffffffff is unsigned int, 4294967295 is long), and has no `_` separators.
            uint64_t v = 0;
            char digits[24];
            int k = 24;
            number_value(TSTART(t), t->len, &v, NULL);
            do
            {
                digits[--k] = (char)('0' + v % 10);
                v /= 10;
            } while (v);
            put_n(e, digits + k, (size_t)(24 - k));
        }
        if (literal_above_i64(e->u, id))
        {
            PUT(e, "u"); // C's own type for it, said out loud (no warning)
        }
        break;
    case AST_STRING_LIT:
        PUT(e, "M_STR(");
        for (uint32_t i = 0; i < t->len; i++) // a Modal string may span lines; a C one may not
        {
            char ch = TSTART(t)[i];
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
        else if (s && SYM(s)->kind == SYM_CONST && emit_unsized(e, id))
        {
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
                 (!memcmp(TSTART(t), "true", 4) || !memcmp(TSTART(t), "false", 5)))
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
        if (t->kind == TOK_MINUS && type >= T_U8 && type <= T_INT) // wraps, as `0 - x` does
        {
            PUT(e, "((");
            out_type(e, type, n->tok);
            PUT(e, ")(0 - (uint64_t)(");
            emit_expr(e, n->a);
            PUT(e, ")))");
            break;
        }
        put_c(e, '(');
        put_c(e, *TSTART(t));
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
        // `+ - *` are computed in uint64_t and converted back: that is the same wrap as the
        // native code, where C's own arithmetic would be undefined on a signed overflow (and on
        // u16 * u16, which C promotes to int).
        int wraps = type >= T_U8 && type <= T_INT &&
                    (t->kind == TOK_PLUS || t->kind == TOK_MINUS || t->kind == TOK_STAR);
        if (wraps)
        {
            PUT(e, "((");
            out_type(e, type, n->tok);
            PUT(e, ")((uint64_t)(");
            emit_expr(e, n->a);
            PUT(e, ") ");
            put_n(e, TSTART(t), t->len);
            PUT(e, " (uint64_t)(");
            emit_expr(e, n->b);
            PUT(e, ")))");
            break;
        }
        if (t->kind == TOK_SLASH && type >= T_U8 && type <= T_INT)
        {
            // A zero divisor stops the program with its place, as in the native code.
            int line, col;
            source_pos(u->src, TSTART(t), &line, &col);
            PUT(e, "m_div_");
            out_type(e, type, n->tok);
            PUT(e, "(");
            emit_expr(e, n->a);
            PUT(e, ", ");
            emit_expr(e, n->b);
            PUT(e, ", \"");
            for (const char *c = u->path; *c; c++)
            {
                if (*c == '"' || *c == '\\')
                {
                    put_c(e, '\\');
                }
                put_c(e, *c);
            }
            char pos[48];
            snprintf(pos, sizeof(pos), ":%d:%d: division by zero\\n\")", line, col);
            put(e, pos);
            break;
        }
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
            put_n(e, TSTART(t), t->len);
        }
        put_c(e, ' ');
        emit_expr(e, n->b);
        PUT(e, "))");
        break;
    }
    case AST_SELECTOR:
    {
        SymId member = u->refs[id];
        if (member && SYM(member)->kind == SYM_CONST && emit_unsized(e, id))
        {
            break; // `util.limit`, an unsized constant of another module
        }
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
        for (NodeId s = n->a; s; s = NEXT(u, s))
        {
            emit_stmt(e, s, ret);
        }
        e->indent--;
        out_indent(e);
        PUT(e, "}\n");
        return;
    case AST_DEFINE:
        if (NODE(u, n->a)->kind == AST_FN_LIT)
        {
            return; // the literal was emitted as a function of its own; calls name it directly
        }
        out_indent(e);
        out_type(e, u->types[id], n->tok);
        PUT(e, " v_");
        out_tok(e, n->tok);
        PUT(e, " = ");
        emit_as(e, n->a, u->types[id]);
        PUT(e, ";\n");
        return;
    case AST_FIELD_DECL: // a local with its type: its value converted to it, or zero
        out_indent(e);
        out_type(e, u->types[id], n->tok);
        PUT(e, " v_");
        out_tok(e, n->tok);
        PUT(e, " = ");
        if (n->b)
        {
            emit_as(e, n->b, u->types[id]);
        }
        else
        {
            put(e, type_kind(u->types[id]) == T_STRUCT ? "{0}" : "0");
        }
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
        PUT(e, "m_");
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, " *self");
        first = 0;
    }
    int index = 0;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p), index++)
    {
        put(e, first ? "" : ", ");
        out_type(e, sema_param_type(u, fn, p), NODE(u, p)->tok);
        put_c(e, ' ');
        out_param_name(e, p, index);
        first = 0;
    }
    put(e, first ? "void)" : ")");
}

// A clause with patterns starts by testing them; when they do not match, the same arguments
// go to the next clause.
static void emit_clause_test(Emit *e, SymId fn, TypeId ret)
{
    Unit *u = e->u;
    SymId next = sema_next_clause(u, fn);
    if (!next)
    {
        return;
    }
    PUT(e, "    if (!(");
    int index = 0, first = 1;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p), index++)
    {
        uint64_t value;
        if (!sema_pattern(u, p, SYM(fn)->owner, &value, 0))
        {
            continue;
        }
        put(e, first ? "" : " && ");
        out_param_name(e, p, index);
        char num[32];
        snprintf(num, sizeof(num), " == (");
        put(e, num);
        out_type(e, sema_param_type(u, fn, p), NODE(u, p)->tok);
        snprintf(num, sizeof(num), ")%lluull", (unsigned long long)value);
        put(e, num);
        first = 0;
    }
    PUT(e, "))\n    {\n        ");
    put(e, ret == T_VOID ? "" : "return ");
    out_fn_name(e, next);
    PUT(e, "(");
    first = 1;
    if (SYM(fn)->owner)
    {
        PUT(e, "self");
        first = 0;
    }
    index = 0;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p), index++)
    {
        put(e, first ? "" : ", ");
        out_param_name(e, p, index);
        first = 0;
    }
    put(e, ret == T_VOID ? ");\n        return;\n    }\n" : ");\n    }\n");
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
    emit_clause_test(e, fn, ret);
    if (n->flags & AST_F_ONELINE)
    {
        out_indent(e);
        PUT(e, "return ");
        emit_as(e, fn_body(u, (NodeId)(n - u->nodes.data)), ret);
        PUT(e, ";\n");
    }
    else
    {
        for (NodeId s = NODE(u, fn_body(u, (NodeId)(n - u->nodes.data)))->a; s; s = NEXT(u, s))
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
    int fields = 0, has_str = 0;
    int block = self && sema_is_register_block(self);
    uint32_t pos = 0;

    PUT(e, "struct m_");
    put_n(e, TSTART(name), name->len);
    PUT(e, "\n{\n");
    for (NodeId m = n->a; m; m = NEXT(u, m))
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
        SymId s = scope_find(scope_at(SYM(self)->scope_ref), TSTART(field), field->len);
        while (s && SYM(s)->kind != SYM_FIELD)
        {
            s = SYM(s)->next; // skip an accessor that shares the field's name
        }
        if (block && s) // a register: padding up to its offset, and every access volatile
        {
            uint32_t at = field_offset_of(s);
            if (at > pos)
            {
                char pad[64];
                snprintf(pad, sizeof(pad), "    uint8_t m_pad_%u[%u];\n", at, at - pos);
                put(e, pad);
            }
            pos = at + type_layout(sema_value_type(u, s)).size;
        }
        put(e, block ? "    volatile " : "    ");
        out_type(e, s ? sema_value_type(u, s) : T_NONE, member->tok);
        put_c(e, ' ');
        put_n(e, TSTART(field), field->len);
        PUT(e, ";\n");
        fields++;
        has_str |= s && (sema_value_type(u, s) == T_STR || type_kind(sema_value_type(u, s)) == T_STRUCT);
    }
    if (!fields)
    {
        PUT(e, "    char m_empty;\n"); // C has no empty structs
    }
    PUT(e, "};\n");
    // The C compiler checks that sema/layout.c put every field where C does (a string differs:
    // two words in C, one pointer natively). C99: an array of size -1 is an error.
    for (NodeId m = n->a; m && !has_str && self; m = NEXT(u, m))
    {
        const AstNode *member = NODE(u, m);
        if (member->kind != AST_FIELD_DECL)
        {
            continue;
        }
        const Token *field = TOK(u, member->tok);
        SymId s = scope_find(scope_at(SYM(self)->scope_ref), TSTART(field), field->len);
        while (s && SYM(s)->kind != SYM_FIELD)
        {
            s = SYM(s)->next;
        }
        if (!s)
        {
            continue;
        }
        char check[96];
        PUT(e, "typedef char m_layout_");
        put_n(e, TSTART(name), name->len);
        put_c(e, '_');
        put_n(e, TSTART(field), field->len);
        PUT(e, "[offsetof(struct m_");
        put_n(e, TSTART(name), name->len);
        PUT(e, ", ");
        put_n(e, TSTART(field), field->len);
        snprintf(check, sizeof(check), ") == %u ? 1 : -1];\n", field_offset_of(s));
        put(e, check);
    }
    if (self && !has_str)
    {
        char check[96];
        PUT(e, "typedef char m_layout_");
        put_n(e, TSTART(name), name->len);
        snprintf(check, sizeof(check), "_size[sizeof(struct m_%.*s) == %u ? 1 : -1];\n",
                 (int)name->len, TSTART(name), type_layout(type_make(T_STRUCT, self)).size);
        put(e, check);
    }
    PUT(e, "\n");
}

static const char *const prelude =
    "// Generated by the Modal compiler (backend: c). Do not edit.\n"
    "#include <stdbool.h>\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "\n"
    "typedef struct\n"
    "{\n"
    "    const uint8_t *ptr;\n"
    "    size_t len;\n"
    "} m_str;\n"
    "\n"
    "#define M_STR(s) ((m_str){(const uint8_t *)(s), sizeof(s) - 1})\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#define M_RT static inline __attribute__((unused))\n"
    "#else\n"
    "#define M_RT static inline\n"
    "#endif\n"
    "\n"
    "M_RT void m_write_str(m_str s) { fwrite(s.ptr, 1, s.len, stdout); }\n"
    // A trap: the output so far, then the place, then status 1 (as the native backends do).
    "M_RT void m_trap(const char *m) { fflush(stdout); fputs(m, stderr); exit(1); }\n"
    // Division: a zero divisor traps; the smallest signed value by -1 wraps (as `0 - x` does).
    "#define M_DIV(T) M_RT T m_div_##T(T a, T b, const char *m) { if (!b) m_trap(m); \\\n"
    "    if ((T)-1 < 0 && b == (T)-1) return (T)(0 - (uint64_t)a); return (T)(a / b); }\n"
    "M_DIV(uint8_t) M_DIV(uint16_t) M_DIV(uint32_t) M_DIV(uint64_t)\n"
    "M_DIV(int8_t) M_DIV(int16_t) M_DIV(int32_t) M_DIV(int64_t)\n"
    "M_RT void m_write_int(long long n) { printf(\"%lld\", n); }\n"
    "M_RT void m_write_uint(unsigned long long n) { printf(\"%llu\", n); }\n"
    "M_RT void m_write_bool(bool b) { fputs(b ? \"true\" : \"false\", stdout); }\n"
    "M_RT void m_write_byte(uint8_t b) { putchar(b); }\n"
    "\n"
    "/* output through the program's `out`: every byte to its put method */\n"
    "typedef void (*m_sink)(void *, uint8_t);\n"
    "M_RT void m_sink_str(void *o, m_sink p, m_str s) { for (size_t i = 0; i < s.len; i++) p(o, s.ptr[i]); }\n"
    "M_RT void m_sink_uint(void *o, m_sink p, unsigned long long n) {\n"
    "    uint8_t b[24]; int i = 24;\n"
    "    do { b[--i] = (uint8_t)('0' + n % 10); n /= 10; } while (n);\n"
    "    while (i < 24) p(o, b[i++]);\n"
    "}\n"
    "M_RT void m_sink_int(void *o, m_sink p, long long n) {\n"
    "    if (n < 0) { p(o, '-'); m_sink_uint(o, p, 0ull - (unsigned long long)n); }\n"
    "    else m_sink_uint(o, p, (unsigned long long)n);\n"
    "}\n"
    "M_RT void m_sink_bool(void *o, m_sink p, bool b) { m_sink_str(o, p, b ? M_STR(\"true\") : M_STR(\"false\")); }\n"
    "\n";

// One Emit for the whole job: it holds the output buffer between the three calls.
static Emit emit_state;


// A string or struct value the compiler computed (docs/comptime.md), as a C initializer: the
// fields by name, a string as its bytes and length.
static void emit_computed(Emit *e, const ComptimeAggregate *c, TypeId t, uint32_t off)
{
    char buf[64];
    if (t == T_STR)
    {
        const uint8_t *b = c->str[off / 8];
        uint32_t n = c->len[off / 8];
        PUT(e, "{(const uint8_t *)\"");
        for (uint32_t i = 0; i < n; i++)
        {
            snprintf(buf, sizeof(buf), "\\%03o", b[i]); // octal: never joins the next character
            put(e, buf);
        }
        snprintf(buf, sizeof(buf), "\", %u}", n);
        put(e, buf);
        return;
    }
    if (type_kind(t) == T_STRUCT)
    {
        SymId st = type_ref(t);
        Unit *home = &cx.units[SYM(st)->unit];
        PUT(e, "{");
        int first = 1;
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            if (NODE(home, m)->kind != AST_FIELD_DECL)
            {
                continue;
            }
            const Token *field = TOK(home, NODE(home, m)->tok);
            put(e, first ? "." : ", .");
            first = 0;
            put_n(e, TSTART(field), field->len);
            PUT(e, " = ");
            emit_computed(e, c, sema_type_of(home, NODE(home, m)->a, st), off + layout_member_offset(st, m));
        }
        PUT(e, "}");
        return;
    }
    // a number or a bool: its bytes at off, little-endian in the slots
    uint32_t size = type_layout(t).size;
    uint64_t v = 0;
    memcpy(&v, (const uint8_t *)c->slots + off, size > 8 ? 8 : size);
    int is_signed = t == T_I8 || t == T_I16 || t == T_I32 || t == T_I64 || t == T_INT;
    if (is_signed && size < 8 && (v >> (size * 8 - 1)) & 1)
    {
        v |= ~0ull << (size * 8);
    }
    if (is_signed && (int64_t)v < 0)
    {
        snprintf(buf, sizeof(buf), "(-%lldll - 1)", (long long)~v); // -2^63 too
    }
    else
    {
        snprintf(buf, sizeof(buf), "%lluull", (unsigned long long)v);
    }
    put(e, buf);
}

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
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            const Token *name = TOK(u, NODE(u, d)->tok);
            PUT(e, "typedef struct m_");
            put_n(e, TSTART(name), name->len);
            PUT(e, " m_");
            put_n(e, TSTART(name), name->len);
            PUT(e, ";\n");
        }
    }
    PUT(e, "\n");
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        if (NODE(u, d)->kind == AST_STRUCT_DECL)
        {
            emit_struct(e, d);
        }
    }

    // 2. Module-level values.
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
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
        if (type == T_INT_LIT)
        {
            continue; // unsized: written as its value where it is used
        }
        if (s && sema_device_address(s)) // a device: a constant reference to its registers
        {
            char at[48];
            PUT(e, "static ");
            out_type(e, type, n->tok);
            PUT(e, " const g_");
            put_n(e, TSTART(name), name->len);
            PUT(e, " = (");
            out_type(e, type, n->tok);
            snprintf(at, sizeof(at), ")0x%llxull;\n", (unsigned long long)sema_device_address(s));
            put(e, at);
            continue;
        }

        // A struct is not `const` in C: methods take its address as `self` (Modal keeps it
        // immutable).
        put(e, (n->kind == AST_DEFINE || !(n->flags & AST_F_MUT)) && type_kind(type) != T_STRUCT ? "static const "
                                                                                              : "static ");
        out_type(e, type, n->tok);
        PUT(e, " g_");
        put_n(e, TSTART(name), name->len);
        uint64_t computed;
        const ComptimeAggregate *agg = init && s ? sema_comptime_aggregate(s) : NULL;
        if (agg)
        {
            PUT(e, " = ");
            emit_computed(e, agg, type, 0);
        }
        else if (init && s && sema_comptime_value(s, &computed))
        {
            // computed by the compiler (docs/comptime.md): the number, in its type
            char num[48];
            PUT(e, " = (");
            out_type(e, type, n->tok);
            snprintf(num, sizeof(num), ")%lluull", (unsigned long long)computed);
            put(e, num);
        }
        else if (init)
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

    // 4. For each `put(self: out, b: u8)`: the adapter print and write call it through.
    for (SymId fn = 1; fn < cx.syms.size; fn++)
    {
        int len;
        const char *name = symbol_name(fn, &len);
        SymId owner = SYM(fn)->owner;
        int olen = 0;
        const char *oname = owner ? symbol_name(owner, &olen) : "";
        NodeId param = SYM(fn)->kind == SYM_METHOD ? sema_fn_params(u, fn) : 0;
        if (SYM(fn)->unit != u->id || SYM(fn)->kind != SYM_METHOD || len != 3 ||
            memcmp(name, "put", 3) != 0 || olen != 3 || memcmp(oname, "out", 3) != 0 || !param ||
            NEXT(u, param) || !emittable(e, fn, 1))
        {
            continue;
        }
        PUT(e, "static void ");
        out_sink_name(e, fn);
        PUT(e, "(void *o, uint8_t b) { ");
        out_fn_name(e, fn);
        PUT(e, "((m_out *)o, b); }\n");
    }
    PUT(e, "\n");
}

// Called once per function, right after it is checked, while its body is still in memory.
static void backend_c_function(const BackendJob *job, SymId fn)
{
    (void)job;
    if ((SYM(fn)->flags & SYM_F_CAPTURES))
    {
        unsupported(&emit_state, SYM(fn)->tok,
                    "a function literal that uses a local of the function around it (a closure)");
        return;
    }
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
