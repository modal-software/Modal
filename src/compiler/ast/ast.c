#include "ast/ast.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void printer(AstNode *tree)
{
    switch (tree->kind)
    {
    case AST_WRITE_STMT:
    {
        printf("\n\nAST_WRITE_STMT:\n     \tdata:\n\t     "
               "--\tnode: "
               "write\n"
               "\t     -- reference: %p\n",
               &tree->data.write);
        break;
    }
    case AST_PAREN_GROUP:
    {
        AstNode **stmts = tree->data.block_or_group.stmts;
        size_t stmts_count = tree->data.block_or_group.count;

        if (stmts_count == 0)
        {
            break;
        }

        printf("\n\nAST_PAREN_GROUP:\n     \tdata:\n\t     --\tnode: "
               "group\n\t     --\tstmts_count: "
               "%zu\n\t     -- stmts: [ ",
               tree->data.block_or_group.count);

        for (size_t i = 0; i < stmts_count; i++)
        {
            if (i > 0)
            {
                printf(", ");
            }
            write(STDOUT_FILENO, stmts[i]->data.string.raw, stmts[i]->data.string.span);
        }
        printf(" ]");

        break;
    }
    case AST_STRING_LIT:
    {
        printf("\n\nAST_STRING_LIT:\n     \tdata:\n\t     --\tnode: "
               "string\n\t     --\tlen: "
               "%zu\n\t     --\tvalue: %.*s\n\t     --\traw: %.*s\n\t     -- span: %zu\n",
               tree->data.string.len, (int)tree->data.string.len, tree->data.string.value,
               (int)tree->data.string.span, tree->data.string.raw, tree->data.string.span);
        break;
    }
    case AST_ASSIGN_STMT:
    {
        printf("\n\nAST_EXPR:\n     \tdata:\n\t     --\tnode: "
               "ASSIGN_STMT\n\t     --\tlhs_len: "
               "%zu\n\t     --\tlhs_value: %.*s\n",
               tree->data.expr.lhs->data.ident.len, (int)tree->data.expr.lhs->data.ident.len,
               tree->data.expr.lhs->data.ident.name);

        printf("\t     --\trhs_len: "
               "%zu\n\t     --\trhs_value: %.*s\n",
               tree->data.expr.rhs->data.ident.len, (int)tree->data.expr.rhs->data.ident.len,
               tree->data.expr.rhs->data.ident.name);
        break;
    }
    default:
        break;
    }
}

static AstNode *ast_new_number(Token tok, long long val)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    *node = (AstNode){.kind = AST_NUMBER_LIT, .token = tok, .data = {.number = {val}}};
    return node;
}

static AstNode *ast_new_ident(Token tok)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    *node = (AstNode){
        .kind = AST_IDENT,
        .token = tok,
        .data =
            {
                .ident =
                    {
                        .name = tok.start,
                        .len = tok.len,
                    },
            },
    };

    return node;
}

static AstNode *ast_new_binop(Token tok, AstNode *left, AstNode *right)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }
    *node = (AstNode){
        .kind = AST_BIN_OP,
        .token = tok,
        .data =
            {
                .binop =
                    {
                        left,
                        right,
                        tok.kind,
                    },
            },
    };
    return node;
}

static AstNode *ast_new_group(Token open_tok, AstNode **stmts, size_t count)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    AstNode **children = malloc(count * sizeof(AstNode *));
    if (!children)
    {
        free(node);
        return NULL;
    }

    memcpy(children, stmts, count * sizeof(AstNode *));

    *node = (AstNode){
        .kind = AST_PAREN_GROUP, .token = open_tok, .data = {.block_or_group = {children, count}}};
    return node;
}

static AstNode *ast_new_block(Token open_tok, AstNode **stmts, size_t count)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    AstNode **children = malloc(count * sizeof(AstNode *));
    if (!children)
    {
        free(node);
        return NULL;
    }
    memcpy(children, stmts, count * sizeof(AstNode *));

    *node = (AstNode){
        .kind = AST_BLOCK, .token = open_tok, .data = {.block_or_group = {children, count}}};
    return node;
}

static AstNode *ast_new_assert(AstNode *expr)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }
    *node = (AstNode){.kind = AST_ASSERT_STMT,
                      .token = expr->token, // usa token da expr pra loc
                      .data = {.unary = {expr}}};
    return node;
}

static AstNode *ast_new_expr(Token tok, AstNode *lhs, AstNode *rhs)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    *node = (AstNode){
        .kind = AST_ASSIGN_STMT,
        .token = tok,
        .data =
            {
                .expr =
                    {
                        lhs,
                        rhs,
                    },
            },
    };

    return node;
}

static AstNode *ast_new_string(Token tok)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    const char *str_raw = (char *)tok.start;

    const size_t span = tok.len;
    tok.len -= 2;

    tok.start += 1;

    *node = (AstNode){
        .kind = AST_STRING_LIT,
        .token = tok,
        .data =
            {
                .string =
                    {
                        ._id = "1",
                        .value = tok.start,
                        .raw = str_raw,
                        .len = tok.len,
                        span,
                    },
            },
    };
    return node;
}

static AstNode *ast_new_test(Token tok, AstNode *block)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    AstNode *str = ast_new_string(tok);

    *node = (AstNode){
        .kind = AST_TEST_STMT,
        .token = tok,
        .data =
            {
                .test =
                    {
                        .name = str->data.string.value,
                        .len = str->data.string.len,
                        .block = block,
                    },
            },
    };

    return node;
}

static AstNode *ast_new_write(AstNode *n, Fmt fmt)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    if (n->kind != AST_PAREN_GROUP)
    {

        *node = (AstNode){.kind = AST_WRITE_STMT,
                          .token = n->token,
                          .data = {.write = {.group = NULL, fmt = fmt}}};
        return node;
    }

    *node = (AstNode){
        .kind = AST_WRITE_STMT, .token = n->token, .data = {.write = {.group = n, fmt = fmt}}};

    return node;
}

static void ast_free(struct AstNode *node)
{
    if (!node)
    {
        return;
    }

    switch (node->kind)
    {
    case AST_BIN_OP:
        ast_free(node->data.binop.left);
        ast_free(node->data.binop.right);
        break;
    case AST_UNARY_OP:
        ast_free(node->data.unary.expr);
        break;
    case AST_BLOCK:
    case AST_PAREN_GROUP:
        for (size_t i = 0; i < node->data.block_or_group.count; i++)
        {
            ast_free(node->data.block_or_group.stmts[i]);
        }
        free(node->data.block_or_group.stmts);
        break;
    case AST_TEST_STMT:
        ast_free(node->data.test.block);
        break;
    case AST_ASSERT_STMT:
        ast_free(node->data.unary.expr);
        break;
    case AST_ASSIGN_STMT:
    case AST_IDENT:
    case AST_STRING_LIT:
        ast_free(node);
        break;
    default:
        break;
    }
    free(node);
}

const AstImpl ast = (AstImpl){
    .print = printer,
    .free = ast_free,
    .new =
        {
            .string = ast_new_string,
            .write = ast_new_write,
            .test = ast_new_test,
            .assert = ast_new_assert,
            .binop = ast_new_binop,
            .number = ast_new_number,
            .ident = ast_new_ident,
            .block = ast_new_block,
            .group = ast_new_group,
            .expr = ast_new_expr,
        },
};
