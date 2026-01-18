#include "parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const char *parse_kind_to_string(Kind kind) {
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

void parse_get_source_line(const char *src, int line) {
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

void parse_advance(Parser *p) {
  p->prev = p->curr;
  p->curr = next(p->tokenizer);
}

int parse_match(Parser *p, Kind kind) {
  if (p->curr.kind == kind) {
    parse_advance(p);
    return 1;
  }
  return 0;
}

// __attribute__((format(printf, 2, 3))) __attribute__((noreturn)) void
// ast_error(Token *token, const char *format, ...) {
//   va_list ap;
//   va_start(ap, format);
//   fprintf(stderr, "Error: Line %d, column %d: ", token->line, token->col);
//   vfprintf(stderr, format, ap);
//   fprintf(stderr, "\n");
//   va_end(ap);
//   exit(1);
// }

__attribute__((noreturn)) void error_at(Parser *ctx, Token *tok,
                                        const char *msg, const char *hint) {
  fprintf(stderr,
          "error: %s\n"
          " --> %s:%d:%d\n"
          "  |\n"
          "%2d| ",
          msg, ctx->filename, tok->line, tok->col, tok->line);

  parse_get_source_line(ctx->tokenizer->buffer, tok->line);

  fprintf(stderr, "  | ");
  for (int i = 1; i < tok->col; i++)
    fputc(' ', stderr);

  fprintf(stderr, "^\n");

  if (hint) {
    warn_msg(hint);
  }
  exit(1);
}

void parse_expect(Parser *p, Kind kind, const char *msg, const char *hint) {
  error_at(p, &p->curr, msg, hint);

  if (!parse_match(p, kind)) {
    error_at(p, &p->curr, msg, NULL);
  }
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
    const char *msg = "\n\n test test_name {\n   -- code goes "
                      "here \n  }";
    expect(p, (Kind)IDENTIFIER, "expected `test` name", 1, msg);
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
