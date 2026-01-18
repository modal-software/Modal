#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>
#define MODAL_VERSION "0.0.1"

typedef enum {
  TOK_EOF,
  LPAREN,
  RPAREN,
  LBRACE,
  RBRACE,
  OPERATOR,
  IDENTIFIER,
  UNKNOWN,
  NUMBER,
  TEST,
  ASSERT,
  SIZEOF,
  DEFER,
  AUTOFREE,
  ALIAS,
  USE,
  COMPTIME,
  UNION,
  ASM,
  VOLATILE,
  ASYNC,
  AWAIT,
  AND,
  OR,
  Q_DOT,
  QQ_EQ,
  QQ,
  QUESTION,
  PIPE,
  DCOLON,
  ELLIPSIS,
  DOTDOT,
  ARROW,
  STRING,
} Kind;

typedef enum {
  START,
  EXPECT_NEWLINE,
  INVALID,
  CHAR,
  INT,
  FLOAT,
  FSTRING,
  STRING_LIT,
  BLOCK_COMMENT,
  LINE_COMMENT,
  PREPROC,
} State;

typedef struct {
  Kind kind;
  const char *start;
  int len;
  int line;
  int col;
} Token;

typedef struct {
  const char *buffer;
  int pos;
  int line;
  int col;
  State state;
} Tokenizer;

typedef struct {
  const char *kw;
  size_t len;
  Kind kind;
} Keyword;

Token token_make(Kind kind, const char *start, int len, int line, int col);
void init(Tokenizer *t, const char *buffer);
Token next(Tokenizer *t);

#endif
