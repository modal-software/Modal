// Modal v2 front-end driver: scan -> parse -> declare -> link.
//
//   modal-v2 [--ast] [--syms] [--sig] [--stats] file.modal
#include "v2/compiler/cx.h"
#include <stdio.h>
#include <string.h>

static int usage(const char *argv0)
{
    fprintf(stderr,
            "modal v2 front-end %s\n"
            "usage: %s [--ast] [--syms] [--sig] [--stats] file.modal\n"
            "  --ast    print the syntax tree of every loaded file\n"
            "  --syms   print the symbol table with visibility\n"
            "  --sig    print the derived contract of every domain\n"
            "  --stats  print token, node, symbol and memory counts\n",
            MODAL_VERSION, argv0);
    return 1;
}

int main(int argc, char **argv)
{
    const char *entry = NULL;
    int show_ast = 0, show_syms = 0, show_stats = 0, show_sig = 0;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--ast") == 0)
        {
            show_ast = 1;
        }
        else if (strcmp(argv[i], "--syms") == 0)
        {
            show_syms = 1;
        }
        else if (strcmp(argv[i], "--sig") == 0)
        {
            show_sig = 1;
        }
        else if (strcmp(argv[i], "--stats") == 0)
        {
            show_stats = 1;
        }
        else if (argv[i][0] == '-' || entry)
        {
            return usage(argv[0]);
        }
        else
        {
            entry = argv[i];
        }
    }
    if (!entry)
    {
        fprintf(stderr, "%s: fatal: no input files\n", argv[0]);
        return usage(argv[0]);
    }

    symbol_new(&cx.units[0], SYM_NONE, 0, 0, 0, 0); // slot 0 = "no symbol"

    if (!unit_load(entry))
    {
        fprintf(stderr, "%s: fatal: cannot read '%s'\n", argv[0], entry);
        arena_drop();
        return 1;
    }
    modules_link();

    for (uint16_t i = 0; i < cx.nunits; i++)
    {
        Unit *u = &cx.units[i];
        if (show_ast)
        {
            printf("== ast: %s\n", u->path);
            ast.print(u);
            putchar('\n');
        }
        if (show_syms)
        {
            printf("== symbols: %s\n", u->path);
            symbols_print(u);
            putchar('\n');
        }
    }

    if (show_sig)
    {
        contract_print();
    }

    if (show_stats)
    {
        size_t source = 0, tokens = 0, nodes = 0;
        for (uint16_t i = 0; i < cx.nunits; i++)
        {
            source += cx.units[i].len;
            tokens += cx.units[i].tokens.size;
            nodes += cx.units[i].nodes.size - 1;
        }
        printf("files %u  source %zu B  tokens %zu  nodes %zu  symbols %zu\n", cx.nunits, source,
               tokens, nodes, cx.syms.size - 1);
        printf("arena: %.1f KB in use, %.1f KB taken from the system at peak\n",
               (double)arena_used() / 1024.0, (double)arena_peak() / 1024.0);
    }

    int failed = cx.errors > 0;
    if (failed)
    {
        fprintf(stderr, "\n%d error%s\n", cx.errors, cx.errors == 1 ? "" : "s");
    }
    arena_drop();
    return failed;
}
