#include "ast/ast.impl.h"
#include "ast/ast.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void printer(AstNode *tree)
{
    printf("%c", tree->kind);
}

AstNode *ast_new_number(Token tok, long long val)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    *node = (AstNode){.kind = AST_NUMBER_LIT, .token = tok, .data = {.number = {val}}};
    return node;
}

AstNode *ast_new_ident(Token tok)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }
    *node = (AstNode){
        .kind = AST_IDENT, .token = tok, .data = {.ident = {.name = tok.start, .len = tok.len}}};
    return node;
}

AstNode *ast_new_binop(Token tok, AstNode *left, AstNode *right)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }
    *node = (AstNode){.kind = AST_BIN_OP, .token = tok, .data = {.binop = {left, right, tok.kind}}};
    return node;
}

AstNode *ast_new_group(Token open_tok, AstNode **stmts, size_t count)
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

AstNode *ast_new_block(Token open_tok, AstNode **stmts, size_t count)
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

AstNode *ast_new_break_label(Token token)
{
    if (token.kind == TOK_COLON)
    {
        return NULL;
    }

    return NULL;
}

AstNode *ast_new_assert(AstNode *expr)
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

AstNode *ast_new_string(Token tok)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    tok.start++;
    size_t len = tok.len - 2;

    *node = (AstNode){
        .kind = AST_STRING_LIT, .token = tok, .data = {.string = {.value = tok.start, .len = len}}};

    return node;
}

AstNode *ast_new_test(Token tok, AstNode *block)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    ast_new_string(tok);

    *node = (AstNode){.kind = AST_TEST_STMT,
                      .token = token,
                      .data = {.test = {
                                   .name = name_without_quotes,
                                   .len = len,
                                   .block = block,
                               }}};

    return node;
}

AstNode *ast_new_write(AstNode *n, Fmt fmt)
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

void ast_free(struct AstNode *node)
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
    case AST_STRING_LIT:
        ast_free(node);
        break;
    default:
        break;
    }
    free(node);
}

static const AstImpl ast = (AstImpl){.print = printer};
