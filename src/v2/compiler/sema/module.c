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
#include "v2/compiler/cx.h"
#include "v2/compiler/syntax/parser/parser.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    return cx.domain_names[domain];
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
    cx.domain_names[cx.ndomains] = base_name(cx.domains[cx.ndomains]);
    return cx.ndomains++;
}

// The absolute form of `path` with `.`, `..` and repeated slashes removed, written to `out`.
// Purely lexical: realpath() walks every component through the file system and costs about
// 100-200 us per call on macOS, more than compiling a small file. Files reached through a link
// are still recognised as one by their inode (see unit_load). Returns the length, 0 if too long.
static size_t path_normalize(const char *path, char *out, size_t cap)
{
    static char cwd[PATH_MAX];
    static size_t cwd_len;
    size_t n = 0;

    if (path[0] != '/')
    {
        if (!cwd_len)
        {
            if (!getcwd(cwd, sizeof(cwd)))
            {
                return 0;
            }
            cwd_len = strlen(cwd);
        }
        if (cwd_len >= cap)
        {
            return 0;
        }
        memcpy(out, cwd, cwd_len);
        n = cwd_len;
        if (n == 1)
        {
            n = 0; // cwd is "/": components are appended as "/name"
        }
    }

    const char *c = path;
    while (*c)
    {
        while (*c == '/')
        {
            c++;
        }
        const char *part = c;
        while (*c && *c != '/')
        {
            c++;
        }
        size_t len = (size_t)(c - part);
        if (len == 0 || (len == 1 && part[0] == '.'))
        {
            continue;
        }
        if (len == 2 && part[0] == '.' && part[1] == '.')
        {
            while (n > 0 && out[n - 1] != '/')
            {
                n--;
            }
            if (n > 0)
            {
                n--; // drop the '/' too; at the root this leaves n == 0, which is "/"
            }
            continue;
        }
        if (n + 1 + len >= cap)
        {
            return 0;
        }
        out[n++] = '/';
        memcpy(out + n, part, len);
        n += len;
    }
    if (n == 0)
    {
        out[n++] = '/';
    }
    out[n] = '\0';
    return n;
}

// Reads a regular file into the arena: one open, fstat, read and close, no stdio.
// Returns the descriptor's identity through dev/ino, or 0 if the file cannot be read.
static const char *read_source(const char *path, size_t *len, uint64_t *dev, uint64_t *ino)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    char *buffer = (char *)arena_alloc(size + 1 + SOURCE_PADDING);
    size_t got = 0;
    while (got < size)
    {
        ssize_t r = read(fd, buffer + got, size - got);
        if (r <= 0)
        {
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    arena_shrink(buffer, size + 1 + SOURCE_PADDING, got + 1 + SOURCE_PADDING);
    memset(buffer + got, 0, 1 + SOURCE_PADDING);
    *len = strlen(buffer); // a stray NUL byte ends the source
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return buffer;
}

static Unit *unit_from_source(const char *path, const char *canon, size_t canon_len, const char *src,
                              size_t len, uint64_t dev, uint64_t ino);

Unit *unit_load(const char *path)
{
    char canon[PATH_MAX];
    size_t canon_len = path_normalize(path, canon, sizeof(canon));
    if (!canon_len)
    {
        return NULL;
    }
    // Same path as a loaded file: no system call at all.
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (strcmp(cx.units[i].canon, canon) == 0)
        {
            return &cx.units[i];
        }
    }

    // Opening is the existence test: a missing file costs one failed open(), and leaves
    // nothing behind (no domain, no strings), so probing candidate paths is cheap.
    ArenaMark mark = arena_mark();
    size_t len = 0;
    uint64_t dev = 0, ino = 0;
    const char *src = read_source(canon, &len, &dev, &ino);
    if (!src)
    {
        arena_release(mark);
        return NULL;
    }
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (cx.units[i].dev == dev && cx.units[i].ino == ino)
        {
            arena_release(mark); // the same file under another name (a link)
            return &cx.units[i];
        }
    }
    return unit_from_source(path, canon, canon_len, src, len, dev, ino);
}

// A unit from its source text, read from disk or made by the compiler.
static Unit *unit_from_source(const char *path, const char *canon, size_t canon_len, const char *src,
                              size_t len, uint64_t dev, uint64_t ino)
{
    if (cx.nunits == MAX_UNITS)
    {
        fprintf(stderr, "fatal: too many source files (limit is %d)\n", MAX_UNITS);
        exit(2);
    }

    Unit *u = &cx.units[cx.nunits];
    *u = (Unit){.id = cx.nunits, .src = src, .len = len, .dev = dev, .ino = ino};
    u->path = arena_strndup(path, strlen(path));
    u->canon = arena_strndup(canon, canon_len);

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

    cx.nunits++;

    uint64_t t = clock_ns();
    int lex_errors = scan_all(u->path, u->src, u->len, cx.eager, &u->tokens, &u->max_body);
    u->errors += lex_errors;
    cx.errors += lex_errors;

    t = phase_end(PHASE_SCAN, t);

    // The node array starts small and doubles; slot 0 means "no node".
    node_new(u, AST_NONE, 0, 0, 0);

    Parser parser;
    parser_init(&parser, u);
    u->root = parse_program(&parser);

    // Both arrays end their regions, so they are cut to the declarations plus room for the
    // largest deferred body in place: the scanner bounded that body's token count while
    // skipping it, and a body has fewer nodes than tokens. Bodies are tokenized and parsed
    // into that room one at a time, so neither array grows or moves again, and the next file's
    // arrays start right after. Nothing is copied.
    size_t room = u->max_body ? u->max_body + 16 : 0;
    u->tokens.cap = u->tokens.size + room;
    region_set_end(REGION_TOKENS, u->tokens.data + u->tokens.cap);
    u->nodes.cap = u->nodes.size + room;
    region_set_end(REGION_NODES, u->nodes.data + u->nodes.cap);

    cache_unit_loaded(u);
    t = phase_end(PHASE_PARSE, t);

    sema_declare(u);
    u->broken = u->errors > 0;
    phase_end(PHASE_DECLARE, t);
    return u;
}

