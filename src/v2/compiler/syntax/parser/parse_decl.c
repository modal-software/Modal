// parse_decl.c (v2): declarations.
//
// One rule covers fields and methods: where the type is written decides mutability.
//
//   name: T                     immutable field            (type last)
//   a, b: T                     immutable fields, grouped
//   T name                      mutable field              (type first, one name)
//   name: struct { ... }        struct (a domain of fields and methods)
//   name(params): T { ... }     immutable method, defined once
//   T name(params) { ... }      mutable method, may be overloaded
//   T name(params): expr        mutable method, one-line body
//   name := value               binding (a function literal, an import, a constant)
//   val (a as b, c): import(x)  destructuring import
//   fun name(params) T { ... }  keyword profile of the immutable method
//
// The form is decided from the first two tokens, so nothing is ever re-parsed.
#include "v2/compiler/syntax/parser/parser.h"

static NodeId parse_struct_body(Parser *p);

int type_starts_at(const Token *t)
{
    switch (t->kind)
    {
    case TOK_IDENTIFIER:
    case TOK_STAR:
    case TOK_STRUCT:
        return 1;
    case TOK_LBRACKET:
        return t[1].kind == TOK_RBRACKET;
    default:
        return 0;
    }
}

// type := '[' ']' type | '*' type | 'struct' '{' members '}' | IDENT ('.' IDENT)*
NodeId parse_type(Parser *p)
{
    Unit *u = p->unit;
    const Token *t = p->current;
    NodeId type = 0;

    if (!parser_enter(p))
    {
        return 0;
    }

    switch (t->kind)
    {
    case TOK_LBRACKET:
        parser_advance(p);
        if (parser_consume(p, TOK_RBRACKET, "expected ']' (a slice type is written []T)"))
        {
            NodeId elem = parse_type(p);
            type = elem ? node_new(u, AST_TYPE_SLICE, tok_idx(p, t), elem, 0) : 0;
        }
        break;
    case TOK_STAR:
    {
        parser_advance(p);
        NodeId elem = parse_type(p);
        type = elem ? node_new(u, AST_TYPE_PTR, tok_idx(p, t), elem, 0) : 0;
        break;
    }
    case TOK_STRUCT:
    {
        parser_advance(p);
        NodeId members = parse_struct_body(p);
        type = p->panic ? 0 : node_new(u, AST_TYPE_STRUCT, tok_idx(p, t), members, 0);
        break;
    }
    case TOK_IDENTIFIER:
    {
        uint16_t extra = 0;
        parser_advance(p);
        while (p->current->kind == TOK_DOT && !(p->current->nl) &&
               p->current[1].kind == TOK_IDENTIFIER)
        {
            parser_advance(p);
            parser_advance(p);
            extra++;
        }
        type = node_new(u, AST_TYPE_NAME, tok_idx(p, t), 0, 0);
        NODE(u, type)->aux = extra;
        break;
    }
    default:
        parser_error_at(p, t, "expected a type");
        break;
    }

    parser_leave(p);
    return type;
}

