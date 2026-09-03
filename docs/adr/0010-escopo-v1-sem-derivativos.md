# ADR-0010 — Escopo v1 sem opções, termo ou BTC

Status: aceito (02/09/2026)

## Contexto
Derivativos e empréstimo de ativos trazem margem, garantias e risco da câmara — outro motor, outras regras.

## Decisão
v1 cobre ações à vista (lote padrão e fracionário), ETF, FII e BDR como tipos de instrumento no mesmo modelo de custódia.

## Alternativas consideradas
Cobrir opções desde o início.

## Consequências
O tipo de instrumento é um enum extensível; o bucket 'bloqueado' já existe para garantias futuras.
