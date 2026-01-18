#ifndef PARSER_H
#define PARSER_H

#include "../tokenizer/tokenizer.h" // Token, TokenKind, Tokenizer
#include "ast.h"                    // AstNode, AstNodeKind

#include <stdarg.h> // va_list (pra error variádico)
#include <stddef.h> // size_t

typedef struct Parser Parser;

struct Parser {
  Tokenizer *lexer;
  Token current;
  Token previous;
  const char *filename;
  int had_error; // flag pra saber se rolou erro em algum ponto
};

// Inicialização e entry point principal
void parser_init(Parser *p, Tokenizer *lexer, const char *filename);
AstNode *
parse_program(Parser *p); // retorna raiz da AST (um AST_BLOCK top-level)

// Helpers de consumo e avanço (usados em todos parse_*.c)
void parser_advance(Parser *p);
int parser_match(Parser *p, Kind kind);
void parser_consume(Parser *p, Kind kind, const char *msg);
void parser_error_at(Parser *p, Token *tok, const char *fmt, ...);
void parser_synchronize(Parser *p); // recovery básico após erro

// Funções de parse expostas (pra modularidade — cada uma em seu .c)
AstNode *parse_expression(Parser *p); // em parse_expr.c
AstNode *parse_statement(Parser *p);  // em parse_stmt.c
AstNode *parse_block(Parser *p);      // em parse_stmt.c
AstNode *parse_assert(Parser *p);     // em parse_stmt.c
// Futuro:
// AstNode  *parse_test_decl(Parser *p);
// AstNode  *parse_declaration(Parser *p);        // fn, struct, var...

#endif // PARSER_H
