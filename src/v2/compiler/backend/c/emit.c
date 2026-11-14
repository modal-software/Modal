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
#include <stdlib.h>
#include <string.h>

typedef struct
{
    Unit *u;
    FILE *out;
    int errors;
    int indent;
    SymId fn;   // the function being written
    size_t len; // bytes waiting in buf
    char buf[1 << 16];
} Emit;

static void emit_expr(Emit *e, NodeId id);
static void emit_match(Emit *e, NodeId id);
static void emit_match_as(Emit *e, NodeId id, int returned);


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
    const Unit *home = &cx.units[SYM(fn)->unit];
    PUT(e, "m_");
    if (home->id != 0) // a function of another file: its file's number keeps the names apart
    {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "u%u_", (unsigned)home->id);
        put(e, prefix);
    }
    if (SYM(fn)->owner)
    {
        out_sym_name(e, SYM(fn)->owner);
        PUT(e, "_");
    }
    out_sym_name(e, fn);
    if (sema_spec_index(fn)) // made for one type: one C function per specialization
    {
        char k[16];
        snprintf(k, sizeof(k), "__s%d", sema_spec_index(fn));
        put(e, k);
    }
    if (SYM(fn)->flags & SYM_F_MUT)
    {
        int arity = 0;
        for (NodeId p = sema_fn_params(home, fn); p; p = NEXT(home, p))
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
        int clause = sema_clause_index((Unit *)home, fn);
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

// A struct's C name: m_<name>, or m_u<file>_<name> for one declared in another file.
static void out_struct_name(Emit *e, SymId st)
{
    PUT(e, "m_");
    if (SYM(st)->unit != 0)
    {
        char prefix[16];
        snprintf(prefix, sizeof(prefix), "u%u_", (unsigned)SYM(st)->unit);
        put(e, prefix);
    }
    out_sym_name(e, st);
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
        out_struct_name(e, type_ref(t));
    }
    else if (k == T_PTR && type_kind(type_ref(t)) == T_ARRAY) // *[N]T: a pointer to its first element
    {
        out_type(e, type_ref(type_ref(t)), at);
        PUT(e, " *");
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
// ---- type descriptors (std's type and its member table) as C data ----------------------------
//
// C relocates addresses in data itself, so a descriptor is a static object: std's type, and for
// a struct its member table, each `of` the address of its field type's descriptor. Made in
// begin for every type known then; one met only later is given as a value with no members.
static int c_struct(SymId st);
static uint8_t *c_desc;  // per TypeId: 1 when m_desc_<t> exists
static size_t c_desc_cap;

static int c_has_desc(TypeId t)
{
    return t < c_desc_cap && c_desc[t];
}

static void out_desc_value(Emit *e, TypeId t) // std's type for t, as a C value
{
    char num[160];
    if (c_has_desc(t))
    {
        snprintf(num, sizeof(num), "m_desc_%u", (unsigned)t);
        put(e, num);
        return;
    }
    uint64_t d[4];
    sema_type_desc(t, d);
    PUT(e, "((");
    out_struct_name(e, sema_std_type());
    snprintf(num, sizeof(num), "){ %lluull, %lluull, %lluull, %lluull, 0 })", (unsigned long long)d[0],
             (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3]);
    put(e, num);
}

static void out_desc_address(Emit *e, TypeId t)
{
    if (c_has_desc(t))
    {
        char num[48];
        snprintf(num, sizeof(num), "(&m_desc_%u)", (unsigned)t);
        put(e, num);
        return;
    }
    PUT(e, "&");
    out_desc_value(e, t);
}

static void c_desc_collect(TypeId t)
{
    if (type_kind(t) == T_TYPE)
    {
        t = type_make(T_STRUCT, type_ref(t));
    }
    if (t >= c_desc_cap)
    {
        size_t cap = c_desc_cap ? c_desc_cap : 256;
        while (cap <= t)
        {
            cap *= 2;
        }
        c_desc = realloc(c_desc, cap);
        memset(c_desc + c_desc_cap, 0, cap - c_desc_cap);
        c_desc_cap = cap;
    }
    if (c_desc[t])
    {
        return;
    }
    c_desc[t] = 1;
    if (type_kind(t) == T_STRUCT)
    {
        SymId st = type_ref(t);
        Unit *home = &cx.units[SYM(st)->unit];
        for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
        {
            if (NODE(home, m)->kind == AST_FIELD_DECL)
            {
                c_desc_collect(sema_type_of(home, NODE(home, m)->a, st));
            }
        }
    }
}

static void emit_descriptors(Emit *e)
{
    SymId st = sema_std_type();
    if (!st)
    {
        return;
    }
    SymId member = scope_find(cx.units[SYM(st)->unit].scope, "member", 6);
    for (TypeId t = T_BOOL; t < T__PRIM_COUNT; t++)
    {
        if (t != T_NIL && t != T_INT_LIT && t != T_EXTERN && t != T_MODULE && type_layout(t).size)
        {
            c_desc_collect(t);
        }
    }
    for (SymId s = 1; s < cx.syms.size; s++)
    {
        if (c_struct(s))
        {
            c_desc_collect(type_make(T_STRUCT, s));
        }
    }
    char line[200];
    for (TypeId t = 0; t < c_desc_cap; t++) // declared first: a member table points at any of them
    {
        if (c_desc[t])
        {
            PUT(e, "static __attribute__((unused)) ");
            out_struct_name(e, st);
            snprintf(line, sizeof(line), " m_desc_%u;\n", (unsigned)t);
            put(e, line);
        }
    }
    for (TypeId t = 0; t < c_desc_cap; t++)
    {
        if (!c_desc[t])
        {
            continue;
        }
        uint64_t d[4];
        sema_type_desc(t, d);
        if (d[3] && member && type_kind(t) == T_STRUCT)
        {
            SymId s = type_ref(t);
            Unit *home = &cx.units[SYM(s)->unit];
            PUT(e, "static __attribute__((unused)) ");
            out_struct_name(e, member);
            snprintf(line, sizeof(line), " m_desc_%u_m[] = {", (unsigned)t);
            put(e, line);
            for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
            {
                if (NODE(home, m)->kind != AST_FIELD_DECL)
                {
                    continue;
                }
                TypeId ft = sema_type_of(home, NODE(home, m)->a, s);
                snprintf(line, sizeof(line), " { %uull, &m_desc_%u },", (unsigned)layout_member_offset(s, m),
                         (unsigned)(type_kind(ft) == T_TYPE ? type_make(T_STRUCT, type_ref(ft)) : ft));
                put(e, line);
            }
            PUT(e, " };\n");
        }
        PUT(e, "static ");
        out_struct_name(e, st);
        snprintf(line, sizeof(line), " m_desc_%u = { %lluull, %lluull, %lluull, %lluull, ", (unsigned)t,
                 (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2],
                 (unsigned long long)d[3]);
        put(e, line);
        if (d[3] && member && type_kind(t) == T_STRUCT)
        {
            snprintf(line, sizeof(line), "m_desc_%u_m };\n", (unsigned)t);
        }
        else
        {
            snprintf(line, sizeof(line), "0 };\n");
        }
        put(e, line);
    }
    PUT(e, "\n");
}

static void emit_as(Emit *e, NodeId id, TypeId want)
{
    TypeId have = e->u->types[id];
    TypeId named = sema_named_type(e->u, id);
    if (named != T_NONE && sema_std_type() && want == type_make(T_STRUCT, sema_std_type()))
    {
        out_desc_value(e, named); // `describe(point)`: the type's descriptor
        return;
    }
    if (sema_is_any_ref(want) && type_kind(have) == T_PTR && !sema_is_any_ref(have))
    {
        char tid[48];
        PUT(e, "(&(");
        out_struct_name(e, type_ref(type_ref(want)));
        PUT(e, "){ .at = (uint64_t)(uintptr_t)(");
        emit_expr(e, id);
        PUT(e, "), .of = ");
        out_desc_address(e, type_ref(have));
        PUT(e, " })");
        (void)tid;
        return;
    }
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
    uint64_t desc[4];
    if (sema_typeof_desc(u, id, desc)) // typeof(point): its descriptor
    {
        out_desc_value(e, (TypeId)desc[0]);
        return;
    }
    if (sema_primitive(u, id) == PRIM_TYPEID && n->b && sema_is_any_ref(u->types[n->b])) // typeof(a)
    {
        PUT(e, "(*(");
        emit_expr(e, n->b);
        PUT(e, ")->of)");
        return;
    }
    if (sema_primitive(u, id) == PRIM_LOAD && n->b) // load(T, p, offset): `*(T *)((uint8_t *)p + offset)`
    {
        NodeId p = NEXT(u, n->b), off = NEXT(u, p);
        uint64_t k;
        PUT(e, "(*(");
        out_type(e, u->types[id], n->tok);
        PUT(e, " *)((uint8_t *)(");
        emit_expr(e, p);
        PUT(e, ") + (");
        if (sema_known(e->fn, off, &k))
        {
            char num[32];
            snprintf(num, sizeof(num), "%lluull", (unsigned long long)k);
            put(e, num);
        }
        else
        {
            emit_expr(e, off);
        }
        PUT(e, ")))");
        return;
    }
    if (sema_primitive(u, id) == PRIM_UNANY && n->b) // point(a)
    {
        int line, col;
        source_pos(u->src, TSTART(TOK(u, n->tok)), &line, &col);
        char tail[120];
        PUT(e, "((");
        out_type(e, u->types[id], n->tok);
        PUT(e, ")m_unany((const uint64_t *)(");
        emit_expr(e, n->b);
        snprintf(tail, sizeof(tail), "), %lluull, \"", (unsigned long long)type_make(T_STRUCT, u->refs[id]));
        put(e, tail);
        put(e, u->path);
        snprintf(tail, sizeof(tail), ":%d:%d: the *any holds another type\\n\"))", line, col);
        put(e, tail);
        return;
    }
    if (sema_primitive(u, id) == PRIM_CONVERT && n->b)
    {
        PUT(e, "((");
        out_type(e, u->types[id], n->tok);
        PUT(e, ")(");
        emit_expr(e, n->b);
        PUT(e, "))");
        return;
    }
    if (sema_primitive(u, id) == PRIM_TRAP)
    {
        PUT(e, "(");
        for (NodeId a = n->b; a; a = NEXT(u, a))
        {
            PUT(e, "m_trap_part(");
            emit_expr(e, a);
            PUT(e, "), ");
        }
        PUT(e, "m_trap(\"\"))");
        return;
    }
    uint64_t size;
    if (sema_sizeof(u, id, &size))
    {
        char num[32];
        snprintf(num, sizeof(num), "%llu", (unsigned long long)size);
        put(e, num);
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
        unsupported(e, callee->tok, "calling something that is not a Modal function");
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
                PUT(e, "&(");
                out_struct_name(e, type_ref(base));
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

    Unit *home = &cx.units[SYM(fn)->unit];
    NodeId param = sema_fn_params(home, fn);
    for (NodeId a = n->b; a && param; a = NEXT(u, a), param = NEXT(home, param))
    {
        put(e, first ? "" : ", ");
        emit_as(e, a, sema_param_type(home, fn, param));
        first = 0;
    }
    PUT(e, ")");
}

// What a pattern test compares: a parameter (by its C name), or a temporary of a match.
typedef struct
{
    NodeId param;
    int index;
    const char *temp; // "m_s12": a match's value
} Subject;

static void out_subject(Emit *e, Subject s)
{
    if (s.temp)
    {
        put(e, s.temp);
    }
    else
    {
        out_param_name(e, s.param, s.index);
    }
}

// `subject` is a value the pattern takes, as a C condition. A string: its length, then each
// byte (no libc: no memcmp). A value: ==. A range: both ends, in the subject's own C type.
static void emit_pattern_cond(Emit *e, const Pattern *pat, TypeId type, Subject s)
{
    char num[64];
    if (pat->kind == PATTERN_TEXT)
    {
        static uint8_t text[4096];
        size_t len = sema_pattern_text(e->u, pat->tok, text, sizeof(text));
        out_subject(e, s);
        snprintf(num, sizeof(num), ".len == %zu", len);
        put(e, num);
        for (size_t k = 0; k < len && k < sizeof(text); k++)
        {
            PUT(e, " && ");
            out_subject(e, s);
            snprintf(num, sizeof(num), ".ptr[%zu] == %u", k, (unsigned)text[k]);
            put(e, num);
        }
        return;
    }
    // (only == <= >= joined by &&: no parentheses needed inside any && chain)
    for (int end = 0; end < (pat->kind == PATTERN_RANGE ? 2 : 1); end++)
    {
        put(e, end ? " && " : "");
        out_subject(e, s);
        put(e, pat->kind == PATTERN_VALUE ? " == (" : end ? " <= (" : " >= (");
        out_type(e, type, 0);
        snprintf(num, sizeof(num), ")%lluull", (unsigned long long)(end ? pat->hi : pat->lo));
        put(e, num);
    }
}

// ---- arms (docs/std.md §8) ------------------------------------------------------------------------
//
// A match on a value is a GNU statement expression (clang and GCC): its value in m_s<id>, the arms
// an if / else chain, the result in m_r<id>. A body of arms on the parameters is that chain of
// returns. A name arm declares its name, bound to the value.
static void emit_stmt(Emit *e, NodeId id, TypeId ret);
static int tail_call(Emit *e, NodeId id);
static void emit_tail(Emit *e, NodeId call);

static int c_statement(const Unit *u, NodeId r)
{
    int k = NODE(u, r)->kind;
    return k == AST_BLOCK || k == AST_RETURN || k == AST_ASSIGN || k == AST_FIELD_DECL || k == AST_IF;
}

static void emit_match(Emit *e, NodeId id)
{
    emit_match_as(e, id, 0);
}

// returned: `return if n { ... }`, written as a statement whose arms return (a call there is a
// tail call, as in a body of arms).
static void emit_match_as(Emit *e, NodeId id, int returned)
{
    Unit *u = e->u;
    const AstNode *m = NODE(u, id);
    TypeId type = u->types[id], types[16];
    Subject subj[16];
    char temps[16][24];
    int ns = 0;
    TypeId ret = e->fn ? sema_fn_ret(&cx.units[SYM(e->fn)->unit], e->fn) : T_VOID;
    if (m->a)
    {
        put(e, returned ? "{ " : "({ "); // (put: PUT takes literals only)
        for (NodeId v = m->a; v && ns < 16; v = NEXT(u, v), ns++) // one temporary per value
        {
            types[ns] = u->types[v] == T_INT_LIT ? T_I64 : u->types[v]; // (as the checker matched it)
            snprintf(temps[ns], sizeof(temps[ns]), "m_s%u_%d", id, ns);
            subj[ns] = (Subject){0, 0, temps[ns]};
            out_type(e, types[ns], m->tok);
            put(e, " ");
            put(e, temps[ns]);
            PUT(e, " = ");
            emit_as(e, v, types[ns]);
            PUT(e, "; ");
        }
        if (!returned && type != T_VOID && type != T_NONE)
        {
            out_type(e, type, m->tok);
            char r[24];
            snprintf(r, sizeof(r), " m_r%u", id);
            put(e, r);
            PUT(e, "; ");
        }
    }
    else
    {
        int index = 0;
        for (NodeId p = sema_fn_params(u, e->fn); p && ns < 16; p = NEXT(u, p), index++)
        {
            const AstNode *pn = NODE(u, p);
            const Token *name = TOK(u, pn->tok);
            if (!(pn->flags & AST_F_CONTEXT) && !is_self_token(TSTART(name), (int)name->len))
            {
                types[ns] = sema_param_type(u, e->fn, p);
                subj[ns++] = (Subject){p, index, NULL};
            }
        }
        PUT(e, "{\n");
    }
    NodeId only = e->fn ? sema_taken_arm(e->fn, id) : 0; // decided while compiling: that arm, untested
    for (NodeId arm = m->b; arm; arm = NEXT(u, arm))
    {
        if (only && arm != only)
        {
            continue;
        }
        const AstNode *a = NODE(u, arm);
        int last = !NEXT(u, arm) || only;
        int tests = 0, k = 0;
        for (NodeId pat = a->a; pat && k < ns && !only; pat = NEXT(u, pat), k++)
        {
            Pattern pt;
            NodeId name;
            if (last && (m->aux & MATCH_LAST_FREE))
            {
                continue;
            }
            if (sema_arm_pattern(u, pat, types[k], &pt)) // `_`: none
            {
                put(e, tests ? " && " : "if (");
                emit_pattern_cond(e, &pt, types[k], subj[k]);
                tests++;
            }
            else if ((name = sema_arm_name(u, pat, types[k]))) // a value known when the program runs
            {
                put(e, tests ? " && " : "if (");
                out_subject(e, subj[k]);
                PUT(e, " == (");
                emit_as(e, name, types[k]);
                PUT(e, ")");
                tests++;
            }
        }
        put(e, tests ? ") {\n" : "{\n");
        NodeId r = a->b;
        int saved = e->indent;
        e->indent = 2;
        if ((!m->a || returned) && !c_statement(u, r) && ret != T_VOID) // on the parameters: the return
        {
            if (tail_call(e, r))
            {
                emit_tail(e, r);
            }
            else
            {
                out_indent(e);
                PUT(e, "return ");
                emit_as(e, r, ret);
                PUT(e, ";\n");
            }
        }
        else if (m->a && !returned && type != T_VOID && type != T_NONE)
        {
            char line[32];
            snprintf(line, sizeof(line), "m_r%u = (", id);
            out_indent(e);
            put(e, line);
            out_type(e, type, m->tok); // said out loud: the arm's value wraps into the match's type
            PUT(e, ")(");
            emit_as(e, r, type);
            PUT(e, ");\n");
        }
        else
        {
            if (!m->a && ret == T_VOID && !c_statement(u, r) && tail_call(e, r))
            {
                emit_tail(e, r);
            }
            else
            {
                emit_stmt(e, r, ret);
            }
            if (!m->a && ret == T_VOID) // (a block of a function with a result returns by itself)
            {
                PUT(e, "        return;\n");
            }
        }
        e->indent = saved;
        put(e, last ? "}\n" : "} else ");
    }
    if (m->a && !returned)
    {
        if (type != T_VOID && type != T_NONE)
        {
            char r[24];
            snprintf(r, sizeof(r), "m_r%u; })", id);
            put(e, r);
        }
        else
        {
            PUT(e, "})");
        }
    }
    else
    {
        PUT(e, "}");
    }
}

// `"file:line:col: <what>\n"`, as a C string.
static void out_place(Emit *e, uint32_t tok, const char *what)
{
    Unit *u = e->u;
    int line, col;
    source_pos(u->src, TSTART(TOK(u, tok)), &line, &col);
    PUT(e, "\"");
    for (const char *c = u->path; *c; c++)
    {
        if (*c == '"' || *c == '\\')
        {
            put_c(e, '\\');
        }
        put_c(e, *c);
    }
    char pos[160];
    snprintf(pos, sizeof(pos), ":%d:%d: %s\\n\"", line, col, what);
    put(e, pos);
}

// a[i]: the element, its index checked unless the checker proved it (std.md §9).
static void emit_index(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    TypeId at = u->types[n->a];
    if (at == T_STR)
    {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "m_i%u", id);
        PUT(e, "({ m_str ");
        put(e, tmp);
        PUT(e, " = ");
        emit_expr(e, n->a);
        PUT(e, "; ");
        put(e, tmp);
        PUT(e, ".ptr[m_index((uint64_t)(");
        emit_expr(e, n->b);
        PUT(e, "), ");
        put(e, tmp);
        PUT(e, ".len, ");
        out_place(e, n->tok, "index out of range (past the string's end)");
        PUT(e, ")]; })");
        return;
    }
    emit_expr(e, n->a);
    at = type_kind(at) == T_PTR ? type_ref(at) : at; // `p: *[N]T`: indexed in place
    const AstNode *ix = NODE(u, n->b);
    uint64_t k;
    if (ix->kind == AST_NUMBER_LIT && number_value(TSTART(TOK(u, ix->tok)), TOK(u, ix->tok)->len, &k, NULL) &&
        k < type_len(at))
    {
        char lit[32];
        snprintf(lit, sizeof(lit), "[%llu]", (unsigned long long)k);
        put(e, lit);
        return;
    }
    if (n->aux & INDEX_SAFE)
    {
        PUT(e, "[(uint64_t)(");
        emit_expr(e, n->b);
        PUT(e, ")]");
        return;
    }
    PUT(e, "[m_index((uint64_t)(");
    emit_expr(e, n->b);
    char len[96];
    snprintf(len, sizeof(len), "), %lluull, ", (unsigned long long)type_len(at));
    put(e, len);
    snprintf(len, sizeof(len), "index out of range (0 to %llu)", (unsigned long long)type_len(at) - 1);
    out_place(e, n->tok, len);
    PUT(e, ")]");
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
    case AST_SITE:
    {
        char text[600];
        int k = sema_site_text(u, id, text, sizeof(text));
        char len[24];
        PUT(e, "((m_str){(const uint8_t *)");
        put_c_bytes(e, (const uint8_t *)text, (size_t)k);
        snprintf(len, sizeof(len), ", %d})", k);
        put(e, len);
        break;
    }
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
    case AST_STRUCT_LIT: // a compound literal: C zeroes what it does not name (here, nothing)
    {
        // A field with no bytes (nothing reads it, or a constant) is not in the C struct, but a
        // value of it that does something (a call) still runs: inside the first initializer
        // there is, so the literal stays one (`&(T){ ... }` takes its address).
        PUT(e, "((");
        out_type(e, u->types[id], n->tok);
        PUT(e, "){");
        int any = 0, pending = 1;
        for (NodeId f = n->a; f || pending; f = f ? NEXT(u, f) : 0)
        {
            SymId field = f ? u->refs[f] : 0;
            if (f && (!field || layout_field_use(field, NULL) != FIELD_KEPT))
            {
                continue;
            }
            put(e, any ? ", ." : " .");
            any = 1;
            if (f)
            {
                out_tok(e, NODE(u, f)->tok);
            }
            else
            {
                PUT(e, "m_empty"); // every field is gone: C's one byte
            }
            PUT(e, " = ");
            if (pending)
            {
                pending = 0;
                for (NodeId g = n->a; g; g = NEXT(u, g))
                {
                    SymId gone = u->refs[g];
                    const AstNode *v = NODE(u, NODE(u, g)->a);
                    if (gone && layout_field_use(gone, NULL) != FIELD_KEPT && v->kind != AST_NUMBER_LIT &&
                        v->kind != AST_STRING_LIT && v->kind != AST_IDENT &&
                        !(v->kind == AST_UNARY_OP && NODE(u, v->a)->kind == AST_NUMBER_LIT))
                    {
                        PUT(e, "(void)(");
                        emit_as(e, NODE(u, g)->a, sema_value_type(&cx.units[SYM(gone)->unit], gone));
                        PUT(e, "), ");
                    }
                }
            }
            if (f)
            {
                emit_as(e, NODE(u, f)->a, sema_value_type(&cx.units[SYM(field)->unit], field));
            }
            else
            {
                PUT(e, "0");
            }
            if (!f)
            {
                break;
            }
        }
        PUT(e, " })");
        break;
    }
    case AST_UNARY_OP:
        if (t->kind == TOK_AMP && is_self_ident(u, n->a))
        {
            PUT(e, "self"); // &self: the reference itself
            break;
        }
        if (t->kind == TOK_AMP && type_kind(u->types[n->a]) == T_ARRAY) // &a: its first element's address
        {
            PUT(e, "((");
            out_type(e, type_ref(u->types[n->a]), n->tok);
            PUT(e, " *)");
            emit_expr(e, n->a);
            PUT(e, ")");
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
        uint64_t known;
        if (sema_typeof_field(u, id, &known)) // `typeof(point).size`
        {
            char num[32];
            snprintf(num, sizeof(num), "%lluull", (unsigned long long)known);
            put(e, num);
            break;
        }
        if (!member || SYM(member)->kind != SYM_FIELD)
        {
            unsupported(e, n->tok, "using a method as a value");
            PUT(e, "0");
            break;
        }
        uint64_t constant;
        if (layout_field_use(member, &constant) != FIELD_KEPT) // no bytes: the constant it always is
        {
            char num[48];
            PUT(e, "(");
            if (NODE(u, n->a)->kind != AST_IDENT)
            {
                PUT(e, "(void)(");
                emit_expr(e, n->a);
                PUT(e, "), ");
            }
            PUT(e, "(");
            out_type(e, u->types[id], n->tok);
            snprintf(num, sizeof(num), ")%lluull)", (unsigned long long)constant);
            put(e, num);
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
        if (NODE(u, n->a)->kind == AST_SELECTOR && u->refs[n->a] && SYM(u->refs[n->a])->kind == SYM_FIELD &&
            layout_field_use(u->refs[n->a], NULL) != FIELD_KEPT)
        {
            PUT(e, "(void)("); // a field with no bytes: the value is computed, and goes nowhere
            emit_as(e, n->b, u->types[n->a]);
            PUT(e, ")");
            break;
        }
        emit_expr(e, n->a);
        PUT(e, " = ");
        emit_as(e, n->b, u->types[n->a]);
        break;
    case AST_FN_LIT:
        unsupported(e, n->tok, "a function literal inside another function");
        PUT(e, "0");
        break;
    case AST_MATCH:
        emit_match(e, id);
        break;
    case AST_IF:
    {
        const AstNode *br = NODE(u, n->b);
        TypeId ret = e->fn ? sema_fn_ret(&cx.units[SYM(e->fn)->unit], e->fn) : T_VOID;
        int saved = e->indent;
        PUT(e, "if (");
        emit_as(e, n->a, T_BOOL);
        PUT(e, ") {\n");
        e->indent = saved + 1;
        emit_stmt(e, br->a, ret);
        e->indent = saved;
        out_indent(e);
        PUT(e, "}");
        if (br->b)
        {
            PUT(e, " else {\n");
            e->indent = saved + 1;
            emit_stmt(e, br->b, ret);
            e->indent = saved;
            out_indent(e);
            PUT(e, "}");
        }
        break;
    }
    case AST_INDEX:
        emit_index(e, id);
        break;
    default:
        unsupported(e, n->tok, "this expression");
        PUT(e, "0");
        break;
    }
}

// ---- statements ------------------------------------------------------------------------------

// Is the call `id`, as the last act of function e->fn, one C can make a tail call (M_TAIL:
// clang's and GCC's musttail)? Modal's loops are recursion, and without it a long one runs out
// of stack: at -O0 always (`--run` builds without flags), and even optimized when the C
// compiler chooses not to. musttail needs the same prototype on both sides; with only numbers
// and bools passed, no argument can point into the frame that the callee takes over.
static int scalar_type(TypeId t)
{
    return t == T_VOID || t == T_BOOL || (type_is_int(t) && t != T_INT_LIT);
}

static int tail_call(Emit *e, NodeId id)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    SymId caller = e->fn;
    if (n->kind != AST_CALL || !caller || (n->flags & AST_F_SINK) || sema_primitive(u, id))
    {
        return 0;
    }
    const AstNode *callee_node = NODE(u, n->a);
    SymId fn = u->refs[id];
    uint64_t size;
    if (callee_node->kind != AST_IDENT || TOK(u, callee_node->tok)->kind == TOK_WRITE || !fn ||
        SYM(fn)->kind != SYM_METHOD || !SYM(fn)->decl || SYM(fn)->owner || SYM(caller)->owner ||
        sema_is_std_print(u, u->refs[n->a]) || sema_std_member(u, u->refs[n->a]) == STD_SYS_PUT ||
        sema_sizeof(u, id, &size))
    {
        return 0;
    }
    Unit *home = &cx.units[SYM(fn)->unit], *mine = &cx.units[SYM(caller)->unit];
    TypeId ret = sema_fn_ret(home, fn);
    if (ret != sema_fn_ret(mine, caller) || !scalar_type(ret))
    {
        return 0;
    }
    NodeId p = sema_fn_params(home, fn), q = sema_fn_params(mine, caller);
    for (; p && q; p = NEXT(home, p), q = NEXT(mine, q))
    {
        TypeId t = sema_param_type(home, fn, p);
        if (t != sema_param_type(mine, caller, q) || !scalar_type(t) || t == T_VOID)
        {
            return 0;
        }
    }
    return !p && !q;
}

// `return call` as a tail call when it can be one (the value needs no conversion: the same type).
static void emit_tail(Emit *e, NodeId call)
{
    out_indent(e);
    PUT(e, "M_TAIL ");
    emit_call(e, call);
    PUT(e, ";\n");
}

// An array local: its elements packed, at zero or copied from `init`'s (`c := a` is a copy).
static void emit_array_local(Emit *e, NodeId id, TypeId t, NodeId init)
{
    Unit *u = e->u;
    const AstNode *n = NODE(u, id);
    out_indent(e);
    out_type(e, type_ref(t), n->tok);
    PUT(e, " v_");
    out_tok(e, n->tok);
    char len[96];
    snprintf(len, sizeof(len), "[%u] = {0};\n", type_len(t));
    put(e, len);
    if (!init)
    {
        return;
    }
    out_indent(e);
    PUT(e, "{ const ");
    out_type(e, type_ref(t), n->tok);
    PUT(e, " *m_s = ");
    emit_expr(e, init);
    snprintf(len, sizeof(len), "; for (uint64_t m_k = 0; m_k < %uu; m_k++) v_", type_len(t));
    put(e, len);
    out_tok(e, n->tok);
    PUT(e, "[m_k] = m_s[m_k]; }\n");
}

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
        if (type_kind(u->types[id]) == T_ARRAY)
        {
            emit_array_local(e, id, u->types[id], n->a);
            return;
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
        if (type_kind(u->types[id]) == T_ARRAY) // its elements start at zero, or are init's
        {
            emit_array_local(e, id, u->types[id], n->b);
            return;
        }
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
        if (n->a && NODE(u, n->a)->kind == AST_UNARY_OP && TOK(u, NODE(u, n->a)->tok)->kind == TOK_AMP &&
            NODE(u, NODE(u, n->a)->a)->kind == AST_STRUCT_LIT) // `return &point{ ... }`: built in self
        {
            out_indent(e);
            PUT(e, "*self = ");
            emit_expr(e, NODE(u, n->a)->a);
            PUT(e, ";\n");
            out_indent(e);
            PUT(e, "return self;\n");
            return;
        }
        if (n->a && tail_call(e, n->a))
        {
            emit_tail(e, n->a);
            return;
        }
        if (n->a && NODE(u, n->a)->kind == AST_MATCH && NODE(u, n->a)->a && ret != T_VOID &&
            type_kind(ret) != T_STRUCT)
        {
            out_indent(e);
            emit_match_as(e, n->a, 1);
            PUT(e, "\n");
            return;
        }
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
        out_struct_name(e, SYM(fn)->owner);
        PUT(e, " *self");
        first = 0;
    }
    int index = 0;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p), index++)
    {
        put(e, first ? "" : ", ");
        TypeId pt = sema_param_type(u, fn, p);
        if (type_kind(pt) == T_ARRAY) // by its address; a mutable one is copied in (emit_function)
        {
            PUT(e, "const ");
            out_type(e, type_ref(pt), NODE(u, p)->tok);
            PUT(e, " *");
            out_param_name(e, p, index);
            if (NODE(u, p)->flags & AST_F_MUT)
            {
                PUT(e, "__in");
            }
            first = 0;
            continue;
        }
        out_type(e, pt, NODE(u, p)->tok);
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
    int patterned = 0;
    for (NodeId p = sema_fn_params(u, fn); p && !next; p = NEXT(u, p))
    {
        Pattern pat;
        patterned |= sema_pattern_of(u, p, SYM(fn)->owner, &pat, 0);
    }
    if (!next && !patterned)
    {
        return;
    }
    PUT(e, "    if (!(");
    int index = 0, first = 1;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p), index++)
    {
        Pattern pat;
        if (!sema_pattern_of(u, p, SYM(fn)->owner, &pat, 0))
        {
            continue;
        }
        put(e, first ? "" : " && ");
        first = 0;
        emit_pattern_cond(e, &pat, sema_param_type(u, fn, p), (Subject){p, index, NULL});
    }
    PUT(e, "))\n    {\n        ");
    if (!next) // no clause takes these values: the call does nothing (a nil clause set)
    {
        PUT(e, "return;\n    }\n");
        return;
    }
    int tail = scalar_type(ret) && !SYM(fn)->owner; // the same prototype: a tail call
    for (NodeId p = sema_fn_params(u, fn); p && tail; p = NEXT(u, p))
    {
        tail = scalar_type(sema_param_type(u, fn, p));
    }
    put(e, tail ? "M_TAIL " : ret == T_VOID ? "" : "return ");
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
    put(e, ret == T_VOID && !tail ? ");\n        return;\n    }\n" : ");\n    }\n");
}

