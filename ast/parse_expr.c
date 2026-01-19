#include "parser.h"
#include <stdlib.h>

static AstNode *parse_primary(Parser *p) {
  if (parser_match(p, NUMBER)) {
    long long val = strtoll(p->previous.start, NULL, 10);
    return ast_new_number(p->previous, val);
  }
  if (parser_match(p, IDENTIFIER)) {
    return ast_new_ident(p->previous);
  }
  if (parser_match(p, LPAREN)) {
    AstNode *expr = parse_expression(p); // recursão
    parser_consume(p, RPAREN, "espera ')'");
    return expr; // ou ast_new_paren se quiser preservar parens
  }
  parser_error_at(p, &p->current, "espera expressão primária");
  return NULL;
}

static AstNode *parse_factor(Parser *p) { // * /
  AstNode *left = parse_primary(p);
  while (p->current.kind == OPERATOR) {
    char op = *p->current.start;
    if (op != '*' && op != '/')
      break;
    Token op_tok = p->current;
    parser_advance(p);
    AstNode *right = parse_primary(p);
    left = ast_new_binop(op_tok, left, right);
  }
  return left;
}

static AstNode *parse_term(Parser *p) { // + -
  AstNode *left = parse_factor(p);
  while (p->current.kind == OPERATOR) {
    char op = *p->current.start;
    if (op != '+' && op != '-')
      break;
    Token op_tok = p->current;
    parser_advance(p);
    AstNode *right = parse_factor(p);
    left = ast_new_binop(op_tok, left, right);
  }
  return left;
}

AstNode *parse_expression(Parser *p) { // entry point das expr
  return parse_term(p); // por agora, term é o topo (expande com == > etc.)
}
