#ifndef LEXER_H
#define LEXER_H

#include <stddef.h>
#define MODAL_VERSION "0.0.1"

// Token kinds - what the token represents
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
    // Multi-char operators
    TOK_Q_DOT,
    TOK_QQ_EQ,
    TOK_QQ,
    TOK_QUESTION,
    TOK_PIPE,
    TOK_DCOLON,
    TOK_ELLIPSIS,
    TOK_DOTDOT,
    TOK_ARROW,
} TokenKind;

// Lexer states - internal scanning state machine
typedef enum
{
    LEX_STATE_START,
    LEX_STATE_IDENTIFIER,
    LEX_STATE_NUMBER_INT,
    LEX_STATE_NUMBER_FLOAT,
    LEX_STATE_STRING_LIT,
    LEX_STATE_LINE_COMMENT,
    LEX_STATE_BLOCK_COMMENT,
} LexerState;

typedef struct
{
    TokenKind kind;
    const char *start;
    int len;
    int line;
    int col;
} Token;

typedef struct
{
    const char *buffer;
    int pos;
    int line;
    int col;
    LexerState state;
} Tokenizer;

typedef struct
{
    const char *kw;
    size_t len;
    TokenKind kind;
} Keyword;

Token token_make(TokenKind kind, const char *start, int len, int line, int col);
void init(Tokenizer *t, const char *buffer);
Token next(Tokenizer *t);

// Legacy aliases for compatibility - can be removed once parser is updated
#define Kind TokenKind
#define LPAREN TOK_LPAREN
#define RPAREN TOK_RPAREN
#define LBRACE TOK_LBRACE
#define RBRACE TOK_RBRACE
#define OPERATOR TOK_OPERATOR
#define IDENTIFIER TOK_IDENTIFIER
#define UNKNOWN TOK_UNKNOWN
#define NUMBER TOK_NUMBER
#define STRING TOK_STRING
#define TEST TOK_TEST
#define ASSERT TOK_ASSERT
#define SIZEOF TOK_SIZEOF
#define DEFER TOK_DEFER
#define AUTOFREE TOK_AUTOFREE
#define ALIAS TOK_ALIAS
#define USE TOK_USE
#define COMPTIME TOK_COMPTIME
#define UNION TOK_UNION
#define ASM TOK_ASM
#define VOLATILE TOK_VOLATILE
#define ASYNC TOK_ASYNC
#define AWAIT TOK_AWAIT
#define AND TOK_AND
#define OR TOK_OR
#define Q_DOT TOK_Q_DOT
#define QQ_EQ TOK_QQ_EQ
#define QQ TOK_QQ
#define QUESTION TOK_QUESTION
#define PIPE TOK_PIPE
#define DCOLON TOK_DCOLON
#define ELLIPSIS TOK_ELLIPSIS
#define DOTDOT TOK_DOTDOT
#define ARROW TOK_ARROW
#define PREPROC TOK_PREPROC

#endif
