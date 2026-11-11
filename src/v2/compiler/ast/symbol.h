// symbol.h (v2): symbols, lexical scopes and visibility.
//
// A symbol is 24 bytes and refers back to its name through the token stream (no copies).
// Visibility needs no keyword and no list; it follows from where a thing is written:
//   inside a struct's braces   -> private to the struct (fields always are)
//   outside, with `self: T`    -> the struct's face
//   inside a file              -> private to the domain (folder)
//   bound in the mod.modal     -> part of the domain's face
// See docs/v2-frontend.md.
#ifndef MODAL_V2_SYMBOL_H
#define MODAL_V2_SYMBOL_H

#include "v2/compiler/ast/ast.h"
#include <stdint.h>

typedef uint32_t SymId; // 0 = no symbol

typedef enum
{
    SYM_NONE,
    SYM_STRUCT,
    SYM_FIELD,
    SYM_METHOD,
    SYM_CONST,
    SYM_IMPORT,
} SymKind;

enum
{
    SYM_F_MUT = 1u << 0,     // mutable field, or overloadable (type-first) method
    SYM_F_EXPOSED = 1u << 1, // the struct's face: a method declared outside its braces
};

typedef struct Symbol
{
    uint32_t tok; // name token, in the owning unit's stream
    uint8_t kind;
    uint8_t flags;
    uint16_t unit;
    SymId owner; // enclosing struct, 0 = the module itself
    NodeId decl; // declaring node, in the owning unit
    union
    {
        SymId next_overload; // SYM_METHOD: next definition in the overload set
        uint32_t scope_ref;  // SYM_STRUCT: member scope (see scope_at)
    };
    SymId next; // hash bucket chain
} Symbol;

_Static_assert(sizeof(Symbol) == 24, "Symbol must stay 24 bytes");

typedef struct
{
    Symbol *data;
    size_t size;
    size_t cap;
} SymVec;

typedef struct Scope
{
    struct Scope *parent;
    SymId owner; // struct that owns this scope, 0 = module scope
    uint32_t nbuckets;
    uint32_t id; // index in cx.scopes
    SymId buckets[];
} Scope;

#define SCOPE_MODULE_BUCKETS 16 // minimums: a scope is sized from its declaration count
#define SCOPE_STRUCT_BUCKETS 8

#define SYM(id) (&cx.syms.data[(id)])

Scope *scope_new(Scope *parent, SymId owner, uint32_t nbuckets);
uint32_t scope_ref(const Scope *s);
Scope *scope_at(uint32_t ref);

SymId symbol_new(Unit *u, SymKind kind, uint8_t flags, uint32_t tok, SymId owner, NodeId decl);
void scope_insert(Scope *s, SymId id);
// Looks in this scope only.
SymId scope_find(const Scope *s, const char *name, size_t len);
// Looks in this scope, then outwards through the parents.
SymId scope_lookup(const Scope *s, const char *name, size_t len);

const char *symbol_name(SymId id, int *len);
void symbols_print(const Unit *u);

#endif // MODAL_V2_SYMBOL_H