// ---- import("target"): the machine compiled for, as compile-time values ------------------------
//
// `target := import("target")` reads what the compiler knows about the machine (MachineTarget,
// sema/layout.c) as constants: `target.ptr_size`, `target.arch`, `target.os`, `target.has_output`.
// They are compile-time values (docs/comptime.md §2.2): module level reads them, a body cannot.
// The module is made from text, once per compilation, and needs no file.
Unit *module_target_unit(void)
{
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (cx.units[i].provided)
        {
            return &cx.units[i];
        }
    }
    const MachineTarget *t = target_get();
    static const char *const arch[] = {[TCPU_ARM64] = "arm64", [TCPU_X86_64] = "x86_64"};
    static const char *const os[] = {[TOS_MACOS] = "macos", [TOS_LINUX] = "linux", [TOS_NONE] = "none"};
    char text[512];
    int n = snprintf(text, sizeof(text),
                     "-- made by the compiler: the machine this program is compiled for\n"
                     "ptr_size := %u\nword_size := %u\narch := \"%s\"\nos := \"%s\"\n"
                     "has_output := %s\nbig_endian := %s\n",
                     t->ptr_size, t->word_size, arch[t->cpu], os[t->os],
                     (t->caps & TCAP_SYSOUT) ? "true" : "false", t->big_endian ? "true" : "false");
    char *src = (char *)arena_alloc((size_t)n + 1);
    memcpy(src, text, (size_t)n + 1);
    static const char path[] = "/(compiler)/target.modal";
    Unit *u = unit_from_source(path, path, sizeof(path) - 1, src, (size_t)n, 0, 0);
    if (u)
    {
        u->provided = 1;
        u->bound = 1; // part of no domain's face rules: everyone may import it
    }
    return u;
}

// ---- import resolution -----------------------------------------------------------------------

