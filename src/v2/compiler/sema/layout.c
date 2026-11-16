// layout.c (v2): the target being compiled for, and the size, alignment and field offsets of
// every type on it.
//
// Nothing else in the compiler may assume a pointer is 8 bytes or that memory is little-endian:
// it asks here. Each field goes at the next multiple of its alignment and the size is rounded up
// to the largest alignment, as in C; the fields go largest alignment first (no padding between
// them), except in a register block, which is laid out as written over a device's registers
// (docs/context.md §14). Every backend asks here, so a struct is the same bytes in each.
#include "v2/compiler/cx.h"
#include "v2/compiler/syntax/scanner.h"
#include <stdlib.h>
#include <string.h>

// name, cpu, os, pointer size, word size, i64 alignment, big-endian, 64-bit arithmetic native,
// capabilities
static const MachineTarget target_arm64_macos = {"arm64-macos", TCPU_ARM64, TOS_MACOS, 8, 8, 8, 0, 1, TCAP_SYSOUT | TCAP_EXIT};
static const MachineTarget target_x86_64_macos = {"x86_64-macos", TCPU_X86_64, TOS_MACOS, 8, 8, 8, 0, 1, TCAP_SYSOUT | TCAP_EXIT};
static const MachineTarget target_arm64_linux = {"arm64-linux", TCPU_ARM64, TOS_LINUX, 8, 8, 8, 0, 1, TCAP_SYSOUT | TCAP_EXIT};
static const MachineTarget target_x86_64_linux = {"x86_64-linux", TCPU_X86_64, TOS_LINUX, 8, 8, 8, 0, 1, TCAP_SYSOUT | TCAP_EXIT};

// Bare metal: no system output, no process to exit. The memory map is in mod.modal.
static const MachineTarget target_arm64_none = {"arm64-none", TCPU_ARM64, TOS_NONE, 8, 8, 8, 0, 1, 0};
static const MachineTarget target_x86_64_none = {"x86_64-none", TCPU_X86_64, TOS_NONE, 8, 8, 8, 0, 1, 0};

const MachineTarget *const targets[] = {&target_arm64_macos, &target_arm64_linux,
                                        &target_arm64_none,  &target_x86_64_macos,
                                        &target_x86_64_linux, &target_x86_64_none, NULL};

const MachineTarget *target_find(const char *name)
{
    for (int i = 0; targets[i]; i++)
    {
        if (strcmp(targets[i]->name, name) == 0)
        {
            return targets[i];
        }
    }
    return NULL;
}

void target_set(const MachineTarget *t)
{
    cx.target = t;
}

const MachineTarget *target_get(void)
{
    if (!cx.target)
    {
#if defined(__x86_64__)
        cx.target = &target_x86_64_macos;
#else
        cx.target = &target_arm64_macos;
#endif
    }
    return cx.target;
}

// ---- per-struct cache ------------------------------------------------------------------------------
//
// Computed on first use and kept for the whole run: one 8-byte entry per struct symbol (its size
// and alignment) and one 4-byte entry per field symbol (its offset + 1, 0: not computed).

typedef struct
{
    uint32_t size;
    uint16_t align;
    uint8_t state; // 0: not computed, 1: being computed (a struct inside itself), 2: done
    uint8_t ok;    // every field has a layout
} StructLayout;

static StructLayout *structs;
static uint32_t *field_off;
static size_t cache_cap;

static void cache_fit(void)
{
    if (cache_cap >= cx.syms.cap)
    {
        return;
    }
    size_t cap = cx.syms.cap;
    StructLayout *s = (StructLayout *)arena_alloc(cap * sizeof(StructLayout));
    uint32_t *f = (uint32_t *)arena_alloc(cap * sizeof(uint32_t));
    memset(s, 0, cap * sizeof(StructLayout));
    memset(f, 0, cap * sizeof(uint32_t));
    if (cache_cap)
    {
        memcpy(s, structs, cache_cap * sizeof(StructLayout));
        memcpy(f, field_off, cache_cap * sizeof(uint32_t));
    }
    structs = s;
    field_off = f;
    cache_cap = cap;
}

