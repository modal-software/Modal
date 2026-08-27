#include "v1/compiler/syntax/parser/parser.h"

static AstNode *parse_gen_decl(Parser *p)
{
    parser_advance(p);

    const Token name = p->current;

    parser_consume(p, TOK_IDENTIFIER, "expected function name");
    parser_consume(p, TOK_LPAREN, "expected '(' after function name");

    if (p->current.kind != TOK_RPAREN)
    {
        parser_error_at(p, &p->current, "function parameters not supported");
        return NULL;
    }

    parser_advance(p);
    AstNode *body = parse_block(p);
    if (!body)
    {
        return NULL:
    }

    return ast.new.gendecl(name, body);
}
