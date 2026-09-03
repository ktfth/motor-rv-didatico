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
