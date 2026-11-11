#include "syntax/parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// def sum(num: i8) -> i8
char *run_macro_blocK(Parser *p, LexerState *l);

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

    return ast.new.test(test_name, block);
}

AstNode *parse_assert(Parser *p)
{
    parser_advance(p);
    AstNode *expr = parse_expression(p);
    if (!expr)
    {
        return NULL;
    }

    return ast.new.assertFn(expr);
}

AstNode *parse_statement(Parser *p)
{
    switch (p->current.kind)
    {
    case ASSERT:
        return parse_assert(p);
    case TEST:
        return parse_test(p);
    default:
    {
        parser_error_at(p, &p->current, "unexpected statement");

        return NULL;
    }
    }
}
