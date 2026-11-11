// module.c (v2): files, domains and visibility.
//
//   example/john.modal   module `john`, in domain `example`
//   example/mod.modal    the domain's configuration, and the modules that form its face
//
// One rule at every level: a body hides its inside, and its face is written just outside it.
//   struct  inside the braces: fields and helpers    outside: methods with `self: T`
//   file    its bindings and free functions          outside: what mod.modal binds or defines
//   folder  its files                                outside: what the parent's mod.modal binds
// Nothing is listed anywhere, and a type crosses a domain boundary as an opaque type.
#define _XOPEN_SOURCE 700
#include "v2/compiler/ast/visitor.h"
#include "v2/compiler/cx.h"
#include "v2/compiler/sema/check.h"
#include "v2/compiler/syntax/parser/parser.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHAIN_MAX 32
#define FOLLOW_MAX 8

typedef enum
{
    CHAIN_SILENT, // just find where the chain leads
    CHAIN_CHECK,  // report names that do not exist or cannot be seen from here
} ChainMode;

typedef struct
{
    Unit *unit;
    Scope *scope;
    int known;
} Target;

static Target chain_eval(Unit *u, NodeId chain, ChainMode mode, int depth);

// ---- paths and domains -----------------------------------------------------------------------

static const char *arena_strndup(const char *s, size_t n)
{
    char *copy = (char *)arena_alloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

const char *domain_name(uint16_t domain)
{
    return base_name(cx.domains[domain]);
}

static uint16_t domain_intern(const char *dir, size_t len)
{
    for (uint16_t i = 0; i < cx.ndomains; i++)
    {
        if (strlen(cx.domains[i]) == len && memcmp(cx.domains[i], dir, len) == 0)
        {
            return i;
        }
    }
    if (cx.ndomains == MAX_DOMAINS)
    {
        fprintf(stderr, "fatal: too many domains (limit is %d)\n", MAX_DOMAINS);
        exit(2);
    }
    cx.domains[cx.ndomains] = arena_strndup(dir, len);
    return cx.ndomains++;
}

static int read_file(Unit *u)
{
    FILE *f = fopen(u->canon, "rb");
    if (!f)
    {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(f);
        return 0;
    }
    char *buffer = (char *)arena_alloc((size_t)size + 1);
    size_t got = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[got] = '\0';
    u->src = buffer;
    u->len = strlen(buffer); // a stray NUL byte ends the source
    return 1;
}

Unit *unit_load(const char *path)
{
    char canon[PATH_MAX];
    if (!realpath(path, canon))
    {
        return NULL;
    }
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (strcmp(cx.units[i].canon, canon) == 0)
        {
            return &cx.units[i];
        }
    }
    if (cx.nunits == MAX_UNITS)
    {
        fprintf(stderr, "fatal: too many source files (limit is %d)\n", MAX_UNITS);
        exit(2);
    }

    Unit *u = &cx.units[cx.nunits];
    *u = (Unit){.id = cx.nunits};
    u->path = arena_strndup(path, strlen(path));
    u->canon = arena_strndup(canon, strlen(canon));

    const char *base = base_name(u->canon);
    size_t dir_len = base > u->canon + 1 ? (size_t)(base - u->canon) - 1 : 1;
    u->domain = domain_intern(u->canon, dir_len);
    u->is_manifest = strcmp(base, "mod.modal") == 0;

    if (u->is_manifest)
    {
        u->name = domain_name(u->domain);
        u->name_len = (int)strlen(u->name);
    }
    else
    {
        const char *dot = strrchr(base, '.');
        u->name = base;
        u->name_len = dot ? (int)(dot - base) : (int)strlen(base);
    }

    if (!read_file(u))
    {
        return NULL;
    }
    cx.nunits++;

    int lex_errors = scan_all(u->path, u->src, &u->tokens);
    u->errors += lex_errors;
    cx.errors += lex_errors;

    // A node needs at least one token of its own (the root and string-calls aside), so the
    // token count bounds the tree: reserve once, then hand the unused tail back.
    u->nodes.cap = u->tokens.size + 16;
    u->nodes.data = (AstNode *)arena_alloc(u->nodes.cap * sizeof(AstNode));
    u->nodes.data[u->nodes.size++] = (AstNode){0}; // slot 0 = "no node"

    Parser parser;
    parser_init(&parser, u);
    u->root = parse_program(&parser);

    arena_shrink(u->nodes.data, u->nodes.cap * sizeof(AstNode), u->nodes.size * sizeof(AstNode));
    u->nodes.cap = u->nodes.size;

    sema_declare(u);
    u->broken = u->errors > 0;
    return u;
}

