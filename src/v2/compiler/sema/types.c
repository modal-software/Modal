#include "v2/compiler/sema/types.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

// Interning table: open addressing over (kind, ref), holding TypeIds. 0 = empty slot.
static uint32_t *type_map;
static size_t type_map_cap;

static size_t type_slot(uint8_t kind, uint32_t ref, size_t cap)
{
    return (size_t)((ref * 2654435761u) ^ ((uint32_t)kind * 40503u)) & (cap - 1);
}

static void type_map_put(TypeId id)
{
    const Type *t = &cx.types.data[id];
    size_t i = type_slot(t->kind, t->ref, type_map_cap);
    while (type_map[i])
    {
        i = (i + 1) & (type_map_cap - 1);
    }
    type_map[i] = id;
}

static TypeId type_push(uint8_t kind, uint32_t ref, uint32_t len)
{
    if (cx.types.size == cx.types.cap)
    {
        cx.types.data = (Type *)region_grow_array(REGION_TYPES, cx.types.data, sizeof(Type),
                                                  &cx.types.cap, cx.types.size + 1);
    }
    cx.types.data[cx.types.size] = (Type){kind, ref, len};
    return (TypeId)cx.types.size++;
}

void types_init(void)
{
    for (uint8_t k = 0; k < T__PRIM_COUNT; k++)
    {
        type_push(k, 0, 0); // a primitive's id is its kind
    }
    type_map_cap = 64;
    type_map = (uint32_t *)arena_alloc(type_map_cap * sizeof(uint32_t));
    memset(type_map, 0, type_map_cap * sizeof(uint32_t));
}

static TypeId type_intern(TypeKind kind, uint32_t ref, uint32_t len)
{
    if (kind < T__PRIM_COUNT)
    {
        return (TypeId)kind;
    }
    for (size_t i = type_slot((uint8_t)kind, ref, type_map_cap); type_map[i];
         i = (i + 1) & (type_map_cap - 1))
    {
        const Type *t = &cx.types.data[type_map[i]];
        if (t->kind == kind && t->ref == ref && t->len == len)
        {
            return type_map[i];
        }
    }

    TypeId id = type_push((uint8_t)kind, ref, len);
    if ((cx.types.size - T__PRIM_COUNT) * 2 > type_map_cap) // keep the table under half full
    {
        type_map_cap *= 2;
        type_map = (uint32_t *)arena_alloc(type_map_cap * sizeof(uint32_t));
        memset(type_map, 0, type_map_cap * sizeof(uint32_t));
        for (TypeId t = T__PRIM_COUNT; t < cx.types.size; t++)
        {
            type_map_put(t);
        }
    }
    else
    {
        type_map_put(id);
    }
    return id;
}

TypeId type_make(TypeKind kind, uint32_t ref)
{
    return type_intern(kind, ref, 0);
}

TypeId type_array(TypeId elem, uint32_t len)
{
    return type_intern(T_ARRAY, elem, len);
}

uint32_t type_len(TypeId t)
{
    return cx.types.data[t].len;
}

TypeKind type_kind(TypeId t)
{
    return (TypeKind)cx.types.data[t].kind;
}

uint32_t type_ref(TypeId t)
{
    return cx.types.data[t].ref;
}

int type_is_int(TypeId t)
{
    return t >= T_INT_LIT && t <= T_INT;
}

char *type_str(TypeId t, char *buf, size_t cap)
{
    static const char *const prims[T__PRIM_COUNT] = {
        [T_NONE] = "?",        [T_VOID] = "nothing", [T_BOOL] = "bool", [T_NIL] = "nil",
        [T_INT_LIT] = "int",   [T_U8] = "u8",        [T_U16] = "u16",   [T_U32] = "u32",
        [T_U64] = "u64",       [T_I8] = "i8",        [T_I16] = "i16",   [T_I32] = "i32",
        [T_I64] = "i64",       [T_INT] = "int",      [T_STR] = "[]u8",  [T_EXTERN] = "external",
        [T_MODULE] = "module",
    };
    size_t n = 0;

    buf[0] = '\0';
    for (;;)
    {
        TypeKind k = type_kind(t);
        if (k < T__PRIM_COUNT)
        {
            snprintf(buf + n, cap - n, "%s", prims[k]);
            return buf;
        }
        if (k == T_ARRAY)
        {
            n += (size_t)snprintf(buf + n, cap - n, "[%u]", type_len(t));
            if (n >= cap)
            {
                return buf;
            }
            t = type_ref(t);
            continue;
        }
        if (k == T_PTR || k == T_SLICE)
        {
            n += (size_t)snprintf(buf + n, cap - n, "%s", k == T_PTR ? "*" : "[]");
            if (n >= cap)
            {
                return buf;
            }
            t = type_ref(t);
            continue;
        }
        int len;
        const char *name = symbol_name(type_ref(t), &len);
        snprintf(buf + n, cap - n, k == T_FUNC ? "function %.*s" : "%.*s", len, name);
        return buf;
    }
}