// params := '(' [ param (',' param)* ] ')'      param := IDENT [ ':' type ] | type IDENT
//
// The same rule as fields: where the type is written decides mutability.
//   a: u8     immutable parameter       u8 a     mutable parameter (it can be assigned to)
// A type applies backwards to the untyped names before it: (n3, n4: u8).
// `self` is the only parameter that may stay untyped, and it never joins a group:
// (self, n3: u8) leaves self as the owner.
NodeId parse_params(Parser *p, int *has_self)
{
    Unit *u = p->unit;
    int pending = -1; // first name still waiting for its type: an index in the list
    int index = 0;

    *has_self = 0;
    if (!parser_consume(p, TOK_LPAREN, "expected '(' to open the parameter list"))
    {
        return 0;
    }
    NodeList params = list_begin();
    p->paren_depth++;

    while (p->current->kind != TOK_RPAREN && p->current->kind != TOK_EOF)
    {
        // Type first: a name cannot be followed by another name, a '.' or start with '*' / '[]'.
        const Token *t = p->current;
        // A literal after the type is a pattern (`u8 fact(u8 0): 1`).
        int type_first =
            (t->kind == TOK_IDENTIFIER && !t[1].nl &&
             (t[1].kind == TOK_IDENTIFIER || t[1].kind == TOK_DOT || t[1].kind == TOK_NUMBER ||
              t[1].kind == TOK_STRING || (t[1].kind == TOK_MINUS && t[2].kind == TOK_NUMBER))) ||
            t->kind == TOK_STAR || (t->kind == TOK_LBRACKET && t[1].kind == TOK_RBRACKET);
        if (type_first)
        {
            if (pending >= 0)
            {
                const Token *wait = TOK(u, list_at(&params, (uint32_t)pending)->tok);
                parser_error_at(p, wait,
                                "parameter '%.*s' needs its type before a type-first parameter",
                                wait->len, TSTART(wait));
                break;
            }
            NodeId type = parse_type(p);
            if (!type)
            {
                break;
            }
            // A pattern: a literal where the name would be. It binds nothing.
            int negative = p->current->kind == TOK_MINUS && p->current[1].kind == TOK_NUMBER;
            const Token *lit = negative ? p->current + 1 : p->current;
            int is_bool = lit->kind == TOK_IDENTIFIER &&
                          ((lit->len == 4 && memcmp(TSTART(lit), "true", 4) == 0) ||
                           (lit->len == 5 && memcmp(TSTART(lit), "false", 5) == 0));
            if (lit->kind == TOK_STRING)
            {
                parser_error_at(p, lit, "a string pattern is not supported yet");
                break;
            }
            if (lit->kind == TOK_NUMBER || is_bool)
            {
                if (negative)
                {
                    parser_advance(p);
                }
                parser_advance(p);
                NodeId param = node_new(u, AST_PARAM, tok_idx(p, lit), type, 0);
                NODE(u, param)->flags |= AST_F_MUT | AST_F_PATTERN;
                NODE(u, param)->aux = (uint16_t)negative;
                node_list_push(u, &params, param);
                index++;
                if (!parser_match(p, TOK_COMMA))
                {
                    break;
                }
                continue;
            }
            const Token *name = p->current;
            if (!parser_consume(p, TOK_IDENTIFIER, "expected a parameter name after its type"))
            {
                break;
            }
            if (is_self_token(TSTART(name), name->len))
            {
                parser_error_at(p, name, "'self' has no type-first form: write `self` or `self: T`");
                break;
            }
            const Token *tt = TOK(u, NODE(u, type)->tok);
            if (NODE(u, type)->kind == AST_TYPE_NAME && !NODE(u, type)->aux && tt->len == name->len &&
                memcmp(TSTART(tt), TSTART(name), name->len) == 0)
            {
                parser_error_at(p, name,
                                "a context parameter is immutable: write `%.*s: %.*s` (type last)",
                                name->len, TSTART(name), name->len, TSTART(name));
                break;
            }
            NodeId param = node_new(u, AST_PARAM, tok_idx(p, name), type, 0);
            NODE(u, param)->flags |= AST_F_MUT;
            node_list_push(u, &params, param);
            index++;
            if (!parser_match(p, TOK_COMMA))
            {
                break;
            }
            continue;
        }

        const Token *name = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a parameter name"))
        {
            break;
        }

        NodeId param = node_new(u, AST_PARAM, tok_idx(p, name), 0, 0);
        int is_self = is_self_token(TSTART(name), name->len);
        node_list_push(u, &params, param);
        int mine = (int)params.count - 1; // the parameter is staged now: reached by index

        if (is_self)
        {
            if (index != 0)
            {
                parser_error_at(p, name, "'self' must be the first parameter");
                break;
            }
            *has_self = 1;
        }

        if (parser_match(p, TOK_COLON))
        {
            NodeId type = parse_type(p);
            if (!type)
            {
                break;
            }
            list_at(&params, (uint32_t)mine)->a = type;
            for (int i = pending; i >= 0 && i < mine; i++)
            {
                list_at(&params, (uint32_t)i)->a = type; // a type applies backwards
            }
            pending = -1;
        }
        else if (!is_self && pending < 0)
        {
            pending = mine;
        }

        index++;
        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }

    p->paren_depth--;
    if (p->panic)
    {
        list_abort(&params);
        return 0;
    }
    if (pending >= 0)
    {
        const Token *name = TOK(u, list_at(&params, (uint32_t)pending)->tok);
        parser_error_at(p, name, "parameter '%.*s' needs a type", name->len, TSTART(name));
        list_abort(&params);
        return 0;
    }
    parser_consume(p, TOK_RPAREN, "expected ')' after the parameters");
    NodeId head = list_finish(u, &params);
    // A parameter named exactly like its (simple) type is a context parameter.
    for (NodeId id = head; id; id = NEXT(u, id))
    {
        AstNode *n = NODE(u, id);
        if (n->a && !(n->flags & AST_F_MUT) && NODE(u, n->a)->kind == AST_TYPE_NAME &&
            !NODE(u, n->a)->aux)
        {
            const Token *name = TOK(u, n->tok), *type = TOK(u, NODE(u, n->a)->tok);
            if (name->len == type->len && memcmp(TSTART(name), TSTART(type), name->len) == 0)
            {
                n->flags |= AST_F_CONTEXT;
            }
        }
    }
    return head;
}