static void emit_function(Emit *e, SymId fn)
{
    Unit *u = e->u;
    const AstNode *n = fn_node(u, fn);
    TypeId ret = sema_fn_ret(u, fn);

    emit_signature(e, fn);
    PUT(e, "\n{\n");
    e->indent = 1;
    e->fn = fn;
    if (SYM(fn)->owner)
    {
        PUT(e, "    (void)self;\n");
    }
    int pindex = 0;
    for (NodeId p = sema_fn_params(u, fn); p; p = NEXT(u, p), pindex++) // a mutable array: its own copy
    {
        TypeId pt = sema_param_type(u, fn, p);
        if (type_kind(pt) != T_ARRAY || !(NODE(u, p)->flags & AST_F_MUT))
        {
            continue;
        }
        PUT(e, "    ");
        out_type(e, type_ref(pt), NODE(u, p)->tok);
        put_c(e, ' ');
        out_param_name(e, p, pindex);
        char len[96];
        snprintf(len, sizeof(len), "[%u]; for (uint64_t m_k = 0; m_k < %uu; m_k++) ", type_len(pt), type_len(pt));
        put(e, len);
        out_param_name(e, p, pindex);
        PUT(e, "[m_k] = ");
        out_param_name(e, p, pindex);
        PUT(e, "__in[m_k];\n");
    }
    emit_clause_test(e, fn, ret);
    if (n->flags & AST_F_ONELINE)
    {
        NodeId body = fn_body(u, (NodeId)(n - u->nodes.data));
        if (tail_call(e, body))
        {
            emit_tail(e, body);
        }
        else
        {
            out_indent(e);
            PUT(e, "return ");
            emit_as(e, body, ret);
            PUT(e, ";\n");
        }
    }
    else
    {
        for (NodeId s = NODE(u, fn_body(u, (NodeId)(n - u->nodes.data)))->a; s; s = NEXT(u, s))
        {
            // A void function's last statement, a call of the same prototype: a tail call too.
            if (!NEXT(u, s) && ret == T_VOID && tail_call(e, s))
            {
                emit_tail(e, s);
                continue;
            }
            emit_stmt(e, s, ret);
        }
    }
    e->indent = 0;
    e->fn = 0;
    PUT(e, "}\n\n");
}

