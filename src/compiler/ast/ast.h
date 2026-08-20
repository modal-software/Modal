// ast.h
#ifndef AST_H
#define AST_H

#include "syntax/tokenizer.h"

// unused
typedef enum
{
    T_NIL,
    T_BOOL,

    // Unsigned integers
    T_U0,
    T_U8,
    T_U16,
    T_U32,
    T_U64,

    // Signed Integers
    T_I0,
    T_i8,
    T_I16,
    T_I32,
    T_I64,
} TypeKind;

// unused
typedef struct Type
{
    TypeKind kind;
    char *name;
    struct Type *child;
} Type;

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
    AST_WRITE_STMT,
    AST_STRING_LIT,
    AST_ASSIGN_STMT,
} AstNodeKind;

typedef enum
{
    FMT_GROUPED,
    FMT_LITERAL
} Fmt;

typedef struct AstNode
{
    AstNodeKind kind;
    Token token;

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
            struct AstNode *left;
            struct AstNode *right;
            Kind op; // +, -, *, / etc.
        } binop;

        struct
        { // AST_UNARY_OP
            struct AstNode *expr;
            Kind op; // -, ! etc.
        } unary;

        struct
        {
            Kind op;
            size_t op_len;

            struct AstNode *lhs;
            struct AstNode *rhs;
        } expr;

        struct
        {
            const char *name;
        } var;

        struct
        {                           // AST_PAREN_GROUP / AST_BLOCK
            struct AstNode **stmts; // array dinâmico (ou lista)
            size_t count;
        } block_or_group;

        struct
        {
            struct AstNode *block;
            size_t len;
            const char *name;
        } test;

        struct
        {
            struct AstNode *group;
            Fmt fmt;
        } write;

        struct
        {
            size_t len;
            size_t span;
            const char *value;
            const char *raw;
            char *_id;
        } string;

    } data;
} AstNode;

// Iterate over children of a BLOCK or PAREN_GROUP node.
// Usage: AST_EACH(block_node, child) { /* use child */ }
#define AST_EACH(block, child)                                                                     \
    for (size_t _i = 0; _i < (block)->data.block_or_group.count &&                                 \
                        ((child) = (block)->data.block_or_group.stmts[_i], 1);                     \
         _i++)

typedef struct
{
    AstNode *(*string)(Token tok);
    AstNode *(*ident)(Token tok);
    AstNode *(*binop)(Token op_tok, AstNode *left, AstNode *right);
    AstNode *(*group)(Token open_brace, AstNode **stmts, size_t count);
    AstNode *(*block)(Token open_brace, AstNode **stmts, size_t count);
    AstNode *(*test)(Token token, AstNode *block);
    AstNode *(*write)(AstNode *n, Fmt fmt);
    AstNode *(*assert)(AstNode *expr);
    AstNode *(*number)(Token tok, long long val);
    AstNode *(*expr)(Token tok, AstNode *lhs, AstNode *rhs);
} AstConstructor;

typedef struct
{
    AstConstructor new;
    void (*print)(AstNode *tree);
    void (*free)(struct AstNode *node);
} AstImpl;

extern const AstImpl ast;

#endif
