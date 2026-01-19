#include "ast.h"
#include "parser.h"
#include <stdlib.h>

// This function is deprecated - use parse_statement instead
// Kept for compatibility but should be removed
AstNode *parse_test_decl(Parser *p) {
  // Expect 'test' keyword was already consumed

  if (p->current.kind != STRING) {
    parser_error_at(p, &p->current, "expected string literal for test name");
    return NULL;
  }

  Token test_name = p->current;
  parser_advance(p);

  AstNode *body = parse_block(p);
  if (!body) {
    return NULL;
  }

  return ast_new_test(test_name, body);
}