// Everything after the method name. `flags` carries AST_F_MUT for the type-first form, in
// which case `ret` is the type that was already parsed.
static NodeId parse_method(Parser *p, const Token *name, NodeId ret, uint8_t flags)
{
    Unit *u = p->unit;
    int has_self = 0;
    NodeId body = 0;
    int deferred = 0;
    NodeId params = parse_params(p, &has_self);

    if (p->panic)
    {
        return 0;
    }

    if (flags & AST_F_MUT)
    {
        if (parser_match(p, TOK_COLON))
        {
            body = parse_expression(p); // T name(params): expr
            flags |= AST_F_ONELINE;
        }
        else if (is_body_start(p->current))
        {
            body = parse_body(p, &deferred);
        }
        else
        {
            parser_error_at(p, p->current,
                            "expected '{' or ': expression' after the parameters of '%.*s'",
                            name->len, TSTART(name));
        }
    }
    else
    {
        if (parser_match(p, TOK_COLON))
        {
            if ((p->current->nl) || !type_starts_at(p->current))
            {
                parser_error_at(p, p->previous, "expected a return type after ':'");
                return 0;
            }
            ret = parse_type(p);
        }
        else if (!is_body_start(p->current) && !(p->current->nl) && type_starts_at(p->current))
        {
            ret = parse_type(p); // keyword profile: fun name(params) T { ... }
        }

        if (!p->panic && !is_body_start(p->current))
        {
            parser_error_at(p, p->current,
                            "expected '{' to open the body of '%.*s' (a one-line body needs the "
                            "type-first form: `T %.*s(...): expression`)",
                            name->len, TSTART(name), name->len, TSTART(name));
        }
        if (!p->panic)
        {
            body = parse_body(p, &deferred);
        }
    }

    if (p->panic || !body)
    {
        return 0;
    }

    NodeId method = node_new_fn(u, AST_METHOD_DECL, tok_idx(p, name), params, ret, body);
    NODE(u, method)->flags |=
        (uint8_t)(flags | (has_self ? AST_F_SELF : 0) | (deferred ? AST_F_LAZY : 0));
    NODE(u, NODE(u, method)->b)->flags |= deferred ? AST_F_LAZY : 0; // its signature node knows too
    return method;
}

static NodeId parse_initializer(Parser *p)
{
    return parser_match(p, TOK_EQ) ? parse_expression(p) : 0;
}

// `@ n`: where a field (an offset) or a device (an address) is placed. 0 if there is no `@`.
static NodeId parse_place(Parser *p)
{
    if (p->current->kind != TOK_AT || p->current->nl)
    {
        return 0;
    }
    parser_advance(p);
    if (p->current->kind != TOK_NUMBER || p->current->nl)
    {
        parser_error_at(p, p->previous, "expected a number after '@': an offset or an address");
        return 0;
    }
    NodeId n = node_new(p->unit, AST_NUMBER_LIT, tok_idx(p, p->current), 0, 0);
    parser_advance(p);
    return n;
}

