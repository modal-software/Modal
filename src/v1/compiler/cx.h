#include "v1/compiler/syntax/tokenizer.h"
#include "v1/utils/mvec.h"
#include <stdint.h>

MVEC_IMPL(token, Token);
typedef struct
{
    _mvec_token tokens;
    Pool *pool;
    Tokenizer lexer;
    const char *source;
    uint8_t ok;
} context;

static const _mvec_constructor_token tokenvec = {
    .init = mvec_init_token,
    .push = mvec_push_token,
    .free = mvec_free_token,
    .pop = mvec_pop_token,
    .release = mvec_release_token,
};

extern context cx;
