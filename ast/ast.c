// ast.c (adicione ao seu projeto)
#include "ast.h"
#include <stdlib.h>
#include <string.h>

AstNode *ast_new_number(Token tok, long long val) {
  AstNode *node = malloc(sizeof(AstNode)); // aloca nó base
  if (!node)
    return NULL;
  *node = (AstNode){
      .kind = AST_NUMBER_LIT, .token = tok, .data = {.number = {val}}};
  return node;
}

AstNode *ast_new_ident(Token tok) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  *node = (AstNode){.kind = AST_IDENT,
                    .token = tok,
                    .data = {.ident = {.name = tok.start, .len = tok.len}}};
  return node;
}

AstNode *ast_new_binop(Token op_tok, AstNode *left, AstNode *right) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  *node = (AstNode){.kind = AST_BIN_OP,
                    .token = op_tok,
                    .data = {.binop = {left, right, op_tok.kind}}};
  return node;
}

AstNode *ast_new_block(Token open_tok, AstNode **stmts, size_t count) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  // Aloca array de children (C17 style, sem VLA)
  AstNode **children = malloc(count * sizeof(AstNode *));
  if (!children) {
    free(node);
    return NULL;
  }
  memcpy(children, stmts, count * sizeof(AstNode *));

  *node = (AstNode){.kind = AST_BLOCK,
                    .token = open_tok,
                    .data = {.block_or_group = {children, count}}};
  return node;
}

// assert e test simples (expande depois)
AstNode *ast_new_assert(AstNode *expr) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  *node = (AstNode){.kind = AST_ASSERT_STMT,
                    .token = expr->token, // usa token da expr pra loc
                    .data = {.unary = {expr}}};
  return node;
}
