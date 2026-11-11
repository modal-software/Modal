#include "v2/compiler/syntax/scanner.h"
#include "v2/compiler/cx.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SCAN_MAX_NESTING 256

static const Keyword keywords[] = {
    {"test", 4, TOK_TEST},     {"assert", 6, TOK_ASSERT},     {"sizeof", 6, TOK_SIZEOF},
    {"defer", 5, TOK_DEFER},   {"autofree", 8, TOK_AUTOFREE}, {"fun", 3, TOK_FUNCTION},
    {"alias", 5, TOK_ALIAS},   {"use", 3, TOK_USE},           {"comptime", 8, TOK_COMPTIME},
    {"union", 5, TOK_UNION},   {"asm", 3, TOK_ASM},           {"volatile", 8, TOK_VOLATILE},
    {"async", 5, TOK_ASYNC},   {"await", 5, TOK_AWAIT},       {"and", 3, TOK_AND},
    {"or", 2, TOK_OR},         {"write", 5, TOK_WRITE},       {"const", 5, TOK_CONST},
    {"struct", 6, TOK_STRUCT}, {"import", 6, TOK_IMPORT},     {"as", 2, TOK_AS},
    {"val", 3, TOK_VAL},       {"return", 6, TOK_RETURN},
};

static TokenKind keyword_kind(const char *s, int len)
{
    if (len < 2 || len > 8)
    {
        return TOK_IDENTIFIER;
    }
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
    {
        if ((int)keywords[i].len == len && keywords[i].kw[0] == s[0] &&
            memcmp(s, keywords[i].kw, (size_t)len) == 0)
        {
            return keywords[i].kind;
        }
    }
    return TOK_IDENTIFIER;
}

static inline int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static inline int is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static inline int is_ident_char(char c)
{
    return is_ident_start(c) || is_digit(c);
}

