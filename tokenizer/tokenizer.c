#include "tokenizer.h"
#include <ctype.h>
#include <string.h>

static const Keyword keywords[] = {
    {"test", 4, TEST},   {"assert", 6, ASSERT},     {"sizeof", 6, SIZEOF},
    {"defer", 5, DEFER}, {"autofree", 8, AUTOFREE}, {"alias", 5, ALIAS},
    {"use", 3, USE},     {"comptime", 8, COMPTIME}, {"union", 5, UNION},
    {"asm", 3, ASM},     {"volatile", 8, VOLATILE}, {"async", 5, ASYNC},
    {"await", 5, AWAIT}, {"and", 3, AND},           {"or", 2, OR},
};

static Kind getKeywords(const char *s, int len) {
  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if ((int)keywords[i].len == len && memcmp(s, keywords[i].kw, len) == 0) {
      return keywords[i].kind;
    }
  }
  return (Kind)IDENTIFIER;
}

char peek(Tokenizer *t) { return t->buffer[t->pos]; }

char peek_next(Tokenizer *t) { return t->buffer[t->pos + 1]; }

char advance(Tokenizer *t) {
  char c = t->buffer[t->pos++];
  if (c == '\n') {
    t->line++;
    t->col = 1;
  } else {
    t->col++;
  }
  return c;
}

Token tag(Kind kind, const char *start, int len, int line, int col) {
  return (Token){
      .kind = kind,
      .start = start,
      .len = len,
      .line = line,
      .col = col,
  };
}

void init(Tokenizer *t, const char *buffer) {
  t->buffer = buffer;
  t->pos = 0;
  t->line = 1;
  t->col = 1;
  t->state = START;
}

Token next(Tokenizer *t) {
  const char *start = NULL;
  int start_line = 0;
  int start_col = 0;

  for (;;) {
    char c = peek(t);

    if (c == '\0') {
      return tag(TOK_EOF, t->buffer + t->pos, 0, t->line, t->col);
    }

    switch (t->state) {

    case START:
      if (c == ' ' || c == '\t' || c == '\r') {
        advance(t);
        continue;
      }

      if (c == '\n') {
        advance(t);
        continue;
      }

      start = t->buffer + t->pos;
      start_line = t->line;
      start_col = t->col;

      /* Ident / keyword */
      if (isalpha(c) || c == '_') {
        t->state = (State)IDENTIFIER;
        advance(t);
        continue;
      }

      /* Number */
      if (isdigit(c)) {
        t->state = INT;
        advance(t);
        continue;
      }

      /* Line comment */
      if (c == '/' && peek_next(t) == '/') {
        t->state = LINE_COMMENT;
        advance(t);
        advance(t);
        continue;
      }

      /* Block comment */
      if (c == '/' && peek_next(t) == '*') {
        t->state = BLOCK_COMMENT;
        advance(t);
        advance(t);
        continue;
      }

      /* Operators / punctuation */
      advance(t);
      switch (c) {
      case '(':
        return tag(LPAREN, start, 1, start_line, start_col);
      case ')':
        return tag(RPAREN, start, 1, start_line, start_col);
      case '{':
        return tag(LBRACE, start, 1, start_line, start_col);
      case '}':
        return tag(RBRACE, start, 1, start_line, start_col);
      case '?':
        if (peek(t) == '?') {
          advance(t);
          if (peek(t) == '=') {
            advance(t);
            return tag(QQ_EQ, start, 3, start_line, start_col);
          }
          return tag(QQ, start, 2, start_line, start_col);
        }
        if (peek(t) == '.') {
          advance(t);
          return tag(Q_DOT, start, 2, start_line, start_col);
        }
        return tag(QUESTION, start, 1, start_line, start_col);
      case '.':
        if (peek(t) == '.' && peek_next(t) == '.') {
          advance(t);
          advance(t);
          return tag(ELLIPSIS, start, 3, start_line, start_col);
        }
        if (peek(t) == '.') {
          advance(t);
          return tag(DOTDOT, start, 2, start_line, start_col);
        }
        break;
      case '-':
        if (peek(t) == '>') {
          advance(t);
          return tag(ARROW, start, 2, start_line, start_col);
        }
        break;
      case ':':
        if (peek(t) == ':') {
          advance(t);
          return tag(DCOLON, start, 2, start_line, start_col);
        }
        break;
      case '|':
        return tag(PIPE, start, 1, start_line, start_col);
      }

      return tag(OPERATOR, start, 1, start_line, start_col);

      // Identifiers
    case IDENTIFIER:
      if (isalnum(c) || c == '_') {
        advance(t);
        continue;
      } else {
        int len = (int)((t->buffer + t->pos) - start);
        t->state = START;
        return tag(getKeywords(start, len), start, len, start_line, start_col);
      }

      // Numbers
    case INT:
      if (isdigit(c)) {
        advance(t);
        continue;
      }
      if (c == '.') {
        t->state = FLOAT;
        advance(t);
        continue;
      }
      {
        int len = (int)((t->buffer + t->pos) - start);
        t->state = START;
        return tag(NUMBER, start, len, start_line, start_col);
      }

    case FLOAT:
      if (isdigit(c)) {
        advance(t);
        continue;
      }
      {
        int len = (int)((t->buffer + t->pos) - start);
        t->state = START;
        return tag(NUMBER, start, len, start_line, start_col);
      }

      // Comments
    case LINE_COMMENT:
      if (c == '\n' || c == '\0') {
        t->state = START;
      }
      advance(t);
      continue;

    case BLOCK_COMMENT:
      if (c == '*' && peek_next(t) == '/') {
        advance(t);
        advance(t);
        t->state = START;
        continue;
      }
      advance(t);
      continue;

    default:
      advance(t);
      t->state = START;
      return tag(UNKNOWN, start, 1, start_line, start_col);
    }
  }
}
