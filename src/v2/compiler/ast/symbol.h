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
#include "v2/compiler/syntax/scanner.h"
#include <stdint.h>
#include <string.h>

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
    // A function literal inside a body: anonymous, and its declaration lives only while that
    // body is in memory (it is generated then). Listings and contracts skip it.
    SYM_F_LITERAL = 1u << 2,
    SYM_F_CAPTURES = 1u << 3, // a literal that reads a local of the function around it
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

// ---- interned names (LuaJIT's string interning, Go's syntax.Name) ---------------------------
//
// Every distinct identifier gets one NameId, given by the scanner as it reads the identifier
// and stored in the token. Scopes then compare names as integers: no hashing or memcmp of the
// text at lookup time, and a name that was never written anywhere is not found at once.
typedef uint32_t NameId; // 0 = no name

typedef struct
{
    const char *start; // into a source buffer (or a string literal), never copied
    uint32_t len : 24;
    uint32_t keyword : 8; // the TokenKind of a reserved word, 0 for a plain name (LuaJIT)
    uint32_t hash;
} Name;

// Names are hashed a word at a time, not a byte at a time: the first and last 8 bytes, the
// length, and every 8 bytes in between, mixed with multiplications (the wyhash idea).
static inline uint64_t name_load64(const char *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
static inline uint32_t name_hash_words(uint64_t a, uint64_t b, size_t len)
{
    // Combine, then the splitmix64 finalizer: every input bit reaches the low bits, which
    // pick the table slot (a plain product would leave them depending on the low bytes only).
    uint64_t h = a ^ ((b ^ len) * 0x9E3779B97F4A7C15ull);
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return (uint32_t)h;
}
static inline uint32_t name_hash_long(const char *s, size_t len) // len >= 8, reads in bounds
{
    uint64_t a = name_load64(s);
    for (size_t i = 8; i + 8 < len; i += 8)
    {
        a = (a ^ name_load64(s + i)) * 0xFF51AFD7ED558CCDull;
    }
    return name_hash_words(a, name_load64(s + len - 8), len);
}
// For text followed by at least 8 readable bytes (a padded source buffer): no length checks.
static inline uint32_t name_hash_padded(const char *s, size_t len)
{
    if (len < 8)
    {
        return name_hash_words(name_load64(s) & ((1ull << (len * 8)) - 1), 0, len);
    }
    return name_hash_long(s, len);
}
// For any text.
static inline uint32_t name_hash(const char *s, size_t len)
{
    if (len < 8)
    {
        uint64_t a = 0;
        memcpy(&a, s, len);
        return name_hash_words(a, 0, len);
    }
    return name_hash_long(s, len);
}
// Bytes of zero padding after every source buffer, so the scanner may read a word past a name.
#define SOURCE_PADDING 16

NameId name_intern_hashed(const char *s, size_t len, uint32_t hash);
NameId name_intern(const char *s, size_t len);
NameId name_find(const char *s, size_t len); // 0 if this text was never interned
const Name *name_at(NameId id);

Scope *scope_new(Scope *parent, SymId owner, uint32_t nbuckets);
uint32_t scope_ref(const Scope *s);
Scope *scope_at(uint32_t ref);

SymId symbol_new(Unit *u, SymKind kind, uint8_t flags, uint32_t tok, SymId owner, NodeId decl);
void scope_insert(Scope *s, SymId id);
// Looks in this scope only.
SymId scope_find(const Scope *s, const char *name, size_t len);
SymId scope_find_name(const Scope *s, NameId name);
SymId scope_lookup_name(const Scope *s, NameId name); // this scope, then its parents
// The name a token spells: free for an identifier, interned on the spot otherwise.
NameId token_name(const Token *t);
static inline SymId scope_find_token(const Scope *s, const Token *t)
{
    return scope_find_name(s, token_name(t));
}
// Looks in this scope, then outwards through the parents.
SymId scope_lookup(const Scope *s, const char *name, size_t len);

const char *symbol_name(SymId id, int *len);
void symbols_print(const Unit *u);

#endif // MODAL_V2_SYMBOL_H
