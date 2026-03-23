# Roadmap
- [] Finish arena allocators
- [] Correction in tokenizer lib
- [] Fix ast problems

# Features roadmap
- Interop: C/C++/(OBJ C/C++)/RUST even ZIG
- Construção pensada em longo prazo.
- Logs decente

**Design**
- Explicit
- Drop/cleaning design, with switchable  memory features: (borrow checker, garbage collector or manual control)

**Decisions to take**
- Packages manager or libraries?
- Buscar constancia em changes (evitar breaking changes), isso traz o interesse de usuários novos

**Diferenciais (talves não sei)**
- Complete standard library
- Few reserved keywords
- Definições parametrizadas (isso é top no zig)
- metaprogramming/macros without sacrificing DX (Developer experience)
- GEP -> General Ecosystem Platform (our core idea)

### How to build
```bash
make
```
