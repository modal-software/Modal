#include "parser.h"
#include <ctype.h> // isprint pra sanitizar output
#include <stdarg.h>
#include <stdio.h>

// Mostra erro com linha do fonte e ^
void parser_error_at(Parser *p, Token *tok, const char *fmt, ...) {
  p->had_error = 1; // flag global — por quê? Pra main saber se parse deu bom

  fprintf(stderr, "Erro [%s:%d:%d]: ", p->filename, tok->line, tok->col);

  va_list args;
  va_start(args, fmt);
  vfprintf(
      stderr, fmt,
      args); // mensagem flexível — por quê? Como printf, fácil usar %s %d etc.
  va_end(args);
  fprintf(stderr, "\n");

  // Pega linha do buffer
  const char *line_start = tok->start;
  while (line_start > p->lexer->buffer && *(line_start - 1) != '\n') {
    line_start--; // volta até começo da linha — por quê? Mostra contexto todo
  }
  const char *line_end = tok->start;
  while (*line_end && *line_end != '\n')
    line_end++; // até fim

  fprintf(stderr, "%3d | %.*s\n", tok->line, (int)(line_end - line_start),
          line_start); // linha numerada — por quê? Legível

  fprintf(stderr, "      | "); // alinhamento
  for (int i = 1; i < tok->col; i++)
    fputc(' ', stderr); // espaços até coluna
  fputc('^', stderr);   // o ^
  fprintf(stderr, "\n");
}

// Pula até próximo sync point (ex: ; ou })
void parser_synchronize(Parser *p) {
  parser_advance(p); // pula o ruim
  while (p->current.kind != TOK_EOF) {
    if (p->previous.kind == OPERATOR && *p->previous.start == ';')
      return;                  // ; fecha stmt
    switch (p->current.kind) { // keywords que começam novo stmt
    case TEST:
    case ASSERT:
    case LBRACE:
    case RBRACE:
      return; // sync aqui — por quê? Continua parseando o resto do arquivo
    default:
      parser_advance(p);
    }
  }
}
