#include "v2/compiler/ast/symbol.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

// ---- interned names ------------------------------------------------------------------------

static void name_table_grow(void)
{
    size_t cap = cx.name_table_cap ? cx.name_table_cap * 2 : 1024;
    uint64_t *table = (uint64_t *)arena_alloc(cap * sizeof(uint64_t));
    memset(table, 0, cap * sizeof(uint64_t));
    for (NameId id = 1; id < cx.nnames; id++)
    {
        size_t i = cx.names[id].hash & (cap - 1);
        while (table[i])
        {
            i = (i + 1) & (cap - 1);
        }
        table[i] = (uint64_t)cx.names[id].hash << 32 | id;
    }
    cx.name_table = table;
    cx.name_table_cap = cap;
}

static inline int name_equal(const Name *n, const char *s, size_t len)
{
    if (n->len != len)
    {
        return 0;
    }
    for (size_t i = 0; i < len; i++) // names are short: inline, no call into libc
    {
        if (n->start[i] != s[i])
        {
            return 0;
        }
    }
    return 1;
}

// The slot holding this name, or the empty slot where it belongs.
static inline size_t name_slot(const char *s, size_t len, uint32_t hash)
{
    size_t mask = cx.name_table_cap - 1;
    size_t i = hash & mask;
    for (uint64_t e; (e = cx.name_table[i]); i = (i + 1) & mask)
    {
        // the hash is in the slot: a probe touches cx.names only when it is very likely a hit
        if ((uint32_t)(e >> 32) == hash && name_equal(&cx.names[(uint32_t)e], s, len))
        {
            return i;
        }
    }
    return i;
}

NameId name_intern_hashed(const char *s, size_t len, uint32_t hash)
{
    if (!cx.name_table_cap)
    {
        name_table_grow();
        cx.names = (Name *)region_grow_array(REGION_NAMES, cx.names, sizeof(Name), &cx.names_cap, 1);
        cx.nnames = 1; // slot 0: no name
        scan_register_keywords();
    }
    size_t i = name_slot(s, len, hash);
    if (cx.name_table[i])
    {
        return (NameId)(uint32_t)cx.name_table[i];
    }

    if (cx.nnames == cx.names_cap)
    {
        cx.names = (Name *)region_grow_array(REGION_NAMES, cx.names, sizeof(Name), &cx.names_cap,
                                             cx.nnames + 1);
    }
    NameId id = (NameId)cx.nnames++;
    cx.names[id] = (Name){.start = s, .len = (uint32_t)len, .hash = hash};
    cx.name_table[i] = (uint64_t)hash << 32 | id;
    if (cx.nnames * 2 > cx.name_table_cap) // keep the table under half full
    {
        name_table_grow();
    }
    return id;
}

NameId name_intern(const char *s, size_t len)
{
    return name_intern_hashed(s, len, name_hash(s, len));
}

NameId name_find(const char *s, size_t len)
{
    if (!cx.name_table_cap)
    {
        return 0;
    }
    return (NameId)(uint32_t)cx.name_table[name_slot(s, len, name_hash(s, len))];
}

const Name *name_at(NameId id)
{
    return &cx.names[id];
}

NameId token_name(const Token *t)
{
    if (t->kind == TOK_IDENTIFIER && t->match)
    {
        return t->match;
    }
    return name_intern(TSTART(t), t->len);
}

Scope *scope_new(Scope *parent, SymId owner, uint32_t nbuckets)
{
    uint32_t pow2 = 1; // a power of two, so a bucket is a mask, not a division
    while (pow2 < nbuckets)
    {
        pow2 <<= 1;
    }
    nbuckets = pow2;
    Scope *s = (Scope *)arena_alloc(sizeof(Scope) + nbuckets * sizeof(SymId));
    s->parent = parent;
    s->owner = owner;
    s->nbuckets = nbuckets;
    if (cx.nscopes == cx.scopes_cap)
    {
        size_t cap = cx.scopes_cap;
        cx.scopes = (Scope **)region_grow_array(REGION_SCOPES, cx.scopes, sizeof(Scope *), &cap,
                                                cx.nscopes + 1);
        cx.scopes_cap = (uint32_t)cap;
    }
    s->id = cx.nscopes;
    cx.scopes[cx.nscopes++] = s;
    memset(s->buckets, 0, nbuckets * sizeof(SymId));
    return s;
}

// A 24-byte Symbol has no room for a pointer, so it refers to its member scope by index.
uint32_t scope_ref(const Scope *s)
{
    return s->id;
}

Scope *scope_at(uint32_t ref)
{
    return cx.scopes[ref];
}