// Finds the file an import names, relative to the importing file:
//   import("./a/b.modal")  that file
//   import("./a/b")        a/b.modal, or the domain a/b/mod.modal
//   import(b)              a bare name: also looked for in every folder above
// Only `std` may be missing from disk; any other import that resolves to nothing is an error.
static Unit *import_target(Unit *u, NodeId import)
{
    const Token *arg = TOK(u, NODE(u, import)->tok);
    const char *name = TSTART(arg);
    int len = arg->len;
    if (arg->kind == TOK_STRING)
    {
        name++;
        len -= (len >= 2 && TSTART(arg)[len - 1] == '"') ? 2 : 1;
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

    // `std` is Modal source in the compiler's std folder (docs/std.md), read only when a
    // program imports it. Without the folder it stays external: what the compiler itself
    // provides (print, sys.put) still works, and nothing else is there.
    if (!relative && len == 6 && memcmp(name, "target", 6) == 0)
    {
        return module_target_unit();
    }
    if (!relative && len == 3 && memcmp(name, "std", 3) == 0)
    {
        char std_path[PATH_MAX];
        int n = cx.std_dir ? snprintf(std_path, sizeof(std_path), "%s/mod.modal", cx.std_dir) : -1;
        return n > 0 && (size_t)n < sizeof(std_path) ? unit_load(std_path) : NULL;
    }

    // A path (`./x`, `a/b`, `x.modal`) is looked for next to the importing file only. A bare
    // name is a module or a domain: it is looked for next to the file, then in each folder
    // above it, so `import("example")` means the same anywhere inside the project, including
    // inside `example/` itself, where it finds the domain's own mod.modal.
    char path[PATH_MAX];
    const char *const forms[] = {"%.*s/%.*s", "%.*s/%.*s.modal", "%.*s/%.*s/mod.modal"};
    int bare = !relative && !has_ext && !memchr(name, '/', (size_t)len);
    int up_len = dir_len;
    for (;;)
    {
        for (int i = has_ext ? 0 : 1; i < (has_ext ? 1 : 3); i++)
        {
            int n = snprintf(path, sizeof(path), forms[i], up_len, dir, rel_len, rel);
            if (n > 0 && (size_t)n < sizeof(path))
            {
                Unit *target = unit_load(path); // a failed open() is the "not found"
                if (target)
                {
                    return target;
                }
            }
        }
        if (!bare || !slash)
        {
            break;
        }
        while (up_len > 0 && dir[up_len - 1] != '/') // one folder up
        {
            up_len--;
        }
        if (up_len <= 1)
        {
            break;
        }
        up_len--; // drop the '/'
    }

    // Nothing was found. An unresolved import is always an error, so a mistyped or
    // unreachable path can never pass unchecked.
    if (has_ext)
    {
        diag_at(u, NODE(u, import)->tok, "error", "cannot find module %.*s (looked for %.*s/%.*s)",
                arg->len, TSTART(arg), dir_len, dir, rel_len, rel);
    }
    else
    {
        diag_at(u, NODE(u, import)->tok, "error",
                "cannot find module %.*s (looked for %.*s/%.*s.modal and %.*s/%.*s/mod.modal%s)",
                arg->len, TSTART(arg), dir_len, dir, rel_len, rel, dir_len, dir, rel_len, rel,
                bare ? ", and in the folders above" : "");
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
// below that parent. Further out, it is reached only through the parent's mod.modal. Any file
// of a domain in reach can be imported: what another domain uses is public (std.md §10).
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
    int is_std = cx.std_dir && strcmp(tdir, cx.std_dir) == 0; // a root wherever it lives

    if (!is_std && (strncmp(udir, tdir, plen) != 0 || (udir[plen] != '/' && udir[plen] != '\0')))
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

// What one name may reach across files. Every member and module-level name of a domain can be
// used; a use from another domain makes it public (std.md §10). What stays out of reach is the
// compile-time side of a domain: its mod.modal configuration and functions. Returns 0 after
// reporting a violation.
static int may_see(Unit *u, uint32_t tok, SymId id)
{
    const Token *name = TOK(u, tok);
    Symbol *s = SYM(id);
    Unit *owner = &cx.units[s->unit];

    if (owner->is_manifest && !sees_inside(u, owner))
    {
        if (s->kind == SYM_CONST)
        {
            diag_at(u, tok, "error", "'%.*s' is configuration of domain '%s', not part of its face",
                    name->len, TSTART(name), domain_name(owner->domain));
            return 0;
        }
        if (s->kind == SYM_METHOD) // compile time only, like C's `static` (docs/comptime.md §2.2)
        {
            diag_at(u, tok, "error",
                    "'%.*s' is a function of %s's mod.modal: it runs in the compiler for domain '%s' "
                    "only. To share it, define it in a module of the domain",
                    name->len, TSTART(name), domain_name(owner->domain), domain_name(owner->domain));
            return 0;
        }
        return 1; // a module mod.modal binds: a name for it
    }
    if (owner->domain != u->domain && !owner->provided && !owner->is_manifest)
    {
        s->flags |= SYM_F_PUBLIC;
        audit_use(id, u->domain);
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

    SymId id = cur.scope ? scope_find_token(cur.scope, name) : 0;
    if (!id)
    {
        if (mode == CHAIN_CHECK)
        {
            diag_at(u, tok, "error", "'%.*s' has no member '%.*s'", container_len, container,
                    name->len, TSTART(name));
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
    for (; item; item = NEXT(u, item))
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
        SymId binding = scope_find_token(u->scope, name);
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
//
// The node array is scanned linearly, not walked as a tree: a chain is checked once, from
// its outermost node, and a first pass marks which chain nodes sit inside another (one bit
// per node) so the second can tell. Two sequential sweeps over a flat array are several
// times cheaper than a traversal, and a file with no import is skipped outright, since no
// name in it can lead to another module.
void module_check_paths(Unit *u, NodeId from, NodeId to)
{
    if (!u->nimports || from >= to)
    {
        return;
    }

    ArenaMark mark = arena_mark();
    size_t words = (to - from + 63) / 64;
    uint64_t *inner = (uint64_t *)arena_alloc(words * sizeof(uint64_t));
    memset(inner, 0, words * sizeof(uint64_t));

    for (NodeId id = from; id < to; id++)
    {
        const AstNode *n = NODE(u, id);
        NodeId child = is_chain_segment(n->kind) ? n->a : n->kind == AST_VAL_DECL ? n->b : 0;
        if (child >= from) // a chain's inner nodes are created just before it
        {
            inner[(child - from) >> 6] |= (uint64_t)1 << ((child - from) & 63);
        }
    }

    for (NodeId id = from; id < to; id++)
    {
        const AstNode *n = NODE(u, id);
        if (n->kind == AST_VAL_DECL)
        {
            Target end = chain_eval(u, n->b, CHAIN_CHECK, 0);
            if (end.known)
            {
                chain_items(u, n->a, end, CHAIN_CHECK, 0);
            }
        }
        else if ((n->kind == AST_IMPORT || is_chain_segment(n->kind)) &&
                 !(inner[(id - from) >> 6] >> ((id - from) & 63) & 1))
        {
            NodeId base = id;
            while (is_chain_segment(NODE(u, base)->kind))
            {
                base = NODE(u, base)->a;
            }
            uint8_t kind = NODE(u, base)->kind;
            if (kind == AST_IMPORT || kind == AST_IDENT)
            {
                chain_eval(u, id, CHAIN_CHECK, 0);
            }
        }
    }
    arena_release(mark);
}

// Finds the file behind every import among the nodes [from, to) of a unit. Returns how many
// imports were found.
// `import("std").io...` and `import("std").sys...` are the compiler's namespaces (print, put:
// docs/std.md §4): they stay external, so a program that only prints reads no std file. The
// selector on an import is created right after it.
static int compiler_namespace(const Unit *u, NodeId import)
{
    const Token *arg = TOK(u, NODE(u, import)->tok);
    if (!((arg->len == 5 && memcmp(TSTART(arg), "\"std\"", 5) == 0) ||
          (arg->len == 3 && memcmp(TSTART(arg), "std", 3) == 0)))
    {
        return 0;
    }
    for (NodeId id = import + 1; id < u->nodes.size && id <= import + 4; id++)
    {
        const AstNode *n = NODE(u, id);
        if (n->kind == AST_SELECTOR && n->a == import)
        {
            const Token *t = TOK(u, n->tok);
            return (t->len == 2 && memcmp(TSTART(t), "io", 2) == 0) ||
                   (t->len == 3 && memcmp(TSTART(t), "sys", 3) == 0);
        }
    }
    return 0;
}

static uint32_t discover_imports(Unit *u, NodeId from, NodeId to)
{
    uint32_t found = 0;
    for (NodeId id = from; id < to; id++)
    {
        if (NODE(u, id)->kind == AST_IMPORT)
        {
            Unit *target = compiler_namespace(u, id) ? NULL : import_target(u, id);
            NODE(u, id)->aux = target ? (uint16_t)(target->id + 1) : 0;
            found++;
        }
    }
    u->nimports += found;
    return found;
}

// ---- mod.modal: configuration and the face of the domain ----------------------------------------

typedef enum
{
    CONFIG_BOOL,
    CONFIG_STRING,
    CONFIG_INT, // a whole number of bytes
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
    // Memory budgets, checked when the program is compiled (native backend):
    {"max_code", CONFIG_INT}, // bytes of machine code the program may take (flash)
    {"max_ram", CONFIG_INT},  // bytes of memory it may use at run time: stacks and buffers
    {"stack", CONFIG_INT},    // bytes of value stack for a program whose depth is not known
    {"output_buffer", CONFIG_INT}, // bytes buffered before the system output is written
    // Compile time (docs/comptime.md §4): what one evaluation may take.
    {"comptime_steps", CONFIG_INT}, // MIR instructions (1,000,000 by default)
    {"comptime_depth", CONFIG_INT}, // nested calls (1,000 by default)
    // The memory map of a bare-metal target (`--target arm64-none`): where the image goes and
    // where the stack is. flash_size and ram_size are also the default max_code and max_ram.
    {"flash_origin", CONFIG_INT},
    {"flash_size", CONFIG_INT},
    {"ram_origin", CONFIG_INT},
    {"ram_size", CONFIG_INT},
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
            memcmp(config_keys[i].name, TSTART(name), (size_t)name->len) != 0)
        {
            continue;
        }
        int is_bool =
            value->kind == AST_IDENT && ((vt->len == 4 && !memcmp(TSTART(vt), "true", 4)) ||
                                         (vt->len == 5 && !memcmp(TSTART(vt), "false", 5)));
        int is_int = value->kind == AST_NUMBER_LIT && !memchr(TSTART(vt), '.', vt->len);
        int ok = config_keys[i].kind == CONFIG_BOOL  ? is_bool
                 : config_keys[i].kind == CONFIG_INT ? is_int
                                                     : value->kind == AST_STRING_LIT;
        if (!ok)
        {
            diag_at(u, value->tok, "error", "'%.*s' expects %s", name->len, TSTART(name),
                    config_keys[i].kind == CONFIG_BOOL  ? "true or false"
                    : config_keys[i].kind == CONFIG_INT ? "a whole number of bytes"
                                                        : "a string");
        }
        return;
    }
    diag_at(u, NODE(u, decl)->tok, "error",
            "'%.*s' is not a known configuration of a domain (known: implicit_self, nil_return, "
            "profile, version, max_code, max_ram, stack, output_buffer, comptime_steps, "
            "comptime_depth, flash_origin, flash_size, ram_origin, ram_size)",
            name->len, TSTART(name));
}

// Reads one mod.modal: validates its configuration and marks the modules it binds.
static void read_manifest(Unit *m)
{
    for (NodeId d = NODE(m, m->root)->a; d; d = NEXT(m, d))
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
        for (NodeId d = NODE(m, m->root)->a; d; d = NEXT(m, d))
        {
            const Token *name = TOK(m, NODE(m, d)->tok);
            if (is_config(m, d) && name->len == klen && memcmp(TSTART(name), key, klen) == 0)
            {
                const Token *v = TOK(m, NODE(m, NODE(m, d)->a)->tok);
                return v->len == 4 && memcmp(TSTART(v), "true", 4) == 0;
            }
        }
    }
    return fallback;
}

uint64_t domain_config_int(uint16_t domain, const char *key, uint64_t fallback)
{
    size_t klen = strlen(key);
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        const Unit *m = &cx.units[i];
        if (!m->is_manifest || m->domain != domain || m->broken)
        {
            continue;
        }
        for (NodeId d = NODE(m, m->root)->a; d; d = NEXT(m, d))
        {
            const Token *name = TOK(m, NODE(m, d)->tok);
            const AstNode *v = NODE(m, NODE(m, d)->a);
            if (is_config(m, d) && name->len == klen && memcmp(TSTART(name), key, klen) == 0 &&
                v->kind == AST_NUMBER_LIT)
            {
                const Token *t = TOK(m, v->tok);
                uint64_t n = 0;
                number_value(TSTART(t), t->len, &n, NULL);
                return n;
            }
        }
    }
    return fallback;
}

// A function the domain's mod.modal declares: every file of the domain sees it by its name, after
// its own declarations (docs/comptime.md §2.2). It runs only in the compiler, so it stays
// inside the domain, as C's `static` keeps a function inside its file.
SymId module_domain_function(const Unit *u, const Token *name)
{
    if (u->is_manifest)
    {
        return 0; // its own scope already has them
    }
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        const Unit *m = &cx.units[i];
        if (m->is_manifest && m->domain == u->domain && !m->broken)
        {
            SymId s = scope_find_token(m->scope, name);
            if (s && SYM(s)->kind == SYM_CONST && is_config(m, SYM(s)->decl))
            {
                return s; // the domain's configuration: a compile-time value (`max_ram`)
            }
            return s && SYM(s)->kind == SYM_METHOD ? s : 0;
        }
    }
    return 0;
}

int module_wildcard_provides(Unit *u, const char *name, size_t len)
{
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
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

// One line of a contract: the signature, then its kind in a column (two spaces at least).
static void sig_line(const char *indent, const char *sig, const char *kind)
{
    int w = printf("%s%s", indent, sig);
    if (*kind)
    {
        printf("%*s%s", w <= 38 ? 40 - w : 2, "", kind);
    }
    putchar('\n');
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

        for (NodeId d = NODE(m, m->root)->a; d; d = NEXT(m, d))
        {
            const AstNode *n = NODE(m, d);
            if (is_config(m, d))
            {
                const Token *k = TOK(m, n->tok), *v = TOK(m, NODE(m, n->a)->tok);
                printf("config %.*s = %.*s\n", k->len, TSTART(k), v->len, TSTART(v));
            }
            else if (n->kind == AST_METHOD_DECL || (n->kind == AST_DEFINE && !decl_chain(m, d)))
            {
                sig_line("", ast.sig_str(m, d, sig, sizeof(sig)),
                         n->kind == AST_METHOD_DECL ? "compile time  (declared in mod.modal)" : "");
            }
        }

        for (uint16_t j = 0; j < cx.nunits; j++)
        {
            Unit *u = &cx.units[j];
            if (u->domain != m->domain || u->is_manifest)
            {
                continue;
            }
            // What other domains use of this module (public by use, std.md §10).
            int header = 0;
            for (SymId id = 1; id < cx.syms.size; id++)
            {
                const Symbol *s = SYM(id);
                int len;
                const char *name = symbol_name(id, &len);
                if (s->unit != u->id || (s->flags & SYM_F_LITERAL) || !(s->flags & SYM_F_PUBLIC))
                {
                    continue;
                }
                if (!header)
                {
                    printf("module %.*s\n", u->name_len, u->name);
                    header = 1;
                }
                if (s->kind == SYM_STRUCT)
                {
                    printf("  type %.*s\n", len, name);
                }
                else if (s->kind == SYM_FIELD)
                {
                    char t[64];
                    printf("    %.*s: %s%s\n", len, name, type_str(sema_value_type(u, id), t, sizeof(t)),
                           (s->flags & SYM_F_MUT) ? "  (writable)" : "");
                }
                else
                {
                    char why[160];
                    sig_line(s->owner ? "    " : "  ", ast.sig_str(u, s->decl, sig, sizeof(sig)),
                             s->kind == SYM_METHOD ? sema_fn_kind(id, why, sizeof(why)) : "");
                }
            }
        }
        putchar('\n');
    }

    // Files no contract covers (no mod.modal binds them, as an entry file): their functions and
    // kinds, which is what a reader of this file can rely on.
    for (uint16_t j = 0; j < cx.nunits; j++)
    {
        Unit *u = &cx.units[j];
        if (u->is_manifest || u->bound)
        {
            continue;
        }
        int header = 0;
        for (SymId id = 1; id < cx.syms.size; id++)
        {
            const Symbol *s = SYM(id);
            if (s->unit != u->id || s->kind != SYM_METHOD || (s->flags & SYM_F_LITERAL))
            {
                continue;
            }
            if (!header)
            {
                printf("== functions: %s\n", u->path);
                header = 1;
            }
            char why[160];
            if (sema_is_generic(id))
            {
                sig_line("  ", ast.sig_str(u, s->decl, sig, sizeof(sig)), "generic: made once per type, below");
                continue;
            }
            if (sema_spec_index(id)) // a specialization: its own types, as made
            {
                size_t k = sema_fn_text(u, id, sig, sizeof(sig));
                (void)k;
                memmove(sig + 2, sig + 9, strlen(sig + 9) + 1); // `function ` -> `  ` (indented under it)
                sig[0] = sig[1] = ' ';
            }
            else
            {
                ast.sig_str(u, s->decl, sig, sizeof(sig));
            }
            sig_line("  ", sig, sema_fn_kind(id, why, sizeof(why)));
            {
                uint8_t panics;
                const uint64_t *ids;
                uint32_t nids;
                int any;
                if (sema_error_set(id, &panics, &ids, &nids, &any) >= 0 && (panics || nids || any))
                {
                    char line[600], name[64];
                    size_t ll = 0;
                    for (uint32_t i = 0; i < nids && ll + 70 < sizeof(line); i++)
                    {
                        sema_error_name(ids[i], name, sizeof(name));
                        ll += (size_t)snprintf(line + ll, sizeof(line) - ll, "%s%s", ll ? ", " : "", name);
                    }
                    for (int k = 0; k < 7 && ll + 70 < sizeof(line); k++)
                    {
                        if (panics & (1u << k))
                        {
                            sema_error_name((uint64_t)(PANIC_DIVISION + k), name, sizeof(name));
                            ll += (size_t)snprintf(line + ll, sizeof(line) - ll, "%s%s", ll ? ", " : "", name);
                        }
                    }
                    if (any)
                    {
                        snprintf(line + ll, sizeof(line) - ll, "%san error it holds", ll ? ", " : "");
                    }
                    printf("      errors: %s\n", line);
                }
            }
            uint32_t budget;
            int notes = comptime_fold_notes(id, &budget);
            if (notes & FOLD_NOTE_VALUE)
            {
                printf("      ran while compiling, for a value the program uses (a module-level binding, a type, ...)\n");
            }
            if (notes & FOLD_NOTE_DONE)
            {
                printf("      a call with known arguments was computed while compiling\n");
            }
            if (notes & FOLD_NOTE_BUDGET)
            {
                printf("      a call with known arguments is left to run time: it takes more than %u steps, "
                       "the limit for computing one while compiling\n",
                       budget);
            }
            if (notes & FOLD_NOTE_REFUSED)
            {
                printf("      a call with known arguments is left to run time: it does what only the running "
                       "program can (see its kind)\n");
            }
        }
        if (header)
        {
            putchar('\n');
        }
    }
}

// ---- linking ---------------------------------------------------------------------------------

// Brings every unit loaded so far up to date: its domain's mod.modal is loaded, its imports
// are resolved (which may load more units, handled by the same loop), and if it is a
// manifest its configuration and bindings are read. Called again whenever a function body
// turns out to contain an import.
static void link_new_units(void)
{
    while (cx.linked < cx.nunits)
    {
        Unit *u = &cx.units[cx.linked++];
        uint64_t t = clock_ns();
        uint64_t loading =
            cx.phase_ns[PHASE_SCAN] + cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_DECLARE];

        // A domain's mod.modal is looked for once, not once per file of the domain.
        if (!u->is_manifest && !cx.domain_manifest[u->domain])
        {
            cx.domain_manifest[u->domain] = 1;
            char manifest[PATH_MAX];
            const char *slash = strrchr(u->path, '/');
            int n = snprintf(manifest, sizeof(manifest), "%.*s/mod.modal",
                             slash ? (int)(slash - u->path) : 1, slash ? u->path : ".");
            if (n > 0 && (size_t)n < sizeof(manifest))
            {
                unit_load(manifest);
            }
        }
        discover_imports(u, 1, (NodeId)u->nodes.size);
        u->module_imports = u->nimports;
        if (u->is_manifest && !u->broken)
        {
            read_manifest(u);
        }

        // loading an imported file is accounted to its own scan/parse/declare
        cx.phase_ns[PHASE_LINK] += clock_ns() - t -
                                   (cx.phase_ns[PHASE_SCAN] + cx.phase_ns[PHASE_PARSE] +
                                    cx.phase_ns[PHASE_DECLARE] - loading);
    }
}

// What a binding names (`point := import("./geo").point`), through re-exports; 0 if it names
// no declaration.
SymId module_binding_target(SymId binding)
{
    for (int depth = 0; binding && SYM(binding)->kind == SYM_IMPORT && depth < FOLLOW_MAX; depth++)
    {
        Unit *owner = &cx.units[SYM(binding)->unit];
        const AstNode *decl = NODE(owner, SYM(binding)->decl);
        if (decl->kind != AST_DEFINE || NODE(owner, decl->a)->kind != AST_SELECTOR)
        {
            return 0;
        }
        const AstNode *sel = NODE(owner, decl->a);
        binding = module_member(owner, sel->a, sel->tok);
    }
    return binding && SYM(binding)->kind != SYM_IMPORT ? binding : 0;
}

// The function a binding names (`assert := import("std").assert`), through re-exports; 0 if it
// names no function.
SymId module_binding_function(SymId binding)
{
    SymId t = module_binding_target(binding);
    return t && SYM(t)->kind == SYM_METHOD ? t : 0;
}

// What `base.name` names when `base` leads to a module (an import binding, or a path through
// them): its declaration there, or 0. Visibility is checked separately (module_check_paths).
SymId module_member(Unit *u, NodeId base, uint32_t name_tok)
{
    Target t = chain_eval(u, base, CHAIN_SILENT, 0);
    if (!t.known || !t.scope)
    {
        return 0;
    }
    return scope_find_token(t.scope, TOK(u, name_tok));
}

// A body just parsed: one walk finds its imports and whether it has a path to check at all (an
// import, a selector, a val); most bodies have neither, and the two walks of module_check_paths
// are not made for them.
void module_body_nodes(Unit *u, NodeId from)
{
    NodeId to = (NodeId)u->nodes.size;
    uint32_t found = 0;
    int paths = 0;
    for (NodeId id = from; id < to; id++)
    {
        uint8_t kind = NODE(u, id)->kind;
        if (kind == AST_IMPORT)
        {
            Unit *target = compiler_namespace(u, id) ? NULL : import_target(u, id);
            NODE(u, id)->aux = target ? (uint16_t)(target->id + 1) : 0;
            found++;
        }
        paths |= kind == AST_IMPORT || kind == AST_VAL_DECL || is_chain_segment(kind);
    }
    u->nimports += found;
    if (found)
    {
        link_new_units(); // an import inside a body: its file must be linked before use
    }
    if (paths)
    {
        module_check_paths(u, from, (NodeId)u->nodes.size);
    }
}

// Files in checking order: the ones a file imports before it (a depth-first walk over the imports
// of its declarations; a cycle keeps the order files were loaded in).
static void unit_order(uint16_t i, uint16_t n, uint8_t *state, uint16_t *order, uint16_t *norder)
{
    if (state[i])
    {
        return;
    }
    state[i] = 1;
    const Unit *u = &cx.units[i];
    if (!u->broken && u->root)
    {
        for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
        {
            const AstNode *dn = NODE(u, d);
            NodeId imp = dn->kind == AST_DEFINE ? ast_import_root(u, dn->a) : 0;
            uint16_t to = imp && NODE(u, imp)->aux ? (uint16_t)(NODE(u, imp)->aux - 1) : (uint16_t)0xffff;
            if (to < n && to != i)
            {
                unit_order(to, n, state, order, norder);
            }
        }
    }
    order[(*norder)++] = i;
}

void modules_link(void)
{
    sema_kinds_reset();
    comptime_reset();
    link_new_units();

    // Every file's declarations first (load order), the entry's module values and the backend's
    // start, then the bodies: a file a file imports before it, so a function of another file is
    // checked (what it can raise is known, it is lowered: inlined if small) before its callers
    // (context.md §63). Files a body brings in while checked go after, as they come.
    uint16_t first = cx.nunits;
    uint8_t *skip = (uint8_t *)calloc(first + 1, 1);
    uint16_t *order = (uint16_t *)malloc((first + 1) * sizeof(uint16_t));
    uint8_t *state = (uint8_t *)calloc(first + 1, 1);
    uint32_t *decl_names = (uint32_t *)calloc(first + 1, sizeof(uint32_t)); // what its declarations
    uint32_t *decl_calls = (uint32_t *)calloc(first + 1, sizeof(uint32_t)); // resolved (the cache's count)
    for (uint16_t i = 0; i < first; i++)
    {
        Unit *u = &cx.units[i];
        if (u->broken)
        {
            sema_syntax_only(u); // still report the syntax errors inside its bodies
            skip[i] = 1;
            continue;
        }
        uint64_t t = clock_ns();
        module_check_paths(u, 1, (NodeId)u->nodes.size);
        t = phase_end(PHASE_LINK, t);
        if (cache_lookup(u))
        {
            phase_end(PHASE_CHECK, t);
            skip[i] = 1; // unchanged, and so were the declarations it can see: checked before
            continue;
        }
        uint32_t n0 = cx.names_resolved, c0 = cx.calls_resolved;
        sema_check_decls(u);
        decl_names[i] = cx.names_resolved - n0;
        decl_calls[i] = cx.calls_resolved - c0;
        phase_end(PHASE_CHECK, t);
    }
    if (first && !skip[0])
    {
        Unit *entry = &cx.units[0];
        entry->valued = 1;
        if (!entry->errors)
        {
            comptime_unit(entry); // C writes its module values at the start
        }
        codegen_begin(entry);
    }
    uint16_t norder = 0;
    for (uint16_t i = 0; i < first; i++)
    {
        unit_order(i, first, state, order, &norder);
    }
    for (uint16_t k = 0; k < norder; k++)
    {
        uint16_t i = order[k];
        if (skip[i])
        {
            continue;
        }
        Unit *u = &cx.units[i];
        uint64_t t = clock_ns();
        uint64_t inside = cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_EMIT];
        uint32_t names = cx.names_resolved, calls = cx.calls_resolved;
        uint16_t units = cx.nunits;
        sema_check_bodies(u);
        if (!u->errors && cx.nunits == units) // clean, and no body brought in another file
        {
            cache_record(u, cx.names_resolved - names + decl_names[i], cx.calls_resolved - calls + decl_calls[i]);
        }
        cx.phase_ns[PHASE_CHECK] +=
            clock_ns() - t - (cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_EMIT] - inside);
    }
    free(skip);
    free(order);
    free(state);
    free(decl_names);
    free(decl_calls);
    for (uint16_t i = first; i < cx.nunits; i++) // brought in by a body: in turn
    {
        Unit *u = &cx.units[i];
        if (u->broken)
        {
            sema_syntax_only(u);
            continue;
        }
        uint64_t t = clock_ns();
        module_check_paths(u, 1, (NodeId)u->nodes.size);
        t = phase_end(PHASE_LINK, t);
        if (cache_lookup(u))
        {
            phase_end(PHASE_CHECK, t);
            continue;
        }
        uint64_t inside = cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_EMIT];
        uint32_t names = cx.names_resolved, calls = cx.calls_resolved;
        uint16_t units = cx.nunits;
        sema_check(u);
        if (!u->errors && cx.nunits == units)
        {
            cache_record(u, cx.names_resolved - names, cx.calls_resolved - calls);
        }
        cx.phase_ns[PHASE_CHECK] +=
            clock_ns() - t - (cx.phase_ns[PHASE_PARSE] + cx.phase_ns[PHASE_EMIT] - inside);
    }
    sema_errors_finish(); // the errors each function can give are known now
}