// ---- import resolution -----------------------------------------------------------------------

static int file_exists(const char *path)
{
    return access(path, R_OK) == 0;
}

// Finds the file an import names, relative to the importing file:
//   import("./a/b.modal")  that file
//   import("./a/b")        a/b.modal, or the domain a/b/mod.modal
//   import(b)              the same lookup
// Only `std` may be missing from disk; any other import that resolves to nothing is an error.
static Unit *import_target(Unit *u, NodeId import)
{
    const Token *arg = TOK(u, NODE(u, import)->tok);
    const char *name = arg->start;
    int len = arg->len;
    if (arg->kind == TOK_STRING)
    {
        name++;
        len -= (len >= 2 && arg->start[len - 1] == '"') ? 2 : 1;
    }
    if (len <= 0)
    {
        diag_at(u, NODE(u, import)->tok, "error", "import needs a module name");
        return NULL;
    }

    const char *slash = strrchr(u->path, '/');
    int dir_len = slash ? (int)(slash - u->path) : 1;
    const char *dir = slash ? u->path : ".";
    int relative = name[0] == '.' && (name[1] == '/' || (name[1] == '.' && name[2] == '/'));
    int has_ext = len > 6 && memcmp(name + len - 6, ".modal", 6) == 0;

    const char *rel = name; // "./x" composes as "dir/x", which reads better in diagnostics
    int rel_len = len;
    while (rel_len > 2 && rel[0] == '.' && rel[1] == '/')
    {
        rel += 2;
        rel_len -= 2;
    }

    char path[PATH_MAX];
    const char *const forms[] = {"%.*s/%.*s", "%.*s/%.*s.modal", "%.*s/%.*s/mod.modal"};
    for (int i = has_ext ? 0 : 1; i < (has_ext ? 1 : 3); i++)
    {
        int n = snprintf(path, sizeof(path), forms[i], dir_len, dir, rel_len, rel);
        if (n > 0 && (size_t)n < sizeof(path) && file_exists(path))
        {
            Unit *target = unit_load(path);
            if (target)
            {
                return target;
            }
        }
    }

    // Nothing was found next to the importing file. The only modules that live elsewhere are
    // the external roots below; every other unresolved import is an error, so a mistyped or
    // unreachable path can never pass unchecked.
    static const char *const external_roots[] = {"std"};
    for (size_t i = 0; i < sizeof(external_roots) / sizeof(external_roots[0]); i++)
    {
        if (!relative && strlen(external_roots[i]) == (size_t)len &&
            memcmp(external_roots[i], name, (size_t)len) == 0)
        {
            return NULL;
        }
    }
    if (has_ext)
    {
        diag_at(u, NODE(u, import)->tok, "error", "cannot find module %.*s (looked for %.*s/%.*s)",
                arg->len, arg->start, dir_len, dir, rel_len, rel);
    }
    else
    {
        diag_at(u, NODE(u, import)->tok, "error",
                "cannot find module %.*s (looked for %.*s/%.*s.modal and %.*s/%.*s/mod.modal)",
                arg->len, arg->start, dir_len, dir, rel_len, rel, dir_len, dir, rel_len, rel);
    }
    return NULL;
}

// The import chain a top-level declaration carries, if any.
static NodeId decl_chain(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    NodeId expr = n->kind == AST_DEFINE ? n->a : n->kind == AST_VAL_DECL ? n->b : decl;
    return ast_import_root(u, expr) ? expr : 0;
}

// ---- location rules --------------------------------------------------------------------------

// Like Go's internal/: a domain can be imported from its parent folder and from anything
// below that parent. Further out, it is reached only through the parent's mod.modal. And a
// file of another domain can be imported directly only if its domain's mod.modal binds it.
// A file sees its own domain and every domain above it, as a child sees its ancestors.
static int sees_inside(const Unit *u, const Unit *target)
{
    const char *tdir = cx.domains[target->domain];
    const char *udir = cx.domains[u->domain];
    size_t tlen = strlen(tdir);
    return strncmp(udir, tdir, tlen) == 0 && (udir[tlen] == '/' || udir[tlen] == '\0');
}

