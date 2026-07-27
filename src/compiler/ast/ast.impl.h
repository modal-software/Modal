#ifndef AST_IMPL_H
#define AST_IMPL_H

#include "ast/ast.h"
#include "tokenizer/tokenizer.h"

typedef struct
{
    AstNode (*string)(Token tok);
    AstNode (*number_lit)(Token tok, long long val);
    AstNode (*ident)(Token tok);
    AstNode (*binop)(Token op_tok, AstNode *left, AstNode *right);
    AstNode (*group)(Token open_brace, AstNode **stmts, size_t count);
    AstNode (*block)(Token open_brace, AstNode **stmts, size_t count);
    AstNode (*test)(Token token, AstNode *block);
    AstNode (*write)(AstNode *n, Fmt fmt);
    AstNode (*assert)(AstNode *expr);
    AstNode (*number)(Token tok, long long val);
} AstConstructor;

typedef struct
{
    AstConstructor new;
    void (*print)(AstNode *tree);
} AstImpl;

#endif