// ---- what the program can reach, from its text (development.md §9.4.2) ------------------------
//
// Native generated every function and dropped those the entry does not reach at the end (shake):
// a function nobody calls cost its lowering, optimizing and emitting. Callees are generated
// first, before their callers are checked, so the call graph is not known yet when a callee's
// turn comes. This is found once, before the first function is generated, from the text alone.
// The entry file is generated whole (a program's own file holds little it does not use, and
// reading its bodies cost 7% of the compile of a file whose functions are all used): what it
// reaches in other files is what its imports bind. A binding of a module makes that file whole
// too; a binding of a declaration reaches it, and from there every name a reached declaration's
// text holds reaches every declaration of that name (a function, a constant, a struct and all its
// methods, a binding and what it names). A name in a comment or a string reaches too: it can only
// keep more, never less, than the calls do. An `import(` in a whole file's body turns it all off.
static uint8_t *reach;      // per symbol: 1 reached (0 past reach_n: not known, so reached)
static uint8_t *reach_whole; // per unit: every function of it is generated
static size_t reach_n;      // symbols when it was found
static uint16_t reach_units; // units when it was found: a later unit turns it off
static int reach_state;     // 0 not found yet, 1 found, 2 off (everything is generated)

typedef struct
{
    uint32_t *start; // per top-level declaration of a unit, in source order: its first byte
    uint32_t *tok;   // and its first module-level token
    uint32_t n;
    uint8_t *scanned;
} DeclRanges;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// A symbol's declaration: the index of the top-level declaration whose text holds its name.
static uint32_t decl_index(const DeclRanges *r, uint32_t off)
{
    uint32_t lo = 0, hi = r->n;
    while (hi - lo > 1)
    {
        uint32_t mid = (lo + hi) / 2;
        if (r->start[mid] <= off)
        {
            lo = mid;
        }
        else
        {
            hi = mid;
        }
    }
    return lo;
}

