// parser.h (v2)
//
// Two layers over the flat token stream:
//   - declarations and statements: one- or two-token lookahead, no backtracking
//     (parse_decl.c, parse_stmt.c)
//   - expressions: Pratt loop over a NUD/LED function-pointer table (parse_expr.c)
// Tokens are never copied: current/previous point INTO unit->tokens.data.
#ifndef MODAL_V2_PARSER_H
#define MODAL_V2_PARSER_H

#include "v2/compiler/cx.h"

#define PARSE_MAX_DEPTH 200

typedef struct Parser
{
    Unit *unit;
    const Token *current;
    const Token *previous;
    const Token *err_tok; // where the last error was reported
    int panic;            // set by an error, cleared by synchronisation: one report per mistake
    int depth;            // recursion guard
    int paren_depth;      // > 0 inside ( ) or [ ]: newlines do not end the expression there
} Parser;

// A list being parsed. Its elements wait on a stage (nested lists stack on it) and are laid out
// as consecutive nodes when the list is finished, so no node stores a link to its sibling.
typedef struct
{
    uint32_t base;  // where its elements start on the stage
    uint32_t count;
} NodeList;

typedef enum
{
    DECL_TOP,
    DECL_STRUCT,
} DeclContext;

// parser.c
void parser_init(Parser *p, Unit *u);
void parser_advance(Parser *p);
int parser_match(Parser *p, TokenKind kind);
int parser_consume(Parser *p, TokenKind kind, const char *msg);
void parser_error_at(Parser *p, const Token *tok, const char *fmt, ...);
void parser_synchronize(Parser *p);
void parser_skip_one(Parser *p); // one token, or a whole ( ) [ ] { } group
int parser_enter(Parser *p);     // returns 0 (and reports) when nesting is too deep
void parser_leave(Parser *p);
NodeList list_begin(void);
// Moves node `id`, just made, onto the list. It is the list's element now: change it through
// list_at, never through its old id.
void node_list_push(Unit *u, NodeList *list, NodeId id);
AstNode *list_at(NodeList *list, uint32_t index);
NodeId list_finish(Unit *u, NodeList *list); // lays the elements out; the first one's id, or 0
void list_abort(NodeList *list);              // drops what is staged (an error path)
void list_truncate(NodeList *list, uint32_t count);
NodeId parse_program(Parser *p);
// Is this token where a function body starts: a '{', or a body the scanner deferred?
static inline int is_body_start(const Token *t)
{
    return t->kind == TOK_LBRACE || t->kind == TOK_BODY;
}
// Parses the function body at the current token. If the scanner deferred it (TOK_BODY), it
// is one token: it is consumed, *deferred is set and its token index returned.
NodeId parse_body(Parser *p, int *deferred);
// Tokenizes and parses a deferred body into the tails of the unit's token and node arrays.
// `body_tok` is the token index of its TOK_BODY. Returns the block, or 0 if the body has
// errors (already reported). The caller cuts both tails off when it is done with the body.
NodeId parse_function_body(Unit *u, uint32_t body_tok);

static inline uint32_t tok_idx(const Parser *p, const Token *t)
{
    return (uint32_t)(t - p->unit->tokens.data);
}

// parse_decl.c
// Parses one declaration into `into` (grouped fields, `a, b: T`, are several). 0 on an error,
// after which nothing of it is on the list.
int parse_declaration(Parser *p, DeclContext ctx, NodeList *into);
NodeId parse_define(Parser *p);
NodeId parse_type(Parser *p);
int type_starts_at(const Token *t);
NodeId parse_params(Parser *p, int *has_self);
NodeId parse_import_items(Parser *p, const Token *open);

// parse_stmt.c
NodeId parse_statement(Parser *p);
NodeId parse_block(Parser *p);

// parse_expr.c
NodeId parse_expression(Parser *p);

#endif // MODAL_V2_PARSER_H