static void scan_error(const char *filename, int line, int col, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\nerror [%s:%d:%d]: ", filename, line, col);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static TokenKind opener_of(TokenKind closer)
{
    switch (closer)
    {
    case TOK_RPAREN:
        return TOK_LPAREN;
    case TOK_RBRACKET:
        return TOK_LBRACKET;
    default:
        return TOK_LBRACE;
    }
}

void source_pos(const char *src, const char *at, int *line, int *col)
{
    const char *line_start = src;
    *line = 1;
    for (const char *c = src; c < at; c++)
    {
        if (*c == '\n')
        {
            (*line)++;
            line_start = c + 1;
        }
    }
    *col = (int)(at - line_start) + 1;
}

// One pass over the source. With out == NULL it only counts tokens: nothing is reported and
// no brackets are matched. With an array it fills it, reports errors and matches brackets.
static size_t scan_pass(const char *filename, const char *src, Token *out, int *error_count)
{
    size_t count = 0;
    int open_stack[SCAN_MAX_NESTING];
    int sp = 0;
    int errors = 0;
    int too_deep = 0;

    const char *c = src;
    const char *line_start = src;
    int line = 1;
    uint32_t nl = 1;

    for (;;)
    {
        // Whitespace and comments: `-- line` and `-{ block }-`.
        for (;;)
        {
            if (*c == ' ' || *c == '\t' || *c == '\r')
            {
                c++;
            }
            else if (*c == '\n')
            {
                c++;
                line++;
                line_start = c;
                nl = 1;
            }
            else if (c[0] == '-' && c[1] == '-')
            {
                while (*c && *c != '\n')
                {
                    c++;
                }
            }
            else if (c[0] == '-' && c[1] == '{')
            {
                int at_line = line;
                int at_col = (int)(c - line_start) + 1;
                c += 2;
                while (*c && !(c[0] == '}' && c[1] == '-'))
                {
                    if (*c == '\n')
                    {
                        line++;
                        line_start = c + 1;
                        nl = 1;
                    }
                    c++;
                }
                if (!*c)
                {
                    if (out)
                    {
                        scan_error(filename, at_line, at_col, "unterminated block comment");
                    }
                    errors++;
                }
                else
                {
                    c += 2;
                }
            }
            else
            {
                break;
            }
        }

        const char *start = c;
        int tok_line = line;
        int tok_col = (int)(c - line_start) + 1;
        TokenKind kind = TOK_UNKNOWN;

        if (!*c)
        {
            if (out)
            {
                out[count] = (Token){.start = start, .kind = TOK_EOF, .nl = 1};
            }
            count++;
            break;
        }

        if (is_ident_start(*c))
        {
            do
            {
                c++;
            } while (is_ident_char(*c));
            kind = keyword_kind(start, (int)(c - start));
        }
        else if (is_digit(*c))
        {
            do
            {
                c++;
            } while (is_digit(*c));
            if (c[0] == '.' && is_digit(c[1]))
            {
                do
                {
                    c++;
                } while (is_digit(*c));
            }
            kind = TOK_NUMBER;
        }
        else if (*c == '"')
        {
            c++;
            while (*c && *c != '"')
            {
                if (*c == '\\' && c[1])
                {
                    c++;
                }
                if (*c == '\n')
                {
                    line++;
                    line_start = c + 1;
                }
                c++;
            }
            if (*c == '"')
            {
                c++;
            }
            else
            {
                if (out)
                {
                    scan_error(filename, tok_line, tok_col, "unterminated string literal");
                }
                errors++;
            }
            kind = TOK_STRING;
        }
        else
        {
            char ch = *c++;
            switch (ch)
            {
            case '(':
                kind = TOK_LPAREN;
                break;
            case ')':
                kind = TOK_RPAREN;
                break;
            case '{':
                kind = TOK_LBRACE;
                break;
            case '}':
                kind = TOK_RBRACE;
                break;
            case '[':
                kind = TOK_LBRACKET;
                break;
            case ']':
                kind = TOK_RBRACKET;
                break;
            case ',':
                kind = TOK_COMMA;
                break;
            case ';':
                kind = TOK_SEMICOLON;
                break;
            case '+':
                kind = TOK_PLUS;
                break;
            case '*':
                kind = TOK_STAR;
                break;
            case '/':
                kind = TOK_SLASH;
                break;
            case '&':
                kind = TOK_AMP;
                break;
            case '|':
                kind = TOK_PIPE;
                break;
            case '-':
                kind = TOK_MINUS;
                if (*c == '>')
                {
                    c++;
                    kind = TOK_ARROW;
                }
                break;
            case '=':
                kind = TOK_EQ;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_CMP;
                }
                break;
            case '!':
                kind = TOK_BANG;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_NEQ;
                }
                break;
            case '<':
                kind = TOK_LT;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_LE;
                }
                break;
            case '>':
                kind = TOK_GT;
                if (*c == '=')
                {
                    c++;
                    kind = TOK_GE;
                }
                break;
            case ':':
                kind = TOK_COLON;
                if (*c == ':')
                {
                    c++;
                    kind = TOK_DCOLON;
                }
                else if (*c == '=')
                {
                    c++;
                    kind = TOK_DEFINE;
                }
                break;
            case '.':
                kind = TOK_DOT;
                if (c[0] == '.' && c[1] == '.')
                {
                    c += 2;
                    kind = TOK_ELLIPSIS;
                }
                else if (c[0] == '.')
                {
                    c++;
                    kind = TOK_DOTDOT;
                }
                break;
            case '?':
                kind = TOK_QUESTION;
                if (c[0] == '?' && c[1] == '=')
                {
                    c += 2;
                    kind = TOK_QQ_EQ;
                }
                else if (c[0] == '?')
                {
                    c++;
                    kind = TOK_QQ;
                }
                else if (c[0] == '.')
                {
                    c++;
                    kind = TOK_Q_DOT;
                }
                break;
            default:
                if (out)
                {
                    scan_error(filename, tok_line, tok_col, "unexpected character '%c'", ch);
                }
                errors++;
                break;
            }
        }

        if (out)
        {
            uint32_t match = 0;

            // Bracket matching happens here, once, so the parser can jump over any group in
            // O(1) (function-literal detection, error recovery).
            if (kind == TOK_LPAREN || kind == TOK_LBRACKET || kind == TOK_LBRACE)
            {
                if (sp < SCAN_MAX_NESTING)
                {
                    open_stack[sp++] = (int)count;
                }
                else if (!too_deep)
                {
                    scan_error(filename, tok_line, tok_col,
                               "brackets are nested too deeply (limit is %d levels)",
                               SCAN_MAX_NESTING);
                    errors++;
                    too_deep = 1; // say it once; the unmatched closers would only repeat it
                }
            }
            else if (kind == TOK_RPAREN || kind == TOK_RBRACKET || kind == TOK_RBRACE)
            {
                if (sp > 0 && out[open_stack[sp - 1]].kind == (uint32_t)opener_of(kind))
                {
                    match = (uint32_t)open_stack[--sp];
                    out[match].match = (uint32_t)count;
                }
                else if (!too_deep)
                {
                    scan_error(filename, tok_line, tok_col, "unbalanced '%c'", *start);
                    errors++;
                }
            }

            out[count] = (Token){
                .start = start,
                .len = (uint32_t)(c - start),
                .kind = kind,
                .match = match,
                .nl = nl,
            };
        }
        count++;
        nl = 0;
    }

    while (out && sp > 0 && !too_deep)
    {
        const Token *open = &out[open_stack[--sp]];
        int open_line, open_col;
        source_pos(src, open->start, &open_line, &open_col);
        scan_error(filename, open_line, open_col, "'%c' is never closed", *open->start);
        errors++;
    }

    *error_count = errors;
    return count;
}

int scan_all(const char *filename, const char *src, TokenVec *out)
{
    int errors = 0;
    size_t count = scan_pass(filename, src, NULL, &errors);

    out->data = (Token *)arena_alloc(count * sizeof(Token));
    out->size = count;
    out->cap = count;
    scan_pass(filename, src, out->data, &errors);
    return errors;
}

const char *token_kind_name(TokenKind kind)
{
    switch (kind)
    {
    case TOK_EOF:
        return "end of file";
    case TOK_LPAREN:
        return "'('";
    case TOK_RPAREN:
        return "')'";
    case TOK_LBRACE:
        return "'{'";
    case TOK_RBRACE:
        return "'}'";
    case TOK_LBRACKET:
        return "'['";
    case TOK_RBRACKET:
        return "']'";
    case TOK_COLON:
        return "':'";
    case TOK_COMMA:
        return "','";
    case TOK_SEMICOLON:
        return "';'";
    case TOK_DEFINE:
        return "':='";
    case TOK_EQ:
        return "'='";
    case TOK_DOT:
        return "'.'";
    case TOK_IDENTIFIER:
        return "identifier";
    case TOK_NUMBER:
        return "number";
    case TOK_STRING:
        return "string";
    default:
        return "token";
    }
}
