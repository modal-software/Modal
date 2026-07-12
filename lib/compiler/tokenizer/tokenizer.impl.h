#ifndef TOKENIZER_IMPL_H
#define TOKENIZER_IMPL_H

#include "tokenizer/tokenizer.h"
typedef struct
{
    Token (*new)(TokenKind kind, const char *start, int len, int line, int col);
} TokenImpl;
extern const TokenImpl token;

typedef struct
{
    char (*advance)(Tokenizer *t);
    void (*init)(Tokenizer *t, const char *buffer);
} TokenizerImpl;
extern const TokenizerImpl tokenizer;

#endif
