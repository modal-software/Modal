#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// def sum(num: i8) -> i8
char *run_macro_blocK(Parser *p, LexerState *l);

AstNode *parse_group(Parser *p)
{
    if (!p)
    {
        return NULL;
    }

    Token open_tok = p->previous;
    if (open_tok.kind != LPAREN)
    {
        parser_error_at(p, &p->previous, "expected '(' to start group");
        return NULL;
    }

    AstNode **stmts = NULL;
    size_t count = 0, cap = 4;

    stmts = malloc(cap * sizeof(AstNode *));
    if (!stmts)
    {
        free(stmts);
        return NULL;
    }

    while (p->current.kind != RPAREN && p->current.kind != TOK_EOF)
    {
        AstNode *stmt = parse_statement(p);
        if (p->had_error || !stmt)
        {
            parser_synchronize(p);
            continue;
        }

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

    if (p->current.kind != RPAREN)
    {

        parser_error_at(p, &p->current, "expected ')' at end of group");
        free(stmts);
        return NULL;
    }

    parser_advance(p);
    return ast_new_group(open_tok, stmts, count);
}

AstNode *parse_block(Parser *p)
{
    if (!parser_match(p, LBRACE))
    {
        parser_error_at(p, &p->current, "expected '{' to start block");
        return NULL;
    }

    Token open_tok = p->previous;

    size_t count = 0, cap = 4;
    AstNode **stmts = malloc(cap * sizeof(AstNode *));
    if (!stmts)
    {
        return NULL;
    }

    while (p->current.kind != RBRACE && p->current.kind != TOK_EOF)
    {
        AstNode *stmt = parse_statement(p);
        if (p->had_error || !stmt)
        {
            parser_synchronize(p);
            continue;
        }

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

    parser_consume(p, RBRACE, "expected '}' at end of block");
    AstNode *node = ast_new_block(open_tok, stmts, count);
    return node;
}

// AstNode *parse_string(Parser *p)
// {
//     Token tok = p->current;
//     const char *str = tok.start + 1;
//     int strl = tok.len - 2;
//
//     write(STDOUT_FILENO, str, strl);
//     parser_advance(p);
//     return ast_new_string(tok);
// }

AstNode *parse_write(Parser *p)
{
    parser_consume(p, TOK_WRITE, "missing 'write' statement");

    AstNode *arg = {0};
    Fmt fmt = 0;

    if (p->current.kind == LPAREN)
    {
        arg = parse_expression(p);
        fmt = FMT_GROUPED;
    }

    if (p->current.kind == STRING)
    {
        arg = parse_expression(p);
        fmt = FMT_LITERAL;
    }

    return ast_new_write(arg, fmt);
}

AstNode *parse_test(Parser *p)
{
    parser_advance(p);

    if (p->current.kind == IDENTIFIER)
    {
        parser_error_at(p, &p->current,
                        "test name must be a string literal (use quotes: test \"name\" { ... })");
        return NULL;
    }

    if (p->current.kind != STRING)
    {
        parser_error_at(p, &p->current, "expected string literal after 'test'");
        return NULL;
    }

    Token test_name = p->current;
    if (test_name.len < 3)
    {
        parser_error_at(p, &test_name, "test name cannot be empty");
        return NULL;
    }

    parser_advance(p);

    AstNode *block = parse_block(p);
    if (!block)
    {
        return NULL;
    }

    return ast_new_test(test_name, block);
}

AstNode *parse_assert(Parser *p)
{
    parser_advance(p);
    AstNode *expr = parse_expression(p);
    if (!expr)
    {
        return NULL;
    }

    return ast_new_assert(expr);
}

AstNode *parse_statement(Parser *p)
{
    switch (p->current.kind)
    {
    case ASSERT:
        return parse_assert(p);
    case WRITE:
        return parse_write(p);
    case LPAREN:
    case STRING:
        return parse_expression(p);
    case TEST:
        return parse_test(p);
    case LBRACE:
        return parse_block(p);

    default:
    {
        parser_error_at(p, &p->current, "unexpected statement");
        return NULL;
    }
    }
}
