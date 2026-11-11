// scanner.h (v2): flat, zero-copy token stream.
//
// Tokens are slices into the source buffer (start + len). The whole file is scanned once into
// one contiguous array, sized exactly, that ends with a TOK_EOF sentinel, so the parser walks it
// with a plain pointer and can look ahead with tok[1] at no cost.
#ifndef MODAL_V2_SCANNER_H
#define MODAL_V2_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#define MODAL_VERSION "0.0.3"

typedef enum
{
    TOK_EOF,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COLON,
    TOK_COMMA,
    TOK_SEMICOLON,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_OPERATOR,
    TOK_IDENTIFIER,
    TOK_UNKNOWN,
    TOK_NUMBER,
    TOK_STRING,
    TOK_PREPROC,
    // DECLARATION
    TOK_CONST,
    // Keywords
    TOK_FUNCTION,
    TOK_TEST,
    TOK_ASSERT,
    TOK_SIZEOF,
    TOK_DEFER,
    TOK_AUTOFREE,
    TOK_ALIAS,
    TOK_USE,
    TOK_COMPTIME,
    TOK_UNION,
    TOK_ASM,
    TOK_VOLATILE,
    TOK_ASYNC,
    TOK_AWAIT,
    TOK_AND,
    TOK_OR,
    TOK_WRITE,
    // Multi-char operators
    TOK_Q_DOT,
    TOK_EQ,
    TOK_CMP,
    TOK_DEFINE,
    TOK_QQ_EQ,
    TOK_QQ,
    TOK_QUESTION,
    TOK_PIPE,
    TOK_DCOLON,
    TOK_ELLIPSIS,
    TOK_DOTDOT,
    TOK_ARROW,
    // v2: operators that used to be lumped into TOK_OPERATOR
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_DOT,
    TOK_STAR,
    TOK_PLUS,
    TOK_MINUS,
    TOK_SLASH,
    TOK_AMP,
    TOK_BANG,
    TOK_NEQ,
    TOK_LT,
    TOK_GT,
    TOK_LE,
    TOK_GE,
    // v2: keywords
    TOK_STRUCT,
    TOK_IMPORT,
    TOK_AS,
    TOK_VAL,
    TOK_RETURN,
    TOK__COUNT,
} TokenKind;

// 16 bytes. A token is a slice of the source buffer plus what the parser needs to move fast.
// Line and column are not stored: they are only needed for a diagnostic, and are recomputed
// from `start` at that moment (see source_pos).
typedef struct
{
    const char *start;   // into the source buffer, never copied
    uint32_t len : 24;   // a token is at most 16 MB long
    uint32_t kind : 8;   // TokenKind
    uint32_t match : 31; // for ( [ { and their closers: index of the partner, 0 if unbalanced
    uint32_t nl : 1;     // first token on its line: this is how statements end without ';'
} Token;

_Static_assert(sizeof(Token) == 16, "Token must stay 16 bytes");

typedef struct
{
    Token *data;
    size_t size;
    size_t cap;
} TokenVec;

typedef struct
{
    const char *kw;
    size_t len;
    TokenKind kind;
} Keyword;

// Scans `src` (NUL-terminated) into `out`. The source is scanned twice: once to count the
// tokens, once to fill an array of exactly that size, so nothing is over-reserved or moved.
// Returns the number of lexical errors (already reported to stderr).
int scan_all(const char *filename, const char *src, TokenVec *out);

// Line and column (1-based) of a position in `src`.
void source_pos(const char *src, const char *at, int *line, int *col);

const char *token_kind_name(TokenKind kind);

#endif // MODAL_V2_SCANNER_H
