#include "syntax/tokenizer.h"
#include "syntax/tokenizer.impl.h"

static Token token_new(TokenKind kind, const char *start, int len, int line, int col)
{
    return (Token){
        kind, start, len, line, col,
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
        buffer, .pos = 0, .line = 1, .col = 1, .state = LEX_STATE_START,
    };
}

const TokenizerImpl tokenizer = (TokenizerImpl){
    .advance = tokenizer_advance,
    .init = tokenizer_init,
};
