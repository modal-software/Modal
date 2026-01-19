// tokenizer.c
#include "tokenizer.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

static const Keyword keywords[] = {
    {"test", 4, TEST},   {"assert", 6, ASSERT},     {"sizeof", 6, SIZEOF},
    {"defer", 5, DEFER}, {"autofree", 8, AUTOFREE}, {"alias", 5, ALIAS},
    {"use", 3, USE},     {"comptime", 8, COMPTIME}, {"union", 5, UNION},
    {"asm", 3, ASM},     {"volatile", 8, VOLATILE}, {"async", 5, ASYNC},
    {"await", 5, AWAIT}, {"and", 3, AND},           {"or", 2, OR},
};

static Kind get_keyword(const char *s, int len) {
  for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
    if ((int)keywords[i].len == len && memcmp(s, keywords[i].kw, len) == 0) {
      return keywords[i].kind;
    }
  }
  return IDENTIFIER;
}

char peek(Tokenizer *t) {
  printf("%.s", t->buffer);
  return t->buffer[t->pos];
}

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

Token token_make(Kind kind, const char *start, int len, int line, int col) {
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

  // printf("%s", t->buffer);

  for (;;) {
    char c = peek(t);

    if (!c) {
      return token_make(TOK_EOF, t->buffer + t->pos, 0, t->line, t->col);
    }
    // Debugger to token validation purposes (experimental)
    // printf("token: '%c' (code %d)\npos=%d\n\n", c, (int)c, t->pos);

    switch (t->state) {
    case START:
      if (c == '"') {
        start = t->buffer + t->pos;
        start_line = t->line;
        start_col = t->col;
        t->state = START;

        advance(t);

        continue;
      }

      if (isspace(c) || c == '\t' || c == '\r') {
        advance(t);
        continue;
      }
      // printf("START: c='%c' code=%d\n", c, (int)c);

      if (isalpha(c) || c == '_') {
        t->state = STATE_IDENTIFIER;
        advance(t);
        continue;
      }

      if (c == '\n') {
        advance(t);
        continue;
      }

      if (c == '#') {
        const char *start = t->buffer + t->pos;
        int start_line = t->line;
        int start_col = t->col;
        int len = 0;

        // Consome até o fim da linha lógica (respeitando \ no final da linha)
        for (;;) {
          char curr = peek(t);
          if (curr == '\0' || curr == '\n') {
            break;
          }

          if (curr == '\\' && peek_next(t) == '\n') {
            advance(t);
            t->line++;
            len += 2;
            continue;
          }

          advance(t);
          len++;
        }

        t->state = START; // volta pro estado normal
        return token_make((Kind)PREPROC, start, len, start_line, start_col);
      }

      start = t->buffer + t->pos;
      start_line = t->line;
      start_col = t->col;

      if (isdigit(c)) {
        t->state = INT;
        advance(t);
        continue;
      }

      if (c == '-' && peek_next(t) == '-') {
        t->state = LINE_COMMENT;
        advance(t);
        advance(t);
        continue;
      }

      if (c == '-' && peek_next(t) == '{') {
        t->state = BLOCK_COMMENT;
        advance(t);
        advance(t);
        continue;
      }

      advance(t);
      switch (c) {
      case '(':
        printf("STATE: '('\ntoken: '%c'\n\n", c);
        return token_make(LPAREN, start, 1, start_line, start_col);
      case ')':
        return token_make(RPAREN, start, 1, start_line, start_col);
      case '{':
        return token_make(LBRACE, start, 1, start_line, start_col);
      case '}':
        return token_make(RBRACE, start, 1, start_line, start_col);
      case '?':
        if (peek(t) == '?') {
          advance(t);
          if (peek(t) == '=') {
            advance(t);
            return token_make(QQ_EQ, start, 3, start_line, start_col);
          }
          return token_make(QQ, start, 2, start_line, start_col);
        }
        if (peek(t) == '.') {
          advance(t);
          return token_make(Q_DOT, start, 2, start_line, start_col);
        }
        return token_make(QUESTION, start, 1, start_line, start_col);
      case '.':
        if (peek(t) == '.' && peek_next(t) == '.') {
          advance(t);
          advance(t);
          return token_make(ELLIPSIS, start, 3, start_line, start_col);
        }
        if (peek(t) == '.') {
          advance(t);
          return token_make(DOTDOT, start, 2, start_line, start_col);
        }
        break;
      case '-':
        if (peek(t) == '>') {
          advance(t);
          return token_make(ARROW, start, 2, start_line, start_col);
        }
        break;
      case ':':
        if (peek(t) == ':') {
          advance(t);
          return token_make(DCOLON, start, 2, start_line, start_col);
        }
        break;
      case '|':
        return token_make(PIPE, start, 1, start_line, start_col);
      }

      return token_make(OPERATOR, start, 1, start_line, start_col);

    case STATE_IDENTIFIER:
      if (isalnum(c) || c == '_') {
        advance(t);
        continue;
      }
      int len = (int)((t->buffer + t->pos) - start);
      t->state = START;
      return token_make(get_keyword(start, len), start, len, start_line,
                        start_col);

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
        return token_make(NUMBER, start, len, start_line, start_col);
      }

    case FLOAT:
      if (isdigit(c)) {
        advance(t);
        continue;
      }
      {
        int len = (int)((t->buffer + t->pos) - start);
        t->state = START;
        return token_make(NUMBER, start, len, start_line, start_col);
      }
    case STRING_LIT: {
      const char *buf = t->buffer;
      int pos = t->pos;
      printf("%c", pos);

      while (buf[pos] != '\0' && buf[pos] != '"') {
        if (buf[pos] == '\\') {
          pos++;
          t->col++;
          if (buf[pos] != '\0') {
            if (buf[pos] == 'n') {
              pos++;
              t->col++;
            } else {
              pos++;
              t->col++;
            }
          }
        } else {
          if (buf[pos] == '\n') {
            t->line++;
            t->col = 1;
          } else {
            t->col++;
          }
          pos++;
        }
      }

      // Update tokenizer position
      t->pos = pos;

      // Consume closing quote if present
      if (buf[pos] == '"') {
        t->pos++;
        t->col++;
      }

      int len = (int)((buf + t->pos) - start);
      t->state = START;
      return token_make(STRING, start, len, start_line, start_col);
    }
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
      return token_make(UNKNOWN, start, 1, start_line, start_col);
    }
  }
}
