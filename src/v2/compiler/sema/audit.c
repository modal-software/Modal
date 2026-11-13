// audit.c (v2): `--audit`, what a program depends on and where it can stop (std.md §12).
//
// Nothing here runs in a normal compile: every hook returns at once unless cx.audit is set, and
// the report is made only of what the compiler knows anyway.
//   - dependencies: each use of a member of another domain (check.c note_use, module.c may_see)
//   - reach: the calls in each function's final MIR, followed from main
//   - stops at run time: the MIR_TRAPs left after optimization (a guard the compiler could not
//     prove away), with the place and the reason the trap's message carries
//   - the stack: a function that calls itself not as a tail call takes a frame per turn
//   - the system: the writes each function makes
#include "v2/compiler/cx.h"
#include "v2/compiler/mir/mir.h"
#include "v2/compiler/syntax/scanner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    SymId member;
    uint16_t domain; // the domain that uses it
} Use;

typedef struct
{
    SymId from, to;
} Edge;

typedef struct
{
    SymId fn;
    char *text; // "path:line:col: reason"
} Stop;

static struct
{
    Use *uses;
    size_t nuses, cuses;
    Edge *edges;
    size_t nedges, cedges;
    Stop *stops;
    size_t nstops, cstops;
    SymId *self_calls; // functions that call themselves not as a tail call
    size_t nself, cself;
    SymId *writers;
    uint32_t *writes;
    size_t nwriters, cwriters;
    SymId *lowered;
    size_t nlowered, clowered;
    SymId *ran; // functions the compiler ran (module-level values, asserts)
    size_t nran, cran;
} A;

#define PUSH(arr, n, cap, value)                                                                   \
    do                                                                                             \
    {                                                                                              \
        if ((n) == (cap))                                                                          \
        {                                                                                          \
            (cap) = (cap) ? (cap) * 2 : 64;                                                        \
            (arr) = realloc((arr), (cap) * sizeof(*(arr)));                                        \
        }                                                                                          \
        (arr)[(n)++] = (value);                                                                    \
    } while (0)

void audit_use(SymId member, uint16_t domain)
{
    if (!cx.audit)
    {
        return;
    }
    for (size_t i = A.nuses; i > 0 && i + 16 > A.nuses; i--) // the same use, just before: once
    {
        if (A.uses[i - 1].member == member && A.uses[i - 1].domain == domain)
        {
            return;
        }
    }
    PUSH(A.uses, A.nuses, A.cuses, ((Use){member, domain}));
}

void audit_call(SymId from, SymId to)
{
    if (!cx.audit || !from)
    {
        return;
    }
    for (size_t i = A.nedges; i > 0 && i + 16 > A.nedges; i--)
    {
        if (A.edges[i - 1].from == from && A.edges[i - 1].to == to)
        {
            return;
        }
    }
    PUSH(A.edges, A.nedges, A.cedges, ((Edge){from, to}));
}

void audit_comptime(SymId fn)
{
    if (!cx.audit)
    {
        return;
    }
    for (size_t i = 0; i < A.nran; i++)
    {
        if (A.ran[i] == fn)
        {
            return;
        }
    }
    PUSH(A.ran, A.nran, A.cran, fn);
}

// The text a slot holds, when an MIR_STR before instruction `at` made it.
static const MirString *string_of(const MirFunc *f, size_t at, MirSlot s)
{
    for (size_t i = at; i > 0; i--)
    {
        const MirInst *in = &f->code[i - 1];
        if (in->op == MIR_STR && in->d == s)
        {
            return &f->strings[in->imm];
        }
    }
    return NULL;
}

static int same_function(SymId a, SymId b)
{
    if (a == b)
    {
        return 1;
    }
    int la, lb;
    const char *na = symbol_name(a, &la), *nb = symbol_name(b, &lb);
    return SYM(a)->kind == SYM_METHOD && SYM(b)->kind == SYM_METHOD && SYM(a)->owner == SYM(b)->owner &&
           SYM(a)->unit == SYM(b)->unit && la == lb && memcmp(na, nb, (size_t)la) == 0; // clauses
}