// name: T    a, b: T    name: struct { ... }
// The names go straight onto `into`: a grouped declaration is several elements of that list.
static int parse_type_last(Parser *p, NodeList *into)
{
    Unit *u = p->unit;
    uint32_t first = into->count;
    int count = 0;

    do
    {
        const Token *name = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a name"))
        {
            list_truncate(into, first);
            return 0;
        }
        node_list_push(u, into, node_new(u, AST_FIELD_DECL, tok_idx(p, name), 0, 0));
        count++;
    } while (parser_match(p, TOK_COMMA));

    if (!parser_consume(p, TOK_COLON, "expected ':' and a type after the name"))
    {
        list_truncate(into, first);
        return 0;
    }
    if ((p->current->nl) || !type_starts_at(p->current))
    {
        parser_error_at(p, p->previous,
                        "expected a type after ':' on the same line (there are no `pub:` / "
                        "`mut:` sections: mutability is `T name`, exposure is declared in the "
                        "domain's mod.modal)");
        list_truncate(into, first);
        return 0;
    }

    if (count == 1 && p->current->kind == TOK_STRUCT)
    {
        parser_advance(p);
        NodeId members = parse_struct_body(p);
        if (p->panic)
        {
            list_truncate(into, first);
            return 0;
        }
        AstNode *decl = list_at(into, first);
        decl->kind = AST_STRUCT_DECL;
        decl->a = members;
        return 1;
    }

    NodeId type = parse_type(p);
    if (!type)
    {
        list_truncate(into, first);
        return 0;
    }
    NodeId place = parse_place(p);
    if (place && count > 1)
    {
        parser_error_at(p, p->previous, "a placed field has one name: `name: T @ offset`");
        list_truncate(into, first);
        return 0;
    }
    NodeId init = place ? place : parse_initializer(p);
    if (p->panic)
    {
        list_truncate(into, first);
        return 0;
    }
    // A grouped declaration shares one type (and initializer) node between its names.
    for (uint32_t i = first; i < into->count; i++)
    {
        list_at(into, i)->a = type;
        list_at(into, i)->b = init;
        list_at(into, i)->flags |= place ? AST_F_PLACED : 0;
    }
    return 1;
}

// T name    T name(params) ...
static NodeId parse_type_first(Parser *p)
{
    Unit *u = p->unit;
    const Token *start = p->current;
    NodeId type = parse_type(p);

    if (!type)
    {
        return 0;
    }
    if (p->current->kind != TOK_IDENTIFIER || (p->current->nl))
    {
        parser_error_at(p, start,
                        "expected a declaration: `name: T`, `T name`, `name(...): T { }`, "
                        "`T name(...) { }` or `name := value`");
        return 0;
    }

    const Token *name = p->current;
    parser_advance(p);

    if (p->current->kind == TOK_LPAREN && !(p->current->nl))
    {
        return parse_method(p, name, type, AST_F_MUT);
    }

    NodeId place = parse_place(p);
    NodeId init = place ? place : parse_initializer(p);
    if (p->panic)
    {
        return 0;
    }
    NodeId field = node_new(u, AST_FIELD_DECL, tok_idx(p, name), type, init);
    NODE(u, field)->flags |= AST_F_MUT | (place ? AST_F_PLACED : 0);
    return field;
}

// name := value
NodeId parse_define(Parser *p)
{
    const Token *name = p->current;
    parser_advance(p); // name
    parser_advance(p); // :=
    NodeId value = parse_expression(p);
    if (!value)
    {
        return 0;
    }
    return node_new(p->unit, AST_DEFINE, tok_idx(p, name), value, 0);
}

// items := item (',' item)*      item := IDENT [ 'as' IDENT ]
//
// Called just after the '(' token `open`; stops at the closing ')' without consuming it.
// A malformed item is reported and skipped on its own: the scanner already paired the
// parentheses, so the parser can always find the next ',' or the end of this list and carry
// on, instead of losing the rest of the file.
NodeId parse_import_items(Parser *p, const Token *open)
{
    Unit *u = p->unit;

    if (p->current->kind == TOK_RPAREN)
    {
        parser_error_at(p, open, "the import list is empty");
        p->panic = 0;
        return 0;
    }
    NodeList items = list_begin();

    while (p->current->kind != TOK_RPAREN && p->current->kind != TOK_EOF)
    {
        const Token *name = p->current;
        NodeId item = 0;

        if (name->kind == TOK_IDENTIFIER)
        {
            parser_advance(p);
            item = node_new(u, AST_IMPORT_ITEM, tok_idx(p, name), 0, 0);
            if (parser_match(p, TOK_AS))
            {
                if (p->current->kind == TOK_IDENTIFIER)
                {
                    NODE(u, item)->b = tok_idx(p, p->current); // the alias token (not staged yet)
                    parser_advance(p);
                }
                else
                {
                    parser_error_at(p, p->current, "expected an alias name after 'as'");
                }
            }
        }
        else
        {
            parser_error_at(p, name, "expected a name in the import list");
        }

        if (!p->panic && p->current->kind != TOK_COMMA && p->current->kind != TOK_RPAREN)
        {
            parser_error_at(p, p->current, "expected ',' or ')' in the import list");
        }

        if (p->panic)
        {
            while (p->current->kind != TOK_EOF && p->current->kind != TOK_COMMA &&
                   p->current->kind != TOK_RPAREN && !(open->match == 0 && (p->current->nl)))
            {
                parser_skip_one(p);
            }
            p->panic = 0;
        }
        else
        {
            node_list_push(u, &items, item);
        }

        if (!parser_match(p, TOK_COMMA))
        {
            break;
        }
    }
    return list_finish(u, &items);
}

