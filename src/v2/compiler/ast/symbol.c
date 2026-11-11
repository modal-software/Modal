#include "v2/compiler/ast/symbol.h"
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

static inline uint32_t symbol_hash(const char *s, size_t len)
{
    uint32_t h = 2166136261u; // FNV-1a
    for (size_t i = 0; i < len; i++)
    {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }
    return h;
}

Scope *scope_new(Scope *parent, SymId owner, uint32_t nbuckets)
{
    Scope *s = (Scope *)arena_alloc(sizeof(Scope) + nbuckets * sizeof(SymId));
    s->parent = parent;
    s->owner = owner;
    s->nbuckets = nbuckets;
    if (cx.nscopes == cx.scopes_cap)
    {
        size_t cap = cx.scopes_cap;
        cx.scopes = (Scope **)arena_grow(cx.scopes, sizeof(Scope *), &cap);
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
        cx.syms.data = (Symbol *)arena_grow(cx.syms.data, sizeof(Symbol), &cx.syms.cap);
    }
    cx.syms.data[cx.syms.size++] = s;
    return (SymId)(cx.syms.size - 1);
}

const char *symbol_name(SymId id, int *len)
{
    const Symbol *s = SYM(id);
    const Token *t = TOK(&cx.units[s->unit], s->tok);
    *len = t->len;
    return t->start;
}

void scope_insert(Scope *s, SymId id)
{
    int len;
    const char *name = symbol_name(id, &len);
    uint32_t b = symbol_hash(name, (size_t)len) % s->nbuckets;
    SYM(id)->next = s->buckets[b];
    s->buckets[b] = id;
}

SymId scope_find(const Scope *s, const char *name, size_t len)
{
    for (SymId id = s->buckets[symbol_hash(name, len) % s->nbuckets]; id; id = SYM(id)->next)
    {
        int n;
        const char *other = symbol_name(id, &n);
        if ((size_t)n == len && memcmp(other, name, len) == 0)
        {
            return id;
        }
    }
    return 0;
}

SymId scope_lookup(const Scope *s, const char *name, size_t len)
{
    for (; s; s = s->parent)
    {
        SymId id = scope_find(s, name, len);
        if (id)
        {
            return id;
        }
    }
    return 0;
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
        if (s->unit != u->id)
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
