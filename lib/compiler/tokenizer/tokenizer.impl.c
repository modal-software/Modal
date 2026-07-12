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
const TokenImpl token = (TokenImpl){
    .new = token_new,
};

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

static void tokenizer_init(Tokenizer *t, const char *buffer)
{
    *t = (Tokenizer){
        .buffer = buffer,
        .pos = 0,
        .line = 1,
        .col = 1,
        .state = LEX_STATE_START,
    };
}

const TokenizerImpl tokenizer = (TokenizerImpl){
    .advance = tokenizer_advance,
    .init = tokenizer_init,
};
