// parse_decl.c — Parse de declarações (fn, test, var, etc.)
#include "parser.h"
#include <stdlib.h>

AstNode *parse_test_decl(Parser *p) {
  Token tok = p->previous; // o "test" — por quê? Pra loc no nó raiz

  // Espera nome (ident ou string? Por agora ident)
  parser_consume(p, IDENTIFIER,
                 "espera nome depois de 'test'"); // erro se não
  AstNode *name = ast_new_ident(
      p->previous); // cria nó ident — por quê? Nome é expr simples

  AstNode *body =
      parse_block(p); // recursão pro bloco — por quê? Corpo é subtree

  if (!name || !body) { // erro propagado
    ast_free(name);
    ast_free(body); // clean up parcial — por quê? Evita leak em erro
    return NULL;
  }

  // TODO: Cria AST_TEST_STMT (expanda ast.h com .test.name + .test.body)
  // Por agora, usa AST_BLOCK como placeholder com nome dentro — ajusta depois
  AstNode **stmts = malloc(2 * sizeof(AstNode *)); // nome + body
  stmts[0] = name;
  stmts[1] = body;
  return ast_new_block(
      tok, stmts,
      2); // retorna "bloco especial" — por quê? Simples pra agora
}
