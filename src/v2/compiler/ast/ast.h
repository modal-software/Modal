// ast.h (v2): compact, index-linked AST.
//
// A node is 24 bytes and lives in one flat array per file. Children are referenced by index
// (NodeId), never by pointer, so the array can sit in the arena and the tree stays valid no
// matter where it is stored. Lists (statements, members, params, args) are chained through
// `next`. Source text is reached through `tok`, an index into the unit's token stream.
#ifndef MODAL_V2_AST_H
#define MODAL_V2_AST_H

#include <stddef.h>
#include <stdint.h>

typedef struct Unit Unit;
typedef uint32_t NodeId; // 0 = no node

typedef enum
{
    AST_NONE,
    //                  tok          a            b            c
    AST_MODULE,      // -            decls
    AST_STRUCT_DECL, // name         members
    AST_FIELD_DECL,  // name         type         init
    AST_METHOD_DECL, // name         params       return type  body
    AST_PARAM,       // name         type
    AST_DEFINE,      // name         value                               name := value
    AST_VAL_DECL,    // 'val'        items        source                 val (a as b): import(x)
    AST_TYPE_NAME,   // first ident  (aux = number of extra `.segment`s)
    AST_TYPE_SLICE,  // '['          element
    AST_TYPE_PTR,    // '*'          pointee
    AST_TYPE_STRUCT, // 'struct'     members
    AST_FN_LIT,      // '('          params       return type  body
    AST_BLOCK,       // '{'          statements
    AST_RETURN,      // 'return'     value
    AST_ASSIGN,      // '='          target       value
    AST_BIN_OP,      // operator     left         right
    AST_UNARY_OP,    // operator     operand
    AST_CALL,        // '(' / string callee       args
    AST_INDEX,       // '['          target       index
    AST_SELECTOR,    // member name  target
    AST_PAREN_GROUP, // '('          expr
    AST_IDENT,       // name
    AST_NUMBER_LIT,  // literal
    AST_STRING_LIT,  // literal
    AST_IMPORT,      // module arg   (aux = target unit id + 1, 0 = external; set by the linker)
    AST_IMPORT_ALL,  // '*'          chain                               import(x).*
    AST_IMPORT_PICK, // '('          chain        items                  import(x).(a, b as c)
    AST_IMPORT_ITEM, // name                                   alias TOKEN index (0 = none)
    AST__COUNT,
} AstNodeKind;

enum
{
    AST_F_MUT = 1u << 0,     // type-first declaration: mutable field / overloadable method
    AST_F_ONELINE = 1u << 1, // method body is `: expression`
    AST_F_SELF = 1u << 2,    // callable whose first parameter is `self`
};

typedef struct AstNode
{
    uint8_t kind;
    uint8_t flags;
    uint16_t aux;
    uint32_t tok;
    NodeId a, b, c;
    NodeId next; // next sibling in the list this node belongs to
} AstNode;

_Static_assert(sizeof(AstNode) == 24, "AstNode must stay 24 bytes");

typedef struct
{
    AstNode *data;
    size_t size;
    size_t cap;
} NodeVec;

#define NODE(u, id) (&(u)->nodes.data[(id)])
#define TOK(u, i) (&(u)->tokens.data[(i)])

// Which of a/b/c hold child nodes, per kind (bit 0 = a, bit 1 = b, bit 2 = c).
extern const uint8_t ast_child_mask[AST__COUNT];

typedef struct
{
    NodeId (*node)(Unit *u, AstNodeKind kind, uint32_t tok, NodeId a, NodeId b, NodeId c);
} AstConstructor;

typedef struct
{
    AstConstructor new;
    void (*print)(Unit *u);
    const char *(*kind_name)(AstNodeKind kind);
    // Renders a type node as source-like text into buf; returns buf.
    char *(*type_str)(const Unit *u, NodeId type, char *buf, size_t cap);
    // Renders the signature of a method declaration or function binding; returns buf.
    char *(*sig_str)(const Unit *u, NodeId decl, char *buf, size_t cap);
} AstImpl;

extern const AstImpl ast;

// If `expr` is an import chain (import(x), import(x).a.b, import(x).*, import(x).(a, b)),
// returns its AST_IMPORT node; otherwise 0.
NodeId ast_import_root(const Unit *u, NodeId expr);

static inline int is_self_token(const char *s, int len)
{
    return len == 4 && s[0] == 's' && s[1] == 'e' && s[2] == 'l' && s[3] == 'f';
}

#endif // MODAL_V2_AST_H
