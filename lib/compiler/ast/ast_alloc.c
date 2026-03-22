#include "ast.h"
#include <stdlib.h>
#include <string.h>

AstNode *ast_new_number_lit(Token tok, long long val) {
  AstNode *node = malloc(sizeof(AstNode)); // aloca o nó base — por quê? Cada nó
                                           // precisa do seu espaço na memória
  if (!node)
    return NULL;               // erro de alocação? Volta null pra parser lidar
  node->kind = AST_NUMBER_LIT; // tag do tipo — por quê? Pra switch rápido
                               // depois (visitor/codegen)
  node->token =
      tok; // guarda token pra erros ou debug — por quê? Locação exata no fonte
  node->data.number.value =
      val; // preenche o union — por quê? Valor pronto pra eval ou codegen
  return node;
}

// Cria um identificador (ex: 'foo')
AstNode *ast_new_ident(Token tok) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  node->kind = AST_IDENT;
  node->token = tok;
  node->data.ident.name = tok.start; // aponta direto pro buffer — por quê? Zero
                                     // cópia, eficiente (mas buffer vive!)
  node->data.ident.len = tok.len; // tamanho pra strcmp depois — por quê? Evita
                                  // null-terminator se não tiver
  return node;
}

// Cria operação binária (ex: a + b)
AstNode *ast_new_binop(Token op_tok, AstNode *left, AstNode *right) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  node->kind = AST_BIN_OP;
  node->token = op_tok; // token do op (+/-) pra loc — por quê? Erros apontam
                        // pro culpado exato
  node->data.binop.left =
      left; // filhos — por quê? Árvore recursiva, pra traversal depois
  node->data.binop.right = right;
  node->data.binop.op =
      op_tok.kind; // tipo do op — por quê? Switch rápido em eval/codegen
  return node;
}

// Cria bloco ou grupo (ex: { stmt1; stmt2; } ou (expr))
AstNode *ast_new_block(Token open_brace, AstNode **stmts, size_t count) {
  AstNode *node = malloc(sizeof(AstNode));
  if (!node)
    return NULL;
  node->kind = AST_BLOCK; // ou AST_PAREN_GROUP se for () — por quê? Diferencia
                          // semântica futura
  node->token = open_brace; // { ou ( pra loc
  // Aloca array de stmts — por quê? Pra ter ownership próprio, evita free
  // separado
  node->data.block_or_group.stmts = malloc(count * sizeof(AstNode *));
  if (!node->data.block_or_group.stmts) {
    free(node); // clean up se falhar — por quê? Evita leak parcial
    return NULL;
  }
  memcpy(node->data.block_or_group.stmts, stmts,
         count *
             sizeof(AstNode *)); // copia pointers — por quê? Seguro e rápido
  node->data.block_or_group.count =
      count; // tamanho — por quê? Loop simples depois (visitor)
  return node;
}

// Libera nó recursivamente (top-down)
void ast_free(AstNode *node) {
  if (!node)
    return;             // null safe — por quê? Evita crash em erros parciais
  switch (node->kind) { // por tipo — por quê? Libera filhos só onde tem
  case AST_BIN_OP:
    ast_free(node->data.binop.left);
    ast_free(node->data.binop.right);
    break;
  case AST_UNARY_OP:
    ast_free(node->data.unary.expr);
    break;
  case AST_BLOCK:
  case AST_PAREN_GROUP:
    for (size_t i = 0; i < node->data.block_or_group.count; i++) {
      ast_free(
          node->data.block_or_group
              .stmts[i]); // recursão em filhos — por quê? Libera árvore toda
    }
    free(node->data.block_or_group
             .stmts); // array depois — por quê? Ordem certa evita dangling
    break;
  case AST_TEST_STMT:
  case AST_ASSERT_STMT:
    // TODO: liberar nome/body/expr se tiverem — por quê? Ainda não definidos no
    // ast.h
    break;
  default:
    break; // lits/idents não tem filhos
  }
  free(node); // nó base por último — por quê? Clean up completo
}
