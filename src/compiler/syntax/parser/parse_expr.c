#include "parser/parser.h"
#include "test_runner.h"
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static inline AstNode *parse_primary(Parser *p)
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
        return parse_group(p);
    }

    if (parser_match(p, STRING))
    {
        Token tok = p->previous;
        const char *str = tok.start + 1;
        int strl = tok.len - 2;

        write(STDOUT_FILENO, str, strl);
        return ast_new_string(tok);
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