static int may_import(Unit *u, uint32_t tok, const Unit *target)
{
    if (sees_inside(u, target))
    {
        return 1;
    }

    const char *tdir = cx.domains[target->domain];
    const char *udir = cx.domains[u->domain];
    const char *slash = strrchr(tdir, '/');
    size_t plen = slash ? (size_t)(slash - tdir) : 0;

    if (strncmp(udir, tdir, plen) != 0 || (udir[plen] != '/' && udir[plen] != '\0'))
    {
        const char *parent_end = tdir + plen;
        const char *parent = parent_end;
        while (parent > tdir && parent[-1] != '/')
        {
            parent--;
        }
        diag_at(u, tok, "error",
                "domain '%s' is internal to '%.*s'. From here, reach it through %.*s/mod.modal",
                domain_name(target->domain), (int)(parent_end - parent), parent,
                (int)(parent_end - parent), parent);
        return 0;
    }
    if (!target->is_manifest && !target->bound)
    {
        diag_at(u, tok, "error",
                "module '%.*s' is not part of the face of domain '%s'. To share it, bind it in "
                "%s/mod.modal: `%.*s := import(\"./%.*s\")`",
                target->name_len, target->name, domain_name(target->domain),
                domain_name(target->domain), target->name_len, target->name, target->name_len,
                target->name);
        return 0;
    }
    return 1;
}

// ---- chain evaluation ------------------------------------------------------------------------

// Where a binding leads: `john := import("./john")` leads into john's module scope.
static Target follow_import(SymId id, int depth)
{
    const Symbol *s = SYM(id);
    Unit *owner = &cx.units[s->unit];
    const AstNode *decl = NODE(owner, s->decl);

    // `a := import(x).y` and also the aliases built on it: `b := a.z`, `c := b`.
    if (decl->kind != AST_DEFINE || depth >= FOLLOW_MAX)
    {
        return (Target){0};
    }
    return chain_eval(owner, decl->a, CHAIN_SILENT, depth + 1);
}

// The inside / outside rule, applied to one name. Returns 0 after reporting a violation.
static int may_see(Unit *u, uint32_t tok, SymId id)
{
    const Token *name = TOK(u, tok);
    const Symbol *s = SYM(id);
    Unit *owner = &cx.units[s->unit];

    if (s->owner) // a struct member: only the struct's face can be named from elsewhere
    {
        int tlen;
        const char *type = symbol_name(s->owner, &tlen);
        if (s->kind == SYM_FIELD)
        {
            diag_at(u, tok, "error",
                    "'%.*s' is a field of '%.*s', and fields are never visible outside their "
                    "struct. Add an accessor below it: `T %.*s(self: %.*s): self.%.*s`",
                    name->len, name->start, tlen, type, name->len, name->start, tlen, type,
                    name->len, name->start);
            return 0;
        }
        if (!(s->flags & SYM_F_EXPOSED))
        {
            diag_at(u, tok, "error",
                    "'%.*s' is inside '%.*s'. To use it here, move it below the closing brace "
                    "of the struct",
                    name->len, name->start, tlen, type);
            return 0;
        }
        return 1;
    }

    if (sees_inside(u, owner))
    {
        return 1; // files of one domain share everything at module level
    }
    if (owner->is_manifest)
    {
        if (s->kind == SYM_CONST)
        {
            diag_at(u, tok, "error", "'%.*s' is configuration of domain '%s', not part of its face",
                    name->len, name->start, domain_name(owner->domain));
            return 0;
        }
        return 1; // what mod.modal binds or defines is the domain's face
    }
    if (s->kind != SYM_STRUCT) // a type travels as an opaque type; nothing else leaves the file
    {
        diag_at(u, tok, "error",
                "'%.*s' is inside module '%.*s', private to domain '%s'. To share it, define "
                "it in %s/mod.modal",
                name->len, name->start, owner->name_len, owner->name, domain_name(owner->domain),
                domain_name(owner->domain));
        return 0;
    }
    return 1;
}