// A unit's declarations and their first tokens (the module-level ones, in order: a body being
// checked appends its own after them, so they stop where an offset goes back).
static void decl_ranges(DeclRanges *r, uint16_t i)
{
    Unit *u = &cx.units[i];
    uint32_t count = 0;
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        count++;
    }
    r->start = malloc((count + 1) * sizeof(uint32_t));
    r->tok = malloc((count + 2) * sizeof(uint32_t));
    r->scanned = calloc(count + 1, 1);
    r->start[0] = 0; // text before the first declaration belongs to it
    uint32_t k = 1;
    for (NodeId d = NODE(u, u->root)->a; d; d = NEXT(u, d))
    {
        r->start[k++] = (uint32_t)(TSTART(TOK(u, NODE(u, d)->tok)) - u->src);
    }
    qsort(r->start + 1, count, sizeof(uint32_t), cmp_u32);
    r->n = count + 1;
    size_t ntok = 0;
    while (ntok < u->tokens.size && (ntok == 0 || u->tokens.data[ntok].off >= u->tokens.data[ntok - 1].off))
    {
        ntok++;
    }
    size_t t = 0;
    for (uint32_t d = 0; d < r->n; d++)
    {
        while (t < ntok && (uint32_t)(TSTART(&u->tokens.data[t]) - u->src) < r->start[d])
        {
            t++;
        }
        r->tok[d] = (uint32_t)t;
    }
    r->tok[r->n] = (uint32_t)ntok;
}

