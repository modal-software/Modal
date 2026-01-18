#include "parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void parser_init(Parser *p, Tokenizer *lexer, const char *filename) {
  p->lexer = lexer;
  p->filename = filename;
  p->had_error = 0;
  p->current = next(lexer); // prime token
  p->previous = (Token){0};
}

void parser_advance(Parser *p) {
  p->previous = p->current;
  p->current = next(p->lexer);
}

int parser_match(Parser *p, Kind kind) {
  if (p->current.kind == kind) {
    parser_advance(p);
    return 1;
  }
  return 0;
}

void parser_consume(Parser *p, Kind kind, const char *msg) {
  if (p->current.kind == kind) {
    parser_advance(p);
    return;
  }
  parser_error_at(p, &p->current, msg);
}

void parser_error_at(Parser *p, Token *tok, const char *fmt, ...) {
  p->had_error = 1;

  fprintf(stderr, "error [%s:%d:%d]: ", p->filename, tok->line, tok->col);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n  | ");

  // Mostra linha do fonte (simples por agora)
  const char *line_start = tok->start;
  while (line_start > p->lexer->buffer && *(line_start - 1) != '\n')
    line_start--;
  const char *line_end = tok->start;
  while (*line_end && *line_end != '\n')
    line_end++;
  fprintf(stderr, "%.*s\n  | ", (int)(line_end - line_start), line_start);

  // Seta o ^
  for (int i = 1; i < tok->col; i++)
    fputc(' ', stderr);
  fprintf(stderr, "^\n\n");
}

// Pula tokens até achar ; ou } ou EOF (recovery básico)
void parser_synchronize(Parser *p) {
  parser_advance(p);
  while (p->current.kind != TOK_EOF) {
    if (p->previous.kind == ';')
      return;
    if (p->current.kind == RBRACE)
      return;
    parser_advance(p);
  }
}

// O entry point principal – parseia múltiplos statements até EOF
AstNode *parse_program(Parser *p) {
  AstNode **stmts = NULL;
  size_t count = 0;
  size_t cap = 8;

  stmts = malloc(cap * sizeof(AstNode *));
  if (!stmts)
    return NULL;

  while (p->current.kind != TOK_EOF) {
    AstNode *stmt = parse_statement(p);
    if (p->had_error) {
      parser_synchronize(p);
      continue;
    }

    if (count >= cap) {
      cap *= 2;
      stmts = realloc(stmts, cap * sizeof(AstNode *));
    }
    stmts[count++] = stmt;
  }

  return ast_new_block(p->current, stmts,
                       count); // root = block de top-level stmts
}
