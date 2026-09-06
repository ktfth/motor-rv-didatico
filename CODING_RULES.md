# CODING_RULES — motor-rv

Aplicam-se a `src/`. O `verificador` revisa contra este arquivo; `toolchain` automatiza o que
for automatizável (clang-tidy, sanitizers, flags).

## Hot path (núcleo, WAL, apply)

1. Sem alocação após warm-up. Arenas por partição, pools de eventos, `std::pmr` onde a API pedir.
2. Sem `double`/`float` em dinheiro, quantidade ou preço. `Fixed<int64_t, escala>`; produtos e
   somas em `__int128`; política de arredondamento explícita por operação (nome da função diz qual).
3. Sem relógio (`std::chrono::system_clock`, `CLOCK_REALTIME`), RNG ou I/O externo dentro de
   `apply` e do replay. Timestamp de auditoria vem do evento.
4. Sem exceções no hot path (`-fno-exceptions` nos alvos de núcleo). Erro de invariante em
   build debug é `assert`; em release, métrica + fail-stop da partição quando a violação for de
   durabilidade ou de ledger.
5. Sem locks. Único ponto de atomics: cursores dos SPSC rings, em cache lines separadas.
6. Codecs SBE gerados por `sbe-tool` a partir de `schema/*.xml`. Nunca escritos à mão.
7. Layout SoA para estado de posição; IDs internados (`uint32`); índices densos (open addressing).
8. Structs de mensagem com `static_assert` de tamanho e alinhamento.

## Geral

9. C++23. `-Wall -Wextra -Wpedantic -Werror`. Sanitizers (ASan, UBSan, TSan) verdes antes de
   qualquer benchmark contar.
10. Direção de dependência: `edge` lê o formato de snapshot (`wal/snapshot_format.hpp`), nunca
    `core` internals. `wal` conhece `core` só pela interface de `apply`. `core` não conhece
    ninguém.
11. Todo formato persistente (WAL, snapshot, manifesto) tem versão no cabeçalho; mudança =
    bump + teste de leitura da versão anterior.
12. Teste de propriedade para cada invariante numerado; teste de equivalência de replay para
    cada tipo de evento novo.
13. Nada de `TODO` sem issue/ADR referenciado. Nada de otimização sem número em
    `bench/baseline.json` ou em `bench/reports/`.
14. Segurança: parsers de entrada externa (JWS, JSON, TLS) têm harness de fuzz em `tests/edge`;
    limites explícitos de tamanho antes de qualquer parse.

## Conflitos entre regras, e como foram resolvidos

Regra que contradiz outra regra não se resolve por bom senso na hora: resolve-se uma vez, por
escrito, com o motivo. A lista é curta de propósito.

### §2 (`__int128`) contra §9 (`-Wpedantic -Werror`)

`__int128` é extensão de compilador; `-Wpedantic` a recusa. Descoberto na primeira compilação de
`src/base/rounding.hpp`. **Resolução:** a extensão é isolada em `src/base/int128.hpp`, que silencia
o aviso apenas naquele arquivo e exporta `rv::i128` / `rv::u128`. O resto do projeto compila com
`-Wpedantic` de verdade e nunca escreve `__int128` diretamente. As alternativas descartadas — e por
quê — estão no cabeçalho do próprio arquivo.

### §4 (`-fno-exceptions` no hot path) contra o uso de GoogleTest

Os binários de teste compilam **com** exceções e linkam bibliotecas compiladas **sem** elas. É
seguro porque nenhuma das bibliotecas do núcleo lança — a flag remove o mecanismo, não altera a
ABI das funções que já eram `noexcept`. O que isso permite: `EXPECT_DEATH` e `EXPECT_THROW` nos
testes de pré-condição, sem afrouxar a regra no código de produção.

### CLAUDE.md §Idioma contra o português de `bench/`, `src/app/` e do simulador de `src/ingress/`

Descoberto ao revisar `bench/`: o diretório é português inteiro (`Serie`, `medir`, `Carga`,
`Ambiente`), enquanto `src/base`, `src/codec`, `src/core`, `src/format` e `src/wal` são inglês
inteiro. `src/app/` e o simulador de `src/ingress/` divergem do mesmo jeito e há mais tempo
(`Instrumento`, `DiaDePregao`, `EventoRoteado`, `ConfigSimulacao`).

**Resolução:** a regra vale, e o escopo dela é o **código do motor** — as camadas cujos
identificadores são internos e nunca saem do binário: `base`, `codec`, `core`, `format`, `wal`,
`edge`. Ali o vocabulário vem das especificações que o projeto implementa (SBE, WAL, ledger, FAPI,
Open Finance), que são em inglês; traduzi-lo cobraria uma tradução em cada leitura cruzada com a
especificação, que é a leitura que mais acontece nessas camadas.

Ela **não** vale para as camadas de ferramenta — `bench/`, `src/app/` e o simulador de
`src/ingress/` — pelo que as distingue: ali o identificador **é** o vocabulário publicado.
`--negocios` é opção de linha de comando; `carga` e `metricas` são chaves do JSON que o comparador
lê e que `bench/baseline.json` versiona; `Instrumento` e `DiaDePregao` são as colunas dos CSVs da
B3. CLI, esquema de arquivo e dado de entrada são **documento**, e documento é português pela mesma
frase de CLAUDE.md que pede inglês nos identificadores. Chamar `Carga` de `Load` enquanto a chave
do JSON continua `carga` instalaria uma tradução por par nome↔chave — a mesma duplicação de verdade
que `bench/contrato.hpp` acabou de tirar deste diretório, reintroduzida por regra.

Daqui em diante:

1. Código de motor: identificadores em inglês, sem exceção.
2. Camada de ferramenta: português no que nomeia algo publicado (opção de CLI, chave de arquivo,
   seção de relatório, coluna de CSV); inglês no resto, que é infraestrutura e não vocabulário —
   `Histogram`, vindo de `src/base`, continua `Histogram` dentro de `bench/`.
3. Nada de renomeação em massa. Isto vale para código novo e para o arquivo que já for reescrito
   por outro motivo; a divergência existente fica onde está.
4. `Partitioner` (inglês) conviver com `EventoRoteado` (português) dentro de `src/ingress/` é
   consequência aceita: o particionador é motor, o simulador é ferramenta. A linha que separa é a
   função da camada, não o diretório.

Descartadas: renomear `bench/` para inglês (churn grande, risco alto e — porque as chaves do JSON
continuariam em português — produziria justamente a tradução do parágrafo acima); e afrouxar
§Idioma para "tanto faz", que apagaria o motivo pelo qual `core` é inglês.

### §6 (`sbe-tool`) contra o ambiente

Não há JVM na máquina de referência. Resolvido por ADR-0017: gerador próprio em Python, com o
mesmo contrato — codecs gerados no build, nunca versionados, nunca editados à mão. A regra §6
continua valendo; mudou apenas quem a executa.
