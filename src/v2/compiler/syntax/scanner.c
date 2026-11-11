#include "v2/compiler/syntax/scanner.h"
#include "v2/compiler/cx.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_MAX_NESTING 256

// The reserved words. They are interned before any source is read (see scan_register_keywords).
static const Keyword keywords[] = {
    {"as", 2, TOK_AS},
    {"or", 2, TOK_OR},
    {"fun", 3, TOK_FUNCTION},
    {"use", 3, TOK_USE},
    {"asm", 3, TOK_ASM},
    {"and", 3, TOK_AND},
    {"val", 3, TOK_VAL},
    {"test", 4, TOK_TEST},
    {"defer", 5, TOK_DEFER},
    {"alias", 5, TOK_ALIAS},
    {"union", 5, TOK_UNION},
    {"async", 5, TOK_ASYNC},
    {"await", 5, TOK_AWAIT},
    {"write", 5, TOK_WRITE},
    {"const", 5, TOK_CONST},
    {"assert", 6, TOK_ASSERT},
    {"sizeof", 6, TOK_SIZEOF},
    {"struct", 6, TOK_STRUCT},
    {"import", 6, TOK_IMPORT},
    {"return", 6, TOK_RETURN},
    {"autofree", 8, TOK_AUTOFREE},
    {"comptime", 8, TOK_COMPTIME},
    {"volatile", 8, TOK_VOLATILE},
};


void scan_register_keywords(void)
{
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
    {
        NameId id = name_intern(keywords[i].kw, keywords[i].len);
        cx.names[id].keyword = (uint8_t)keywords[i].kind;
    }
}

// One table lookup classifies a byte, instead of a chain of range comparisons.
enum
{
    C_IDENT_START = 1,
    C_IDENT = 2, // letters, digits, underscore
    C_DIGIT = 4,
    C_BLANK = 8, // space, tab, carriage return
};

static uint8_t char_class[256];

static void char_class_init(void)
{
    for (int c = 'a'; c <= 'z'; c++)
    {
        char_class[c] = C_IDENT_START | C_IDENT;
        char_class[c - 'a' + 'A'] = C_IDENT_START | C_IDENT;
    }
    for (int c = '0'; c <= '9'; c++)
    {
        char_class[c] = C_IDENT | C_DIGIT;
    }
    char_class['_'] = C_IDENT_START | C_IDENT;
    char_class[' '] = char_class['\t'] = char_class['\r'] = C_BLANK;
}

#define CLASS(ch) (char_class[(unsigned char)(ch)])

void source_pos(const char *src, const char *at, int *line, int *col)
{
    // Diagnostics mostly arrive in source order, so counting resumes from the last answer
    // instead of from the top of the file each time.
    static const char *last_src, *last_at, *last_line_start;
    static int last_line;

    const char *c = src;
    const char *line_start = src;
    *line = 1;
    if (last_src == src && last_at <= at)
    {
        c = last_at;
        line_start = last_line_start;
        *line = last_line;
    }
    for (; c < at; c++)
    {
        if (*c == '\n')
        {
            (*line)++;
            line_start = c + 1;
        }
    }
    *col = (int)(at - line_start) + 1;

    last_src = src;
    last_at = at;
    last_line_start = line_start;
    last_line = *line;
}

