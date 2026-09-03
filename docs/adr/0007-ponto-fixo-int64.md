# ADR-0007 — Dinheiro, quantidade e preço em ponto fixo int64

Status: aceito (02/09/2026)

## Contexto
double introduz erro de representação em somas financeiras; libs decimais custam ordens de grandeza a mais no hot path.

## Decisão
Fixed<int64_t, escala>: 1e-8 para quantidade e preço, 1e-4 para BRL (a API expõe 4 casas); intermediários em __int128; arredondamento explícito por operação.

## Alternativas consideradas
double; decimal de software; int128 em todo lugar.

## Consequências
Conversão para string na borda via std::to_chars/fmt; testes de arredondamento por regra; nada de double em src/core (regra automatizada por clang-tidy).
