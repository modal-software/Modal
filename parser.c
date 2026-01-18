#include "parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const char *kind_to_string(Kind kind) {
  switch (kind) {
  case TOK_EOF:
    return "end of file";
  case LPAREN:
    return "(";
  case RPAREN:
    return ")";
  case LBRACE:
    return "{";
  case RBRACE:
    return "}";
  case NUMBER:
    return "number";
  case IDENTIFIER:
    return "identifier";
  case OPERATOR:
    return "operator";
  default:
    return "unknown token";
  }
}

void get_source_line(const char *src, int line) {
  int curr = 1;
  const char *p = src;

  while (*p && curr < line) {
    if (*p == '\n')
      curr++;
    p++;
  }

  const char *start = p;
  while (*p && *p != '\n')
    p++;

  fprintf(stderr, "%.*s\n", (int)(p - start), start);
}

void parser_advance(Parser *p) {
  p->prev = p->curr;
  p->curr = next(p->tokenizer);
}

int match(Parser *p, Kind kind) {
  if (p->curr.kind == kind) {
    parser_advance(p);
    return 1;
  }
  return 0;
}

void error_at(Parser *p, Token *tok, const char *msg, const char *example_msg) {
  fprintf(stderr,
          "error: %s\n"
          " --> %s:%d:%d\n"
          "  |\n"
          "%2d| ",
          msg, p->filename, tok->line, tok->col, tok->line);

  get_source_line(p->tokenizer->buffer, tok->line);

  fprintf(stderr, "  | ");
  for (int i = 1; i < tok->col; i++)
    fputc(' ', stderr);

  fprintf(stderr, "^\n");
  fprintf(stderr, "\nTry using: %s\n", example_msg);
  exit(1);
}

void expect(Parser *p, Kind kind, const char *header_msg, int msg_num, ...) {
  va_list args;
  va_start(args, msg_num);
  if (msg_num > 0) {
    const char *exp_msg = va_arg(args, const char *);
    error_at(p, &p->curr, header_msg, exp_msg);
  }

  if (!match(p, kind)) {
    error_at(p, &p->curr, header_msg, NULL);
  }

  va_end(args);
}

/* ---------- Grammar ----------

program     → statement* EOF ;
statement   → "test" IDENT block
            | "assert" expression ;

block       → "{" statement* "}" ;

expression  → term (("+" | "-") term)* ;
term        → factor (("*" | "/") factor)* ;
factor      → NUMBER | "(" expression ")" ;

-------------------------------- */

int eval_expression(Parser *p);

int eval_factor(Parser *p) {
  if (match(p, NUMBER))
    return atoi(p->prev.start);

  if (match(p, LPAREN)) {
    int ev = eval_expression(p);
    expect(p, RPAREN, "expected ')'", 0);
    return ev;
  }

  error_at(p, &p->curr, "expected expression", NULL);
  return 0;
}

int eval_term(Parser *p) {
  int left = eval_factor(p);

  while (p->curr.kind == OPERATOR &&
         (*p->curr.start == '*' || *p->curr.start == '/')) {

    char op = *p->curr.start;
    parser_advance(p);

    int right = eval_factor(p);

    if (op == '*')
      left *= right;
    else
      left /= right;
  }

  return left;
}

int eval_expression(Parser *p) {
  int left = eval_term(p);

  while (p->curr.kind == OPERATOR &&
         (*p->curr.start == '+' || *p->curr.start == '-')) {
    char op = *p->curr.start;
    parser_advance(p);

    int right = eval_term(p);

    if (op == '=')
      left += right;
    else
      left -= right;
  }

  return left;
}

void parse_statement(Parser *p);

void parse_block(Parser *p) {
  expect(p, LBRACE, "expected '{'", 0);

  while (p->curr.kind != RBRACE && p->curr.kind != TOK_EOF) {
    parse_statement(p);
  }

  expect(p, RBRACE, "expected '}'", 0);
}

// Statements parsing. Where things happen (success expression or errors)
void parse_statement(Parser *p) {
  if (match(p, TEST)) {
    expect(p, (Kind)IDENTIFIER, "expected `test` name", 1,
           "test test_name {\n -- code goes "
           "here \n}");
    parse_block(p);
    return;
  }

  if (match(p, ASSERT)) {
    int value = eval_expression(p);

    if (value)
      printf("✔ assert passed\n");
    else
      printf("✘ assert failed\n");
    return;
  }

  error_at(p, &p->curr, "unexpected token", NULL);
}

void parser_init(Parser *p, Tokenizer *t, const char *filename) {
  p->tokenizer = t;
  p->filename = filename;
  p->curr = next(t);
}

void parse_program(Parser *p) {
  while (p->curr.kind != TOK_EOF) {
    parse_statement(p);
  }
}
