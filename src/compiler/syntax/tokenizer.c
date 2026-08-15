#include "tokenizer.h"
#include "tokenizer.impl.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static const Keyword keywords[] = {
    {"test", 4, TOK_TEST},   {"assert", 6, TOK_ASSERT},     {"sizeof", 6, TOK_SIZEOF},
    {"defer", 5, TOK_DEFER}, {"autofree", 8, TOK_AUTOFREE}, {"fun", 3, TOK_FUNCTION},
    {"alias", 5, TOK_ALIAS}, {"use", 3, TOK_USE},           {"comptime", 8, TOK_COMPTIME},
    {"union", 5, TOK_UNION}, {"asm", 3, TOK_ASM},           {"volatile", 8, TOK_VOLATILE},
    {"async", 5, TOK_ASYNC}, {"await", 5, TOK_AWAIT},       {"and", 3, TOK_AND},
    {"or", 2, TOK_OR},       {"write", 5, TOK_WRITE}};

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

// static const char *string_lit(const char *s)
// {
// }

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
            return token.new(TOK_EOF, t->buffer + t->pos, 0, t->line, t->col);
        }

        switch (t->state)
        {
        case LEX_STATE_START:
            if (c == '"')
            {
                start = t->buffer + t->pos;
                start_line = t->line;
                start_col = t->col;
                tokenizer.advance(t);
                t->state = LEX_STATE_STRING_LIT;
                continue;
            }

            if (isspace(c) || c == '\t' || c == '\r')
            {
                tokenizer.advance(t);
                continue;
            }

            if (isalpha(c) || c == '_')
            {
                start = t->buffer + t->pos;
                start_line = t->line;
                start_col = t->col;
                t->state = LEX_STATE_IDENTIFIER;
                tokenizer.advance(t);
                continue;
            }

            // if (c == '#')
            // {
            //     start = t->buffer + t->pos;
            //     start_line = t->line;
            //     start_col = t->col;
            //     int len = 0;
            //
            //     for (;;)
            //     {
            //         char curr = peek(t);
            //         if (curr == '\0' || curr == '\n')
            //         {
            //             break;
            //         }
            //
            //         if (curr == '\\' && peek_next(t) == '\n')
            //         {
            //             tokenizer.advance(t);
            //             len += 2;
            //             continue;
            //         }
            //
            //         tokenizer.advance(t);
            //         len++;
            //     }
            //
            //     t->state = LEX_STATE_START;
            //     return token.new(TOK_PREPROC, start, len, start_line, start_col);
            // }

            start = t->buffer + t->pos;
            start_line = t->line;
            start_col = t->col;

            if (isdigit(c))
            {
                t->state = LEX_STATE_NUMBER_INT;
                tokenizer.advance(t);
                continue;
            }

            if (c == '-' && peek_next(t) == '-')
            {
                t->state = LEX_STATE_LINE_COMMENT;
                tokenizer.advance(t);
                continue;
            }

            if (c == '-' && peek_next(t) == '{')
            {
                t->state = LEX_STATE_BLOCK_COMMENT;
                tokenizer.advance(t);
                continue;
            }

            tokenizer.advance(t);
            switch (c)
            {
            case '(':
                return token.new(TOK_LPAREN, start, 1, start_line, start_col);
            case ')':
                return token.new(TOK_RPAREN, start, 1, start_line, start_col);
            case '{':
                return token.new(TOK_LBRACE, start, 1, start_line, start_col);
            case '}':
                return token.new(TOK_RBRACE, start, 1, start_line, start_col);
            case '?':
                if (peek(t) == '?')
                {
                    tokenizer.advance(t);
                    if (peek(t) == '=')
                    {
                        tokenizer.advance(t);
                        return token.new(TOK_QQ_EQ, start, 3, start_line, start_col);
                    }
                    return token.new(TOK_QQ, start, 2, start_line, start_col);
                }
                if (peek(t) == '.')
                {
                    tokenizer.advance(t);
                    return token.new(TOK_Q_DOT, start, 2, start_line, start_col);
                }
                return token.new(TOK_QUESTION, start, 1, start_line, start_col);
            case '.':
                if (peek(t) == '.' && peek_next(t) == '.')
                {
                    tokenizer.advance(t);
                    tokenizer.advance(t);
                    return token.new(TOK_ELLIPSIS, start, 3, start_line, start_col);
                }
                if (peek(t) == '.')
                {
                    tokenizer.advance(t);
                    return token.new(TOK_DOTDOT, start, 2, start_line, start_col);
                }
                break;
            case '-':
                if (peek(t) == '>')
                {
                    tokenizer.advance(t);
                    return token.new(TOK_ARROW, start, 2, start_line, start_col);
                }
                break;
            case ':':
                if (peek(t) == ':')
                {
                    tokenizer.advance(t);
                    return token.new(TOK_DCOLON, start, 2, start_line, start_col);
                }
                break;
            case '|':
                return token.new(TOK_PIPE, start, 1, start_line, start_col);
            }

            return token.new(TOK_OPERATOR, start, 1, start_line, start_col);

        case LEX_STATE_IDENTIFIER:
            if (isalnum(c) || c == '_')
            {
                tokenizer.advance(t);
                continue;
            }
            int len = (int)((t->buffer + t->pos) - start);
            t->state = LEX_STATE_START;
            return token.new(get_keyword(start, len), start, len, start_line, start_col);

        case LEX_STATE_NUMBER_INT:
            if (isdigit(c))
            {
                tokenizer.advance(t);
                continue;
            }
            if (c == '.')
            {
                t->state = LEX_STATE_NUMBER_FLOAT;
                tokenizer.advance(t);
                continue;
            }
            {
                int len = (int)((t->buffer + t->pos) - start);
                t->state = LEX_STATE_START;
                return token.new(TOK_NUMBER, start, len, start_line, start_col);
            }

        case LEX_STATE_NUMBER_FLOAT:
            if (isdigit(c))
            {
                tokenizer.advance(t);
                continue;
            }
            {
                int len = (int)((t->buffer + t->pos) - start);
                t->state = LEX_STATE_START;
                return token.new(TOK_NUMBER, start, len, start_line, start_col);
            }
        case LEX_STATE_STRING_LIT:
        {
            const char *buf = t->buffer;
            int pos = t->pos;
            // printf("STRING_LIT entry: \npos=%d \nchar='%c'\n\n", pos, buf[pos]);

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

            if (buf[pos] == '"')
            {
                pos++;
                t->col++;
            }
            t->pos = pos;

            int len = (int)((buf + t->pos) - start);
            t->state = LEX_STATE_START;
            // printf("STRING_LIT exit: \n\t-> pos=%d \n\t-> len=%d \n\t-> delimiter_char='%c' "
            //        "\n\t-> raw=%.*s\n\n",
            //        t->pos, len, buf[t->pos], len, start);

            return token.new(TOK_STRING, start, len, start_line, start_col);
        }

        case LEX_STATE_LINE_COMMENT:
            if (c == '\n' || c == '\0')
            {
                tokenizer.advance(t);
                t->state = LEX_STATE_START;
                continue;
            }
            tokenizer.advance(t);
            continue;

        case LEX_STATE_BLOCK_COMMENT:
            if (c == '}' && peek_next(t) == '-')
            {
                tokenizer.advance(t);
                tokenizer.advance(t);
                t->state = LEX_STATE_START;
                continue;
            }
            tokenizer.advance(t);
            continue;

        default:
            tokenizer.advance(t);
            t->state = LEX_STATE_START;
            return token.new(TOK_UNKNOWN, start, 1, start_line, start_col);
        }
    }
}