// Is this a function the backend can emit? Module-level `self` functions are not, yet.
static int emittable(Emit *e, SymId fn, int report)
{
    if (sema_is_generic(fn))
    {
        return 0; // made once per type: only its specializations are C functions
    }
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

    PUT(e, "struct ");
    out_struct_name(e, self);
    PUT(e, "\n{\n");
    // The fields in the order sema/layout.c put them (largest alignment first, or as written in
    // a register block): C then lays them out at the same offsets.
    NodeId by_offset[64];
    uint32_t nf = 0, total = 0;
    for (NodeId m = n->a; m; m = NEXT(u, m))
    {
        if (NODE(u, m)->kind == AST_STRUCT_DECL)
        {
            unsupported(e, NODE(u, m)->tok, "a struct declared inside another struct");
        }
        if (NODE(u, m)->kind != AST_FIELD_DECL || (self && layout_member_gone(self, m)))
        {
            continue; // no bytes: not declared in C either
        }
        total++;
        uint32_t at = self ? layout_member_offset(self, m) : 0, k = nf < 64 ? nf++ : 64;
        while (k < 64 && k > 0 && layout_member_offset(self, by_offset[k - 1]) > at)
        {
            by_offset[k] = by_offset[k - 1];
            k--;
        }
        if (k < 64)
        {
            by_offset[k] = m;
        }
    }
    int as_written = !self || total > 64;
    uint32_t next = 0;
    for (NodeId m = as_written ? n->a : (nf ? by_offset[0] : 0); m;
         m = as_written ? NEXT(u, m) : (++next < nf ? by_offset[next] : 0))
    {
        const AstNode *member = NODE(u, m);
        if (member->kind != AST_FIELD_DECL || (self && layout_member_gone(self, m)))
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
        if (member->kind != AST_FIELD_DECL || (self && layout_member_gone(self, m)))
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
        PUT(e, "typedef char ");
        out_struct_name(e, self);
        PUT(e, "_layout_");
        put_n(e, TSTART(field), field->len);
        PUT(e, "[offsetof(struct ");
        out_struct_name(e, self);
        PUT(e, ", ");
        put_n(e, TSTART(field), field->len);
        snprintf(check, sizeof(check), ") == %u ? 1 : -1];\n", field_offset_of(s));
        put(e, check);
    }
    if (self && !has_str)
    {
        char check[64];
        PUT(e, "typedef char ");
        out_struct_name(e, self);
        PUT(e, "_size[sizeof(struct ");
        out_struct_name(e, self);
        snprintf(check, sizeof(check), ") == %u ? 1 : -1];\n", type_layout(type_make(T_STRUCT, self)).size);
        put(e, check);
    }
    PUT(e, "\n");
}

