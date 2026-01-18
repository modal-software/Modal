#ifndef PARSER_H
#define PARSER_H

#include "tokenizer/tokenizer.h"
#include <stdio.h>

typedef enum {
  AST_NUMBER,
  AST_IDENTIFIER,
  AST_BINARY,
  AST_UNARY,
  AST_GROUP,
} AstKind;

typedef struct Ast Ast;

struct Ast {
  AstKind kind;
  Token token;

  union {
    struct {
      Ast *left;
      Ast *right;
    } binary;

    struct {
      Ast *expr;
    } unary;
  };
};

typedef struct {
  Tokenizer *tokenizer;
  Token curr;
  Token prev;
  const char *filename;
  int panic;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGN,
  PREC_OR,
  PREC_AND,
  PREC_EQUALITY,
  PREC_COMPARE,
  PREC_TERM,
  PREC_FACTOR,
  PREC_UNARY,
  PREC_CALL,
  PREC_PRIMARY,
} Precedence;

typedef Ast *(*ParseFn)(Parser *);

typedef struct {
  ParseFn prefix;
  ParseFn infix;
  Precedence node;
} ParseRule;

static void warn_msg(const char *msg) {
  fprintf(stderr, "\nTry using: %s\n", msg);
}

void error_at(Parser *p, Token *token, const char *msg, const char *wmsg);

__attribute__((format(printf, 2, 3))) void ast_error(Token *token,
                                                     const char *format, ...);

void parser_init(Parser *p, Tokenizer *t, const char *filename);
void parse_program(Parser *p);

#endif
