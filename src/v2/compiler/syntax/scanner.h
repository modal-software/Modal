// scanner.h (v2): flat, zero-copy token stream.
//
// Tokens are slices into the source buffer (start + len). The whole file is scanned once into
// one contiguous array that ends with a TOK_EOF sentinel, so the parser walks it
// with a plain pointer and can look ahead with tok[1] at no cost.
#ifndef MODAL_V2_SCANNER_H
#define MODAL_V2_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#define MODAL_VERSION "0.0.4"

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
    // A whole function body, `{ ... }`, as ONE token: `start` is its '{' and `len` covers it
    // to its '}'. The scanner produces it in place of the body's own tokens, which are made
    // only when that function is compiled (see scan_body).
    TOK_BODY,
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
    // For ( [ { and their closers: index of the partner, 0 if unbalanced.
    // For an identifier: its interned name (NameId), so names compare as integers.
    uint32_t match : 31;
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

// Scans `src` (NUL-terminated, `len` bytes) into `out` in one pass.
//
// Unless `eager`, a function body is not tokenized: it is skipped as raw text and becomes a
// single TOK_BODY token. The token stream of a file is then only its declarations, and
// `*max_body` receives an upper bound on the token count of its largest body.
//
// The token array is malloc'd (it grows while scanning, which an arena cannot do without
// leaving old copies behind) and freed by the driver.
// Returns the number of lexical errors (already reported to stderr).
int scan_all(const char *filename, const char *src, size_t len, int eager, TokenVec *out,
             size_t *max_body);

// Tokenizes one deferred body: appends its tokens, '{' through '}' and then an EOF, to the
// end of `out`, which must have room for them (see `max_body` above).
// Returns the number of lexical errors in the body.
int scan_body(const char *filename, const char *src, const Token *body, TokenVec *out);

// Line and column (1-based) of a position in `src`.
void source_pos(const char *src, const char *at, int *line, int *col);

const char *token_kind_name(TokenKind kind);

// Interns the reserved words, marked with their token kinds, so that the scanner's one lookup
// of an identifier also tells whether it is a keyword. Called when the name table is created.
void scan_register_keywords(void);

#endif // MODAL_V2_SCANNER_H
