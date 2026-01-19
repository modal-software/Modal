#include "tokenizer.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *kind_to_string(TokenKind kind)
{
    switch (kind)
    {
    case TOK_EOF:
        return "end of file";
    case TOK_LPAREN:
        return "(";
    case TOK_RPAREN:
        return ")";
    case TOK_LBRACE:
        return "{";
    case TOK_RBRACE:
        return "}";
    case TOK_NUMBER:
        return "number";
    case TOK_IDENTIFIER:
        return "identifier";
    case TOK_OPERATOR:
        return "operator";
    default:
        return "unknown token";
    }
}

static const Keyword keywords[] = {
    {"test", 4, TOK_TEST},   {"assert", 6, TOK_ASSERT},     {"sizeof", 6, TOK_SIZEOF},
    {"defer", 5, TOK_DEFER}, {"autofree", 8, TOK_AUTOFREE}, {"fun", 8, TOK_FUNCTION},
    {"alias", 5, TOK_ALIAS}, {"use", 3, TOK_USE},           {"comptime", 8, TOK_COMPTIME},
    {"union", 5, TOK_UNION}, {"asm", 3, TOK_ASM},           {"volatile", 8, TOK_VOLATILE},
    {"async", 5, TOK_ASYNC}, {"await", 5, TOK_AWAIT},       {"and", 3, TOK_AND},
    {"or", 2, TOK_OR},
};

static TokenKind get_keyword(const char *s, int len)
{
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
    {
        if ((int)keywords[i].len == len && memcmp(s, keywords[i].kw, len) == 0)
        {
            return keywords[i].kind;
        }
    }
    return TOK_IDENTIFIER;
}

static char peek(Tokenizer *t)
{
    return t->buffer[t->pos];
}

static char peek_next(Tokenizer *t)
{
    return t->buffer[t->pos + 1];
}

static char advance(Tokenizer *t)
{
    char c = t->buffer[t->pos++];
    if (c == '\n')
    {
        t->line++;
        t->col = 1;
    }
    else
    {
        t->col++;
    }
    return c;
}

Token token_make(TokenKind kind, const char *start, int len, int line, int col)
{
    return (Token){
        .kind = kind,
        .start = start,
        .len = len,
        .line = line,
        .col = col,
    };
}

void init(Tokenizer *t, const char *buffer)
{
    t->buffer = buffer;
    t->pos = 0;
    t->line = 1;
    t->col = 1;
    t->state = LEX_STATE_START;
}

