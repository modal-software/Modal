#include "parser.h"
#include "../test_runner.h"
#include "../writer.h"
#include <stdlib.h>

static AstNode *parse_primary(Parser *p)
{
    if (parser_match(p, NUMBER))
    {
        long long val = strtoll(p->previous.start, NULL, 10);
        return ast_new_number(p->previous, val);
    }

    if (parser_match(p, IDENTIFIER))
    {
        return ast_new_ident(p->previous);
    }

    if (parser_match(p, LPAREN))
    {
        AstNode *expr = parse_expression(p);
        parser_consume(p, RPAREN, "expected ')' after expression");
        return expr;
    }

    // Give more helpful error messages
    if (p->current.kind == STRING)
    {
        parser_error_at(p, &p->current, "unexpected string literal in expression");
        return NULL;
    }

    parser_error_at(p, &p->current, "expected expression (number, identifier, or '(')");
    return NULL;
}

static AstNode *parse_factor(Parser *p)
{
    AstNode *left = parse_primary(p);
    if (!left)
    {
        return NULL;
    }

    while (p->current.kind == OPERATOR)
    {
        char op = *p->current.start;
        if (op != '*' && op != '/')
        {
            break;
        }
        Token op_tok = p->current;
        parser_advance(p);
        AstNode *right = parse_primary(p);
        if (!right)
        {
            ast_free(left);
            return NULL;
        }
        left = ast_new_binop(op_tok, left, right);
    }
    return left;
}

static AstNode *parse_term(Parser *p)
{
    AstNode *left = parse_factor(p);
    if (!left)
    {
        return NULL;
    }

    while (p->current.kind == OPERATOR)
    {
        char op = *p->current.start;
        if (op != '+' && op != '-')
        {
            break;
        }
        Token op_tok = p->current;
        parser_advance(p);
        AstNode *right = parse_factor(p);
        if (!right)
        {
            ast_free(left);
            return NULL;
        }
        left = ast_new_binop(op_tok, left, right);
    }
    return left;
}

AstNode *parse_expression(Parser *p)
{
    return parse_term(p);
}

long long eval_expr(AstNode *expr)
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
        long long left = eval_expr(expr->data.binop.left);
        long long right = eval_expr(expr->data.binop.right);
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

void exec_node(AstNode *node)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
    case AST_PRINT_STMT:
        write(node);
        break;
    case AST_TEST_STMT:
        exec_test(node);
        break;
    default:
        break;
    }
}

void exec_program(AstNode *root)
{
    if (!root || root->kind != AST_BLOCK)
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