// A cheap filter in front of name_find for a body's names: most (`return`, a parameter) name no
// declaration. Two bits per declared name, from its length and three of its bytes.
#define REACH_FILTER_BITS (1u << 18)
static uint32_t reach_filter_hash(const char *p, uint32_t n)
{
    uint32_t h = n * 0x9e3779b1u;
    h ^= (uint8_t)p[0] * 0x85ebca6bu;
    h ^= (uint8_t)p[n - 1] * 0xc2b2ae35u;
    h ^= (uint8_t)p[n / 2] * 0x27d4eb2fu;
    return h ^ (h >> 15);
}

// How many times a file's text holds the word `import` outside a `--` comment or a string on its
// line (memchr for its first byte; the line is read from its start). A `-{ }-` block over several
// lines is still counted: that can only turn pruning off, never miss an import.
static uint32_t text_imports(const char *src, size_t len)
{
    const char *p = src, *end = src + len;
    uint32_t n = 0;
    while (p + 6 <= end && (p = memchr(p, 'i', (size_t)(end - p - 5))) != NULL)
    {
        const char *at = p++;
        if (memcmp(at, "import", 6) != 0 ||
            (at > src && (at[-1] == '_' || ((unsigned char)at[-1] | 32) - 'a' < 26u || (unsigned char)at[-1] - '0' < 10u)))
        {
            continue;
        }
        const char *line = at;
        while (line > src && line[-1] != '\n')
        {
            line--;
        }
        int in_string = 0, comment = 0;
        for (const char *c = line; c < at && !comment; c++)
        {
            if (in_string)
            {
                c += *c == '\\' && c + 1 < at;
                in_string = *c != '"';
            }
            else if (*c == '"')
            {
                in_string = 1;
            }
            else if (c[0] == '-' && c + 1 < at && c[1] == '-')
            {
                comment = 1;
            }
        }
        n += !comment && !in_string;
        p = at + 6;
    }
    return n;
}

