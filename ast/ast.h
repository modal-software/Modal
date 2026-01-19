// ast.h
#ifndef AST_H
#define AST_H

#include "../tokenizer/tokenizer.h" // Token

typedef enum
{
    AST_NUMBER_LIT,
    AST_IDENT,
    AST_BIN_OP,
    AST_UNARY_OP,
    AST_PAREN_GROUP,
    AST_BLOCK,
    AST_TEST_STMT,
    AST_ASSERT_STMT,
    // futuro: AST_FN_DEF, AST_VAR_DECL, AST_STRUCT etc.
} AstNodeKind;

typedef struct AstNode AstNode;

struct AstNode
{
    AstNodeKind kind;
    Token token; // token principal (pra localização + valor)

    union
    {
        struct
        {                    // AST_NUMBER_LIT
            long long value; // ou double se quiser float
        } number;

        struct
        {                     // AST_IDENT
            const char *name; // apontador pro token.start (não copia)
            size_t len;
        } ident;

        struct
        { // AST_BIN_OP
            AstNode *left;
            AstNode *right;
            Kind op; // +, -, *, / etc.
        } binop;

        struct
        { // AST_UNARY_OP
            AstNode *expr;
            Kind op; // -, ! etc.
        } unary;

        struct
        {                    // AST_PAREN_GROUP / AST_BLOCK
            AstNode **stmts; // array dinâmico (ou lista)
            size_t count;
        } block_or_group;

        struct
        {
            const char *name;
            size_t len;
            AstNode *block;
        } test;

        // AST_TEST_STMT, AST_ASSERT_STMT podem herdar fields de block + nome
    } data;
};

AstNode *ast_new_number_lit(Token tok, long long val);
AstNode *ast_new_ident(Token tok);
AstNode *ast_new_binop(Token op_tok, AstNode *left, AstNode *right);
AstNode *ast_new_block(Token open_brace, AstNode **stmts, size_t count);
AstNode *ast_new_test(Token token, AstNode *block);
AstNode *ast_new_assert(AstNode *expr);
AstNode *ast_new_number(Token tok, long long val);
void ast_free(AstNode *node);

// ... mais construtores

void ast_free(AstNode *node); // recursivo, libera filhos primeiro

#endif
