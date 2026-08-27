#ifndef PARSER_H
#define PARSER_H

#include "ast/ast.h"                      // AstNode, AstNodeKind
#include "v1/compiler/syntax/tokenizer.h" // Token, TokenKind, Tokenizer

#include <stdarg.h> // va_list (pra error variádico)
#include <stdbool.h>
#include <stddef.h> // size_t
#include <stdint.h>

typedef struct Parser Parser;

struct Parser
{
    const Token *current;
    const Token *previous;
    const char *filename;
    int had_error;
};

typedef enum
{
    SYNTAX_DEFAULT,
    SYNTAX_USER,
    SYNTAX_BUILTIN
} SyntaxOrigin;

// Inicialização e entry point principal
void parser_init(Parser *p, const char *filename);
void parser_advance(Parser *p);
int parser_match(Parser *p, TokenKind kind);
void parser_consume(Parser *p, TokenKind kind,
                    // const char *msg
);

AstNode *parse_program(Parser *p);
AstNode *parse_declaration(Parser *p);
AstNode *parse_statement(Parser *p);
AstNode *parse_block(Parser *p);
AstNode *parse_expression(Parser *p); // módulo LR(0)

uint64_t eval_expr(AstNode *expr); // sanity check (usado pelo test_runner)

#endif // PARSER_H