// Errors are rare, so the scanner does not track lines: the position is computed here.
static void scan_error(const char *filename, const char *src, const char *at, const char *fmt, ...)
{
    int line, col;
    va_list ap;
    source_pos(src, at, &line, &col);
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

// The next byte at or after `c` that can matter inside a body being skipped: a brace, a quote,
// a '-' (which may start a comment) or the terminating NUL. 16 bytes are tested per step with
// the vector unit every 64-bit target has (SSE2 on x86-64, NEON on arm64), so no runtime
// dispatch is needed. The source is padded (SOURCE_PADDING), so a load that starts at or before
// the NUL never reads past the buffer.
#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
static inline const char *next_body_special(const char *c)
{
    const uint8x16_t quote = vdupq_n_u8('"'), dash = vdupq_n_u8('-'), open = vdupq_n_u8('{'),
                     close = vdupq_n_u8('}'), nul = vdupq_n_u8(0);
    for (;; c += 16)
    {
        uint8x16_t v = vld1q_u8((const uint8_t *)c);
        uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, quote), vceqq_u8(v, dash)),
                                vorrq_u8(vorrq_u8(vceqq_u8(v, open), vceqq_u8(v, close)),
                                         vceqq_u8(v, nul)));
        // narrow each byte of the mask to 4 bits: a 64-bit word with 4 bits per position
        uint64_t bits = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
        if (bits)
        {
            return c + (__builtin_ctzll(bits) >> 2);
        }
    }
}
#elif defined(__SSE2__)
#include <emmintrin.h>
static inline const char *next_body_special(const char *c)
{
    const __m128i quote = _mm_set1_epi8('"'), dash = _mm_set1_epi8('-'), open = _mm_set1_epi8('{'),
                  close = _mm_set1_epi8('}'), nul = _mm_setzero_si128();
    for (;; c += 16)
    {
        __m128i v = _mm_loadu_si128((const __m128i *)c);
        __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote), _mm_cmpeq_epi8(v, dash)),
                                 _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, open),
                                                           _mm_cmpeq_epi8(v, close)),
                                              _mm_cmpeq_epi8(v, nul)));
        unsigned bits = (unsigned)_mm_movemask_epi8(m);
        if (bits)
        {
            return c + __builtin_ctz(bits);
        }
    }
}
#else
static inline const char *next_body_special(const char *c)
{
    while (*c && *c != '"' && *c != '-' && *c != '{' && *c != '}')
    {
        c++;
    }
    return c;
}
#endif

// Finds the '}' that closes the body opened at `open`, stepping over strings and comments
// (the only places a brace does not count). Returns NULL if the body is never closed.
// A body of n bytes has at most n tokens, so its length is all the room it can need later.
static const char *skip_body(const char *open)
{
    const char *c = open + 1;
    int depth = 1;

    for (;;)
    {
        c = next_body_special(c);
        switch (*c)
        {
        case '\0':
            return NULL;
        case '"':
            c++;
            while (*c && *c != '"')
            {
                if (*c == '\\' && c[1])
                {
                    c++;
                }
                c++;
            }
            if (!*c)
            {
                return NULL;
            }
            c++;
            break;
        case '-':
            if (c[1] == '-')
            {
                while (*c && *c != '\n')
                {
                    c++;
                }
            }
            else if (c[1] == '{')
            {
                c += 2;
                while (*c && !(c[0] == '}' && c[1] == '-'))
                {
                    c++;
                }
                if (!*c)
                {
                    return NULL;
                }
                c += 2;
            }
            else
            {
                c++;
            }
            break;
        case '{':
            depth++;
            c++;
            break;
        default: // '}'
            if (--depth == 0)
            {
                return c;
            }
            c++;
            break;
        }
    }
}