// ---- what the program does with each field --------------------------------------------------
//
// Code is made while the program is checked, so the uses of a field across the whole program are
// not known from the checker when a struct is first laid out. They are read from the tokens of
// every file instead, once, when the first struct is laid out: by name, so a fact about `x` holds
// for every field called x (it can only keep a field, never drop one in use).
//   - a read is `.x` not followed by `=` (a write) or `(` (a call); with implicit self off,
//     a field is reached no other way
//   - a write is `.x = v`, or `x: v` / `x` in a literal `T{ ... }`; a constant when v is one
//     number or bool alone
//   - `T.m(...)` makes a T at zero, as `T x` and `[N]T x` do; `sizeof(T)` and the other queries look
//     at T's layout
// A field nobody reads is left out of the layout. A field every write gives the same constant
// (0 also when a T is made at zero) is that constant. Neither for a register block, a struct a
// type query looks at, or when a domain has implicit self.
enum { FN_READ = 1, FN_WRITTEN = 2, FN_VARIES = 4, FN_PINNED = 8, FN_ZEROED = 16 };
static struct
{
    uint8_t done, off, pin_all;
    uint8_t *bits;
    uint64_t *value;
    uint32_t n;
    uint8_t *state; // per field symbol: 0 not decided, 1 + FIELD_*
    uint64_t *field_value;
    uint32_t nstate;
} F;

static void fact_fit(NameId name)
{
    if (name < F.n)
    {
        return;
    }
    uint32_t n = F.n ? F.n : 256;
    while (n <= name)
    {
        n *= 2;
    }
    F.bits = realloc(F.bits, n);
    F.value = realloc(F.value, n * sizeof(uint64_t));
    memset(F.bits + F.n, 0, n - F.n);
    memset(F.value + F.n, 0, (n - F.n) * sizeof(uint64_t));
    F.n = n;
}

// The constant a write gives, when it is one number or bool alone, then the end of the value.
static int written_constant(const Token *t, size_t i, size_t end, uint64_t *v)
{
    int neg = 0;
    if (i < end && t[i].kind == TOK_MINUS)
    {
        neg = 1;
        i++;
    }
    if (i + 1 != end)
    {
        return 0;
    }
    const Token *k = &t[i];
    if (k->kind == TOK_NUMBER)
    {
        uint64_t x;
        if (!number_value(TSTART(k), k->len, &x, NULL))
        {
            return 0;
        }
        *v = neg ? (uint64_t)0 - x : x;
        return 1;
    }
    if (!neg && k->kind == TOK_IDENTIFIER && (k->len == 4 || k->len == 5))
    {
        if (memcmp(TSTART(k), "true", 4) == 0 && k->len == 4)
        {
            *v = 1;
            return 1;
        }
        if (memcmp(TSTART(k), "false", 5) == 0 && k->len == 5)
        {
            *v = 0;
            return 1;
        }
    }
    return 0;
}

static void note_write(NameId name, int constant, uint64_t v)
{
    fact_fit(name);
    if (!constant)
    {
        F.bits[name] |= FN_WRITTEN | FN_VARIES;
    }
    else if (!(F.bits[name] & FN_WRITTEN))
    {
        F.bits[name] |= FN_WRITTEN;
        F.value[name] = v;
    }
    else if (F.value[name] != v)
    {
        F.bits[name] |= FN_VARIES;
    }
}

// Where a statement's value ends: the next token that starts a line, closes the block, or ends
// the file (brackets skipped whole).
static size_t value_end(const Token *t, size_t i, size_t n)
{
    for (; i < n && t[i].kind != TOK_EOF; i++)
    {
        if (t[i].nl || t[i].kind == TOK_RBRACE || t[i].kind == TOK_SEMICOLON || t[i].kind == TOK_COMMA)
        {
            return i;
        }
        if ((t[i].kind == TOK_LPAREN || t[i].kind == TOK_LBRACKET || t[i].kind == TOK_LBRACE) && t[i].match > i)
        {
            i = t[i].match;
        }
    }
    return i;
}

