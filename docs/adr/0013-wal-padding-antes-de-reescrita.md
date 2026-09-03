# ADR-0013 — Padding de bloco antes de reescrita de cauda

Status: aceito (02/09/2026)

## Contexto
O_DIRECT exige escrita de blocos inteiros. Reescrever o bloco de cauda economiza espaço mas depende da atomicidade de setor e complica a recuperação.

## Decisão
v1 preenche o último bloco de cada grupo com zeros. Reescrita de cauda é experimento a medir pelo desempenho (desperdício de espaço vs. amplificação).

## Alternativas consideradas
Reescrita de cauda desde o início.

## Consequências
Espaço desperdiçado proporcional ao número de grupos pequenos; medir antes de decidir.
