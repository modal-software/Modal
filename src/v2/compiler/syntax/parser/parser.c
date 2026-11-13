#include "v2/compiler/syntax/parser/parser.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void parser_init(Parser *p, Unit *u)
{
    *p = (Parser){
        .unit = u,
        .current = u->tokens.data, // the stream always holds at least the EOF sentinel
    };
}

NodeId parse_body(Parser *p, int *deferred)
{
    *deferred = p->current->kind == TOK_BODY;
    if (*deferred)
    {
        parser_advance(p); // the scanner already stepped over it: the body is this one token
        return tok_idx(p, p->previous);
    }
    return parse_block(p);
}

NodeId parse_function_body(Unit *u, uint32_t body_tok)
{
    size_t first = u->tokens.size;
    int lexical = scan_body(u->path, u->src, TOK(u, body_tok), &u->tokens);
    if (lexical)
    {
        u->errors += lexical;
        cx.errors += lexical;
        return 0;
    }

    Parser p;
    int errors_before = u->errors;
    parser_init(&p, u);
    p.current = &u->tokens.data[first];
    NodeId body = parse_block(&p);
    return u->errors == errors_before ? body : 0;
}

void parser_advance(Parser *p)
{
    p->previous = p->current;
    if (p->current->kind != TOK_EOF)
    {
        p->current++;
    }
}

int parser_match(Parser *p, TokenKind kind)
{
    if (p->current->kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    return 0;
}

int parser_consume(Parser *p, TokenKind kind, const char *msg)
{
    if (p->current->kind == kind)
    {
        parser_advance(p);
        return 1;
    }
    parser_error_at(p, p->current, "%s", msg);
    return 0;
}

void parser_error_at(Parser *p, const Token *tok, const char *fmt, ...)
{
    if (p->panic)
    {
        return; // no cascades: one report until the parser has resynchronised
    }
    p->panic = 1;
    p->err_tok = tok;

    va_list ap;
    va_start(ap, fmt);
    diag_vat(p->unit, tok_idx(p, tok), "error", fmt, ap);
    va_end(ap);
}

void parser_skip_one(Parser *p)
{
    const Token *t = p->current;
    if ((t->kind == TOK_LPAREN || t->kind == TOK_LBRACKET || t->kind == TOK_LBRACE) &&
        t->match > tok_idx(p, t))
    {
        p->current = TOK(p->unit, t->match); // the scanner matched it: jump to the closer
    }
    parser_advance(p);
}

// Skips to the next place a declaration or statement can start: a new line, a separator, or
// the '}' of the enclosing body. Bracketed groups are jumped over whole.
void parser_synchronize(Parser *p)
{
    p->panic = 0;

    if (p->current == p->err_tok && p->current->kind != TOK_RBRACE)
    {
        parser_skip_one(p); // always make progress past the offending token
    }

    while (p->current->kind != TOK_EOF)
    {
        if ((p->current->nl) || p->current->kind == TOK_RBRACE)
        {
            return;
        }
        if (p->current->kind == TOK_SEMICOLON || p->current->kind == TOK_COMMA)
        {
            parser_advance(p);
            return;
        }
        parser_skip_one(p);
    }
}

int parser_enter(Parser *p)
{
    if (p->depth >= PARSE_MAX_DEPTH)
    {
        parser_error_at(p, p->current, "nesting is too deep (limit is %d levels)", PARSE_MAX_DEPTH);
        return 0;
    }
    p->depth++;
    return 1;
}

void parser_leave(Parser *p)
{
    p->depth--;
}

static AstNode *stage;
static size_t stage_top, stage_cap;

NodeList list_begin(void)
{
    return (NodeList){(uint32_t)stage_top, 0};
}

void node_list_push(Unit *u, NodeList *list, NodeId id)
{
    if (!id)
    {
        return;
    }
    if (list->base + list->count != stage_top)
    {
        fprintf(stderr, "fatal: parser: a list was pushed to while another was open\n");
        exit(2);
    }
    if (stage_top == stage_cap)
    {
        stage = (AstNode *)region_grow_array(REGION_STAGE, stage, sizeof(AstNode), &stage_cap,
                                             stage_top + 1);
    }
    stage[stage_top++] = *NODE(u, id);
    list->count++;
    if (id == u->nodes.size - 1)
    {
        u->nodes.size--; // the element was the last node made: its slot is free again
    }
    else
    {
        NODE(u, id)->kind = AST_NONE; // left behind, inert
    }
}

AstNode *list_at(NodeList *list, uint32_t index)
{
    return &stage[list->base + index];
}

NodeId list_finish(Unit *u, NodeList *list)
{
    NodeId first = (NodeId)u->nodes.size;
    for (uint32_t i = 0; i < list->count; i++)
    {
        AstNode n = stage[list->base + i];
        n.flags = (uint8_t)((n.flags & ~AST_F_LAST) | (i + 1 == list->count ? AST_F_LAST : 0));
        node_append(u, n);
    }
    stage_top = list->base;
    NodeId head = list->count ? first : 0;
    list->count = 0;
    return head;
}

void list_abort(NodeList *list)
{
    stage_top = list->base;
    list->count = 0;
}

void list_truncate(NodeList *list, uint32_t count)
{
    stage_top = list->base + count;
    list->count = count;
}

NodeId parse_program(Parser *p)
{
    Unit *u = p->unit;
    NodeId root = node_new(u, AST_MODULE, 0, 0, 0);
    NodeList decls = list_begin();

    while (p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }
        if (p->current->kind == TOK_RBRACE)
        {
            parser_error_at(p, p->current, "unexpected '}'");
            parser_advance(p);
            p->panic = 0;
            continue;
        }

        parse_declaration(p, DECL_TOP, &decls);
        if (p->panic)
        {
            parser_synchronize(p);
            continue;
        }

        if (p->current->kind != TOK_EOF && p->current->kind != TOK_SEMICOLON && !(p->current->nl))
        {
            parser_error_at(p, p->current, "expected a newline or ';' after this declaration");
            parser_synchronize(p);
        }
    }

    NodeId head = list_finish(u, &decls);
    NODE(u, root)->a = head;
    return root;
}