static int is_query(const Token *k)
{
    static const char *const q[] = {"sizeof", "alignof", "fieldsof", "offsetof", "typeof"};
    for (int j = 0; j < 5; j++)
    {
        if (k->len == strlen(q[j]) && memcmp(TSTART(k), q[j], k->len) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void scan_facts(const Token *t, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++)
    {
        const Token *k = &t[i];
        if (k->kind == TOK_DOT && t[i + 1].kind == TOK_IDENTIFIER)
        {
            NameId name = t[i + 1].match;
            TokenKind next = i + 2 < n ? (TokenKind)t[i + 2].kind : TOK_EOF;
            fact_fit(name);
            if (next == TOK_EQ)
            {
                size_t end = value_end(t, i + 3, n);
                uint64_t v = 0;
                int c = written_constant(t, i + 3, end, &v);
                note_write(name, c, v);
            }
            else if (next != TOK_LPAREN)
            {
                F.bits[name] |= FN_READ;
            }
            else if (i > 0 && t[i - 1].kind == TOK_IDENTIFIER) // `T.m(...)`: maybe a T made at zero
            {
                fact_fit(t[i - 1].match);
                F.bits[t[i - 1].match] |= FN_ZEROED;
            }
        }
        else if (k->kind == TOK_IDENTIFIER && t[i + 1].kind == TOK_IDENTIFIER && !t[i + 1].nl &&
                 (k->nl || (i > 0 && t[i - 1].kind == TOK_RBRACKET)))
        {
            // `person p` (a local, first on its line), `[3]person ps`: a T (or N of them) made at
            // zero (a reference, `*item it`, makes none)
            fact_fit(k->match);
            F.bits[k->match] |= FN_ZEROED;
        }
        else if (k->kind == TOK_IDENTIFIER && t[i + 1].kind == TOK_LBRACE && t[i + 1].off == k->off + k->len &&
                 t[i + 1].match > i + 1) // `T{ ... }`, the brace right after the name: a literal
        {
            size_t close = t[i + 1].match;
            for (size_t j = i + 2; j < close;)
            {
                if (t[j].kind != TOK_IDENTIFIER)
                {
                    j = value_end(t, j, close) + 1;
                    continue;
                }
                NameId name = t[j].match;
                if (t[j + 1].kind == TOK_COLON)
                {
                    size_t end = value_end(t, j + 2, close);
                    uint64_t v = 0;
                    int c = written_constant(t, j + 2, end, &v);
                    note_write(name, c, v);
                    j = end + 1;
                }
                else // `x` alone: the local x, whatever it holds
                {
                    note_write(name, 0, 0);
                    j = value_end(t, j, close) + 1;
                }
            }
        }
        else if (k->kind == TOK_IDENTIFIER && i > 0 && (t[i - 1].kind == TOK_LPAREN || t[i - 1].kind == TOK_COMMA) &&
                 (t[i + 1].kind == TOK_COMMA || t[i + 1].kind == TOK_RPAREN))
        {
            // a name alone as an argument: when it names a struct, it is a type given as a value
            // (`size_of(rgb, &c)`), whose descriptor reports the layout as declared
            fact_fit(k->match);
            F.bits[k->match] |= FN_PINNED;
        }
        else if (k->kind == TOK_IDENTIFIER && is_query(k) && t[i + 1].kind == TOK_LPAREN && i + 2 < n &&
                 t[i + 2].kind == TOK_IDENTIFIER)
        {
            const Token *arg = &t[i + 2];
            fact_fit(arg->match);
            F.bits[arg->match] |= FN_PINNED;
            // a value (not a type name) could be of any struct: then every layout stays as declared
            TypeId prim = T_NONE;
            static const char *const prims[] = {"u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "bool", "int"};
            for (int p = 0; p < 10; p++)
            {
                if (arg->len == strlen(prims[p]) && memcmp(TSTART(arg), prims[p], arg->len) == 0)
                {
                    prim = T_U8;
                }
            }
            int a_struct = 0;
            for (SymId s = 1; prim == T_NONE && !a_struct && s < cx.syms.size; s++)
            {
                a_struct = (SYM(s)->kind == SYM_STRUCT || SYM(s)->kind == SYM_IMPORT) && cx.sym_names[s] == arg->match;
            }
            F.pin_all |= prim == T_NONE && !a_struct;
        }
    }
}

static void gather_facts(void)
{
    if (F.done)
    {
        return;
    }
    F.done = 1;
    for (uint16_t d = 0; d < cx.ndomains; d++)
    {
        if (domain_config_bool(d, "implicit_self", 0))
        {
            F.off = 1; // a bare name may be a field: the tokens cannot tell
        }
    }
    for (uint16_t i = 0; i < cx.nunits && !F.off; i++)
    {
        Unit *u = &cx.units[i];
        if (u->broken || u->provided || !u->src)
        {
            F.off |= u->broken;
            continue;
        }
        if (cx.eager)
        {
            scan_facts(u->tokens.data, u->tokens.size);
            continue;
        }
        TokenVec all = {0};
        scan_quiet = 1;
        int errors = scan_whole(u->path, u->src, u->len, &all);
        scan_quiet = 0;
        if (errors == 0)
        {
            scan_facts(all.data, all.size);
        }
        else
        {
            F.off = 1;
        }
        free(all.data);
    }
}

// The field symbol a field declaration of struct s declares.
static SymId field_of(SymId s, const Unit *home, NodeId m)
{
    const Token *name = TOK(home, NODE(home, m)->tok);
    SymId f = scope_find(scope_at(SYM(s)->scope_ref), TSTART(name), name->len);
    while (f && SYM(f)->kind != SYM_FIELD)
    {
        f = SYM(f)->next;
    }
    return f;
}

// The struct's own name and the names import bindings give it: whether any is marked.
// The bits the import bindings give each struct, gathered once per symbol: each new symbol is
// looked at once (a walk of every symbol per field was O(fields x symbols): 12,000 structs took
// 0.5 s in emit).
static uint8_t *binding_bits;
static uint32_t binding_bits_cap, binding_bits_upto = 1;

static int struct_marked(SymId s, uint8_t bit)
{
    NameId own = cx.sym_names[s];
    if (own < F.n && (F.bits[own] & bit))
    {
        return 1;
    }
    for (; binding_bits_upto < cx.syms.size; binding_bits_upto++)
    {
        SymId b = binding_bits_upto;
        if (SYM(b)->kind != SYM_IMPORT || cx.sym_names[b] >= F.n || !F.bits[cx.sym_names[b]])
        {
            continue;
        }
        SymId t = module_binding_target(b);
        if (!t)
        {
            continue;
        }
        if (t >= binding_bits_cap)
        {
            uint32_t n = binding_bits_cap ? binding_bits_cap : 256;
            while (n <= t)
            {
                n *= 2;
            }
            binding_bits = realloc(binding_bits, n);
            memset(binding_bits + binding_bits_cap, 0, n - binding_bits_cap);
            binding_bits_cap = n;
        }
        binding_bits[t] |= F.bits[cx.sym_names[b]];
    }
    return s < binding_bits_cap && (binding_bits[s] & bit);
}

int layout_field_use(SymId field, uint64_t *value)
{
    if (field >= F.nstate)
    {
        uint32_t n = F.nstate ? F.nstate : 256;
        while (n <= field)
        {
            n *= 2;
        }
        F.state = realloc(F.state, n);
        F.field_value = realloc(F.field_value, n * sizeof(uint64_t));
        memset(F.state + F.nstate, 0, n - F.nstate);
        F.nstate = n;
    }
    if (!F.state[field])
    {
        int use = FIELD_KEPT;
        SymId s = SYM(field)->owner;
        gather_facts();
        if (!F.off && !F.pin_all && SYM(field)->kind == SYM_FIELD && s && !sema_is_register_block(s) &&
            !sema_is_any(s) && !(sema_std_type() && SYM(s)->unit == SYM(sema_std_type())->unit) &&
            !struct_marked(s, FN_PINNED)) // std's any, type and member: read by the compiler
        {
            NameId name = cx.sym_names[field];
            uint8_t bits = name < F.n ? F.bits[name] : 0;
            TypeId t = sema_value_type(&cx.units[SYM(field)->unit], field);
            TypeKind k = type_kind(t);
            int scalar = type_is_int(t) || k == T_BOOL;
            uint64_t v = (bits & FN_WRITTEN) ? F.value[name] : 0;
            if (!(bits & FN_READ) && k != T_STRUCT)
            {
                use = FIELD_UNREAD;
            }
            else if (scalar && !(bits & FN_VARIES) && (v == 0 || !struct_marked(s, FN_ZEROED)))
            {
                use = FIELD_CONSTANT;
                F.field_value[field] = v;
            }
        }
        F.state[field] = (uint8_t)(1 + use);
    }
    if (value)
    {
        *value = F.field_value[field];
    }
    return F.state[field] - 1;
}

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

// The layout of struct `s`. When `want` is one of its field declarations, *want_off gets that
// field's offset (field offsets are found again each time: a struct has few fields).
static Layout struct_layout_at(SymId s, NodeId want, uint32_t *want_off)
{
    cache_fit();
    StructLayout *sl = &structs[s];
    if (sl->state == 2 && !want)
    {
        return (Layout){sl->ok ? sl->size : 0, sl->align};
    }
    if (sl->state == 1)
    {
        return (Layout){0, 1}; // contains itself: no layout (the checker reports it)
    }
    sl->state = 1;
    Unit *home = &cx.units[SYM(s)->unit];
    uint32_t off = 0, end = 0, align = 1;
    int ok = 1;

    // A struct whose fields give no offsets is laid out by alignment, largest first (stable:
    // equal alignments keep their order), so no padding sits between its fields: fewer bytes to
    // zero, copy and keep in the cache. A register block (`u32 dr @ 0x00`) is laid out as written.
    enum { MAX_SORTED = 64 };
    NodeId order[MAX_SORTED];
    Layout lay[MAX_SORTED];
    uint32_t n = 0, total = 0;
    int placed = 0;
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind == AST_FIELD_DECL)
        {
            total++;
            placed |= (NODE(home, m)->flags & AST_F_PLACED) != 0;
        }
    }
    int sorted = !placed && total <= MAX_SORTED;
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL)
        {
            continue;
        }
        SymId fsym = placed ? 0 : field_of(s, home, m);
        if (fsym && layout_field_use(fsym, NULL) != FIELD_KEPT)
        {
            continue; // no bytes: nothing reads it, or it is a constant
        }
        Layout fl = type_layout(sema_type_of(home, NODE(home, m)->a, s));
        if (!fl.size)
        {
            ok = 0;
            fl = (Layout){8, 8};
        }
        if (sorted) // insertion: after every field of the same or a larger alignment
        {
            uint32_t k = n++;
            while (k > 0 && lay[k - 1].align < fl.align)
            {
                order[k] = order[k - 1];
                lay[k] = lay[k - 1];
                k--;
            }
            order[k] = m;
            lay[k] = fl;
            continue;
        }
        const AstNode *fd = NODE(home, m);
        if (fd->flags & AST_F_PLACED) // a register: exactly where it says (checked in check.c)
        {
            const Token *t = TOK(home, NODE(home, fd->b)->tok);
            uint64_t at = 0;
            number_value(TSTART(t), t->len, &at, NULL);
            off = (uint32_t)at;
        }
        else
        {
            off = align_up(off, fl.align);
        }
        if (m == want)
        {
            *want_off = off;
        }
        off += fl.size;
        end = off > end ? off : end;
        off = end;
        align = fl.align > align ? fl.align : align;
    }
    for (uint32_t i = 0; sorted && i < n; i++)
    {
        off = align_up(off, lay[i].align);
        if (order[i] == want)
        {
            *want_off = off;
        }
        off += lay[i].size;
        end = off;
        align = lay[i].align > align ? lay[i].align : align;
    }
    // An empty struct still has one byte, as in C++: two values never share an address.
    sl->size = end ? align_up(end, align) : 1;
    sl->align = (uint16_t)align;
    sl->ok = (uint8_t)ok;
    sl->state = 2;
    return (Layout){ok ? sl->size : 0, sl->align};
}

