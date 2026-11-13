// types.h (v2): the type table.
//
// A type is 8 bytes and is referred to by a 32-bit TypeId. Primitive types have fixed ids;
// struct, pointer and slice types are interned, so two equal types always have the same id
// and comparing types is comparing integers.
#ifndef MODAL_V2_TYPES_H
#define MODAL_V2_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t TypeId;

typedef enum
{
    T_NONE, // not known: an error was already reported, or the expression has no type
    T_VOID,
    T_BOOL,
    T_NIL,
    T_INT_LIT, // an integer literal: becomes whatever integer type it meets
    T_U8,
    T_U16,
    T_U32,
    T_U64,
    T_I8,
    T_I16,
    T_I32,
    T_I64,
    T_INT,
    T_STR,    // []u8
    T_EXTERN, // comes from outside the checked program (std): anything is allowed on it
    T_MODULE, // `self` of a module-level function
    T__PRIM_COUNT,
    // interned: `ref` says of what
    T_STRUCT = T__PRIM_COUNT, // ref = struct symbol
    T_PTR,                    // ref = pointee type
    T_SLICE,                  // ref = element type
    T_FUNC,                   // ref = function symbol (first of its overload set)
    T_TYPE,                   // ref = struct symbol: the type itself used as a value, `T.new()`
    T_ARRAY,                  // ref = element type, len = its count: `[24]u8`
} TypeKind;

typedef struct
{
    uint8_t kind;
    uint32_t ref;
    uint32_t len; // T_ARRAY: the number of elements
} Type;

typedef struct
{
    Type *data;
    size_t size;
    size_t cap;
} TypeVec;

void types_init(void);
TypeId type_make(TypeKind kind, uint32_t ref);
TypeId type_array(TypeId elem, uint32_t len); // [len]elem
uint32_t type_len(TypeId t);                    // a T_ARRAY's count
TypeKind type_kind(TypeId t);
uint32_t type_ref(TypeId t);
int type_is_int(TypeId t); // a sized integer or an integer literal
// Source-like spelling of a type, for diagnostics; returns buf.
char *type_str(TypeId t, char *buf, size_t cap);

#endif // MODAL_V2_TYPES_H
