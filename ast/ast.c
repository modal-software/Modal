// ast.c (adicione ao seu projeto)
#include "ast.h"
#include <stdlib.h>
#include <string.h>

AstNode *ast_new_number(Token tok, long long val)
{
    AstNode *node = malloc(sizeof(AstNode)); // aloca nó base
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

AstNode *ast_new_binop(Token op_tok, AstNode *left, AstNode *right)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }
    *node = (AstNode){
        .kind = AST_BIN_OP, .token = op_tok, .data = {.binop = {left, right, op_tok.kind}}};
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

void ast_free(AstNode *node)
{
    if (!node)
    {
        return; // null safe — por quê? Evita crash em erros parciais
    }
    switch (node->kind)
    { // por tipo — por quê? Libera filhos só onde tem
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
            ast_free(node->data.block_or_group
                         .stmts[i]); // recursão em filhos — por quê? Libera árvore toda
        }
        free(node->data.block_or_group.stmts); // array depois — por quê? Ordem certa evita dangling
        break;
    case AST_TEST_STMT:
        ast_free(node->data.test.block);
    case AST_ASSERT_STMT:
        // TODO: liberar nome/body/expr se tiverem — por quê? Ainda não definidos no
        // ast.h
        break;
    default:
        break; // lits/idents não tem filhos
    }
    free(node); // nó base por último — por quê? Clean up completo
}

AstNode *ast_new_test(Token token, AstNode *block)
{
    AstNode *node = malloc(sizeof(AstNode));
    if (!node)
    {
        return NULL;
    }

    const char *name_without_quotes = token.start + 1;
    size_t len = token.len - 2;

    *node = (AstNode){.kind = AST_TEST_STMT,
                      .token = token,
                      .data = {.test = {
                                   .name = name_without_quotes,
                                   .len = len,
                                   .block = block,
                               }}};
    return node;
}

// assert e test simples (expande depois)
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
