# Desenvolvimento no Modal: roteiro para quem programa em C

Este guia liga aprendizado a tarefas concretas no compilador. Serve para desenvolvedores C
intermediários e avançados; complementa [development.md](development.md), sem substituir suas
regras. Referência desta revisão: árvore de trabalho de 15/11/2026, incluindo alterações locais.
Os exercícios abaixo são propostas de estudo, não funcionalidades prometidas ou já implementadas.

**Como navegar:** §§1–9 cobrem contribuição e tarefas; §§10–15 aprofundam C, front-end,
análises, MIR e ABI; §§16–19 tratam ELF, linking, código de máquina e carregamento;
§§20–24 cobrem sistemas, desempenho, depuração, projetos e vocabulário.
§§25–30 aprofundam robustez, contratos, limites, oráculos de teste, reprodutibilidade e revisão.

Atalhos para o estudo aprofundado:

- [C, memória e build](#11-c-de-sistemas-o-que-dominar-e-como-praticar-aqui).
- [Front-end](#12-front-end-transformar-bytes-em-um-programa-compreendido),
  [análises](#13-grafos-e-análises-aprender-a-provar-fatos-conservadores) e
  [MIR](#14-mir-e-otimização-transformar-sem-mudar-significado).
- [ABI e stack](#15-isa-abi-e-stack-três-contratos-diferentes).
- [ELF](#16-elf-aprender-a-emitir-e-interpretar-o-arquivo) e
  [linking/relocação](#17-linking-e-relocação-onde-referências-viram-endereços).
- [Emissão de instruções](#18-emissão-de-código-de-máquina-e-copy-and-patch) e
  [loading/Mach-O](#19-loading-execução-em-memória-e-mach-o).
- [Depuração](#22-técnicas-de-desenvolvimento-depuração-e-investigação),
  [projetos](#23-projetos-de-aprendizado-com-entregas-verificáveis) e
  [glossário](#24-glossário-de-pesquisa-e-materiais-de-aprofundamento).

## 1. Como ler a documentação sem implementar uma decisão antiga

| Documento | Como usar |
|---|---|
| [AGENTS.md](../AGENTS.md) | Instruções do repositório; leia antes de editar. |
| [README.md](../README.md) | Objetivos gerais e roadmap inicial, não inventário atualizado. |
| [development.md](development.md) | Regras de projeto, pipeline, ferramentas e critérios de entrega. |
| [guide.md](guide.md) | Introdução à linguagem; confirme exemplos e limitações no código atual. |
| [context.md](context.md) | Histórico das decisões e implementações; procure também as seções posteriores sobre o assunto. |
| [p.md](p.md) | Plano de desempenho, metodologia e medições históricas. |
| [comptime.md](comptime.md) | Avaliação durante a compilação, especialização e seus limites. |
| [guards.md](guards.md) | Falhas e recuperação; §5 substitui o desenho antigo de `fault`. |
| [std.md](std.md) | Fronteira entre primitivas e biblioteca, arrays, reflexão, padrões e pipes. |
| [v2.md](v2.md) | Material histórico com código de uma arquitetura anterior. Não copie como implementação atual. |
| [implementations.md](implementations.md), [journal.md](journal.md), [resolver.md](resolver.md) | Esboços e registros antigos, incluindo pseudocódigo em Go. |

O apêndice B de `context.md` contém um snapshot de fontes, não os arquivos atuais. Há conflitos
concretos: o início do histórico descreve visibilidade por “face”, substituída por uso (§37);
o guia ainda lista algumas lacunas fechadas depois, como structs aninhadas (§43), capturas (§63)
e resultados de arrays (§64). `make test_v2` aparece em textos antigos; o alvo atual é `make test`.
`pdf.md`, na raiz, registra uma conversa sobre proposta comercial e não orienta o compilador.

Para descobrir o comportamento atual, confronte decisão mais recente, fonte e teste executável.
Se eles divergirem, registre a divergência; não transforme automaticamente um bug em regra.

## 2. Preparar o ambiente e estabelecer uma referência

Use ferramentas C17 e `make`. O makefile define `CC = gcc`; `make CC=clang` permite escolher
explicitamente. O backend C usa `$CC` do ambiente ou sua toolchain configurada para compilar o
programa gerado. Python 3 é necessário para fuzzing; clang, para sanitizers e regenerar stencils.
Não há instalação via npm ou Cargo.

```sh
git status --short
git branch --show-current
make
./modal --backends examples/run/hello.modal
./modal examples/run/hello.modal
./modal --run examples/run/hello.modal
./modal --backend native --run examples/run/hello.modal
make test
```

Registre falhas antes de alterar código. `development.md` §4 e `context.md` §64 registram
falhas antigas e exemplos em edição; isso não prova que uma falha atual seja preexistente.
Compare a mesma árvore antes/depois e não ajuste `.expected` só para fazer o teste passar.

O build ativo lista fontes de `src/v2/` manualmente em `SRCS`. `src/v1/` está congelado segundo
`development.md`. O projeto já tem caminhos arm64 e x86-64: não trate x86-64 como único alvo.
Os scripts pulam certas execuções incompatíveis com o host; registre quais alvos realmente rodaram.
No macOS arm64, alguns caminhos x86-64 dependem de Rosetta. O benchmark usa ferramentas de macOS,
como `/usr/bin/time -l`; confirme a portabilidade antes de interpretar resultados em Linux.

## 3. Mapa do compilador e do que aprender em cada parte

Os caminhos da tabela são relativos a `src/v2/`, exceto `std/` e `examples/`.

| Área | Onde começar | Conhecimento C exercitado |
|---|---|---|
| Driver | `main.c` | Arquivos, processos, flags, tratamento de falhas e cache de executáveis. |
| Contexto e memória | `compiler/cx.h`, `cx.c` | Arenas, regiões, alinhamento, capacidade e vida útil de ponteiros. |
| Scanner | `compiler/syntax/scanner.c`, `scanner.h` | Varredura de bytes, limites, interning e representação compacta. |
| Parser | `compiler/syntax/parser/parse_{decl,expr,stmt}.c` | Descida recursiva, Pratt, precedência e recuperação de erros. |
| AST e símbolos | `compiler/ast/` | Nós por índice, tabelas hash, escopos e identidade de nomes. |
| Semântica | `compiler/sema/{resolve,module,check,types,layout}.c` | Resolução, tipos internados, grafos e layout do alvo. |
| Comptime e cache | `compiler/sema/{comptime,cache}.c` | Interpretador, pilha explícita, determinismo e invalidação. |
| MIR | `compiler/mir/{mir.h,lower.c,opt.c}` | Instruções tipadas, fluxo, aliasing, cópias e análise de valores vivos. |
| Backend C | `compiler/backend/c/emit.c` | Geração de C, comportamento indefinido e convenções de chamada. |
| Backend nativo | `compiler/backend/native/` | ABI, registradores, instruções, relocação, Mach-O e ELF. |
| Biblioteca | `std/*.modal` | Abstrações construídas sobre primitivas; especialização sem indireção. |
| Validação | `tests/run.sh`, `tools/`, `examples/run/`, `examples/errors/` | Regressão, testes diferenciais e medição reproduzível. |

Fluxo principal: scan → declarações → resolução/imports → checagem dos corpos → geração.
O backend nativo passa por MIR e otimização; o backend C tem emissão própria a partir da árvore
checada. Portanto, alterar MIR não implementa automaticamente um recurso no backend C.

Os corpos podem ser adiados como `TOK_BODY`, processados e descartados individualmente.
O checker também organiza dependências para visitar callees antes de callers; não suponha que
todos os corpos continuem disponíveis ou que sejam sempre processados na ordem textual.

## 4. Trilha intermediária: aprender fazendo mudanças pequenas

Avance por entregas verificáveis, sem prazo fixo.

1. **Rastrear um programa.** Leia `examples/run/hello.modal`, depois `counter.modal` e
   `arrays.modal`. Use `--ast`, `--syms` e `--sig`; gere MIR com o backend nativo. Entrega:
   explicar como um nome vira símbolo, tipo e operação gerada.
2. **Entender erros.** Escolha um diagnóstico em `examples/errors/`, localize sua emissão com
   `rg` e reduza o exemplo. Entrega: um caso inválido e um vizinho válido, explicando a diferença.
3. **Corrigir uma regressão delimitada.** Priorize diagnóstico, checagem ou um caso de parsing
   cuja regra já foi decidida. Entrega: teste que falha antes e passa depois, sem alterar a linguagem.
4. **Entender memória.** Siga uma alocação desde `cx.c` até o descarte/reset da região.
   Entrega: tabela com proprietário, validade e eventos que invalidam cada referência.
5. **Comparar execução.** Rode um programa com C, nativo e registradores desativados.
   Entrega: mesmos bytes e status; explique por que igualdade de saída não prova ausência de UB.
6. **Contribuir na std.** Estude `std/array.modal` e `examples/run/map_fold.modal`.
   Entrega: pequena função expressável em Modal, com teste e custo observado.

Antes de avançar para backend, domine aritmética com sinal/sem sinal, promoções, overflow,
alinhamento, operações de bits, `memcpy` versus `memmove`, tempo de vida e ponteiros inválidos.
Use o código para investigar esses temas; o objetivo não é decorar todos os arquivos.

## 5. Trilha avançada: escolher um subsistema e demonstrar suas invariantes

| Projeto de estudo | Passos | Evidência de conclusão |
|---|---|---|
| Otimização MIR | Entender fluxo e efeitos; construir contraexemplo; implementar uma transformação local. | Equivalência entre backends, fuzzing e benchmark antes/depois. |
| Alocação de registradores | Ler `regalloc.c`; seguir intervalos, chamadas e spills; comparar `MODAL_REGS=0` e `2`. | Casos com pressão e valores vivos através de chamadas. |
| Comptime | Seguir um inicializador até `CtFrame`; testar orçamento e retorno de erro. | Mesmo valor em compile time/runtime, sem ponteiro do compilador no resultado. |
| ABI de agregados | Rastrear struct/array como argumento e resultado; conferir slots e destino do resultado. | Cópia correta, sobreposição, resultado no caller e chamadas em cauda preservadas. |
| Executáveis | Seguir fixups até `macho.c`/`elf.c`; inspecionar cabeçalhos e carregamento. | Executável carregável no alvo; emissão e execução em memória equivalentes. |
| Cache | Identificar entradas e dependências; mudar corpo, declaração, import e configuração separadamente. | Rebuild frio/quente correto, sem usar resultados antigos indevidamente. |

Não comece por reescrever o compilador ou paralelizá-lo: existem estados globais, memória
temporária compartilhada e uma pipeline deliberadamente econômica. Primeiro documente os
estados que precisariam de isolamento e meça o gargalo que justificaria a mudança.

## 6. Receitas: “quero fazer X; por onde começo?”

### Corrigir um bug ou um crash

1. Reduza a entrada a um `.modal`; anote comando, host, compilador C e comportamento esperado.
2. Separe falha de scan/parse, semântica, lowering, otimização, emissão ou execução.
3. Compare `--eager` com o modo padrão quando houver suspeita de corpos adiados; compare C e nativo.
4. Use ASan/UBSan para memória/UB; preserve o programa e a seed se veio do fuzzer.
5. Adicione regressão no runner e corrija a causa, não apenas a mensagem final.

### Alterar sintaxe, operador ou padrão

Escreva primeiro exemplos válidos, ambiguidades, precedência, erros e custo. Confira as regras
de `development.md` §2: evitar novas keywords, respeitar posição e justificar primitivas.
Depois siga scanner → parser → AST → checker → comptime/lowering → ambos os caminhos de geração.
Não acrescente token se a sintaxe puder usar a representação existente.

Para padrões, investigue `Pattern`, `sema_pattern_of`, `sema_arm_pattern`, cobertura (`Cover`),
`pattern_test` e `emit_pattern_cond`. Teste sobreposição, exaustividade, `_`, condições e
efeitos colaterais. Uma condição posterior em `a[k].(...)` não protege o acesso `a[k]` anterior.

### Alterar tipos, conversões, structs ou arrays

Comece em `sema/types.*`, `check.c` e `layout.c`; depois acompanhe largura e signedness no MIR,
interpretador e emissores. Layout deve consultar `MachineTarget`, não `sizeof` do host.
Não suponha que a ordem declarada seja o layout físico: há reorganização e eliminação de campos.

Teste zero inicial, limites, cópia versus referência (`x` versus `&x`), aninhamento, aliasing,
agregados como argumento/resultado e frames grandes. `[]u8` continua sendo string; parâmetros
`[]T` de outros elementos têm especialização por comprimento (§64), não slices gerais prontos.
`MIR_MOVE` permite sobreposição: uma transformação para `memcpy` precisa provar que não há alias.

### Implementar algo em compile time ou adicionar genéricos

Leia `comptime.md` e `sema/comptime.c`, além das especializações no checker. Determine quais
entradas são conhecidas na compilação e como identificam uma especialização reutilizável.
Preserve a pilha explícita `CtFrame`, limites de passos/profundidade e layout do target.
Teste recursão, orçamento excedido, falhas e igualdade com runtime. Uma avaliação de módulo pode
imprimir no log da compilação; uma otimização não pode antecipar esse efeito de uma chamada runtime.

### Mudar erros, panics ou rollback de arena

Use `guards.md` §5 e `context.md` §§54–59, não o desenho antigo de recuperação. Acompanhe
`, error`, `n, e := f()`, propagação, conjuntos inferidos e fronteiras de captura no checker,
MIR, interpretador e C/nativo. Teste sucesso, erro, panic, fronteiras aninhadas, valor zerado
na falha e ausência de fronteira quando comprovadamente desnecessária.
Rollback exige justificar que nada escapou; confira `arena_decision` e `--audit`.

### Otimizar compilação ou código gerado

Defina uma hipótese e um programa que a isole. Use `--time --repeat` para custo de compilação,
`--stats` para memória/tamanhos e `make bench` para a tabela padronizada. Compare o mesmo host,
flags, entradas, número de repetições e estado de cache. Meça também um caso grande.
Separe tempo de compilação, inicialização do processo e execução do programa.

Em `opt.c`, prove que efeitos, aliases, branches e acessos de dispositivo são preservados.
Use `MIR_ENDS`, `MIR_BRANCHES` e `MIR_NO_FALLTHROUGH`; um `MIR_TRAP_PART` não encerra bloco.
Um ganho de tamanho não compensa automaticamente perda de velocidade: explique o tradeoff.
Para exercitar runtime, evite que todos os argumentos sejam constantes e a chamada desapareça.

### Alterar backend, runtime ou stencils

Leia `backend/backend.h`, `registry.c`, `native/emit.c`, `regalloc.c` e o arquivo `arch_*`
correspondente. Determine convenção de chamada, registradores preservados, alinhamento da
pilha, largura dos offsets, endianness e relocação antes de codificar instruções.
Depois de alterar `stencils.c`, rode `make stencils` e revise os dois headers gerados;
não edite apenas os headers. Preserve x18 no arm64 conforme a configuração de geração.

Compare `--run` e executável com `-o`, modo com/sem registradores, arm64/x86-64 e ELF/Mach-O
quando executáveis no ambiente. Uma nova arquitetura exige também descrição de alvo, layout,
capabilities, writer/runtime e registro; uma linha no registry não basta.
Código AVX2 precisa de dispatch em runtime; não habilite `-mavx2` globalmente.

### Acrescentar std, import ou configuração

Prefira `.modal` em `std/`; confira os bindings de `std/mod.modal`. Uma nova primitiva precisa
explicar o que a própria linguagem não consegue expressar. Para imports/configuração, siga
`sema/module.c`: teste caminhos relativos, aliases, outro domínio, `mod.modal`, tipo de valor
e configuração inválida. Se envolver cache, teste invalidação também.
Ao mover/copiar o executável, ajuste `MODAL_STD` para a pasta da biblioteca.

### Acrescentar um arquivo C ou um diagnóstico

Inclua nova fonte C em `SRCS` do `makefile`; mantenha headers e includes no padrão de v2.
Para diagnóstico, reporte o local correto e uma correção acionável. Informações que sobrevivem
à checagem de um corpo devem guardar offsets de fonte/dados persistentes, não tokens descartáveis.
Teste que o inválido é recusado e o válido próximo continua aceito.

## 7. Regras de C e memória que não podem ficar implícitas

- **Estilo:** `.clang-format` define quatro espaços, sem tabs, chaves Allman e limite de 100
  colunas. `.clang-tidy` configura análise e nomes: funções/variáveis em `lower_case`, structs
  e enums em `CamelCase`. Siga também o contexto local. Formate só arquivos alterados.
- **Ferramentas:** `clang-format --dry-run --Werror caminho.c` verifica formatação;
  `clang-tidy caminho.c -- -std=c17 -I src` é um ponto de partida para análise individual.
  Não há alvo dedicado no makefile para essas verificações. `compile_flags.txt` ainda inclui v1.
- **Validade:** algumas regiões começam compartilhadas e migram ao crescer. Não retenha bases,
  limites ou ponteiros derivados sem conferir a política daquela região.
- **Persistência:** arena não libera alocações individualmente. Uma tabela pequena por símbolo
  multiplica seu custo por dezenas de milhares de funções; estime e depois meça.
- **Reentrância do lowering:** `REGION_MIR` é reutilizada por função. Uma avaliação aninhada deve
  preservar/restaurar estado com `mir_nest_begin` / `mir_nest_end`.
- **Aritmética:** wrapping da linguagem precisa continuar definido no C gerado. Teste divisão
  por zero, mínimo com sinal dividido por −1, narrowing e extensões de sinal.
- **Tail calls:** loops usam recursão em cauda. Transformar jump em call pode estourar a pilha;
  teste loops longos, agregados e referências ao frame. Não dependa só de otimização opcional do C.
- **Emissão:** `PUT(e, "texto")` exige literal por usar `sizeof`; strings calculadas usam `put()`.
  Preserve o namespace dos helpers para não colidir com nomes do programa.

## 8. Como testar e decidir que terminou

O runner é um script shell, não um framework de testes C com meta de cobertura numérica.
Os testes de execução usam listas explícitas: criar `x.modal` e `x.expected` não registra o
caso sozinho. Procure as listas correspondentes em `src/v2/tests/run.sh` e acrescente o nome
nos caminhos necessários. Para erros, adicione `expect_fail`/`expect_output`, incluindo local
e mensagem; mantenha erros semânticos e falhas de avaliação em arquivos separados quando
um impedir que o outro estágio rode. Para programas que falham, confira também o exit status.

```sh
make test
make backends F=examples/run/arrays.modal
make fuzz N=200 SEED=7
FUZZ_KIND=struct make fuzz N=200 SEED=7
FUZZ_KIND=pressure make fuzz N=200 SEED=7
MODAL_REGS=0 make fuzz N=200 SEED=7
MODAL_REGS=2 make fuzz N=200 SEED=7
make asan
BIN=./modal-asan make test
BIN=./modal-asan make fuzz N=200 SEED=7
make bench N=10
```

Escolha os geradores que cobrem a mudança; amplie o fuzzer quando o recurso não estiver coberto.
`make backends` imprime hashes/status para comparar, mas não é uma asserção automática de
igualdade. Inspecione as linhas e os arquivos de saída indicados pelo script.
Sanitizar o compilador não sanitiza automaticamente o programa C gerado: se o problema for
nesse programa, gere C e investigue também sua compilação/execução com sanitizers.

Antes de entregar código, cumpra os critérios de `development.md` §6: regressões cobertas,
equivalência nos alvos exercitados, fuzzing sem divergências, sanitizers e medidas pertinentes.
Declare falhas preexistentes com evidência e alvos não executados; não use contagens históricas
de testes como resultado desta sessão. Uma mudança apenas documental pede revisão de caminhos,
comandos e coerência, sem apresentar testes do compilador como se tivessem sido executados.

## 9. Escolher a próxima contribuição e documentar a entrega

Para uma primeira contribuição, escolha reprodução e diagnóstico de um bug delimitado. Para
um projeto intermediário, acompanhe um recurso existente de ponta a ponta antes de ampliá-lo.
Para trabalho avançado, investigue uma pendência documentada com experimento isolado: proteção
de stack, lacunas de arrays/slices, saída em lote ou invalidação de cache. Confirme que a
pendência ainda existe no código antes de propor implementação e discuta mudanças de linguagem.

Faça a mudança em branch de feature, preservando trabalho local alheio. O histórico usa
mensagens como `feat: ...` e `fix: ...`; siga isso quando houver pedido de commit.
`development.md` determina commit/push somente a pedido do responsável e uma linha de atribuição
quando fornecida na sessão. Não invente atribuições nem inclua binários, `.dSYM`, `.DS_Store`
ou arquivos temporários.

A entrega deve explicar problema, comportamento resultante, arquivos relevantes, validação,
medições e limites. Para uma fase implementada, siga `development.md` §7: registre mecanismo,
resultados medidos, verificações e pendências em `context.md`; atualize o plano da feature,
`p.md` quando aplicável e exemplos/guia afetados. Diferencie resultado observado de proposta.

## 10. Currículo técnico: ordem, pré-requisitos e método

Não é necessário dominar toda a lista antes de contribuir. Para cada tema, complete este ciclo:
**conceito → localização no código → previsão → experimento → explicação do resultado**.
Escreva a previsão antes de executar: isso distingue entendimento de apenas reconhecer a saída.

| Etapa | Pré-requisito | O que estudar | O que produzir |
|---|---|---|---|
| A: C de sistemas | Ponteiros e structs | Representação, vida útil, UB, overflow, I/O e build. | Um programa C pequeno que reproduza cada risco relevante. |
| B: front-end | A; recursão e tabelas hash | Tokens, Pratt, AST, símbolos e tipos. | Rastro de uma expressão da fonte à AST checada. |
| C: análises | B; conjuntos e grafos | CFG, ponto fixo, efeitos, alcance e dependências. | Grafo anotado e um contraexemplo de análise incorreta. |
| D: MIR | C | Lowering, propagação, aliasing, cópias e liveness. | MIR antes/depois com prova de preservação de comportamento. |
| E: máquina | A e D; assembly básico | ISA, ABI, frames, registradores e encoding. | Decodificação manual de instruções emitidas. |
| F: binários | E; aritmética de endereços | ELF, Mach-O, símbolos, relocação e loading. | Mapa do arquivo até os endereços em memória. |
| G: engenharia avançada | C–F | Medição, cache, fuzzing, desenho incremental e portabilidade. | Mudança pequena com evidências reproduzíveis. |

Mantenha um caderno por experimento: revisão do código, hipótese, comando exato, entrada, saída,
status, ferramenta/versão e conclusão. Registre também o que o experimento não demonstra.
Uma execução correta não prova todos os caminhos; um benchmark rápido não prova correção.

## 11. C de sistemas: o que dominar e como praticar aqui

### 11.1 Modelo de objetos, bytes, alinhamento e aliasing

Uma variável C tem tipo, tamanho, alinhamento e vida útil. O endereço existir numericamente
não significa que possa ser dereferenciado. Diferencie objeto, representação em bytes e valor.
Estude padding de structs, effective type, strict aliasing e representação de inteiros.
`memcpy` é útil para transferir representações sem um cast desalinhado; não resolve sozinho
endianness, formato externo ou validade de ponteiros.

No Modal, compare `put16`/`put32`/`put64` de `native/elf.c`, que escrevem little-endian
explicitamente, com `memcpy` de palavras em outros emissores. Identifique pressupostos de host
em cada caminho. Serializar uma struct C inteira pode exportar padding e depender do ABI do
compilador; um formato binário deve ter offsets e larguras definidos pelo formato.

**Exercício:** represente um cabeçalho de 16 bytes com campos de 16, 32 e 64 bits. Escreva um
serializer por bytes e um leitor com validação de tamanho. Faça round-trip e compare um vetor
de bytes conhecido. Só o round-trip é insuficiente: leitor e escritor podem compartilhar o bug.

**Palavras-chave:** object representation, alignment, padding, effective type, strict aliasing,
pointer provenance, endianness, serialization, unaligned access, `_Alignof`, `offsetof`.

### 11.2 Aritmética, tamanhos e comportamento indefinido

Estude integer promotions, usual arithmetic conversions, narrowing, sign extension e shifts.
Overflow sem sinal é modular; overflow aritmético com sinal em C pode ser UB. Shifts exigem
atenção à largura, ao sinal e ao valor deslocado. Converter um inteiro grande para um tipo
menor não equivale a provar que o valor cabe nele.

Para buffers, não valide o resultado depois de uma multiplicação que já pode ter estourado:
para elementos de tamanho `s > 0`, verifique `n <= SIZE_MAX / s` antes de calcular `n * s`.
Para um intervalo dentro de arquivo, com `off <= file_len`, use `len <= file_len - off` em vez
de confiar em `off + len <= file_len`. A condição precisa anteceder a criação/acesso do intervalo.

**Exercício:** monte uma tabela de fronteiras para `u8`, `i8`, `u64` e `i64`: zero, máximo,
mínimo, overflow, divisão por zero e mínimo com sinal dividido por −1. Siga cada operação em
`comptime.c`, `mir/opt.c`, `native/emit.c` e no C gerado. Explique por que a mesma operação
matemática pode precisar de sequências diferentes para preservar a semântica Modal.

**Palavras-chave:** UB, implementation-defined, unspecified behavior, integer promotion,
two's complement, wrapping arithmetic, truncation, checked arithmetic, sign/zero extension.

### 11.3 Ownership, arenas, regiões e handles

Para cada estrutura, responda: quem aloca, quem pode escrever, quem libera, quem guarda alias
e qual evento invalida esses aliases? Separe tamanho lógico, capacidade, memória reservada e
páginas efetivamente tocadas. Reservar espaço virtual não significa consumir RSS equivalente.

Estude crescimento geométrico, fragmentação, bump allocation e marca/restauração de arena.
Um `realloc` pode mover o armazenamento; um índice pode sobreviver a essa mudança, mas precisa
ser validado e resolvido contra a base atual. Um ponteiro para elemento temporário continua
inválido após reset mesmo que os bytes pareçam intactos.

**No código:** `Region`, `ArenaMark`, `region_grow_array`, `arena_alloc`, `arena_release`,
`Unit.types`, `Unit.refs`, `NodeId`, `SymId` e `TypeId`. Diferencie a arena do compilador da
arena que um programa Modal passa como contexto: são camadas e tempos de vida distintos.

**Exercício:** desenhe a vida útil de tokens, nós, símbolos, MIR e código emitido durante duas
funções e uma avaliação aninhada. Marque onde salvar um ponteiro seria inseguro.

**Palavras-chave:** ownership, lifetime, dangling pointer, use-after-free, use-after-reset,
bump allocator, arena mark, virtual reservation, committed pages, handle, index-based storage.

### 11.4 APIs C, invariantes e tratamento de falhas

Aprenda a expressar pré/pós-condições: buffer válido até qual offset, ID zero significa o quê,
quem inicializa a estrutura e o que permanece válido após falha parcial? Uma boa função precisa
ter contrato de ownership e erro tão claro quanto seus parâmetros.

Use funções `static` para detalhes de uma unidade, headers com interface pequena e tipos de
largura fixa para formatos externos. Ao revisar alocação ou I/O, percorra caminhos de sucesso
e falha: liberação, arquivo parcial, retorno de `fwrite`, erro de `mprotect`, símbolo ausente.
Em ferramentas novas, trate interrupção e escritas parciais quando usar chamadas de baixo nível.

**Exercício:** revise uma função sem mudá-la e escreva seu contrato, recursos adquiridos e
caminhos de saída. Só proponha refatoração depois de conseguir explicar o comportamento atual.

### 11.5 Build C: do include ao executável do próprio compilador

Separe quatro passos: preprocessar (`-E`), compilar para assembly (`-S`), montar para objeto
(`-c`) e fazer linking. O driver `cc` pode coordenar todos numa única invocação. Uma translation
unit inclui o `.c` e o conteúdo expandido dos headers; incluir um header não compila seu `.c`
correspondente nem faz linking da implementação.

Um protótipo permite checar uma chamada; uma definição fornece o corpo. Uma função `static`
no escopo de arquivo tem linkage interno; uma declaração `extern` pode referenciar uma definição
externa. Storage duration, scope e linkage são propriedades diferentes. `const` de um objeto C
não o transforma automaticamente em constante de compilação da linguagem Modal.

Leia `SRCS`, `HDRS`, `CFLAGS`, dependências dos stencils e a receita `modal` do makefile.
O caminho atual compila as fontes listadas numa invocação e usa `-flto`; isso permite otimização
entre translation units, mas não elimina a necessidade de listar cada implementação.
Um erro de include é diferente de símbolo indefinido no link; definição duplicada e assinatura
incompatível também têm causas distintas. Nem todo problema do linker se resolve adicionando libs.

**Exercício:** em diretório temporário, crie `main.c`, `helper.c` e `helper.h`. Observe o `.i`,
assembly, símbolos do `.o` com `nm` e resultado final. Provoque separadamente falta de header,
falta de implementação e duas definições externas. Depois explique qual desses problemas seria
causado por esquecer uma nova fonte em `SRCS`.

**Palavras-chave:** preprocessor, translation unit, declaration, definition, linkage,
storage duration, include guard, dependency graph, object file, undefined reference,
multiple definition, LTO, static library, shared library, reproducible build.

## 12. Front-end: transformar bytes em um programa compreendido

### 12.1 Scanner e interning

O scanner reconhece fronteiras de tokens; não deve assumir que uma entrada inválida terá
aspas, comentário ou chave de fechamento. Estude sentinelas, lookahead limitado e offsets de
fonte. Interning associa textos iguais a uma identidade reutilizável, reduzindo comparação e
armazenamento; não confunda identidade do nome com identidade de uma declaração em um escopo.

Em `syntax/scanner.*`, siga uma string com escape, um comentário, um nome e um `TOK_BODY`.
Verifique como o scanner conta linhas e preserva limites ao adiar um corpo inteiro.

**Exercício:** crie entradas com EOF após cada prefixo de string/comentário. O compilador deve
terminar com diagnóstico coerente, sem ler além do buffer nem travar tentando se recuperar.

**Palavras-chave:** lexer, tokenization, character classes, maximal munch, lookahead, sentinel,
source span, string interning, hash collision, open addressing, load factor.

### 12.2 Gramática, Pratt e recuperação

Estude associatividade e precedência separadamente. Em um parser Pratt, uma regra de prefixo
interpreta o início de expressão; regras de infixo/sufixo estendem uma expressão já lida.
Binding power determina quando a chamada recursiva para e quem consome o próximo operador.
Declarações e statements usam sua própria classificação; nem tudo passa pelo Pratt.

Em `parse_expr.c`, siga `a + b * c`, chamada, seletor, índice e `.(`. Em `parse_decl.c`,
compare tipo primeiro e tipo depois; em `parse_stmt.c`, compare condição e match com arms.
Modificar precedência pode alterar um programa válido sem produzir qualquer diagnóstico.

**Exercício:** escreva a árvore esperada para três expressões com e sem parênteses, confira
`--ast`, depois retire um delimitador. Demonstre que a recuperação sempre avança ou encerra;
um parser que repete o mesmo token pode entrar em loop infinito.

**Palavras-chave:** recursive descent, Pratt parser, binding power, NUD, LED, associativity,
FIRST set, ambiguity, synchronization point, panic-mode recovery. LR(0) aparece nos esboços
históricos; não descreve o parser de expressões ativo por isso só.

### 12.3 AST, símbolos, tipos e diagnósticos

AST representa estrutura da fonte; tabela de símbolos representa declarações; tabela de tipos
representa tipos e suas identidades. Separá-las permite que uma referência ao mesmo símbolo
apareça em vários nós. `Unit.refs` e `Unit.types` são tabelas laterais da árvore checada.

Siga `ast/symbol.*`, `sema/resolve.c`, `module.c` e `check.c`. Diferencie resolução lexical,
resolução de módulo, seleção de overload e compatibilidade de tipos. Um identificador não
encontrado não deve ser transformado em valor genérico que cause um erro obscuro muito depois.

**Exercício:** construa nomes iguais em dois escopos e imports com alias. Anote qual símbolo
cada uso resolve e por quê. Depois altere a ordem das declarações para separar regra semântica
de uma dependência acidental da implementação.

**Palavras-chave:** AST, side table, symbol identity, lexical scope, shadowing, name resolution,
overload resolution, type interning, source location, diagnostic recovery.

## 13. Grafos e análises: aprender a provar fatos conservadores

### 13.1 Grafos de chamadas, imports e ciclos

DFS e ordenação topológica ajudam a processar dependências antes de usuários. Ciclos exigem
tratamento explícito: não existe ordenação topológica de todos os nós de um ciclo. Estude
componentes fortemente conexas (SCC), mesmo quando a rotina atual usa outro mecanismo.
Veja `check_order`, `unit_order`, a análise de stack e os conjuntos de erros.

**Exercício:** desenhe `a → b → c`, `c → b` e uma função não alcançada. Mostre como uma
informação descoberta em c chega a a, como a análise termina e o que precisa ser recalculado
quando uma aresta muda. Não suponha que todas essas análises tenham a mesma direção ou regra.

### 13.2 Ponto fixo, fluxo de dados e lattice

Uma análise propaga fatos até nenhuma informação mudar. Defina estado inicial, transferência,
junção e condição de término. Exemplo: erros possíveis de uma função incluem os erros de
chamadas que ela deixa propagar; uma fronteira que captura não propaga da mesma forma.

Para liveness, a forma clássica por bloco é `OUT[B] = união de IN dos sucessores` e
`IN[B] = USE[B] ∪ (OUT[B] − DEF[B])`. Loops pedem iteração. Entenda as equações antes de
tentar comprimir o bitset. Uma análise de “pode acontecer” e uma de “acontece com certeza”
não usam automaticamente a mesma junção.

**Exercício:** resolva manualmente os conjuntos de três blocos com um back edge. Compare com
`regalloc.c:liveness`. Construa um exemplo no qual olhar só a próxima instrução esteja errado.

**Palavras-chave:** CFG, basic block, predecessor, successor, back edge, SCC, DFS, worklist,
fixed point, lattice, meet/join, monotonicity, may/must analysis, live-in, live-out.

### 13.3 Efeitos, aliasing e provas de segurança

Uma expressão pode parecer redundante e ainda ler um dispositivo, escrever saída ou alterar
memória acessível por outro caminho. Otimizar exige distinguir operação pura, acesso a estado,
trap possível e acesso MMIO. No MIR, investigue `MIR_MEM_DEVICE` e seus consumidores.

Conhecer um limite antes de um índice permite remover uma guarda só enquanto esse fato
continuar válido. Uma atribuição ao índice pode invalidá-lo; uma chamada pode alterar memória
com alias. Uma análise conservadora mantém trabalho quando não consegue provar sua remoção.

**Exercício:** construa `store → load` com e sem alias, depois coloque uma chamada entre os dois.
Explique em quais versões o forwarding é válido e o que a análise precisa saber.

**Palavras-chave:** side effect, purity, effect system, alias analysis, escape analysis,
range analysis, abstract interpretation, soundness, conservative approximation, invalidation.

### 13.4 Especialização e capturas: tornar decisões explícitas cedo

Monomorphization cria uma versão de função para argumentos conhecidos na compilação. A chave
de reutilização precisa distinguir as informações que mudam corpo/tipos sem criar uma cópia
para cada chamada equivalente. Especializar por tipo, comprimento ou função conhecida permite
que o código final use chamadas diretas e elimine parâmetros que só existiam na compilação.

Lambda lifting transforma referências a variáveis externas em parâmetros adicionais. Isso
não é o mesmo que alocar um closure object no heap. No Modal, siga `sema_captures`,
`sema_capture_name`, `literal_specs_waiting` e o encaminhamento de capturas em chamadas.
O momento de checar a especialização importa porque o corpo que originou o literal é temporário.

**Exercício:** acompanhe `map` com duas funções conhecidas diferentes e depois com uma função
que captura um local. Conte especializações, identifique argumentos eliminados e parâmetros
ocultos adicionados. Explique como evitar que uma especialização use tokens já descartados.

**Palavras-chave:** monomorphization, specialization key, partial evaluation, lambda lifting,
closure conversion, captured environment, hidden parameter, code bloat, specialization cache.

## 14. MIR e otimização: transformar sem mudar significado

### 14.1 Lowering e representação

Lowering elimina construções de alto nível em favor de operações mais simples e explícitas.
Um match vira testes, labels, jumps e caminhos de resultado; uma chamada prepara argumentos e
um destino de retorno. Preserve ordem de avaliação e efeitos, inclusive nos caminhos de falha.

O MIR deste projeto usa slots e variantes inteiras; não o descreva como SSA só porque usa
instruções de três endereços. SSA, dominância, phi nodes e block arguments são conceitos úteis
para comparar arquiteturas, não estruturas que se deve presumir presentes aqui.

**Exercício:** para um match de dois arms, desenhe AST, CFG e MIR. Numere os slots, identifique
quem escreve o resultado e prove que nenhum caminho chega ao uso sem inicializá-lo.

### 14.2 Transformações a estudar

| Técnica | Ideia | Condição que precisa ser demonstrada |
|---|---|---|
| Constant folding | Calcular operação conhecida. | Mesma largura, wrapping, erros e efeitos do runtime. |
| Constant propagation | Substituir uso por constante. | Nenhuma redefinição/caminho invalida o fato. |
| Copy propagation | Usar a origem de uma cópia. | A origem ainda contém o valor correto no uso. |
| Dead code elimination (DCE) | Remover resultado sem uso. | Operação não tem efeito ou trap observável necessário. |
| Dead store elimination | Remover escrita que será substituída. | Nenhum leitor/alias observa a escrita intermediária. |
| Store-to-load forwarding | Usar o valor escrito anteriormente. | Mesmo local, largura e ausência de escrita interveniente. |
| Scalar replacement | Separar agregado em campos. | Endereço/alias não exigem manter o objeto materializado. |
| Inlining | Substituir chamada pelo corpo. | Argumentos, retorno, erros e vida útil preservados; custo medido. |
| Strength reduction | Trocar operação por equivalente mais barata. | Equivalência em todas as fronteiras de sinal/largura. |
| Tail-call lowering | Reutilizar frame e transferir controle. | Nenhum argumento/referência depende do frame descartado. |

Leia cada passe em `mir/opt.c` com um caso que o habilita e um que o bloqueia. Estude a ordem
dos passes: um expõe oportunidades para outro, mas pode aumentar tempo de compilação se exigir
iterações demais. Remover uma cópia de array também pode ser mais importante que economizar um add.

### 14.3 Registradores e spills

O compilador possui muitos valores virtuais e a máquina tem poucos registradores físicos.
Liveness determina sobreposição; linear scan percorre intervalos para atribuir registradores.
Spill mantém um valor na memória; reload o traz de volta. Chamadas clobberam parte dos registros.

No Modal, estude a separação de slots em valores (`webs`), `RA_VIRTUAL`, classes preservadas e
scratch, `RegAlloc.home`, comparação fundida e fallback quando a função é grande demais para
a análise. Um slot com endereço tomado não pode ser tratado como escalar independente sem prova.

**Exercício:** faça muitos valores permanecerem vivos através de uma chamada. Preveja quais
precisam de memória e compare modos de registradores. Verifique resultado e custo de prologue/
epilogue: reduzir loads pode não compensar salvar vários registradores em uma função minúscula.

**Palavras-chave:** three-address code, SSA, dominator, phi, def-use, reaching definitions,
value numbering, peephole, SROA, liveness, live interval, linear scan, spill, reload, coalescing.

### 14.4 Interpretar MIR: uma segunda execução do mesmo contrato

Um interpretador mantém estado explícito: instrução atual, slots, blocos de memória, frames,
retornos e falhas. Estude dispatch por opcode, transição de frame, representação de valores e
verificação de orçamento. Interpretar MIR do target não significa executar instruções da CPU
target no host; significa implementar suas regras de valores/layout no compilador.

Em `sema/comptime.c`, siga uma chamada desde `evaluate` até frames e retorno; compare com o
caminho de folding. Um orçamento é parte do contrato de compilação: excedê-lo precisa gerar
resultado/diagnóstico definido. Se folding não compensa ou não pode executar uma operação,
deixar a chamada para runtime pode ser correto; aceitar silenciosamente um inicializador
inválido em posição obrigatoriamente compile time não é a mesma decisão.

**Exercício:** execute conceitualmente cinco instruções MIR numa tabela com PC e slots.
Acrescente uma chamada, depois uma falha capturada, depois orçamento esgotado. Especifique o
estado que precisa ser restaurado em cada caso e compare o valor com runtime.

**Palavras-chave:** interpreter dispatch, program counter, activation record, explicit stack,
evaluation budget, deterministic evaluation, constant evaluator, partial evaluation.

## 15. ISA, ABI e stack: três contratos diferentes

**ISA** define instruções e registradores. **ABI** define comunicação binária entre componentes.
**Convenção interna do Modal** organiza seus frames, contexto e slots. Uma chamada ao runtime
C precisa atravessar contratos; não basta colocar argumentos nos slots internos e emitir `call`.

Aprenda caller-saved/callee-saved, alinhamento da pilha, endereço de retorno, prologue/epilogue,
parâmetros, resultado e retorno de agregado por armazenamento do caller (`sret`). Estude red
zone no ABI pertinente; não generalize regras de x86-64 SysV para todos os sistemas.
O papel de x18 em arm64 depende da plataforma; este build o reserva com `-ffixed-x18`.

Há ainda três pilhas diferentes para acompanhar: a pilha C do compilador, a pilha explícita
do interpretador comptime e o armazenamento de frames/stack da execução gerada. Budget de uma
não protege automaticamente as outras. Uma fronteira de panic precisa restaurar o estado
compatível com o ponto de retomada, incluindo registradores preservados.

**Roteiro no código:** `mir.h` → `finish_frame` em `lower.c` → `plan_homes`, `regs_prologue`,
`regs_call`, `regs_tail`, `regs_epilogue` em `native/emit.c` → stencils de chamada/retorno.
No backend C, siga `tail_call`, `emit_tail`, `emit_array_return` e `M_TAIL`.

**Exercício:** desenhe a memória antes, durante e depois de uma chamada que recebe um inteiro,
uma referência e um agregado. Depois substitua por tail call e explique quais endereços
deixariam de ser válidos. Repita para retorno de array no destino do caller.

**Palavras-chave:** calling convention, ABI, stack frame, frame pointer, stack pointer,
link register, return address, callee-saved, caller-saved, clobber, red zone, sret, thunk,
trampoline, tail call, musttail, unwind, non-local jump.

## 16. ELF: aprender a emitir e interpretar o arquivo

### 16.1 Separar os dois usos de ELF neste repositório

| Caminho | Entrada/saída | Estruturas importantes | Papel |
|---|---|---|---|
| `tools/stencilgen.c` | Lê objeto ELF relocável gerado por clang. | Section headers, símbolos, strings e `SHT_RELA`. | Extrair código e referências ainda não resolvidas. |
| `backend/native/elf.c` | Escreve ELF executável Linux ou imagem bare metal. | ELF header e program headers. | Descrever onde carregar código já preparado. |

Um `.o` não é um executável pronto: normalmente ainda precisa de layout e resolução de
referências. Um ELF executável pode não ter section headers. O loader usa segmentos descritos
pelos program headers; seções organizam principalmente material para ferramentas e linking.
Não conclua que faltou código porque `readelf -S` não listou `.text`.

### 16.2 Cabeçalho, arquitetura e endereços

Leia `elf.c` junto de `elf.h`. O escritor atual usa ELF64, little-endian, `ET_EXEC`,
`EM_AARCH64` (183) ou `EM_X86_64` (62). O cabeçalho tem 64 bytes, seguido de dois program
headers de 56 bytes; por isso o código começa em `0xb0` (176). Os campos são escritos por
offset, sem depender do layout de uma struct C do host.

| Campo | Pergunta que responde no arquivo atual |
|---|---|
| `e_ident` | É ELF? Qual classe e byte order? |
| `e_type`, `e_machine` | É executável de qual arquitetura? |
| `e_entry` | Em qual endereço virtual começa a execução? |
| `e_phoff`, `e_phnum`, `e_phentsize` | Onde e como percorrer os program headers? |
| `e_shoff`, `e_shnum` | Há tabela de seções? Aqui são zero. |
| `p_offset`, `p_vaddr` | De qual offset do arquivo para qual endereço virtual? |
| `p_filesz`, `p_memsz` | Quantos bytes vêm do arquivo e quantos existem em memória? |
| `p_flags`, `p_align` | Quais permissões e restrições de alinhamento se aplicam? |

No ELF Linux atual, `ELF_BASE = 0x400000`, o segmento `PT_LOAD` começa no offset zero e cobre
cabeçalhos e código com permissões RX. `code_at = ELF_BASE + ELF_CODE_OFFSET` e
`e_entry = code_at + entry`, onde `entry` é offset dentro do buffer de código.
`PT_GNU_STACK` solicita stack RW sem execução. O writer usa `p_filesz == p_memsz`;
uma região tipo BSS com memória adicional é conceito a estudar, não segmento extra já emitido.

**Exemplo de cálculo:** se `entry = 0x20`, o ponto inicial Linux será
`0x400000 + 0xb0 + 0x20 = 0x4000d0`. Isso é endereço virtual, não offset `0x4000d0` no arquivo.
Para bytes dentro do segmento, `VA = p_vaddr + (file_offset − p_offset)`.

No bare metal, `elf_write_image` faz `PT_LOAD` apontar do offset `0xb0` para `flash_origin`;
o código começa na base da flash e os headers não são copiados junto para esse endereço.
Compare os dois ramos de `write_elf` em vez de reutilizar a fórmula Linux sem adaptação.

### 16.3 Laboratório de inspeção

Execute da raiz. `mktemp` mantém os artefatos fora do repositório; GNU binutils/LLVM podem
precisar estar instalados separadamente. Gerar para outra CPU não implica poder executá-la.

```sh
ELF_LAB=$(mktemp -d)
./modal --target x86_64-linux -o "$ELF_LAB/hello-x86_64" examples/run/hello.modal
./modal --target arm64-linux -o "$ELF_LAB/hello-arm64" examples/run/hello.modal
file "$ELF_LAB/hello-x86_64" "$ELF_LAB/hello-arm64"
od -An -tx1 -N64 "$ELF_LAB/hello-x86_64"
readelf -h -l -W "$ELF_LAB/hello-x86_64"
readelf -S -W "$ELF_LAB/hello-x86_64"
# Alternativa LLVM para os cabeçalhos:
llvm-readobj --file-headers --program-headers "$ELF_LAB/hello-x86_64"
```

**Entrega:** anote magic, machine, entry, segmentos, offsets, tamanhos, flags e alinhamento;
recalcule o endereço do primeiro byte de código sem consultar a função de escrita.
Desmontadores podem depender de seções e não mostrar o payload desse ELF mínimo. Nesse caso,
extraia o segmento/código por seus offsets e use uma ferramenta que aceite bytes brutos e a
ISA correta; ausência de disassembly automático não demonstra que o executável esteja vazio.

### 16.4 O que seria necessário para ampliar o writer

Adicionar dados mutáveis, BSS, PIE ou linking dinâmico envolve novos contratos: separar RX/RW,
layout de segmentos, page alignment, inicialização, símbolos/imports e relocação adequada.
Antes de codificar, desenhe arquivo e memória lado a lado e liste quem inicializa cada byte.
Para `PT_LOAD`, confira a congruência de offset e endereço segundo o alinhamento escolhido.

**Exercício avançado:** escreva um leitor de ELF em ferramenta separada que recuse truncamento,
overflow de offsets, contagens inconsistentes e entry fora dos segmentos executáveis. Não
execute uma imagem só porque o leitor reconheceu o magic. Faça o leitor aceitar os ELFs que o
Modal gera e distinguir o objeto usado por stencilgen do executável final.

**Palavras-chave:** ELF64, ET_REL, ET_EXEC, ET_DYN, Ehdr, Phdr, Shdr, PT_LOAD, PT_GNU_STACK,
PF_R/PF_W/PF_X, virtual address, file offset, load bias, BSS, entry point, segment alignment.

## 17. Linking e relocação: onde referências viram endereços

### 17.1 Três operações que convém distinguir

**Resolução da linguagem** decide a qual declaração um nome se refere. **Linking de código**
decide que código/dados entram no resultado, onde ficam e como referências são corrigidas.
**Loading** mapeia o resultado em memória e transfere controle. O arquivo `sema/module.c`
resolve imports da linguagem; isso não o torna, sozinho, um linker de objetos ELF.

No backend C, o driver entrega C a uma toolchain externa, que faz sua compilação e linking.
No backend nativo, o Modal escreve executáveis diretamente. “Sem linker externo” ainda deixa
trabalho de linking dentro do compilador: símbolos internos, alcance, layout e fixups.
Atualmente `native_end` aceita saída objeto `.o` para macOS; rejeita objetos para Linux/bare metal.
Não anuncie suporte geral de emissão de `.o` ELF a partir do leitor usado para stencils.

### 17.2 Símbolos, relocations e fixups

Um símbolo identifica algo cujo endereço pode ainda ser desconhecido. Uma relocation descreve
onde aplicar uma correção, qual fórmula usar e qual referência/addend participa. Fixup é o
registro de uma correção pendente no emissor; pode ter nascido de uma relocation de objeto,
mas também de um salto criado diretamente pelo compilador.

Em `stencilgen.c`, estude `Sym`, `Rela`, `SHT_SYMTAB`, `SHT_RELA`, `sh_link` e `sh_info`.
O símbolo fornece nome/seção/valor; a string table guarda o texto. Uma relocation RELA contém
addend explícito. A representação interna reduz isso a `StencilReloc`: offset, kind, addend
e target, incluindo `ST_HOLE_A`, `ST_CALLEE`, runtime e externs.

Defina `S = endereço do símbolo`, `A = addend`, `P = endereço do campo a corrigir`.
Uma forma PC-relative comum é `S + A − P`; uma forma absoluta comum é `S + A`. A fórmula
exata, largura, unidade e verificação de overflow dependem do tipo de relocation e da ISA.
Não trate todo campo de 32 bits como uma constante absoluta.

### 17.3 Dois cálculos que você deve conseguir fazer à mão

Em x86-64, uma chamada `call rel32` de cinco bytes usa deslocamento a partir da próxima
instrução. Se o opcode está no offset `0x100` e o destino em `0x180`, então
`disp = 0x180 − 0x105 = 0x7b`. O campo começa em `P = 0x101`; com `A = −4`,
`S + A − P = 0x180 − 4 − 0x101 = 0x7b`. Isso explica o addend de uma relocation,
sem exigir que toda referência x86 use o mesmo formato.

Em AArch64, `b`/`bl` usam um imediato com sinal de 26 bits em unidades de quatro bytes,
relativo ao endereço da instrução. De `0x100` para `0x180`, o imediato é
`(0x180 − 0x100) / 4 = 0x20`. É necessário preservar os bits do opcode, inserir o imediato,
checar alcance e garantir alinhamento. Branches de 19/14 bits têm limites diferentes.

**No código:** `fill_value` trata holes de constantes e pedaços de 16 bits arm64;
`fill_branch` trata destinos PC-relative. `push_fixup`, `push_jump` e `push_extern` registram
o que será resolvido depois. Um endereço truncado que “funciona no exemplo” continua sendo bug.

### 17.4 Seleção de código, layout e remapeamento

Siga `begin_range` → `shake` → `fold_identical` → `remap` → resolução final em `native_end`.
Código não alcançável pode ser removido; duas funções podem compartilhar código quando bytes
e referências relevantes são equivalentes. Comparar só os bytes não basta: dois campos ainda
zerados podem futuramente apontar para callees diferentes.

Ao compactar código, os endereços mudam. Remapeie posições das funções, locais dos fixups,
chamadas externas e entry. Um branch correto antes de remover uma função pode ficar incorreto
depois. Esse é um motivo para preferir offsets/IDs persistentes a ponteiros para bytes móveis.

**Exercício:** desenhe funções A, B, C; A chama C e B é inalcançável. Remova B, calcule a nova
posição de C e repatche A. Depois faça B e C terem bytes iguais e destinos diferentes;
explique por que a fusão não pode se basear só em `memcmp`.

### 17.5 Linking estático e dinâmico como temas de aprofundamento

Aprenda símbolos definidos/indefinidos, visibilidade, strong/weak, archives, extração de membros,
COMDAT, section GC, ICF, symbol interposition, GOT/PLT e binding. São conceitos de linkers
gerais: não suponha que o Modal implemente todos. Aqui há ICF e seleção de alcançáveis próprios;
no macOS há imports/stubs/GOT; o ELF Linux atual usa camada de syscalls sem libc dinâmica.

Um projeto de linker incremental precisaria manter dependências, identidade de fragmentos,
layout e invalidação de relocations. Cachear bytes de uma função sem suas referências e
pressupostos de alvo/ABI não é uma implementação completa.

**Palavras-chave:** symbol table, undefined symbol, relocation, REL/RELA, addend, PC-relative,
absolute relocation, fixup, branch range, relaxation, veneer, thunk, static/dynamic linking,
archive extraction, dead stripping, section GC, tree shaking, ICF, COMDAT, GOT, PLT, binding.

### 17.6 GOT, PLT e stubs: entender o motivo da indireção

Um destino externo pode não ter endereço final na hora de produzir o executável. Uma tabela
de endereços, como a GOT, fornece slots que o loader preenche ou ajusta; uma sequência de
instruções intermediária, como um stub/entrada de PLT, transfere controle pelo endereço
resolvido. Isso evita precisar conhecer aquele endereço em cada call site na emissão inicial.
PLT é terminologia de ELF; o Mach-O tem seus próprios stubs e metadados de binding.

Binding pode acontecer no carregamento ou, em mecanismos que o suportem, na primeira chamada.
Não presuma lazy binding no Modal: acompanhe o writer e os metadados concretos. Em `run.c`,
há outra solução: `dlsym` obtém o endereço no processo atual e o stub gerado salta até ele.
Os três mecanismos resolvem um problema parecido com contratos diferentes.

**Exercício:** desenhe `call site → stub → slot/endereço externo → função`. Anote quais partes
são relativas ao código, quais contêm endereço absoluto e quem pode alterá-las. Explique por
que mover só o código, sem revisar imports e referências absolutas, pode quebrar a execução.

## 18. Emissão de código de máquina e copy-and-patch

### 18.1 Stencils: usar um compilador para fabricar templates

O fluxo de build é `stencils.c → clang → objeto ELF → stencilgen → stencils_*.h`.
O fluxo de execução do compilador é `MIR → seleção/cópia de template → holes/fixups → bytes`.
O clang não é invocado para cada operação de cada programa Modal; os templates são preparados
antes. Os headers versionados permitem construir sem regenerá-los quando não houve mudança.

Leia os flags do makefile: freestanding, function sections, restrições de PIC/unwind/jump
tables e code models. Eles limitam a forma do objeto a algo que stencilgen consegue entender.
Uma nova otimização do clang pode gerar referência a dados ou função auxiliar inesperada;
stencilgen a rejeitar é informação sobre esse contrato, não motivo para ignorar a relocation.

Em `compact_arm64`, imediatos pequenos permitem omitir partes `movk`. Isso muda posições de
instruções: até branches internos ao template precisam ser recalculados. `STENCIL_SET_HASH`
ajuda a verificar a consistência dos IDs entre tabelas de arquiteturas.

**Exercício:** escolha um stencil, encontre seus bytes e relocations no header, identifique
holes e acompanhe o preenchimento. Explique quais valores já eram conhecidos no build e quais
só ficam conhecidos ao compilar o programa do usuário.

### 18.2 Encoding direto: aprender campos, não decorar números hexadecimais

Nem toda instrução vem de stencil. `native/emit.c` também codifica operações diretamente:
`a64_imm`, `a64_mem`, `a64_compare_branch`, `x_op`, `x_imm`, `x_mem` e outras rotinas.
Os pequenos `arch_arm64.c`/`arch_x86_64.c` registram tabelas; não concentram toda a emissão.

Em AArch64, estude palavras de instrução de 32 bits, campos de registradores, imediatos
escalados, `movz/movk`, loads/stores e branches. Em x86-64, estude tamanho variável, prefixos,
REX, opcode, ModR/M, SIB, displacement e immediate. Um erro de comprimento x86 desloca também
o próximo decode e o cálculo de endereços PC-relative.

**Exercício:** use o assembler da toolchain como comparação para uma instrução isolada, fora
do backend. Compare bytes para registradores baixos/altos e offsets no limite. Teste o primeiro
valor que deixa de caber na forma curta e o caminho alternativo que deve ser escolhido.

**Palavras-chave:** instruction selection, instruction encoding, opcode, immediate, displacement,
REX, ModR/M, SIB, scaled offset, movz/movk, copy-and-patch, stencil, compact encoding, code model.

## 19. Loading, execução em memória e Mach-O

### 19.1 O que acontece depois da emissão

Para executar em memória, `run.c` reserva páginas RW com `mmap`, copia código e stubs, resolve
externs via `dlsym`, corrige chamadas, troca permissões para RX com `mprotect` e invalida o
instruction cache no caminho arm64. Só então chama a entry em uma thread com stack configurada.
Esse caminho usa APIs e detalhes de host; não é implementação universal de JIT para todo SO.

W^X é a disciplina de não deixar a mesma região simultaneamente gravável e executável.
Coerência do cache de instruções importa quando código foi escrito como dados. Endereço de
função e ponteiro para dados também atravessam pressupostos de plataforma nas conversões.
Siga todos os caminhos de erro até `munmap` e restauração de estado.

**Exercício:** anote a permissão da região em cada fase e a posição de cada import stub.
Compare `--run` com `-o` seguido de execução em processo separado, incluindo status e falha.
Não chame essa execução de sandbox: ela compartilha o processo com o compilador.

### 19.2 ELF carregado no macOS para testes

`tools/elfrun.c` carrega imagens geradas pelo projeto e adapta pontos específicos de syscall/
halt ao host. Isso permite verificar muito do código e layout fora do SO alvo. Não é emulador
geral de Linux nem substitui teste real de kernel, loader, dispositivos ou boot de uma placa.
Registre essa diferença quando reportar “testado em ELF”.

### 19.3 Mach-O: segundo formato, novas obrigações

Em `macho.c`, estude header e load commands, segmentos/seções, `__TEXT`, `__LINKEDIT`, entry,
imports, stubs, GOT, bind info e assinatura ad hoc. Siga `macho_write_executable` e compare com
`macho_write_object`: o objeto deixa obrigações para uma toolchain, o executável precisa cumprir
o contrato de carregamento do sistema. A assinatura é parte da aceitação do arquivo, não uma
otimização do código Modal.

No macOS, inspecione um executável com ferramentas do sistema. Evite chamar de ELF o arquivo
gerado para `*-macos` ou aplicar offsets ELF a Mach-O.

```sh
MACHO_LAB=$(mktemp -d)
./modal --backend native -o "$MACHO_LAB/hello" examples/run/hello.modal
file "$MACHO_LAB/hello"
otool -hv "$MACHO_LAB/hello"
otool -l "$MACHO_LAB/hello"
otool -tvV "$MACHO_LAB/hello"
codesign -v "$MACHO_LAB/hello"
```

**Entrega:** explique por que tamanho de instruções, tamanho de arquivo e memória mapeada
são métricas diferentes. Encontre padding, comandos de carregamento e assinatura no tamanho total.

**Palavras-chave:** loader, mmap, mprotect, W^X, executable memory, instruction cache flush,
ASLR, PIC, PIE, load bias, dlsym, dynamic loader, Mach-O, load command, dyld, ad hoc signature.

## 20. Sistemas operacionais, embedded e interoperabilidade

### 20.1 Processo, syscall e runtime

Uma chamada C a `write` não é a mesma interface que uma instrução de syscall. Há convenções
distintas de argumentos, números de chamadas e erro. Siga `linux_system_layer`, stencils do
runtime e o caminho macOS com libSystem. Não transporte números de syscall entre SOs/ISAs.

Estude entrada `_start`, estado inicial da stack, chamada de main e encerramento de processo.
No driver, `posix_spawnp`, `waitpid`, sinais e exit status têm contratos diferentes de chamar
uma função no mesmo processo. I/O exige pensar em buffering e ordem entre stdout/stderr,
especialmente antes de panic, encerramento ou execução de programa gerado.

**Exercício:** acompanhe um único byte desde `print` até a saída do sistema. Depois force uma
falha após escrever e demonstre se os bytes pendentes são preservados e em qual ordem aparecem.

### 20.2 Bare metal e MMIO

Sem sistema operacional, alguém precisa definir memória, stack inicial, ponto de entrada,
estado permitido e encerramento/halt. Estude `bare_system_layer`, `memory_map`, `MachineTarget`,
capabilities, `examples/board/` e `examples/budget/`. Um ELF não define sozinho todo o protocolo
de boot de uma placa; loader/firmware/vector table podem impor requisitos adicionais.

MMIO é acesso a registrador de dispositivo, não memória comum para DCE/forwarding. Estude
largura, alinhamento, offsets, ordem e efeitos de leitura/escrita. `volatile` em C não é
sincronização entre threads nem substitui barreiras de memória exigidas pelo hardware.
Não assuma que um teste com memória simulada reproduz comportamento de periférico real.

**Exercício:** desenhe um register block com campos de larguras distintas e acesso reservado.
Confira o diagnóstico de overlap/alinhamento e o acesso emitido. Discuta separadamente como
validaria barriers, interrupts e reset em QEMU ou placa; não marque isso como suporte existente.

### 20.3 Interop C e portabilidade

Para uma interface externa, defina layout, calling convention, ownership, duração de ponteiros,
representação de erros e quem aloca/libera buffers. Layout compatível de struct não garante
compatibilidade de chamada de função: retorno de agregado e registradores também importam.
FFI geral permanece tema de evolução; os imports usados pelo runtime não provam suporte completo.

Para um alvo de 32 bits, audite pointer size, word size, alinhamento de inteiros largos,
aritmética de endereços e pressupostos dos slots. Uma descrição em `MachineTarget` é necessária,
mas não cria um emissor de instruções, ABI ou formato executável para o novo alvo.

**Palavras-chave:** freestanding, hosted, syscall ABI, process entry, MMIO, volatile,
memory barrier, interrupt, vector table, linker script, flash/RAM, FFI, cross-compilation,
host/build/target, data model, LP64, ILP32.

## 21. Desempenho, representação de dados e incrementalidade

### 21.1 Medir o trabalho certo

Complexidade assintótica e custo constante são complementares. Uma busca quadrática pode
passar em exemplos pequenos; uma tabela O(1) com registros enormes pode perder por cache/RSS.
Meça entradas pequenas, grandes e adversariais. Observe bytes por token/nó/símbolo e alocações
por chamada, além de tempo total.

Estude locality, cache lines, branch prediction, TLB, page faults, instruction count e custos
de I/O. Compare array of structs (AoS) e struct of arrays (SoA) com base no acesso real.
Interning e índices compactos economizam tráfego; também impõem limites de capacidade e
representação que precisam de checagem.

**Experimento:** formule “a rotina revisita todos os símbolos para cada chamada”. Instrumente
contagens, dobre a entrada e compare crescimento, depois mude a representação. Não use só o
tempo de uma execução para declarar que removeu comportamento quadrático.

### 21.2 Benchmark reproduzível

Separe aquecimento, compilação fria/quente, startup do processo, tempo de código gerado,
code size, file size, RAM do programa e memória do compilador. `bench.sh` usa o melhor de N;
ao estudar variabilidade, guarde também distribuição/mediana em um experimento separado e
identifique a metodologia. Um resultado não é comparável se a entrada deixou de exercer o recurso.

Faça comparação A/B com uma mudança de cada vez, mesma toolchain e carga de máquina controlada.
Não deixe instrumentação pesada ligada em apenas um lado. No macOS, Instruments pode ajudar;
em Linux, `perf` pode ajudar quando disponível e permitido. Registre versão, host e restrições.
Mais tamanho pode comprar velocidade; apresente os dois números e o motivo.

### 21.3 Cache e compilação incremental

Identifique a unidade cacheada e todas as entradas que determinam seu resultado: fonte,
declarações, imports resolvidos, configuração, target e versão/estratégia de compilação conforme
o cache em questão. O cache de checagem (`sema/cache.c`) e o cache do executável C (`main.c`)
resolvem problemas diferentes. Não transplante a chave de um para o outro.

Estude content hashing, invalidação transitiva, publicação atômica e colisões. Um hash serve
para localizar candidatos; o contrato de correção deve explicar como evita reutilização errada.
Compile-time output também é observável: o histórico registra que cache de checagem pode pular
essa saída. Não trate isso como resolvido sem testar o código atual.

**Exercício:** faça uma matriz frio/quente × edição de corpo/assinatura/import/config/target.
Para cada célula, preveja o trabalho invalidado e confira resultado, não só tempo.

### 21.4 Paralelismo como projeto separado

Antes de adicionar threads, catalogue `cx`, estado do parser/checker/lowering, regiões,
tabelas de tipos/nomes, emissão e arquivos temporários. Decida ownership por worker, estrutura
compartilhada, sincronização, determinismo e ordem de diagnósticos. Saiba distinguir data race,
race lógica e false sharing. Atomics não tornam automaticamente uma pipeline reentrante.

**Palavras-chave:** locality, AoS/SoA, cache miss, TLB, page fault, RSS, peak memory, hot path,
Amdahl's law, amortized complexity, geometric growth, content-addressed cache, invalidation,
atomic rename, determinism, thread-local state, data race, false sharing.

## 22. Técnicas de desenvolvimento, depuração e investigação

### 22.1 Navegar por comportamento e seguir contratos

Comece pela entrada e pelo resultado que observa. Use `rg` para mensagem, flag, operação MIR
ou função relevante; leia callers e callees, não só o match encontrado. Headers revelam
contratos, mas comentários podem estar desatualizados: confirme quem realmente consome o dado.

```sh
rg -n 'fill_branch|push_fixup|shake' src/v2/compiler/backend/native/emit.c
rg -n 'MIR_MOVE|MIR_MEM_DEVICE' src/v2/compiler
rg -n 'elf_write_executable|ELF_CODE_OFFSET' src/v2
git log --oneline -- src/v2/compiler/backend/native/elf.c
git diff -- docs/contribuindo.md
```

Use `git blame` e histórico para recuperar o motivo de uma condição aparentemente estranha.
Antes de refatorar, enumere invariantes e escreva o contraexemplo que cada condição impede.

### 22.2 Redução, hipótese e experimentos

Reduza preservando a falha: retire funções, campos, imports e expressões, uma dimensão de cada
vez. Quando parar de falhar, o último elemento pode ser relevante, mas ainda não prova a causa.
Mantenha dois arquivos vizinhos: o mínimo que falha e o mínimo que funciona.

Divida a investigação por fronteiras: AST correta? Tipo correto? MIR correto? Bytes corretos?
Arquivo correto? Loader correto? O primeiro ponto em que a previsão diverge limita a busca.
Se o problema só surge com otimização, investigue UB e vida útil antes de atribuir culpa à toolchain.

**Palavras-chave:** minimal reproducer, delta debugging, bisection, hypothesis-driven debugging,
invariant, precondition, postcondition, regression, negative test, boundary-value analysis.

### 22.3 Debugger e sanitizers

Use um build com símbolos e otimização apropriada ao problema. Breakpoints úteis incluem
diagnóstico, `mir_lower`/função de lowering pertinente, `fill_branch`, writer e caminho de falha.
Em builds otimizados, variáveis podem estar eliminadas/inlined; isso não significa corrupção.
LLDB e GDB permitem stack trace, registradores, memória e disassembly, mas dependem do host.

Exemplo em LLDB para investigar o writer, em ambiente no qual depuração esteja permitida:

```text
lldb -- ./modal --target x86_64-linux -o /tmp/modal-debug-hello examples/run/hello.modal
(lldb) breakpoint set --name elf_write_executable
(lldb) run
(lldb) frame variable
(lldb) bt
```

ASan ajuda com classes de erros de memória instrumentados; UBSan com várias operações
indefinidas. Eles não provam ausência de todos os bugs e não instrumentam automaticamente
bytes de máquina que o Modal emite. Um crash nativo pode exigir inspecionar MIR, ABI e código.
Veja a [documentação de UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html)
para os checks e limites da ferramenta, sem presumir cobertura além do que foi habilitado.

### 22.4 Testes diferenciais, metamórficos e de formato

Differential testing compara implementações do mesmo contrato: aqui, C, nativo, outros alvos
e comptime conforme o caso. Um teste metamórfico transforma a entrada de modo que alguma
relação de resultado deva permanecer: renomear um local ou inserir parênteses redundantes,
por exemplo. Reordenar operações com efeitos ou chamadas que podem falhar não é transformação
automaticamente válida.

Para um writer, combine três níveis: bytes/campos esperados; interpretação por ferramenta
independente; carregamento/execução no alvo. Cada um encontra classes diferentes de erro.
Verifique também stderr e status. Para fuzzing, anote seed, gerador, entradas recusadas e
programas efetivamente exercitados; “0 divergências” com tudo rejeitado é evidência fraca.

### 22.5 Refatoração incremental e revisão

Separe reorganização de comportamento quando isso facilitar verificar a mudança. Torne um
passo compilável e revisável antes de ampliar o escopo. Ao extrair helper, explicite contratos
de aliasing, ordem e ownership; evitar duplicação textual não justifica esconder diferenças
de semântica entre targets. Adote uma abstração quando ela representar uma regra compartilhada.

Na revisão, peça que outra pessoa consiga responder: qual bug/regra motiva o patch? Onde está
a prova? Qual teste falharia sem ele? Qual custo mudou? Quais alvos não foram exercitados?

## 23. Projetos de aprendizado com entregas verificáveis

Todos são exercícios sugeridos. Faça ferramentas exploratórias em pasta temporária ou branch
própria; não introduza mudanças de linguagem/backend apenas para completar uma trilha.

| Projeto | Nível | Escopo e produto final | Como demonstrar entendimento |
|---|---|---|---|
| Atlas de um programa | Intermediário | Anotar fonte, tokens, AST, tipos, MIR e saída. | Explicar uma operação e um erro em todas as fronteiras. |
| Biblioteca de serialização | Intermediário | Serializer/leitor C pequeno de formato fixo. | Vetores conhecidos, truncamento e overflow recusados. |
| Leitor do ELF Modal | Intermediário/avançado | Imprimir headers/segmentos com bounds checks. | Concordar com ferramenta independente e recusar arquivos truncados. |
| Relocator de laboratório | Avançado | Aplicar um rel32 e um branch26 a bytes conhecidos. | Calcular destinos à mão, testar para trás e overflow. |
| Mini-linker em memória | Avançado | Três fragmentos com símbolos, alcance, layout e fixups. | Remover fragmento morto e corrigir todos os endereços. |
| Auditor de cópias | Avançado | Rastrear um array retornado por vários calls. | Identificar cópia necessária, evitável e alias que bloqueia remoção. |
| Visualizador de liveness | Avançado | Tabela IN/OUT de CFG pequeno, fora da interface do produto. | Resolver loop e explicar um spill através de chamada. |
| Estudo de stencil | Avançado | Anotar objeto, relocation, template e patch final de um stencil. | Distinguir erro de clang/stencilgen/emissor e detectar header antigo. |
| Matriz de cache | Avançado | Casos de invalidação com mudanças isoladas. | Resultado correto em frio/quente e dependências explícitas. |
| Investigação de desempenho | Avançado | Hipótese, contador, benchmark e patch pequeno. | Crescimento por tamanho e ganho sem divergência semântica. |

Uma sequência de conclusão possível é: explicar um programa → corrigir uma regressão → ler
um ELF → aplicar relocations à mão → rastrear uma chamada/ABI → modificar um passe com prova
e medição. Dominar ELF não é pré-requisito para corrigir um diagnóstico de parser.

## 24. Glossário de pesquisa e materiais de aprofundamento

Use os termos em inglês para localizar literatura e manuais. Cada grupo deve ser estudado
com uma pergunta concreta do código, não como uma lista para memorizar.

| Grupo | Termos para pesquisar | Pergunta de controle |
|---|---|---|
| C e memória | object lifetime, strict aliasing, pointer provenance, integer promotions, UB, arena allocator | Posso acessar estes bytes por este ponteiro e com este tipo agora? |
| Estruturas | interning, open addressing, load factor, bitset, tagged ID, compact representation | Qual é o custo por item e o comportamento no limite? |
| Parsing | Pratt parsing, binding power, FIRST sets, error recovery, deferred parsing | Quem consome este token e por que a árvore tem esta forma? |
| Semântica | lexical scope, overload resolution, type inference, monomorphization, lambda lifting | Quando o significado fica decidido e quais dados precisam sobreviver? |
| Análise | CFG, SCC, dataflow, fixed point, abstract interpretation, escape analysis | Qual fato é seguro afirmar em todos os caminhos? |
| Otimização | constant propagation, DCE, SROA, alias analysis, strength reduction, inlining | Qual efeito observável impediria esta transformação? |
| Registradores | liveness, webs, linear scan, interference, spill, clobber, register classes | Onde vive o valor através desta chamada? |
| Máquina | opcode, immediate, ModR/M, SIB, REX, PC-relative, branch relaxation | Estes bytes codificam qual instrução e qual destino? |
| ABI | caller/callee-saved, frame layout, sret, red zone, tail call, trampoline | O caller e o callee concordam sobre argumentos e estado preservado? |
| ELF | ET_REL/ET_EXEC/ET_DYN, segments vs sections, SYMTAB, STRTAB, RELA, PT_LOAD | O endereço é da fonte, da seção, do arquivo ou da memória? |
| Linking | symbol resolution, fixup, addend, GOT/PLT, ICF, section GC, weak symbol | Quem resolve a referência e quando seu valor pode mudar? |
| Loading | mmap, W^X, load bias, ASLR, instruction cache, dynamic binding | Quem mapeia, corrige, protege e começa a executar? |
| Sistemas | syscall ABI, freestanding, MMIO, barriers, interrupts, FFI | Quais garantias vêm do SO, do hardware ou do runtime? |
| Engenharia | differential fuzzing, delta debugging, metamorphic testing, atomic publication | Qual evidência independente sustenta a conclusão? |

Para ELF, leia a especificação genérica do formato junto do psABI da arquitetura, distinguindo
formato de arquivo e calling convention. A [especificação AAELF64 da Arm](https://github.com/ARM-software/abi-aa/blob/main/aaelf64/aaelf64.rst)
é uma referência primária para ELF/relocations AArch64; a [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)
define convenções de procedimento para a arquitetura. Confirme também as regras específicas
da plataforma, especialmente ao atravessar para macOS. Use os manuais da ISA para encoding;
um tutorial de assembly não substitui as tabelas de campos e limites da instrução.

Como estudo comparativo, investigue TCC para compilação rápida, QBE para IR/backend compacto,
Cranelift para engenharia de IR e geração, lld/mold para linking e ferramentas LLVM/binutils
para inspeção. Esses projetos são referências de arquitetura; técnicas deles precisam de
justificativa e medição antes de serem transplantadas para o Modal.

Você domina um tema quando consegue prever o resultado, localizar o contrato, construir um
contraexemplo, explicar uma falha e validar uma mudança sem depender apenas de um caso feliz.

## 25. Leitura defensiva de binários: validar antes de interpretar

Reconhecer o magic de ELF não valida o arquivo inteiro. Um leitor precisa conferir todos os
intervalos antes de formar ponteiros, interpretar estruturas ou percorrer tabelas. Estude essa
questão em `tools/stencilgen.c`, `tools/elfrun.c` e no leitor do cache; os três têm entradas e
pressupostos diferentes. Objeto produzido pela toolchain de build, imagem de teste e cache
persistido não têm necessariamente o mesmo grau de controle sobre sua origem.

Uma estratégia de implementação para uma ferramenta de estudo é centralizar verificação de
intervalos e decodificação de campos. Para uma tabela, primeiro prove que o cabeçalho existe,
depois valide `offset`, `count` e `entry_size`; só então acesse seus elementos.

```c
/* Pré-condição: total é o tamanho real do buffer. Não forma ponteiros. */
static int table_fits(size_t total, size_t offset, size_t count, size_t entry_size)
{
    return entry_size != 0 && offset <= total && count <= (total - offset) / entry_size;
}
```

Esse helper resolve apenas aritmética de intervalo. Um leitor ELF ainda precisa verificar
classe, endianness, tipo de entrada, tamanho esperado da estrutura, índices e relações entre
tabelas. Se o campo externo tiver mais bits que `size_t`, valide sua representabilidade antes
de converter. Para nomes em string tables, procure o terminador dentro do intervalo permitido;
não use `strlen` sobre bytes cuja terminação ainda não foi provada.

| Dado recebido | Validação a investigar |
|---|---|
| Arquivo truncado | Todo campo lido cabe no buffer? |
| Contagem/tamanho muito grande | A multiplicação e a conversão cabem antes de alocar/iterar? |
| Índice de seção/símbolo | Refere uma entrada válida da tabela correspondente? |
| Relocation | O campo corrigido cabe no fragmento e o tipo é suportado? |
| Referência textual | Offset e terminação pertencem à string table correta? |
| Entry ou destino | Aponta para região adequada ao uso, com alinhamento compatível? |

**Exercício:** parta de um objeto válido e altere uma dimensão por vez: corte o último byte,
troque contagem, índice, tamanho de entrada e tipo de relocation. Registre onde deveria ocorrer
a recusa. Essa é uma proposta de auditoria; não uma afirmação de que os leitores atuais já
validam todos esses casos. Nunca valide uma imagem desconhecida executando-a.

**Palavras-chave:** defensive parsing, bounds checking, checked conversion, integer overflow,
untrusted input, parser hardening, fuzz corpus, malformed binary, length-prefixed data.

## 26. Contratos entre fases e verificadores de representação

O compilador fica mais fácil de modificar quando cada fronteira deixa claro o que foi provado
e o que o consumidor ainda precisa verificar. Um tipo aceito pelo checker não pode ser tratado
como totalmente implementado se o backend ainda recusa sua geração. Uma referência válida
durante o corpo não se torna persistente só porque foi copiada para outra estrutura.

| Fronteira | Contrato que vale documentar e conferir |
|---|---|
| Fonte → tokens | Todos os spans pertencem à fonte; EOF e corpos adiados têm representação coerente. |
| Parser → AST | IDs/filhos/listas são válidos; nós têm a forma exigida por seu kind. |
| Resolução → checagem | Referências identificam declarações; escopo e ownership estão definidos. |
| Checagem → backend | Tipos e referências necessários existem; erros não viram código silenciosamente. |
| Lowering → otimização | Operandos, variantes, labels e frames obedecem ao contrato MIR. |
| Otimização → emissão | Fluxo, efeitos e resultados permanecem válidos após transformação. |
| Emissão → writer | Entry, offsets, tamanho de código e fixups resolvidos são consistentes. |
| Writer → loader | Cabeçalhos, segmentos, permissões e endereços descrevem os bytes realmente emitidos. |

Uma técnica útil é um verificador de IR para builds de desenvolvimento: validar estrutura
antes/depois do passe suspeito e localizar a primeira quebra. Ele não substitui testes de
semântica e não deve virar custo obrigatório de release sem medição. Antes de implementá-lo,
defina os invariantes com base nas representações reais; não imponha SSA a um MIR baseado em slots.

**Exercício:** escreva cinco invariantes de uma função MIR e construa uma violação mínima de
cada um numa ferramenta isolada. Classifique a resposta: erro de entrada do usuário, limitação
suportada com diagnóstico ou bug interno. Evite usar `assert` como única validação de entrada
externa: configuração de build pode desabilitá-lo.

**Palavras-chave:** representation invariant, IR verifier, design by contract, internal error,
phase boundary, structural validation, semantic preservation, fail-fast debugging.

## 27. Limites de recursos e comportamento no pior caso

Um compilador também deve ser estudado como programa que consome recursos em função da entrada.
Uma fonte pequena pode induzir grande profundidade de parsing, muitas especializações ou
avaliação longa. Compile time determinístico não significa barato nem limitado automaticamente.

Catalogue limites explícitos, como `MAX_UNITS`, `MAX_DOMAINS`, capacidade dos IDs e budgets de
comptime. Para cada um, investigue: quem verifica antes de incrementar/alocar, qual diagnóstico
aparece e se o estado permanece utilizável após a recusa. Não aumente uma constante sem entender
índices compactos, máscaras, tabelas paralelas e custos que dependem dela.

Separe cinco casos: recursão C do parser/checker, frames explícitos de comptime, expansão de
especializações, memória do compilador e stack do programa gerado. Proteger apenas um deixa
os demais independentes. Trate ainda ciclos de import/calls e algoritmos que fazem trabalho
quadrático sem recursão profunda.

**Experimento escalonado:** gere entradas com profundidade/tamanho crescentes, uma dimensão
por vez, e use um supervisor com timeout e limite de recursos apropriados ao host. Registre
tempo, memória, status e ponto de recusa. Evite testar esgotamento sem limites na sessão de
trabalho principal. O objetivo é encontrar crescimento e erro controlado, não derrubar a máquina.

**Palavras-chave:** resource budget, recursion limit, stack exhaustion, algorithmic complexity,
specialization explosion, worst-case input, graceful failure, bounded evaluation, capacity limit.

## 28. Oráculos de teste: o que cada resultado realmente demonstra

Um oráculo decide o que conta como resultado correto. O C backend é referência diferencial
do projeto, mas pode compartilhar uma interpretação errada com os demais caminhos. Por isso,
combine comparação entre backends com casos cujo resultado você calculou independentemente.

| Oráculo | Detecta bem | Pode deixar passar |
|---|---|---|
| Saída/status esperados | Regressão observável de um programa. | Recurso removido pelo folding antes de ser exercitado. |
| Diferencial entre backends | Implementações que discordam. | Bug comum no checker/lowering ou expectativa de linguagem errada. |
| Vetor de bytes conhecido | Campo/encoding/relocation específico incorreto. | Arquivo estruturalmente correto que não carrega no alvo. |
| Leitor/desmontador independente | Formato e instruções interpretados por outra implementação. | ABI, efeitos e caminhos não executados. |
| Sanitizers | Classes de falhas no código instrumentado. | Código de máquina gerado e bugs fora dos checks habilitados. |
| Relação metamórfica | Inconsistência após transformação semanticamente válida. | Transformação escolhida com pressuposto errado. |

Para uma regressão, inclua o caso mínimo, fronteiras próximas e um caso que bloqueie a
otimização em análise. Um teste de bounds precisa cobrir dentro, exatamente no limite e fora;
um teste de relocation precisa cobrir destino anterior, posterior e fora de alcance.
Não compare assembly inteiro quando apenas uma propriedade estrutural importa: snapshots
grandes podem prender otimizações legítimas e tornar a revisão pouco informativa.

Mantenha a distinção entre “checker aceita”, “gera código”, “arquivo é interpretável”,
“loader aceita” e “programa executa corretamente”. São marcos diferentes, especialmente em
cross-compilation. Um teste pulado não equivale a um teste aprovado.

**Exercício:** escolha uma mudança de backend e escreva três testes com oráculos diferentes.
Explique um bug que cada teste pega e os outros dois poderiam não pegar.

**Palavras-chave:** test oracle, independent implementation, common-mode failure, boundary test,
golden test, structural assertion, mutation testing, coverage gap, skipped target.

## 29. Reprodutibilidade e preservação de artefatos úteis

Reproduzir um bug significa reconstruir as condições relevantes. Reproduzir um build byte a
byte é uma propriedade mais forte e diferente de obter o mesmo comportamento. Caminhos,
nome do executável, metadados, toolchain e configuração podem participar do resultado.
Antes de chamar uma diferença de nondeterminismo, identifique quais entradas mudaram.

Para investigar um problema, preserve um pacote mínimo:

- Fonte reduzida e imports necessários, incluindo configuração do domínio.
- Comando, diretório, variáveis relevantes, host/target e versões de ferramentas.
- Revisão e diff local pertinente; commit sozinho não representa árvore com mudanças locais.
- Saída/status separados, MIR ou C gerado quando relevante e binário que apresentou a falha.
- Seed/corpus do fuzzer e `.traps` correspondente ao executável, quando produzido.

Não salve todo o ambiente: ele pode conter credenciais e ruído. Registre apenas variáveis
que influenciam o experimento, como `CC`, `MODAL_STD`, opções de modo e configuração do target.
Um arquivo `.traps` de outro build pode atribuir o número ao local errado.

**Experimento:** compile duas vezes em condições iguais e compare bytes; depois altere uma
entrada controlada. Para Mach-O, considere que o writer recebe um identificador derivado do
nome de saída. Compare também comportamento separadamente. Se os bytes diferirem, localize
a primeira região diferente e classifique código, layout, metadados ou assinatura.

Em builds longos, geração atômica evita que outro processo leia um header ou executável pela
metade. O makefile já usa arquivo temporário e rename nos headers de stencils; o driver também
tem publicação de executáveis em cache. Estude quais garantias cada fluxo fornece, incluindo
falha entre escrita e rename, em vez de presumir que qualquer arquivo temporário resolve tudo.

**Palavras-chave:** reproducible build, deterministic output, build manifest, provenance,
artifact identity, byte-for-byte comparison, atomic publication, cache poisoning, stale artifact.

## 30. Como revisar mudanças críticas e demonstrar domínio

Revise uma transformação pelo argumento que a torna correta, não só pelo número de testes.
Use estes cenários como roteiro de leitura e exercício; não como exigência de executar trabalho
sem relação com a mudança.

| Mudança | Pergunta técnica central | Evidência útil |
|---|---|---|
| Eliminar bounds check | Qual fato prova o intervalo e quem pode invalidá-lo? | Caso seguro, índice reatribuído e efeito de chamada/alias. |
| Remover cópia | O destino pode sobrepor a origem ou escapar? | Cópia com overlap, referência viva e agregado retornado. |
| Reutilizar frame | Algum argumento aponta para o frame descartado? | Loop longo, referência ao local e chamadas entre funções. |
| Compartilhar código (ICF) | Bytes e referências têm significado equivalente? | Funções iguais com callees/externs diferentes. |
| Reordenar/empaquetar código | Todos os locais e destinos foram remapeados? | Calls antes/depois, função removida e entry remanejada. |
| Reusar cache | Todas as dependências que alteram resultado estão representadas? | Matriz de invalidação e execução com cache frio/quente. |
| Ampliar layout/alvo | Quais pressupostos de largura e alinhamento mudaram? | Limites de offset, agregados e target distinto do host. |

Para discutir uma proposta, escreva uma nota curta com problema, invariante, mecanismo,
contraexemplos, custo esperado e experimento que decide se vale implementá-la. Separe os estados
**observado**, **inferido**, **proposto** e **não verificado**. Um número estimado pode orientar
o experimento, mas não deve entrar nas tabelas de desempenho como medição.

Uma autoavaliação prática para cada subsistema:

1. Consigo explicar a representação e o contrato sem depender dos nomes das funções?
2. Consigo acompanhar um valor e uma falha de ponta a ponta?
3. Sei construir uma entrada que invalide a otimização ingênua?
4. Sei qual ferramenta observa a primeira fronteira incorreta?
5. Consigo demonstrar correção e custo sem confundir compilador, programa gerado e loader?

Se faltar uma resposta, transforme-a no próximo exercício. Isso mantém o estudo orientado
a conhecimento verificável e evita aumentar o documento apenas com nomes de técnicas.
