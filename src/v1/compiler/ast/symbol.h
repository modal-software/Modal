typedef enum
{
    SYM_VAR
} SymKind;

typedef struct Sym
{
    char *name;
    SymKind kind;
    struct Sym *next;
} Sym;
