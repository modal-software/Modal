// parse_stmt.c
#include "ast.c"
#include "parser.h"
#include <stdlib.h>

AstNode *parse_block(Parser *p) {
  if (!parser_match(p, LBRACE)) {
    parser_error_at(p, &p->current, "espera '{' pra bloco");
    return NULL;
  }

  Token open_tok = p->previous; // guarda { pra loc
  AstNode **stmts = NULL;
  size_t count = 0, cap = 4; // cresce como vector

  stmts = malloc(cap * sizeof(AstNode *));
  if (!stmts)
    return NULL;

  while (p->current.kind != RBRACE && p->current.kind != TOK_EOF) {
    AstNode *stmt = parse_statement(p);
    if (p->had_error || !stmt) {
      parser_synchronize(p);
      continue;
    }

    if (count >= cap) { // realloc C17 safe
      cap *= 2;
      AstNode **new_stmts = realloc(stmts, cap * sizeof(AstNode *));
      if (!new_stmts) { /* handle error */
        free(stmts);
        return NULL;
      }
      stmts = new_stmts;
    }
    stmts[count++] = stmt;
  }

  parser_consume(p, RBRACE, "espera '}' no fim do bloco");
  return ast_new_block(open_tok, stmts, count);
}

AstNode *parse_assert(Parser *p) {
  Token kw_tok = p->previous;          // o "assert"
  AstNode *expr = parse_expression(p); // recursão pra expr completa
  if (!expr)
    return NULL;

  // Opcional: ; mas sync cuida
  if (p->current.kind == OPERATOR && *p->current.start == ';') {
    parser_advance(p);
  }

  return ast_new_assert(expr);
}

AstNode *parse_statement(Parser *p) {
  switch (p->current.kind) {
  case ASSERT:
    parser_advance(p);
    return parse_assert(p);

  case TEST:
    parser_advance(p);
    // TODO: parse nome (IDENT) + block
    parser_error_at(p, &p->current, "test 'nome' { ... } ainda em construção");
    return NULL;

  case LBRACE: // bloco solto { ... }
    return parse_block(p);

  default:
    // Tenta como expr stmt (futuro: var x = expr;)
    AstNode *expr = parse_expression(p);
    if (expr)
      return expr; // por agora, expr é stmt
    parser_error_at(p, &p->current, "statement inesperado");
    return NULL;
  }
}