// Resolves one name of a chain inside `cur`.
static Target chain_step(Unit *u, uint32_t tok, Target cur, ChainMode mode, int depth)
{
    const Token *name = TOK(u, tok);
    const char *container = cur.unit->name;
    int container_len = cur.unit->name_len;

    if (cur.scope && cur.scope->owner)
    {
        container = symbol_name(cur.scope->owner, &container_len);
    }

    SymId id = cur.scope ? scope_find(cur.scope, name->start, (size_t)name->len) : 0;
    if (!id)
    {
        if (mode == CHAIN_CHECK)
        {
            diag_at(u, tok, "error", "'%.*s' has no member '%.*s'", container_len, container,
                    name->len, name->start);
        }
        return (Target){0};
    }
    if (mode == CHAIN_CHECK && !may_see(u, tok, id))
    {
        return (Target){0};
    }

    Unit *owner = &cx.units[SYM(id)->unit];
    switch (SYM(id)->kind)
    {
    case SYM_STRUCT:
        return (Target){owner, scope_at(SYM(id)->scope_ref), 1};
    case SYM_IMPORT:
        return follow_import(id, depth);
    default:
        return (Target){owner, NULL, 1}; // a field, method or constant: nothing below it
    }
}

static void chain_items(Unit *u, NodeId item, Target cur, ChainMode mode, int depth)
{
    for (; item; item = NODE(u, item)->next)
    {
        chain_step(u, NODE(u, item)->tok, cur, mode, depth);
    }
}

static int is_chain_segment(uint8_t kind)
{
    return kind == AST_SELECTOR || kind == AST_IMPORT_ALL || kind == AST_IMPORT_PICK;
}

// Walks a chain from its base outwards. The base is either an import, as in
// import(x).a.b / .* / .(c, d), or a name bound to one, as in `ex.a.b` after
// `ex := import(x)`: member access gets exactly the same checks as a direct import.
static Target chain_eval(Unit *u, NodeId chain, ChainMode mode, int depth)
{
    NodeId segs[CHAIN_MAX];
    int n = 0;
    NodeId id = chain;

    while (is_chain_segment(NODE(u, id)->kind))
    {
        if (n == CHAIN_MAX)
        {
            if (mode == CHAIN_CHECK)
            {
                diag_at(u, NODE(u, id)->tok, "error", "import chain is too long");
            }
            return (Target){0};
        }
        segs[n++] = id;
        id = NODE(u, id)->a;
    }

    const AstNode *base = NODE(u, id);
    Target cur = {0};

    if (base->kind == AST_IMPORT)
    {
        uint16_t target = base->aux;
        if (!target || cx.units[target - 1].broken)
        {
            return (Target){0}; // external module, or one that did not compile
        }
        cur = (Target){&cx.units[target - 1], cx.units[target - 1].scope, 1};
        if (mode == CHAIN_CHECK && !may_import(u, base->tok, cur.unit))
        {
            return (Target){0};
        }
    }
    else if (base->kind == AST_IDENT)
    {
        const Token *name = TOK(u, base->tok);
        SymId binding = scope_find(u->scope, name->start, (size_t)name->len);
        if (!binding || (SYM(binding)->kind != SYM_IMPORT && SYM(binding)->kind != SYM_CONST))
        {
            return (Target){0}; // a local value, `self`, ...: not a module path
        }
        cur = follow_import(binding, depth);
    }
    else
    {
        return (Target){0};
    }

    while (n > 0 && cur.known)
    {
        const AstNode *seg = NODE(u, segs[--n]);
        switch (seg->kind)
        {
        case AST_SELECTOR:
            cur = chain_step(u, seg->tok, cur, mode, depth);
            break;
        case AST_IMPORT_PICK:
            chain_items(u, seg->b, cur, mode, depth);
            return cur;
        default: // AST_IMPORT_ALL: brings in whatever is visible
            return cur;
        }
    }
    return cur;
}

