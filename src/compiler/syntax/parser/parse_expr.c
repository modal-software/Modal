#include "compiler/cx.h"
#include "syntax/parser/parser.h"
#include "test_runner.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static inline AstNode *parse_primary(Parser *p)
{
    if (parser_match(p, NUMBER))
    {
        long long val = strtoll(p->previous.start, NULL, 10);
        return ast.new.number(p->previous, val);
    }

    if (parser_match(p, IDENTIFIER))
    {
        return ast.new.ident(p->previous);
    }

    if (parser_match(p, LPAREN))
    {
        AstNode *expr = parse_group(p);
        // ast.print(expr);
        return expr;
    }

    if (parser_match(p, STRING))
    {
        Token tok = p->previous;

        AstNode *expr = ast.new.string(tok);

        // ast.print(expr);
        return expr;
    }

    parser_error_at(p, &p->current, "expected expression (number, identifier, or '(')");
    return NULL;
}

AstNode *parse_expression(Parser *p)
{

    return parse_primary(p);
}

uint64_t eval_expr(AstNode *expr)
{
    if (!expr)
    {
        return 0;
    }

    switch (expr->kind)
    {
    case AST_NUMBER_LIT:
        return expr->data.number.value;

    case AST_BIN_OP:
    {
        uint64_t left = eval_expr(expr->data.binop.left);
        uint64_t right = eval_expr(expr->data.binop.right);
        const char op = *expr->token.start;

        switch (op)
        {
        case '+':
            return left + right;
        case '-':
            return left - right;
        case '*':
            return left * right;
        case '/':
            return right != 0 ? left / right : 0;
        case '=':
            return left == right;
        case '!':
            return left != right;
        case '<':
            return left < right;
        case '>':
            return left > right;
        default:
            return 0;
        }
    }

    default:
        return 0;
    }
}

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
    size_t count = 0;
    size_t cap = 4;

    stmts = (AstNode **)malloc(cap * sizeof(AstNode *));
    if (!stmts)
    {
        free(*stmts);
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
        // printf("stmt: %s\n", stmt->token.start);

        if (count >= cap)
        {
            cap *= 2;
            AstNode **new_stmts = (AstNode **)realloc(*stmts, cap * sizeof(AstNode *));
            if (!new_stmts)
            {
                free(*stmts);
                return NULL;
            }
            stmts = new_stmts;
        }
        stmts[count++] = stmt;
    }

    if (p->current.kind != RPAREN)
    {

        parser_error_at(p, &p->current, "expected ')' at end of group");
        free(*stmts);
        return NULL;
    }
    parser_advance(p);

    return ast.new.group(open_tok, stmts, count);
}

void exec_node(AstNode *node)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
    // case AST_WRITE_STMT:
    //     write(node);
    //     break;
    case AST_TEST_STMT:
        exec_test(node);
        break;
    default:
        break;
    }
}

void exec_program(AstNode *root)
{
    if (!root || root->kind != AST_BLOCK || root->kind != AST_PAREN_GROUP)
    {
        return;
    }

    AstNode *stmt;
    AST_EACH(root, stmt)
    {
        exec_node(stmt);
    }

    print_test_summary();
}