Token next(Tokenizer *t)
{
    const char *start = NULL;
    int start_line = 0;
    int start_col = 0;

    for (;;)
    {
        char c = peek(t);

        if (!c)
        {
            return token_make(TOK_EOF, t->buffer + t->pos, 0, t->line, t->col);
        }

        switch (t->state)
        {
        case LEX_STATE_START:
            if (c == '"')
            {
                start = t->buffer + t->pos;
                start_line = t->line;
                start_col = t->col;
                advance(t);
                t->state = LEX_STATE_STRING_LIT;
                continue;
            }

            if (isspace(c) || c == '\t' || c == '\r')
            {
                advance(t);
                continue;
            }

            if (isalpha(c) || c == '_')
            {
                start = t->buffer + t->pos;
                start_line = t->line;
                start_col = t->col;
                t->state = LEX_STATE_IDENTIFIER;
                advance(t);
                continue;
            }

            if (c == '\n')
            {
                advance(t);
                continue;
            }

            if (c == '#')
            {
                start = t->buffer + t->pos;
                start_line = t->line;
                start_col = t->col;
                int len = 0;

                for (;;)
                {
                    char curr = peek(t);
                    if (curr == '\0' || curr == '\n')
                    {
                        break;
                    }

                    if (curr == '\\' && peek_next(t) == '\n')
                    {
                        advance(t);
                        t->line++;
                        len += 2;
                        continue;
                    }

                    advance(t);
                    len++;
                }

                t->state = LEX_STATE_START;
                return token_make(TOK_PREPROC, start, len, start_line, start_col);
            }

            start = t->buffer + t->pos;
            start_line = t->line;
            start_col = t->col;

            if (isdigit(c))
            {
                t->state = LEX_STATE_NUMBER_INT;
                advance(t);
                continue;
            }

            if (c == '-' && peek_next(t) == '-')
            {
                t->state = LEX_STATE_LINE_COMMENT;
                advance(t);
                advance(t);
                continue;
            }

            if (c == '-' && peek_next(t) == '{')
            {
                t->state = LEX_STATE_BLOCK_COMMENT;
                advance(t);
                advance(t);
                continue;
            }

            advance(t);
            switch (c)
            {
            case '(':
                return token_make(TOK_LPAREN, start, 1, start_line, start_col);
            case ')':
                return token_make(TOK_RPAREN, start, 1, start_line, start_col);
            case '{':
                return token_make(TOK_LBRACE, start, 1, start_line, start_col);
            case '}':
                return token_make(TOK_RBRACE, start, 1, start_line, start_col);
            case '?':
                if (peek(t) == '?')
                {
                    advance(t);
                    if (peek(t) == '=')
                    {
                        advance(t);
                        return token_make(TOK_QQ_EQ, start, 3, start_line, start_col);
                    }
                    return token_make(TOK_QQ, start, 2, start_line, start_col);
                }
                if (peek(t) == '.')
                {
                    advance(t);
                    return token_make(TOK_Q_DOT, start, 2, start_line, start_col);
                }
                return token_make(TOK_QUESTION, start, 1, start_line, start_col);
            case '.':
                if (peek(t) == '.' && peek_next(t) == '.')
                {
                    advance(t);
                    advance(t);
                    return token_make(TOK_ELLIPSIS, start, 3, start_line, start_col);
                }
                if (peek(t) == '.')
                {
                    advance(t);
                    return token_make(TOK_DOTDOT, start, 2, start_line, start_col);
                }
                break;
            case '-':
                if (peek(t) == '>')
                {
                    advance(t);
                    return token_make(TOK_ARROW, start, 2, start_line, start_col);
                }
                break;
            case ':':
                if (peek(t) == ':')
                {
                    advance(t);
                    return token_make(TOK_DCOLON, start, 2, start_line, start_col);
                }
                break;
            case '|':
                return token_make(TOK_PIPE, start, 1, start_line, start_col);
            }

            return token_make(TOK_OPERATOR, start, 1, start_line, start_col);

        case LEX_STATE_IDENTIFIER:
            if (isalnum(c) || c == '_')
            {
                advance(t);
                continue;
            }
            {
                int len = (int)((t->buffer + t->pos) - start);
                t->state = LEX_STATE_START;
                return token_make(get_keyword(start, len), start, len, start_line, start_col);
            }

        case LEX_STATE_NUMBER_INT:
            if (isdigit(c))
            {
                advance(t);
                continue;
            }
            if (c == '.')
            {
                t->state = LEX_STATE_NUMBER_FLOAT;
                advance(t);
                continue;
            }
            {
                int len = (int)((t->buffer + t->pos) - start);
                t->state = LEX_STATE_START;
                return token_make(TOK_NUMBER, start, len, start_line, start_col);
            }

        case LEX_STATE_NUMBER_FLOAT:
            if (isdigit(c))
            {
                advance(t);
                continue;
            }
            {
                int len = (int)((t->buffer + t->pos) - start);
                t->state = LEX_STATE_START;
                return token_make(TOK_NUMBER, start, len, start_line, start_col);
            }

        case LEX_STATE_STRING_LIT:
        {
            const char *buf = t->buffer;
            int pos = t->pos;

            while (buf[pos] != '\0' && buf[pos] != '"')
            {
                if (buf[pos] == '\\')
                {
                    pos++;
                    t->col++;
                    if (buf[pos] != '\0')
                    {
                        pos++;
                        t->col++;
                    }
                }
                else
                {
                    if (buf[pos] == '\n')
                    {
                        t->line++;
                        t->col = 1;
                    }
                    else
                    {
                        t->col++;
                    }
                    pos++;
                }
            }

            t->pos = pos;

            if (buf[pos] == '"')
            {
                t->pos++;
                t->col++;
            }

            int len = (int)((buf + t->pos) - start);
            t->state = LEX_STATE_START;
            return token_make(TOK_STRING, start, len, start_line, start_col);
        }

        case LEX_STATE_LINE_COMMENT:
            if (c == '\n' || c == '\0')
            {
                t->state = LEX_STATE_START;
            }
            advance(t);
            continue;

        case LEX_STATE_BLOCK_COMMENT:
            if (c == '}' && peek_next(t) == '-')
            {
                advance(t);
                advance(t);
                t->state = LEX_STATE_START;
                continue;
            }
            advance(t);
            continue;

        default:
            advance(t);
            t->state = LEX_STATE_START;
            return token_make(TOK_UNKNOWN, start, 1, start_line, start_col);
        }
    }
}
