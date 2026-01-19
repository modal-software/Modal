#include "ast.h"
#include "parser.h"
#include <stdlib.h>

AstNode *parse_test_decl(Parser *p) {
  parser_consume(p, (Kind)STRING, "espera nome depois de 'test'");
  parser_consume(p, IDENTIFIER, "espera nome depois de 'test'");
  AstNode *name = ast_new_ident(p->previous);

  AstNode *body = parse_block(p);

  if (!name || !body) {
    ast_free(name);
    ast_free(body);
    return NULL;
  }

  // TODO: Cria AST_TEST_STMT (expanda ast.h com .test.name + .test.body)
  // Por agora, usa AST_BLOCK como placeholder com nome dentro — ajusta depois
  AstNode **stmts = malloc(2 * sizeof(AstNode *)); // nome + body
  stmts[0] = name;
  stmts[1] = body;
  return ast_new_test(p->previous, *stmts);
}