// A struct the C program declares: a top-level struct of a file that is part of the program.
static int c_struct(SymId st)
{
    const Symbol *s = SYM(st);
    if (s->kind != SYM_STRUCT || s->owner || !s->decl)
    {
        return 0;
    }
    const Unit *home = &cx.units[s->unit];
    return !home->is_manifest && !home->broken && !home->provided &&
           NODE(home, s->decl)->kind == AST_STRUCT_DECL;
}

// Defines st, after the structs its fields hold by value (C needs those complete first).
static void emit_struct_after_fields(Emit *e, SymId st, uint8_t *defined)
{
    if (defined[st] || !c_struct(st))
    {
        return;
    }
    defined[st] = 1;
    Unit *home = &cx.units[SYM(st)->unit];
    for (NodeId m = NODE(home, SYM(st)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL)
        {
            continue;
        }
        const Token *field = TOK(home, NODE(home, m)->tok);
        SymId f = scope_find(scope_at(SYM(st)->scope_ref), TSTART(field), field->len);
        while (f && SYM(f)->kind != SYM_FIELD)
        {
            f = SYM(f)->next;
        }
        TypeId t = f ? sema_value_type(home, f) : T_NONE;
        if (f && type_kind(t) == T_STRUCT)
        {
            emit_struct_after_fields(e, type_ref(t), defined);
        }
    }
    e->u = home;
    emit_struct(e, SYM(st)->decl);
}