static void reach_compute(void)
{
    reach_state = 2;
    SymId main_fn = cx.nunits ? scope_find(cx.units[0].scope, "main", 4) : 0;
    if (cx.eager || !main_fn || SYM(main_fn)->kind != SYM_METHOD)
    {
        return; // no entry to start from (or bodies parsed with their file): everything
    }
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        if (!cx.units[i].src || cx.units[i].broken)
        {
            return;
        }
    }
    size_t n = cx.syms.size;
    // symbols by name (a list per NameId), and methods by their struct
    SymId *by_name = calloc(cx.nnames + 1, sizeof(SymId));
    SymId *next_name = calloc(n, sizeof(SymId));
    SymId *by_owner = calloc(n, sizeof(SymId));
    SymId *next_owner = calloc(n, sizeof(SymId));
    SymId *imports_of = calloc(cx.nunits, sizeof(SymId)); // per unit: its bindings
    SymId *next_import = calloc(n, sizeof(SymId));

    uint64_t *filter = calloc(REACH_FILTER_BITS / 64, sizeof(uint64_t));
    uint64_t *declared = calloc(cx.nnames / 64 + 1, sizeof(uint64_t)); // per NameId: a declaration has it

    for (SymId s = 1; s < n; s++)
    {
        const Symbol *sym = SYM(s);
        if (sym->kind == SYM_FIELD || sym->kind == SYM_NONE || (sym->flags & SYM_F_LITERAL) || !sym->decl)
        {
            continue;
        }
        if (sym->kind == SYM_IMPORT && sym->unit < cx.nunits)
        {
            next_import[s] = imports_of[sym->unit];
            imports_of[sym->unit] = s;
        }
        if (sym->unit == 0)
        {
            continue; // the entry: whole, and no other file names its declarations
        }
        NameId nm = cx.sym_names[s];
        if (nm && nm <= cx.nnames)
        {
            if (!by_name[nm])
            {
                const Name *name = name_at(nm);
                if (name->len)
                {
                    uint32_t h = reach_filter_hash(name->start, (uint32_t)name->len);
                    filter[(h % REACH_FILTER_BITS) / 64] |= 1ull << (h % 64);
                    h = h * 0x2545f491u;
                    filter[(h % REACH_FILTER_BITS) / 64] |= 1ull << (h % 64);
                }
            }
            next_name[s] = by_name[nm];
            by_name[nm] = s;
            declared[nm / 64] |= 1ull << (nm % 64);
        }
        if (sym->kind == SYM_METHOD && sym->owner && sym->owner < n)
        {
            next_owner[s] = by_owner[sym->owner];
            by_owner[sym->owner] = s;
        }
    }
    DeclRanges *ranges = calloc(cx.nunits, sizeof(DeclRanges)); // per unit, made when first read
    reach = realloc(reach, n);
    memset(reach, 0, n);
    reach_whole = realloc(reach_whole, cx.nunits);
    memset(reach_whole, 0, cx.nunits);
    reach_n = n;
    reach_units = cx.nunits;
    SymId *work = malloc(n * sizeof(SymId));
    size_t nwork = 0;
    int off = 0; // an import inside a body: a file may come in later, calling anything
#define REACH(s)                                                                                       \
    do                                                                                                 \
    {                                                                                                  \
        SymId r_ = (s);                                                                                \
        if (r_ && r_ < n && !reach[r_])                                                                \
        {                                                                                              \
            reach[r_] = 1;                                                                             \
            work[nwork++] = r_;                                                                        \
        }                                                                                              \
    } while (0)
