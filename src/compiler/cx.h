#include "syntax/tokenizer.h"
#include <stdint.h>

typedef struct
{
    uint8_t ok;
    Tokenizer lexer;
} context;

extern context cx;
