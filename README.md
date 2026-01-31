# Functionalities roadmap
[] - Fix ast problems
[] - Finish utilities allocators

# Features roadmap
- Interoperabilidade com C/C++.
- Design de uso que evita alocações.
- Design explicito, mas produtividade acima de explicitude.
- Performance (não tem jeito).
- Compilador decente.
- Construção pensada em longo prazo.
- Logs decente
- Gerenciador de pacotes ou bibliotecas inseridas (variante de header)
\0
\0
**Além disso**
- Buscar constancia em changes (evitar breaking changes), isso traz o interesse de usuários novos
\0
\0
**Diferenciais (talves não sei)**
- Baixo acoplamento em quaisquer funcionalidade
- Definições parametrizadas (isso é top no zig)
Macros,
- Estrutura de dados: Structs, traits/interfaces, types ou typedef
- Compile-time checking, algo como templates ou comptime do zig
- Metaprogamação interessante
- Foco inicial em low-level (expansão prevista)

### Build cmd
```bash

gcc -std=c17 -Wall -Wextra \                                                                                                                                                 
                                   main.c \
                                   parser.c \
                                   tokenizer/tokenizer.c \
                                   -I. \
                                   -o lang

```