// val (a as b, c): import(x).y
static NodeId parse_val_decl(Parser *p)
{
    Unit *u = p->unit;
    const Token *val = p->current;
    parser_advance(p);

    const Token *open = p->current;
    if (!parser_consume(p, TOK_LPAREN, "expected '(' after 'val'"))
    {
        return 0;
    }
    p->paren_depth++;
    NodeId items = parse_import_items(p, open);
    p->paren_depth--;

    if (!parser_consume(p, TOK_RPAREN, "expected ')' to close the val list") ||
        !parser_consume(p, TOK_COLON, "expected ': import(...)' after the val list"))
    {
        return 0;
    }

    NodeId source = parse_expression(p);
    if (!source)
    {
        return 0;
    }
    if (!ast_import_root(u, source))
    {
        parser_error_at(p, val, "val (...) destructures an import: `val (a as b): import(x).y`");
        return 0;
    }
    return node_new(u, AST_VAL_DECL, tok_idx(p, val), items, source);
}

// '{' member* '}' with ',', ';' or a newline between members.
static NodeId parse_struct_body(Parser *p)
{
    Unit *u = p->unit;

    if (!parser_consume(p, TOK_LBRACE, "expected '{' to open the struct body") || !parser_enter(p))
    {
        return 0;
    }
    NodeList members = list_begin();
    int saved_paren_depth = p->paren_depth;
    p->paren_depth = 0;

    while (p->current->kind != TOK_RBRACE && p->current->kind != TOK_EOF)
    {
        if (parser_match(p, TOK_COMMA) || parser_match(p, TOK_SEMICOLON))
        {
            continue;
        }

        parse_declaration(p, DECL_STRUCT, &members);
        if (p->panic)
        {
            parser_synchronize(p);
            continue;
        }

        TokenKind k = p->current->kind;
        if (k != TOK_RBRACE && k != TOK_COMMA && k != TOK_SEMICOLON && k != TOK_EOF &&
            !(p->current->nl))
        {
            parser_error_at(p, p->current, "expected ',' or a newline between struct members");
            parser_synchronize(p);
        }
    }

    p->paren_depth = saved_paren_depth;
    parser_leave(p);
    parser_consume(p, TOK_RBRACE, "expected '}' to close the struct");
    return list_finish(u, &members);
}

static NodeId parse_declaration_one(Parser *p, DeclContext ctx);

int parse_declaration(Parser *p, DeclContext ctx, NodeList *into)
{
    const Token *t = p->current;
    if (t->kind == TOK_IDENTIFIER && (t[1].kind == TOK_COLON || t[1].kind == TOK_COMMA))
    {
        return parse_type_last(p, into);
    }
    NodeId decl = parse_declaration_one(p, ctx);
    if (p->panic || !decl)
    {
        return 0;
    }
    node_list_push(p->unit, into, decl);
    return 1;
}

static NodeId parse_declaration_one(Parser *p, DeclContext ctx)
{
    const Token *t = p->current;

    switch (t->kind)
    {
    case TOK_VAL:
    case TOK_IMPORT:
    case TOK_WRITE:
        if (ctx != DECL_TOP)
        {
            parser_error_at(p, t, "only fields, methods and bindings can appear in a struct");
            return 0;
        }
        return t->kind == TOK_VAL ? parse_val_decl(p) : parse_expression(p);

    case TOK_FUNCTION:
        parser_advance(p);
        t = p->current;
        if (!parser_consume(p, TOK_IDENTIFIER, "expected a function name after 'fun'"))
        {
            return 0;
        }
        return parse_method(p, t, 0, 0);

    case TOK_IDENTIFIER:
        switch (t[1].kind)
        {
        case TOK_DEFINE:
            return parse_define(p);
        case TOK_COLON:
        case TOK_COMMA:
            return 0; // handled by parse_declaration
        case TOK_LPAREN:
            if (!(t[1].nl))
            {
                parser_advance(p);
                return parse_method(p, t, 0, 0);
            }
            break;
        default:
            break;
        }
        return parse_type_first(p);

    case TOK_LBRACKET:
    case TOK_STAR:
        return parse_type_first(p);

    default:
        parser_error_at(p, t,
                        "expected a declaration: `name: T`, `T name`, `name(...): T { }`, "
                        "`T name(...) { }` or `name := value`");
        return 0;
    }
}