// Checks every module path in a file, wherever it is written: top level or inside a body,
// spelled as an import or as member access on a binding.
static int check_visit(Unit *u, NodeId id, uint32_t depth, void *ctx)
{
    (void)depth;
    (void)ctx;
    const AstNode *n = NODE(u, id);

    if (n->kind == AST_VAL_DECL)
    {
        Target end = chain_eval(u, n->b, CHAIN_CHECK, 0);
        if (end.known)
        {
            chain_items(u, n->a, end, CHAIN_CHECK, 0);
        }
        return VISIT_SKIP;
    }
    if (n->kind == AST_IMPORT || is_chain_segment(n->kind))
    {
        NodeId base = id;
        while (is_chain_segment(NODE(u, base)->kind))
        {
            base = NODE(u, base)->a;
        }
        uint8_t kind = NODE(u, base)->kind;
        if (kind == AST_IMPORT || kind == AST_IDENT)
        {
            chain_eval(u, id, CHAIN_CHECK, 0); // the whole chain at once, from its outermost node
            return VISIT_SKIP;
        }
    }
    return VISIT_CONTINUE;
}

// Finds the file behind every import in a unit, including imports written inside bodies.
static int discover_visit(Unit *u, NodeId id, uint32_t depth, void *ctx)
{
    (void)depth;
    (void)ctx;
    if (NODE(u, id)->kind == AST_IMPORT)
    {
        Unit *target = import_target(u, id);
        NODE(u, id)->aux = target ? (uint16_t)(target->id + 1) : 0;
    }
    return VISIT_CONTINUE;
}

// ---- mod.modal: configuration and the face of the domain ----------------------------------------

typedef enum
{
    CONFIG_BOOL,
    CONFIG_STRING,
} ConfigKind;

// The closed set of behaviour switches. A name outside it is an error, so a typo never
// silently does nothing.
static const struct
{
    const char *name;
    ConfigKind kind;
} config_keys[] = {
    {"implicit_self", CONFIG_BOOL}, // may `n1` stand for `self.n1` in a method body
    {"nil_return", CONFIG_BOOL},    // the compiler adds the implicit nil to return types
    {"profile", CONFIG_STRING},     // which function declaration forms the domain accepts
    {"version", CONFIG_STRING},     // version of the domain's contract
};

// In a manifest, `name := <literal>` is configuration; `name := import(...)` binds a module.
static int is_config(const Unit *u, NodeId decl)
{
    const AstNode *n = NODE(u, decl);
    return n->kind == AST_DEFINE && NODE(u, n->a)->kind != AST_FN_LIT && !ast_import_root(u, n->a);
}

static void check_config(Unit *u, NodeId decl)
{
    const Token *name = TOK(u, NODE(u, decl)->tok);
    const AstNode *value = NODE(u, NODE(u, decl)->a);
    const Token *vt = TOK(u, value->tok);

    for (size_t i = 0; i < sizeof(config_keys) / sizeof(config_keys[0]); i++)
    {
        if (strlen(config_keys[i].name) != (size_t)name->len ||
            memcmp(config_keys[i].name, name->start, (size_t)name->len) != 0)
        {
            continue;
        }
        int is_bool =
            value->kind == AST_IDENT && ((vt->len == 4 && !memcmp(vt->start, "true", 4)) ||
                                         (vt->len == 5 && !memcmp(vt->start, "false", 5)));
        int ok = config_keys[i].kind == CONFIG_BOOL ? is_bool : value->kind == AST_STRING_LIT;
        if (!ok)
        {
            diag_at(u, value->tok, "error", "'%.*s' expects %s", name->len, name->start,
                    config_keys[i].kind == CONFIG_BOOL ? "true or false" : "a string");
        }
        return;
    }
    diag_at(u, NODE(u, decl)->tok, "error",
            "'%.*s' is not a known configuration of a domain (known: implicit_self, nil_return, "
            "profile, version)",
            name->len, name->start);
}

// Reads one mod.modal: validates its configuration and marks the modules it binds.
static void read_manifest(Unit *m)
{
    for (NodeId d = NODE(m, m->root)->a; d; d = NODE(m, d)->next)
    {
        if (is_config(m, d))
        {
            check_config(m, d);
            continue;
        }
        NodeId chain = decl_chain(m, d);
        uint16_t target = chain ? NODE(m, ast_import_root(m, chain))->aux : 0;
        if (target && cx.units[target - 1].domain == m->domain)
        {
            cx.units[target - 1].bound = 1;
        }
    }
}