static const char *const prelude =
    "// Generated by the Modal compiler (backend: c). Do not edit.\n"
    "#include <stdbool.h>\n"
    "#include <stddef.h>\n"
    "#include <stdint.h>\n"
    "#include <stdlib.h>\n" // exit
    "#include <unistd.h>\n" // write: the only system interface the program uses (docs/std.md §1)
    "\n"
    "typedef struct\n"
    "{\n"
    "    const uint8_t *ptr;\n"
    "    size_t len;\n"
    "} m_str;\n"
    "\n"
    "#define M_STR(s) ((m_str){(const uint8_t *)(s), sizeof(s) - 1})\n"
    // A tail call (`return f(...)` of the same prototype): Modal's loops are recursion.
    "#if defined(__has_attribute)\n"
    "#if __has_attribute(musttail)\n"
    "#define M_TAIL __attribute__((musttail)) return\n"
    "#endif\n"
    "#endif\n"
    "#ifndef M_TAIL\n"
    "#define M_TAIL return\n"
    "#endif\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#define M_RT static inline __attribute__((unused))\n"
    "#else\n"
    "#define M_RT static inline\n"
    "#endif\n"
    "\n"
    // The program's output: a buffer written with write(2) when full and at the end, as the
    // native runtime does. No stdio: the C backend's programs use only write and exit. (mrt_:
    // no Modal name becomes it, so a program's own `out` or `put` cannot clash.)
    "static uint8_t mrt_buf[4096];\n"
    "static size_t mrt_len;\n"
    "M_RT void mrt_out(int fd, const uint8_t *p, size_t n) {\n"
    "    while (n) { ssize_t k = write(fd, p, n); if (k <= 0) return; p += k; n -= (size_t)k; }\n"
    "}\n"
    "M_RT void mrt_flush(void) { mrt_out(1, mrt_buf, mrt_len); mrt_len = 0; }\n"
    "M_RT void mrt_put(const uint8_t *p, size_t n) {\n"
    "    if (n > sizeof(mrt_buf) - mrt_len) { mrt_flush(); if (n > sizeof(mrt_buf)) { mrt_out(1, p, n); return; } }\n"
    "    for (size_t i = 0; i < n; i++) mrt_buf[mrt_len + i] = p[i];\n"
    "    mrt_len += n;\n"
    "}\n"
    "M_RT void m_write_str(m_str s) { mrt_put(s.ptr, s.len); }\n"
    // A trap: the output so far, then the place, then status 1 (as the native backends do).
    "M_RT void m_trap(const char *m) {\n"
    "    size_t n = 0; while (m[n]) n++;\n"
    "    mrt_flush(); mrt_out(2, (const uint8_t *)m, n); exit(1);\n"
    "}\n"
    // a part of trap(a, b, ...) (docs/std.md): written after the output so far, then the next
    "M_RT void m_trap_part(m_str s) { mrt_flush(); mrt_out(2, s.ptr, s.len); }\n"
    // std's any (std/any.modal): T(a) gives a's address when a holds a T, and traps otherwise.
    "M_RT void *m_unany(const uint64_t *a, uint64_t id, const char *m) {\n"
    "    if (((const uint64_t *)(uintptr_t)a[1])[0] != id) m_trap(m); return (void *)(uintptr_t)a[0];\n"
    "}\n"
    // Division: a zero divisor traps; the smallest signed value by -1 wraps (as `0 - x` does).
    "#define M_DIV(T) M_RT T m_div_##T(T a, T b, const char *m) { if (!b) m_trap(m); \\\n"
    "    if ((T)-1 < 0 && b == (T)-1) return (T)(0 - (uint64_t)a); return (T)(a / b); }\n"
    "M_DIV(uint8_t) M_DIV(uint16_t) M_DIV(uint32_t) M_DIV(uint64_t)\n"
    "M_DIV(int8_t) M_DIV(int16_t) M_DIV(int32_t) M_DIV(int64_t)\n"
    // An index: past the length, the program stops with its place (as in the native code).
    "M_RT uint64_t m_index(uint64_t i, uint64_t n, const char *m) { if (i >= n) m_trap(m); return i; }\n"
    "M_RT void m_write_uint(unsigned long long n) {\n"
    "    uint8_t b[24]; int i = 24;\n"
    "    do { b[--i] = (uint8_t)('0' + n % 10); n /= 10; } while (n);\n"
    "    mrt_put(b + i, (size_t)(24 - i));\n"
    "}\n"
    "M_RT void m_write_int(long long n) {\n"
    "    if (n < 0) { uint8_t minus = '-'; mrt_put(&minus, 1); m_write_uint(0ull - (unsigned long long)n); }\n"
    "    else m_write_uint((unsigned long long)n);\n"
    "}\n"
    "M_RT void m_write_bool(bool b) { m_write_str(b ? M_STR(\"true\") : M_STR(\"false\")); }\n"
    "M_RT void m_write_byte(uint8_t b) { mrt_put(&b, 1); }\n"
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
            if (NODE(home, m)->kind != AST_FIELD_DECL || layout_member_gone(st, m))
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

    // Other files (std, imported modules) give their functions to the program: each is emitted
    // in its own file's context, named m_u<file>_... Their types and module-level values are
    // the next step, and are reported where they are declared.
    for (uint16_t i = 1; i < cx.nunits; i++)
    {
        Unit *m = &cx.units[i];
        if (m->is_manifest || m->broken || m->provided) // (target's values are compile time only)
        {
            continue;
        }
        e->u = m;
        for (NodeId d = NODE(m, m->root)->a; d; d = NEXT(m, d))
        {
            const AstNode *n = NODE(m, d);
            int value = n->kind == AST_FIELD_DECL ||
                        (n->kind == AST_DEFINE && NODE(m, n->a)->kind != AST_FN_LIT && !ast_import_root(m, n->a));
            if (value)
            {
                unsupported(e, n->tok, "a module-level value in an imported file");
            }
        }
        e->u = u;
    }

    put(e, prelude);

    // 1. Types, of every file the program reaches: each struct is declared before any is
    //    defined, so they can refer to each other through pointers in any order, and each is
    //    defined after the structs it holds by value.
    for (SymId st = 1; st < cx.syms.size; st++)
    {
        if (c_struct(st))
        {
            PUT(e, "typedef struct ");
            out_struct_name(e, st);
            put_c(e, ' ');
            out_struct_name(e, st);
            PUT(e, ";\n");
        }
    }
    PUT(e, "\n");
    uint8_t *defined = calloc(cx.syms.size, 1);
    for (SymId st = 1; st < cx.syms.size; st++)
    {
        emit_struct_after_fields(e, st, defined);
    }
    free(defined);
    e->u = u;

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
        Unit *home = &cx.units[SYM(fn)->unit];
        if (SYM(fn)->kind != SYM_METHOD || home->is_manifest || home->broken)
        {
            continue;
        }
        e->u = home;
        if (emittable(e, fn, 1))
        {
            emit_signature(e, fn);
            PUT(e, ";\n");
        }
        e->u = u;
    }
    PUT(e, "\n");

    // Type descriptors (std's type), for typeof and *any: data that holds addresses.
    emit_descriptors(e);

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

// A specialization made while checking: its prototype now, between two functions, so the
// function that calls it (written before it) can.
static void backend_c_declare(const BackendJob *job, SymId fn)
{
    (void)job;
    Emit *e = &emit_state;
    Unit *saved = e->u;
    e->u = &cx.units[SYM(fn)->unit];
    if (emittable(e, fn, 0))
    {
        emit_signature(e, fn);
        PUT(e, ";\n");
    }
    e->u = saved;
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
    Unit *caller = emit_state.u;
    emit_state.u = &cx.units[SYM(fn)->unit];
    if (emittable(&emit_state, fn, 0))
    {
        emit_function(&emit_state, fn);
    }
    emit_state.u = caller;
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
        PUT(e, "();\n    mrt_flush();\n    return 0;\n}\n");
    }
    else
    {
        PUT(e, "int main(void)\n{\n    int status = (int)");
        out_fn_name(e, entry);
        PUT(e, "();\n    mrt_flush();\n    return status;\n}\n");
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
    b->declare = backend_c_declare;
    b->end = backend_c_end;
}
