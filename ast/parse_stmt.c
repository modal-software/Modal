#include "parser.h"
#include <stdio.h>
#include <stdlib.h>

// macro fn sum(num: i8) -> i8
char *run_macro_blocK(Parser *p, LexerState *l);

AstNode *parse_block(Parser *p)
{
    if (!parser_match(p, LBRACE))
    {
        parser_error_at(p, &p->current, "expected '{' to start block");
        return NULL;
    }

    Token open_tok = p->previous;

    AstNode **stmts = NULL;
    size_t count = 0, cap = 4;

    stmts = malloc(cap * sizeof(AstNode *));
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
    return ast_new_block(open_tok, stmts, count);
}

AstNode *parse_assert(Parser *p)
{
    AstNode *expr = parse_expression(p);
    if (!expr)
    {
        return NULL;
    }

    return ast_new_assert(expr);
}

AstNode *parse_test(Parser *p)
{
    if (p->current.kind == IDENTIFIER)
    {
        parser_error_at(p, &p->current,
                        "test name must be a string literal (use quotes: test "
                        "\"name\" { ... })");
        return NULL;
    }

    // Expect string literal for test name
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

    if (p->current.kind != LBRACE)
    {
        parser_error_at(p, &p->current, "expected '{' after test name");
        return NULL;
    }

    AstNode *block = parse_block(p);
    if (!block)
    {
        return NULL;
    }

    return ast_new_test(test_name, block);
};

AstNode *parse_statement(Parser *p)
{
    switch (p->current.kind)
    {
    case ASSERT:
        parser_advance(p);
        return parse_assert(p);

    case TEST:
        parser_advance(p);
        return parse_test(p);

    case LBRACE:
        return parse_block(p);

    default:
    {
        AstNode *expr = parse_expression(p);
        if (expr)
        {
            return expr;
        }
        parser_error_at(p, &p->current, "unexpected statement");
        return NULL;
    }
    }
}
