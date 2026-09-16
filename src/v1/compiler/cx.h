#include "v1/compiler/syntax/tokenizer.h"
#include "v1/utils/mvec.h"
#include <stdint.h>

MVEC_IMPL(token, Token);
typedef struct
{
    _mvec_token tokens;
    LFPool *pool;
    Tokenizer lexer;
    const char *source;
    uint8_t ok;
} context;

extern context cx;
