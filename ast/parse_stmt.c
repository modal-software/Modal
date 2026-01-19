#include "parser.h"
#include <stdio.h>
#include <stdlib.h>

AstNode *parse_block(Parser *p) {
  if (!parser_match(p, LBRACE)) {
    parser_error_at(p, &p->current, "espera '{' pra bloco");
    return NULL;
  }

  Token open_tok = p->previous;

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

    if (count >= cap) {
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
  AstNode *expr = parse_expression(p); // recursão pra expr completa
  if (!expr)
    return NULL;

  // Opcional: ; mas sync cuida
  // if (p->current.kind == OPERATOR && *p->current.start == ';') {
  parser_advance(p);
  // }

  return ast_new_assert(expr);
}

AstNode *parse_statement(Parser *p) {
  switch (p->current.kind) {
  case ASSERT:
    parser_advance(p);
    return parse_assert(p);

    // case TEST:
    //   parser_advance(p);
    //
    //   if (p->current.kind != LBRACE) {
    //     parser_error_at(p, &p->current, "`test` statement missing block");
    //     return NULL;
    //   }
    //
    //   if (p->current.kind == IDENTIFIER) {
    //     parser_error_at(p, &p->current,
    //                     "test name must be a string literal (use quotes: test
    //                     "
    //                     "\"name\" { ... })");
    //     return NULL;
    //   }
    //
    //   if (p->current.kind != STRING) {
    //     parser_error_at(p, &p->current, "expected string literal after
    //     `test`"); return NULL;
    //   }
    //
    //   Token test = p->current;
    //   printf("test len %c", p->current.len);
    //
    //   if (test.len < 3) {
    //     parser_error_at(p, &test, "test need to have a name definition");
    //     return NULL;
    //   }
    //
    //   parser_advance(p);
    //
    //   if (p->current.kind != LBRACE) {
    //     parser_error_at(p, &p->current, "expected '{' after test name");
    //     return NULL;
    //   }
    //
    //   AstNode *block = parse_block(p);
    //   if (!block) {
    //     // parser_error_at(p, &p->current, "expected '{' after test name");
    //     return NULL;
    //   }
    //
    //   return ast_new_test(test, block);

  case LBRACE:
    return parse_block(p);

  default: {
    AstNode *expr = parse_expression(p);
    if (expr)
      return expr; // por agora, expr é stmt
    parser_error_at(p, &p->current, "statement inesperado");
    return NULL;
  }
  }
}