#define REACH_NAME(nm)                                                                                 \
    do                                                                                                 \
    {                                                                                                  \
        NameId n_ = (nm);                                                                              \
        for (SymId t_ = n_ && n_ <= cx.nnames ? by_name[n_] : 0; t_; t_ = next_name[t_])               \
        {                                                                                              \
            REACH(t_);                                                                                 \
        }                                                                                              \
    } while (0)
    // a whole file: its bindings are what it reaches elsewhere; an `import(` in a body is not seen
    // by them, so it turns pruning off
#define WHOLE(ui)                                                                                      \
    do                                                                                                 \
    {                                                                                                  \
        uint16_t w_ = (ui);                                                                            \
        if (w_ < cx.nunits && !reach_whole[w_])                                                        \
        {                                                                                              \
            reach_whole[w_] = 1;                                                                       \
            Unit *wu_ = &cx.units[w_];                                                                 \
            off |= text_imports(wu_->src, wu_->len) != wu_->module_imports; /* one in a body, or a comment */ \
            for (SymId b_ = imports_of[w_]; b_; b_ = next_import[b_])                                  \
            {                                                                                          \
                REACH(b_);                                                                             \
            }                                                                                          \
        }                                                                                              \
    } while (0)
    WHOLE(0);
    int partial = 0; // a file that is not whole has a function: something may be dropped
    for (SymId f = 1; f < n && !partial; f++)
    {
        partial = SYM(f)->kind == SYM_METHOD && SYM(f)->unit < cx.nunits && !reach_whole[SYM(f)->unit] &&
                  !cx.units[SYM(f)->unit].is_manifest;
    }
    off |= !partial;
    while (nwork && !off)
    {
        SymId s = work[--nwork];
        const Symbol *sym = SYM(s);
        if (sym->kind == SYM_STRUCT) // its methods, called by name or by the compiler (`out.put`)
        {
            for (SymId m = by_owner[s]; m; m = next_owner[m])
            {
                REACH(m);
            }
        }
        if (sym->kind == SYM_IMPORT)
        {
            SymId target = module_binding_target(s);
            if (target)
            {
                REACH(target);
            }
            else // a module (`util := import("util")`), or a path into one: that file, whole
            {
                Unit *owner = &cx.units[sym->unit];
                const AstNode *decl = NODE(owner, sym->decl);
                Target t = decl->kind == AST_DEFINE ? chain_eval(owner, decl->a, CHAIN_SILENT, 0) : (Target){0};
                if (t.known && t.unit)
                {
                    WHOLE(t.unit->id);
                }
            }
            continue;
        }
        if (sym->kind == SYM_METHOD)
        {
            for (SymId o = sym->next_overload; o; o = SYM(o)->next_overload)
            {
                REACH(o); // its clauses and overloads: one name
            }
        }
        if (sym->unit < cx.nunits && reach_whole[sym->unit])
        {
            continue; // generated with its file: what it reaches elsewhere, its file's bindings say
        }
        Unit *u = &cx.units[sym->unit];
        DeclRanges *r = &ranges[sym->unit];
        if (!r->start)
        {
            decl_ranges(r, sym->unit);
        }
        uint32_t at = decl_index(r, (uint32_t)(TSTART(TOK(u, sym->tok)) - u->src));
        if (r->scanned[at])
        {
            continue;
        }
        r->scanned[at] = 1;
        for (uint32_t k = r->tok[at]; k < r->tok[at + 1]; k++)
        {
            const Token *tk = &u->tokens.data[k];
            if (tk->kind == TOK_IDENTIFIER) // a signature or a one-line body: named by the scanner
            {
                NameId nm = tk->match ? (NameId)tk->match : name_find(TSTART(tk), tk->len);
                if (nm <= cx.nnames && (declared[nm / 64] >> (nm % 64) & 1))
                {
                    REACH_NAME(nm);
                }
                continue;
            }
            if (tk->kind != TOK_BODY)
            {
                continue;
            }
            const char *text = TSTART(tk);
            uint32_t end = tk->len;
            for (uint32_t i = 0; i < end;)
            {
                unsigned char ch = (unsigned char)text[i];
                if (!(ch == '_' || (ch | 32) - 'a' < 26u))
                {
                    i++;
                    continue;
                }
                uint32_t b = i;
                while (i < end && (text[i] == '_' || ((unsigned char)text[i] | 32) - 'a' < 26u ||
                                   (unsigned char)text[i] - '0' < 10u))
                {
                    i++;
                }
                uint32_t len = i - b;
                if (len == 6 && memcmp(text + b, "import", 6) == 0)
                {
                    off = 1;
                    break;
                }
                uint32_t h = reach_filter_hash(text + b, len);
                uint32_t h2 = h * 0x2545f491u;
                if (!(filter[(h % REACH_FILTER_BITS) / 64] >> (h % 64) & 1) ||
                    !(filter[(h2 % REACH_FILTER_BITS) / 64] >> (h2 % 64) & 1))
                {
                    continue; // names no declaration
                }
                REACH_NAME(name_find(text + b, len));
            }
        }
    }
#undef WHOLE
#undef REACH_NAME
#undef REACH
    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        free(ranges[i].start);
        free(ranges[i].tok);
        free(ranges[i].scanned);
    }
    free(ranges);
    free(work);
    free(by_name);
    free(next_name);
    free(by_owner);
    free(next_owner);
    free(filter);
    free(declared);
    free(imports_of);
    free(next_import);

    reach_state = off ? 2 : 1;
}

int module_reachable(SymId fn)
{
    if (!reach_state)
    {
        reach_compute();
    }
    if (reach_state != 1 || cx.nunits != reach_units)
    {
        return 1;
    }
    if (fn < reach_n && reach_whole[SYM(fn)->unit])
    {
        return 1; // its file is generated whole (the entry's always is): the common case, first
    }
    SymId of = sema_spec_generic(fn);
    if (of)
    {
        fn = of; // a specialization: reached when its generic is
    }
    if (fn >= reach_n || (SYM(fn)->flags & SYM_F_LITERAL))
    {
        return 1; // made after (the compiler's routines), or a literal: generated in its body's turn
    }
    return reach[fn] || (SYM(fn)->unit < reach_units && reach_whole[SYM(fn)->unit]);
}

void module_reachable_reset(void)
{
    reach_state = 0;
}

void module_reachable_prepare(void) // before any body is checked: the units hold only their own tokens
{
    if (!reach_state)
    {
        reach_compute();
    }
}