// The scanning loop, shared by whole files and single bodies. It appends to `v` starting at
// v->size and stops at the end of the text, or at `end` when one is given. With `defer`,
// function bodies become TOK_BODY tokens.
static int scan_into(const char *filename, const char *src, const char *from, const char *end,
                     TokenVec *v, int defer, size_t *max_body)
{
    size_t cap = v->cap;
    size_t count = v->size;
    Token *toks = v->data;
    uint32_t open_stack[SCAN_MAX_NESTING];
    int sp = 0;
    int errors = 0;
    int too_deep = 0;

    const char *c = from;
    uint32_t nl = 1;

    if (!char_class['a'])
    {
        char_class_init();
    }

    for (;;)
    {
        // Whitespace and comments: `-- line` and `-{ block }-`.
        for (;;)
        {
            if (CLASS(*c) & C_BLANK)
            {
                c++;
            }
            else if (*c == '\n')
            {
                // Indentation: 8 spaces at a time (the source is padded, so the load is safe).
                c++;
                nl = 1;
                while (name_load64(c) == 0x2020202020202020ull)
                {
                    c += 8;
                }
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
                const char *open = c;
                c += 2;
                while (*c && !(c[0] == '}' && c[1] == '-'))
                {
                    nl |= *c == '\n';
                    c++;
                }
                if (!*c)
                {
                    scan_error(filename, src, open, "unterminated block comment");
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
        TokenKind kind = TOK_UNKNOWN;
        uint32_t match = 0;

        // Never taken: the array was sized for the worst case (see scan_all and max_body).
        if (count + 1 >= cap)
        {
            fprintf(stderr, "fatal: token array overflow while scanning %s\n", filename);
            exit(2);
        }

        if (!*c || (end && c >= end))
        {
            toks[count++] = (Token){.start = start, .kind = TOK_EOF, .nl = 1};
            break;
        }

        uint8_t cls = CLASS(*c);
        if (cls & C_IDENT_START)
        {
            // An identifier leaves the scanner already interned (LuaJIT, Go). The source is
            // padded, so its hash may read whole words past the name.
            do
            {
                c++;
            } while (CLASS(*c) & C_IDENT);
            // One lookup answers both questions: which name, and whether it is reserved.
            size_t n = (size_t)(c - start);
            NameId id = name_intern_hashed(start, n, name_hash_padded(start, n));
            kind = (TokenKind)cx.names[id].keyword;
            if (!kind)
            {
                kind = TOK_IDENTIFIER;
                match = id;
            }
        }
        else if (cls & C_DIGIT)
        {
            do
            {
                c++;
            } while (CLASS(*c) & C_DIGIT);
            if (c[0] == '.' && (CLASS(c[1]) & C_DIGIT))
            {
                do
                {
                    c++;
                } while (CLASS(*c) & C_DIGIT);
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
                c++;
            }
            if (*c == '"')
            {
                c++;
            }
            else
            {
                scan_error(filename, src, start, "unterminated string literal");
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
                // Every '{' at declaration level opens a function body, except the one
                // after `struct`. A body is not tokenized now: it is one token.
                if (defer && !(count > v->size && toks[count - 1].kind == TOK_STRUCT))
                {
                    const char *close = skip_body(start);
                    size_t body_bytes = close ? (size_t)(close - start) + 1 : 0;
                    if (close && body_bytes < (1u << 24))
                    {
                        c = close + 1;
                        kind = TOK_BODY;
                        if (max_body && body_bytes > *max_body)
                        {
                            *max_body = body_bytes; // bounds its tokens, and so its nodes
                        }
                    }
                }
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
                scan_error(filename, src, start, "unexpected character '%c'", ch);
                errors++;
                break;
            }

            // Bracket matching happens here, once, so the parser can jump over any group in
            // O(1) (function-literal detection, error recovery).
            if (kind == TOK_LPAREN || kind == TOK_LBRACKET || kind == TOK_LBRACE)
            {
                if (sp < SCAN_MAX_NESTING)
                {
                    open_stack[sp++] = (uint32_t)count;
                }
                else if (!too_deep)
                {
                    scan_error(filename, src, start,
                               "brackets are nested too deeply (limit is %d levels)",
                               SCAN_MAX_NESTING);
                    errors++;
                    too_deep = 1; // say it once; the unmatched closers would only repeat it
                }
            }
            else if (kind == TOK_RPAREN || kind == TOK_RBRACKET || kind == TOK_RBRACE)
            {
                if (sp > 0 && toks[open_stack[sp - 1]].kind == (uint32_t)opener_of(kind))
                {
                    match = open_stack[--sp];
                    toks[match].match = (uint32_t)count;
                }
                else if (!too_deep)
                {
                    scan_error(filename, src, start, "unbalanced '%c'", *start);
                    errors++;
                }
            }
        }

        toks[count++] = (Token){
            .start = start,
            .len = (uint32_t)(c - start),
            .kind = kind,
            .match = match,
            .nl = nl,
        };
        nl = 0;
    }

    while (sp > 0 && !too_deep)
    {
        const Token *open = &toks[open_stack[--sp]];
        scan_error(filename, src, open->start, "'%c' is never closed", *open->start);
        errors++;
    }

    v->data = toks;
    v->size = count;
    v->cap = cap;
    return errors;
}

int scan_all(const char *filename, const char *src, size_t len, int eager, TokenVec *out,
             size_t *max_body)
{
    // Every token but the EOF takes at least one byte of source, so len + 2 slots can never
    // overflow: the scanning loop needs no growth path. They are the free end of the token
    // region, which costs address space only; the caller claims what was written.
    out->cap = len + 2;
    out->size = 0;
    out->data = (Token *)region_room(REGION_TOKENS, out->cap * sizeof(Token));
    *max_body = 0;
    return scan_into(filename, src, src, NULL, out, !eager, max_body);
}

int scan_body(const char *filename, const char *src, const Token *body, TokenVec *out)
{
    return scan_into(filename, src, body->start, body->start + body->len, out, 0, NULL);
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
    case TOK_BODY:
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