int domain_config_bool(uint16_t domain, const char *key, int fallback)
{
    size_t klen = strlen(key);
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        const Unit *m = &cx.units[i];
        if (!m->is_manifest || m->domain != domain || m->broken)
        {
            continue;
        }
        for (NodeId d = NODE(m, m->root)->a; d; d = NODE(m, d)->next)
        {
            const Token *name = TOK(m, NODE(m, d)->tok);
            if (is_config(m, d) && name->len == klen && memcmp(name->start, key, klen) == 0)
            {
                const Token *v = TOK(m, NODE(m, NODE(m, d)->a)->tok);
                return v->len == 4 && memcmp(v->start, "true", 4) == 0;
            }
        }
    }
    return fallback;
}

int module_wildcard_provides(Unit *u, const char *name, size_t len)
{
    for (NodeId d = NODE(u, u->root)->a; d; d = NODE(u, d)->next)
    {
        NodeId chain = decl_chain(u, d);
        if (!chain || NODE(u, chain)->kind != AST_IMPORT_ALL)
        {
            continue;
        }
        Target t = chain_eval(u, chain, CHAIN_SILENT, 0);
        if (!t.known || !t.scope)
        {
            return 1; // an external module: its names cannot be listed, so it may provide it
        }
        if (scope_find(t.scope, name, len))
        {
            return 1;
        }
    }
    return 0;
}

// The derived contract: what other domains can rely on. Nobody writes this by hand.
void contract_print(void)
{
    char sig[256];

    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *m = &cx.units[i];
        if (!m->is_manifest)
        {
            continue;
        }
        printf("== contract: %s  (%s)\n", domain_name(m->domain), m->path);

        for (NodeId d = NODE(m, m->root)->a; d; d = NODE(m, d)->next)
        {
            const AstNode *n = NODE(m, d);
            if (is_config(m, d))
            {
                const Token *k = TOK(m, n->tok), *v = TOK(m, NODE(m, n->a)->tok);
                printf("config %.*s = %.*s\n", k->len, k->start, v->len, v->start);
            }
            else if (n->kind == AST_METHOD_DECL || (n->kind == AST_DEFINE && !decl_chain(m, d)))
            {
                printf("%s\n", ast.sig_str(m, d, sig, sizeof(sig)));
            }
        }

        for (uint16_t j = 0; j < cx.nunits; j++)
        {
            Unit *u = &cx.units[j];
            if (u->domain != m->domain || !u->bound)
            {
                continue;
            }
            printf("module %.*s\n", u->name_len, u->name);
            for (SymId id = 1; id < cx.syms.size; id++)
            {
                const Symbol *s = SYM(id);
                int len;
                const char *name = symbol_name(id, &len);
                if (s->unit != u->id)
                {
                    continue;
                }
                if (s->kind == SYM_STRUCT && !s->owner)
                {
                    printf("  type %.*s\n", len, name); // opaque: no fields in the contract
                }
                else if (s->owner && (s->flags & SYM_F_EXPOSED))
                {
                    printf("  %s\n", ast.sig_str(u, s->decl, sig, sizeof(sig)));
                }
            }
        }
        putchar('\n');
    }
}

// ---- linking ---------------------------------------------------------------------------------

void modules_link(void)
{
    // A. Discover. cx.units is the worklist: loading appends to it, the loop catches up.
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *u = &cx.units[i];

        if (!u->is_manifest)
        {
            char manifest[PATH_MAX];
            const char *slash = strrchr(u->path, '/');
            int n = snprintf(manifest, sizeof(manifest), "%.*s/mod.modal",
                             slash ? (int)(slash - u->path) : 1, slash ? u->path : ".");
            if (n > 0 && (size_t)n < sizeof(manifest) && file_exists(manifest))
            {
                unit_load(manifest);
            }
        }

        walk_ast_iterative(u, u->root, discover_visit, NULL);
    }

    // B. Read every mod.modal: its configuration, and which modules form the domain's face.
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (cx.units[i].is_manifest && !cx.units[i].broken)
        {
            read_manifest(&cx.units[i]);
        }
    }

    // C. Check every import against what exists and what can be seen from the importer.
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (!cx.units[i].broken)
        {
            walk_ast_iterative(&cx.units[i], cx.units[i].root, check_visit, NULL);
            sema_check(&cx.units[i]); // D. names and invocations inside bodies
        }
    }
}
