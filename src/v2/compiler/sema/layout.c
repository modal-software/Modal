// layout.c (v2): the target being compiled for, and the size, alignment and field offsets of
// every type on it.
//
// Nothing else in the compiler may assume a pointer is 8 bytes or that memory is little-endian:
// it asks here. Struct layout is C's (each field at the next multiple of its alignment, the size
// rounded up to the largest alignment), so a struct is the same bytes in every backend and can be
// laid over a device's registers (docs/context.md §14).
#include "v2/compiler/cx.h"
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
    for (NodeId m = NODE(home, SYM(s)->decl)->a; m; m = NEXT(home, m))
    {
        if (NODE(home, m)->kind != AST_FIELD_DECL)
        {
            continue;
        }
        Layout fl = type_layout(sema_type_of(home, NODE(home, m)->a, s));
        if (!fl.size)
        {
            ok = 0;
            fl = (Layout){8, 8};
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
        return (Layout){8, tg->align_i64};
    case T_STR: // the native string: one pointer to its length and bytes
    case T_PTR:
        return (Layout){tg->ptr_size, tg->ptr_size};
    case T_STRUCT:
        return struct_layout(type_ref(t));
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
