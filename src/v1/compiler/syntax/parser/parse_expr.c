#include "syntax/parser/parser.h"
#include "test_runner.h"
#include "v1/compiler/cx.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

uint32_t hash(char *str)
{
    uint32_t hash = 5381L;
    int8_t c;
    while ((c = *str++))
    {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    // TODO: fix this magic number later (1024)
    return hash % 1024;
}

static void declare(AstNode *expr);
static inline AstNode *parse_primary(Parser *p)
{
    // ANALYSE: NUMBER
    if (parser_match(p, NUMBER))
    {
        long long val = strtoll(p->previous.start, NULL, 10);
        return ast.new.number(p->previous, val);
    }

    // ANALYSE: CONST
    if (parser_match(p, TOK_CONST))
    {
        Token tok = p->current;
        // ANALYSE: CONST -> IDENTIFIER 'X' I.e: 'const x'
        parser_match(p, IDENTIFIER);
        printf("%s", p->current.start);
        return ast.new.ident(p->previous);
    }

    if (parser_match(p, IDENTIFIER))
    {
        Token tok = p->previous;

        char buffer[tok.len + 1];
        sprintf(buffer, "%.*s", tok.len, tok.start);

        const char *ident = buffer;
        printf("%.s", p->previous.start);

        if (parser_match(p, TOK_DEFINE))
        {
            Token t = p->current;

            AstNode *lhs = ast.new.ident(t);
            lhs->data.ident.name = ident;
            lhs->data.ident.len = tok.len;

            AstNode *rhs = ast.new.ident(t);
            AstNode *define = ast.new.define(t, lhs, rhs);

            declare(define);

            return define;
        }

        if (p->current.kind == LPAREN)
        {
            // AstNode *expr = parse_group(p);
            AstNode *expr = parse_invoke(p);
            ast.print(expr);
            return expr;
        }

        if (parser_match(p, TOK_EQ))
        {
            printf("%s", ident);
            return ast.new.ident(p->previous);
        }
        return ast.new.ident(p->previous);
    }

    if (parser_match(p, LPAREN))
    {
        AstNode *expr = parse_group(p);
        // ast.print(expr);
        return expr;
    }

    if (parser_match(p, LBRACE))
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

static void declare(AstNode *expr)
{
    assert(expr);

    switch (expr->kind)
    {
    case AST_DEFINE_EXPR:
    {
        printf("%s %s", expr->data.expr.lhs->data.ident.name, expr->data.expr.rhs->data.ident.name);
    }
    // Validate DECLARATION (PREFIX_HASH)
    // I.e V_1
    // I.e F_123
    // if (sym.id)
    // printf
    default:
        break;
    }
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
    LFPool *lfpool = {0};
    size_t count = 0;
    size_t cap = 4;

    if (!pool->allocator(&lfpool, cap * sizeof(AstNode *)))
    {
        return NULL;
    };

    stmts = (AstNode **)pool->alloc(lfpool);
    if (!stmts)
    {
        pool->drop(lfpool);
        return NULL;
    }

    while (p->current.kind != RPAREN && p->current.kind != TOK_EOF)
    {
        AstNode *stmt = parse_expression(p);
        if (p->had_error || !stmt)
        {
            parser_synchronize(p);
            continue;
        }
        // printf("stmt: %s\n", stmt->token.start);

        if (count >= cap)
        {
            cap *= 2;
            AstNode **new_stmts = (AstNode **)pool->alloc_sz(lfpool, cap * sizeof(AstNode *));
            if (!new_stmts)
            {
                pool->drop(lfpool);
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
AstNode *parse_invoke(Parser *p)
{
    // parser_consume(p, TOK_DEFINE, "missing 'write' statement");

    AstNode *args = {0};
    Fmt fmt = FMT_LITERAL;
    //
    // if (p->current.kind != LPAREN)
    // {
    //     args = parse_expression(p);
    //     write(STDOUT_FILENO, args->data.string.value, args->data.string.len);
    //
    //     return ast.new.write(args, fmt);
    // }

    fmt = FMT_GROUPED;
    AstNode *ident = ast.new.ident(p->previous);
    // printf("%s", ident->data.ident.name);
    args = parse_expression(p);

    // Storage arguments statements
    AstNode **stmts = args->data.block_or_group.stmts;

    size_t stmts_count = args->data.block_or_group.count;
    // Valid those statements, in our case we need to validate if are string
    if (stmts_count == 0)
    {
        parser_error_at(p, &p->previous, "%.*s must have arguments", (int)ident->data.ident.len,
                        ident->data.ident.name);
    }

    // printf("invoked: %.*s\n", (int)ident->data.ident.len, ident->data.ident.name);

    for (size_t i = 0; i < args->data.block_or_group.count; i++)
    {
        // write(STDOUT_FILENO, stmts[i]->data.string.value, stmts[i]->data.string.len);
    }
    // return ast.new.write(args, fmt);
    printf("%s", ident->data.ident.name);
    return ast.new.invoke(args, ident);
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
    if (!root || (root->kind != AST_BLOCK && root->kind != AST_PAREN_GROUP))
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
