# Reference

**X** itself are a reliable compiled language .. insert more text lately.

# Variables

We need to implement variables to the language.
Before implementing that these values to have variable

```c
struct {
    uint32_t _id;
    uint8_t mutable;
    char *name;
    char *type;
} var_decl;

struct {
    Symbol *symbol;
    char *name;
    char *diagnostics;
} var_ref;
```

Why we need to storage distinct values for pure value and reference value?
The first **struct** are the variable itself, and the second **struct** are
the value of that variable