SymId symbol_new(Unit *u, SymKind kind, uint8_t flags, uint32_t tok, SymId owner, NodeId decl)
{
    Symbol s = {
        .tok = tok,
        .kind = (uint8_t)kind,
        .flags = flags,
        .unit = u->id,
        .owner = owner,
        .decl = decl,
    };
    if (cx.syms.size == cx.syms.cap)
    {
        cx.syms.data = (Symbol *)region_grow_array(REGION_SYMS, cx.syms.data, sizeof(Symbol),
                                                   &cx.syms.cap, cx.syms.size + 1);
    }
    if (cx.syms.size >= cx.sym_names_cap)
    {
        cx.sym_names = (NameId *)region_grow_array(REGION_SYMNAMES, cx.sym_names, sizeof(NameId),
                                                   &cx.sym_names_cap, cx.syms.size + 1);
    }
    cx.sym_names[cx.syms.size] = cx.syms.size ? token_name(TOK(u, tok)) : 0;
    cx.syms.data[cx.syms.size++] = s;
    return (SymId)(cx.syms.size - 1);
}

const char *symbol_name(SymId id, int *len)
{
    const Symbol *s = SYM(id);
    const Token *t = TOK(&cx.units[s->unit], s->tok);
    *len = t->len;
    return TSTART(t);
}

void scope_insert(Scope *s, SymId id)
{
    uint32_t b = cx.names[cx.sym_names[id]].hash & (s->nbuckets - 1);
    SYM(id)->next = s->buckets[b];
    s->buckets[b] = id;
}

SymId scope_find_name(const Scope *s, NameId name)
{
    if (!name)
    {
        return 0;
    }
    for (SymId id = s->buckets[cx.names[name].hash & (s->nbuckets - 1)]; id; id = SYM(id)->next)
    {
        if (cx.sym_names[id] == name)
        {
            return id;
        }
    }
    return 0;
}

SymId scope_find(const Scope *s, const char *name, size_t len)
{
    return scope_find_name(s, name_find(name, len)); // never written anywhere: not declared
}

SymId scope_lookup_name(const Scope *s, NameId name)
{
    for (; s && name; s = s->parent)
    {
        SymId id = scope_find_name(s, name);
        if (id)
        {
            return id;
        }
    }
    return 0;
}

SymId scope_lookup(const Scope *s, const char *name, size_t len)
{
    return scope_lookup_name(s, name_find(name, len));
}

static const char *sym_kind_name(uint8_t kind)
{
    switch (kind)
    {
    case SYM_STRUCT:
        return "struct";
    case SYM_FIELD:
        return "field";
    case SYM_METHOD:
        return "method";
    case SYM_CONST:
        return "const";
    case SYM_IMPORT:
        return "import";
    default:
        return "?";
    }
}

// How far a symbol can be seen:
//   inside  only within its struct
//   domain  within its folder
//   public  from other domains
static const char *visibility(const Unit *u, const Symbol *s)
{
    if (s->owner)
    {
        if (!(s->flags & SYM_F_EXPOSED))
        {
            return "inside";
        }
        return u->bound ? "public" : "domain";
    }
    if (u->is_manifest)
    {
        return s->kind == SYM_CONST ? "config" : "public";
    }
    return (s->kind == SYM_STRUCT && u->bound) ? "public" : "domain";
}

// Symbols are appended in declaration order, so a linear pass prints each unit as written.
void symbols_print(const Unit *u)
{
    printf("module %.*s  (domain %s%s)\n", u->name_len, u->name, domain_name(u->domain),
           u->is_manifest ? ", manifest" : "");

    for (SymId id = 1; id < cx.syms.size; id++)
    {
        const Symbol *s = SYM(id);
        if (s->unit != u->id || (s->flags & SYM_F_LITERAL))
        {
            continue;
        }

        char qualified[128];
        int len;
        const char *name = symbol_name(id, &len);
        int n = 0;
        // Build `outer.inner.name`: owners are at most a few levels deep.
        SymId chain[8];
        int depth = 0;
        for (SymId o = s->owner; o && depth < 8; o = SYM(o)->owner)
        {
            chain[depth++] = o;
        }
        while (depth > 0)
        {
            int olen;
            const char *oname = symbol_name(chain[--depth], &olen);
            n += snprintf(qualified + n, sizeof(qualified) - (size_t)n, "%.*s.", olen, oname);
            if (n >= (int)sizeof(qualified))
            {
                n = (int)sizeof(qualified) - 1;
            }
        }
        snprintf(qualified + n, sizeof(qualified) - (size_t)n, "%.*s", len, name);

        printf("  %-7s %-40s %-7s %s\n", sym_kind_name(s->kind), qualified, visibility(u, s),
               (s->flags & SYM_F_MUT) ? "mut" : "");
    }
}
