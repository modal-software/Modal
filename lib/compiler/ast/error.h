#ifndef ERROR_H
#define ERROR_H
#include "parser.h"

void fatal(const char *fmt);
void parser_error_at(Parser *p, Token *tok, const char *fmt, ...);
void parser_synchronize(Parser *p);

#endif
