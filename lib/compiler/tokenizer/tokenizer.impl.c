#include "tokenizer/tokenizer.h"
#include "tokenizer/tokenizer.impl.h"

static Token token_new(TokenKind kind, const char *start, int len, int line, int col)
{
    return (Token){
        .kind = kind,
        .start = start,
        .len = len,
        .line = line,
        .col = col,
    };
}

static char tokenizer_advance(Tokenizer *t)
{
    char c = t->buffer[t->pos++];
    if (c != '\n')
    {
        t->col += 1;
    }

    if (c == '\n')
    {
        t->line++;
        t->col = 1;
    }
    return c;
}

const TokenImpl token = (TokenImpl){.new = token_new};
const TokenizerImpl tokenizer = (TokenizerImpl){.advance = tokenizer_advance};
