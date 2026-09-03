# ADR-0014 — Snapshot EOD por stall-and-copy; fuzzy por chunks como experimento; fork() descartado

Status: aceito (02/09/2026)

## Contexto
O snapshot precisa ser exato no LSN do EodMark. Com mercado fechado, um stall de dezenas de ms uma vez por dia é aceitável; snapshots intradiários exigiriam outra técnica.

## Decisão
v1: pausar o loop, copiar arenas SoA para staging, retomar; thread auxiliar grava. v2 (experimento): chunks com chunk_lsn e replay filtrado por slot.

## Alternativas consideradas
fork() com COW (huge pages copiam 2 MiB por fault; filho não pode tocar malloc); estruturas persistentes.

## Consequências
RTO com snapshot só no EOD ≈ replay do dia; é o número que decide a v2.
