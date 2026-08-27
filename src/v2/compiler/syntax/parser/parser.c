#include "v2/compiler/syntax/parser/parser.h"
#include "syntax/tokenizer.h"
#include "v1/compiler/cx.h"
#include <stdarg.h>
#include <string.h>

void parser_init(Parser *p, const char *filename)
{
    p->current = cx.tokens.data;
    p->previous = NULL;
    p->filename = filename;
    p->had_error = 0;
}

void parser_advance(Parser *p)
{
    p->previous = p->current;
    if (p->current->kind != TOK_EOF)
    {
        p->current++;
    }
}

int parser_match(Parser *p, Kind kind)
{
    if (p->current->kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    return 0;
}

void parser_consume(Parser *p, Kind kind)
{
    if (p->current->kind == kind)
    {
        parser_advance(p);
        return;
    }
    // parser_error_at(p, p->current, msg);
}

AstNode *parse_program(Parser *p)
{
    _mvec_ast stmts = astvec.init(cx.pool, 64);

    while (p->current->kind != TOK_EOF)
    {
        AstNode *stmt = parse_declaration(p);
        if (p->had_error)
        {
            parser_synchronize(p);
            continue;
        }

        if (stmt)
        {
            astvec.push(&stmts, *stmt);
        }
    }
    AstNode *root = (AstNode *)pool.alloc(cx.pool);
    *root = (AstNode){
        .kind = AST_BLOCK,
        .tok_idx = 0,
        .data.block_or_group =
            {
                .stmts = (struct AstNode **)stmts.data,
                .count = stmts.size,
            },
    };
    return root;
}