static Layout struct_layout(SymId s)
{
    return struct_layout_at(s, 0, NULL);
}

Layout type_layout(TypeId t)
{
    const MachineTarget *tg = target_get();
    switch (type_kind(t))
    {
    case T_BOOL:
    case T_U8:
    case T_I8:
        return (Layout){1, 1};
    case T_U16:
    case T_I16:
        return (Layout){2, 2};
    case T_U32:
    case T_I32:
    case T_INT:
    case T_INT_LIT:
        return (Layout){4, 4};
    case T_U64:
    case T_I64:
    case T_ERROR:
        return (Layout){8, tg->align_i64};
    case T_STR: // the native string: one pointer to its length and bytes
    case T_PTR:
        return (Layout){tg->ptr_size, tg->ptr_size};
    case T_STRUCT:
        return struct_layout(type_ref(t));
    case T_ARRAY: // its elements, one after another
    {
        Layout e = type_layout(type_ref(t));
        return (Layout){e.size * type_len(t), e.align};
    }
    default:
        return (Layout){0, 1}; // no runtime representation (yet)
    }
}

uint32_t field_offset_of(SymId field)
{
    cache_fit();
    if (!field_off[field])
    {
        uint32_t off = 0;
        struct_layout_at(SYM(field)->owner, SYM(field)->decl, &off);
        field_off[field] = off + 1;
    }
    return field_off[field] - 1;
}

uint32_t layout_member_offset(SymId strct, NodeId member)
{
    uint32_t off = 0;
    struct_layout_at(strct, member, &off);
    return off;
}

int layout_member_gone(SymId strct, NodeId member)
{
    const Unit *home = &cx.units[SYM(strct)->unit];
    SymId f = sema_is_register_block(strct) ? 0 : field_of(strct, home, member);
    return f && layout_field_use(f, NULL) != FIELD_KEPT;
}