void audit_function(const MirFunc *f)
{
    if (!cx.audit || !f->sym)
    {
        return;
    }
    PUSH(A.lowered, A.nlowered, A.clowered, f->sym);
    char msg[700];
    size_t len = 0;
    uint32_t writes = 0;
    int self_call = 0;
    for (size_t i = 0; i < f->n; i++)
    {
        const MirInst *in = &f->code[i];
        if (in->op == MIR_CALL)
        {
            SymId to = (SymId)in->imm;
            audit_call(f->sym, to);
            self_call |= in->sub != MIR_CALL_TAIL && same_function(f->sym, to);
        }
        else if (in->op == MIR_WRITE)
        {
            writes++;
        }
        else if (in->op == MIR_TRAP)
        {
            const MirString *s = string_of(f, i, in->a);
            size_t n = s ? s->len : 3;
            if (len + n >= sizeof(msg))
            {
                n = sizeof(msg) - 1 - len;
            }
            memcpy(msg + len, s ? (const char *)s->bytes : "...", n);
            len += n;
            if (in->sub == MIR_TRAP_PART)
            {
                continue;
            }
            while (len && (msg[len - 1] == '\n' || msg[len - 1] == ' '))
            {
                len--;
            }
            msg[len] = 0;
            PUSH(A.stops, A.nstops, A.cstops, ((Stop){f->sym, strdup(msg)}));
            len = 0;
        }
    }
    if (self_call)
    {
        PUSH(A.self_calls, A.nself, A.cself, f->sym);
    }
    if (writes)
    {
        if (A.nwriters == A.cwriters)
        {
            A.cwriters = A.cwriters ? A.cwriters * 2 : 64;
            A.writers = realloc(A.writers, A.cwriters * sizeof(*A.writers));
            A.writes = realloc(A.writes, A.cwriters * sizeof(*A.writes));
        }
        A.writers[A.nwriters] = f->sym;
        A.writes[A.nwriters++] = writes;
    }
}

static const char *where(SymId s, char *buf, size_t size)
{
    const Unit *u = &cx.units[SYM(s)->unit];
    int line = 0, col = 0;
    source_pos(u->src, TSTART(TOK(u, SYM(s)->tok)), &line, &col);
    snprintf(buf, size, "%s:%d", u->path, line);
    return buf;
}

static void print_name(SymId s)
{
    int len;
    const char *name = symbol_name(s, &len);
    if (SYM(s)->owner)
    {
        int olen;
        const char *owner = symbol_name(SYM(s)->owner, &olen);
        printf("%.*s.", olen, owner);
    }
    printf("%.*s", len, name);
}

static int ran_by_compiler(SymId s)
{
    for (size_t i = 0; i < A.nran; i++)
    {
        if (same_function(A.ran[i], s))
        {
            return 1;
        }
    }
    return 0;
}

static int is_std(const Unit *u)
{
    return cx.std_dir && strcmp(cx.domains[u->domain], cx.std_dir) == 0;
}

static int by_domain_then_member(const void *a, const void *b)
{
    const Use *x = a, *y = b;
    uint16_t dx = cx.units[SYM(x->member)->unit].domain, dy = cx.units[SYM(y->member)->unit].domain;
    if (x->domain != y->domain)
    {
        return x->domain < y->domain ? -1 : 1;
    }
    if (dx != dy)
    {
        return dx < dy ? -1 : 1;
    }
    return x->member < y->member ? -1 : x->member > y->member;
}

