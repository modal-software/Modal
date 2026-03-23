#include "parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void parser_init(Parser *p, Tokenizer *lexer, const char *filename)
{
    p->lexer = lexer;
    p->filename = filename;
    p->had_error = 0;
    p->current = next(lexer);
    p->previous = (Token){0};
}

void parser_advance(Parser *p)
{
    p->previous = p->current;
    p->current = next(p->lexer);
}

int parser_match(Parser *p, Kind kind)
{
    if (p->current.kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    return 0;
}

void parser_consume(Parser *p, Kind kind, const char *msg)
{
    if (p->current.kind == kind)
    {
        parser_advance(p);
        return;
    }
    parser_error_at(p, &p->current, msg);
}

AstNode *parse_program(Parser *p)
{
    AstNode **stmts = NULL;
    size_t count = 0;
    size_t cap = 8;

    stmts = malloc(cap * sizeof(AstNode *));
    if (!stmts)
    {
        return NULL;
    }

    while (p->current.kind != TOK_EOF)
    {
        AstNode *stmt = parse_statement(p);
        if (p->had_error)
        {
            parser_synchronize(p);
            continue;
        }

        if (stmt)
        { // Only add non-null statements
            if (count >= cap)
            {
                cap *= 2;
                AstNode **new_stmts = realloc(stmts, cap * sizeof(AstNode *));
                if (!new_stmts)
                {
                    free(stmts);
                    return NULL;
                }
                stmts = new_stmts;
            }
            stmts[count++] = stmt;
        }
    }

    return ast_new_block(p->current, stmts, count);
}
