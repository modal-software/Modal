#include "syntax/parser/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// def sum(num: i8) -> i8
char *run_macro_blocK(Parser *p, LexerState *l);

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
    AstNode *node = ast.new.block(open_tok, stmts, count);
    return node;
}

AstNode *parse_write(Parser *p)
{
    parser_consume(p, TOK_WRITE, "missing 'write' statement");

    AstNode *args = {0};
    Fmt fmt = FMT_LITERAL;

    if (p->current.kind != LPAREN)
    {
        args = parse_expression(p);
        // write(STDOUT_FILENO, args->data.string.value, args->data.string.len);

        return ast.new.write(args, fmt);
    }

    fmt = FMT_GROUPED;
    args = parse_expression(p);

    AstNode **stmts = args->data.block_or_group.stmts;
    size_t stmts_count = args->data.block_or_group.count;

    if (stmts_count == 0)
    {
        parser_error_at(p, &p->previous, "write function must have arguments");
    }

    // for (size_t i = 0; i < args->data.block_or_group.count; i++)
    // {
    //     write(STDOUT_FILENO, stmts[i]->data.string.value, stmts[i]->data.string.len);
    // }

    return ast.new.write(args, fmt);
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

    return ast.new.assert(expr);
}

AstNode *parse_statement(Parser *p)
{
    switch (p->current.kind)
    {
    case ASSERT:
        return parse_assert(p);
    case LPAREN:
    case STRING:
        return parse_expression(p);
    case WRITE:
        return parse_write(p);
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