void audit_print(void)
{
    char at[600];
    printf("== audit\n");

    // 1. Dependencies: domain -> domain, with what is used.
    printf("dependencies (what each domain uses of another; std.md §10: public by this use)\n");
    qsort(A.uses, A.nuses, sizeof(*A.uses), by_domain_then_member);
    size_t shown = 0;
    for (size_t i = 0; i < A.nuses; i++)
    {
        const Use *x = &A.uses[i];
        uint16_t to = cx.units[SYM(x->member)->unit].domain;
        if (i && x->member == A.uses[i - 1].member && x->domain == A.uses[i - 1].domain)
        {
            continue;
        }
        int first = !i || x->domain != A.uses[i - 1].domain ||
                    to != cx.units[SYM(A.uses[i - 1].member)->unit].domain;
        if (first)
        {
            printf("%s  %s -> %s:", shown ? "\n" : "", domain_name(x->domain), domain_name(to));
        }
        putchar(' ');
        print_name(x->member);
        shown++;
    }
    printf(shown ? "\n" : "  none\n");

    // 2. Reach: from main through the calls. What is not reached is not in the program.
    SymId root = 0;
    for (size_t i = 0; i < A.nlowered; i++)
    {
        int len;
        const char *name = symbol_name(A.lowered[i], &len);
        if (SYM(A.lowered[i])->unit == 0 && !SYM(A.lowered[i])->owner && len == 4 && memcmp(name, "main", 4) == 0)
        {
            root = A.lowered[i];
        }
    }
    uint8_t *reached = calloc(cx.syms.size + 1, 1);
    SymId *work = malloc((cx.syms.size + 1) * sizeof(SymId));
    size_t nwork = 0;
    if (root)
    {
        reached[root] = 1;
        work[nwork++] = root;
    }
    while (nwork)
    {
        SymId s = work[--nwork];
        for (size_t i = 0; i < A.nedges; i++)
        {
            if (A.edges[i].to < cx.syms.size && !reached[A.edges[i].to] && same_function(A.edges[i].from, s))
            {
                reached[A.edges[i].to] = 1;
                work[nwork++] = A.edges[i].to;
            }
        }
        // the other clauses of a reached function are reached with it
        for (size_t i = 0; i < A.nlowered; i++)
        {
            if (!reached[A.lowered[i]] && same_function(A.lowered[i], s))
            {
                reached[A.lowered[i]] = 1;
                work[nwork++] = A.lowered[i];
            }
        }
    }
    printf("not reached from main (left out of the program)\n");
    size_t unreached = 0;
    SymId last = 0;
    for (SymId s = 1; root && s < cx.syms.size; s++)
    {
        const Symbol *sym = SYM(s);
        const Unit *u = &cx.units[sym->unit];
        if (sym->kind != SYM_METHOD || (sym->flags & SYM_F_LITERAL) || reached[s] || u->is_manifest || is_std(u) ||
            u->provided || ran_by_compiler(s) || (last && same_function(last, s)))
        {
            continue;
        }
        last = s;
        printf("  %s  ", where(s, at, sizeof(at)));
        print_name(s);
        putchar('\n');
        unreached++;
    }
    if (!root)
    {
        printf("  (no main: every function is kept)\n");
    }
    else if (!unreached)
    {
        printf("  none\n");
    }
    printf("run only while compiling (not in the program)\n");
    last = 0;
    size_t ran = 0;
    for (SymId s = 1; s < cx.syms.size; s++)
    {
        if (!ran_by_compiler(s) || (root && reached[s]) || (last && same_function(last, s)) ||
            is_std(&cx.units[SYM(s)->unit]))
        {
            continue;
        }
        last = s;
        printf("  %s  ", where(s, at, sizeof(at)));
        print_name(s);
        putchar('\n');
        ran++;
    }
    if (!ran)
    {
        printf("  none\n");
    }

    // 3. Where the program can stop: each guard left in the code.
    size_t nstops = 0;
    for (size_t i = 0; i < A.nstops; i++)
    {
        int keep = !(A.stops[i].fn < cx.syms.size && root && !reached[A.stops[i].fn]) &&
                   !is_std(&cx.units[SYM(A.stops[i].fn)->unit]); // std's own: each caller's copy is listed
        for (size_t j = 0; keep && j < i; j++)
        {
            keep = A.stops[j].text == NULL || strcmp(A.stops[j].text, A.stops[i].text) != 0;
        }
        if (!keep)
        {
            free(A.stops[i].text);
            A.stops[i].text = NULL;
            continue;
        }
        nstops++;
    }
    printf("can stop at run time (%zu guard%s the compiler could not prove away)\n", nstops,
           nstops == 1 ? "" : "s");
    for (size_t i = 0; i < A.nstops; i++)
    {
        if (!A.stops[i].text)
        {
            continue;
        }
        printf("  %s  (in ", A.stops[i].text);
        print_name(A.stops[i].fn);
        printf(")\n");
    }
    if (!nstops)
    {
        printf("  none\n");
    }

    // 4. The stack: recursion that takes a frame per turn.
    printf("recursion that grows the stack (a call to itself that is not a tail call)\n");
    size_t grows = 0;
    for (size_t i = 0; i < A.nself; i++)
    {
        if ((i && same_function(A.self_calls[i], A.self_calls[i - 1])) || (root && !reached[A.self_calls[i]]))
        {
            continue;
        }
        printf("  %s  ", where(A.self_calls[i], at, sizeof(at)));
        print_name(A.self_calls[i]);
        putchar('\n');
        grows++;
    }
    if (!grows)
    {
        printf("  none\n");
    }

    // 5. The system: who writes.
    printf("system interface (write)\n");
    for (size_t i = 0; i < A.nwriters; i++)
    {
        printf("  %s  ", A.writers[i] < cx.syms.size ? where(A.writers[i], at, sizeof(at)) : "?");
        print_name(A.writers[i]);
        printf("  %u write%s\n", A.writes[i], A.writes[i] == 1 ? "" : "s");
    }
    if (!A.nwriters)
    {
        printf("  none\n");
    }
    free(reached);
    free(work);
}
