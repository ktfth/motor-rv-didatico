# ADR-0022 — Baseline de compilação `x86-64-v2`; `-march=native` só em preset opcional

Status: aceito (02/09/2026). Ajusta a linha de build de `docs/arquitetura.md`.

## Contexto
`docs/arquitetura.md` previa `-O3 -march=x86-64-v3 -flto`. A máquina de referência é um Sandy Bridge
(i7-2600): tem SSE4.2 e PCLMULQDQ, **não tem AVX2 nem BMI2**. `x86-64-v3` exige AVX2: o binário
compila e morre com SIGILL na primeira instrução vetorial larga. Descobrir isso em produção é caro;
descobrir no ADR é de graça.

## Decisão
O baseline dos presets é `-O3 -march=x86-64-v2 -flto`. `x86-64-v2` já garante SSE4.2, ou seja, o
`_mm_crc32_u64` do CRC32C do WAL — que é a única instrução específica de que o hot path depende.
Existe um preset `nativo` com `-march=native` para medir o ganho em máquina moderna, e o harness de
bench grava as flags dentro de `bench/baseline.json`: número medido com flags diferentes não se
compara.

## Alternativas consideradas
- Manter `x86-64-v3` e trocar de máquina: adiaria toda a fase 2 por hardware.
- *Function multiversioning* no CRC32C: complexidade sem ganho medido; o fallback por tabela já
  cobre a máquina sem SSE4.2, e a detecção é em tempo de carga, não por chamada.

## Consequências
Os números do baseline desta máquina são conservadores em relação a um servidor moderno — o que é
seguro na direção certa: uma regressão medida aqui é regressão de verdade. Ao promover o motor para
outra máquina, o primeiro ato é rodar `scripts/probe-ambiente.sh` e refazer o baseline.
