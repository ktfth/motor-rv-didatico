# ADR-0005 — Thread-per-core, shared-nothing, partição por hash do documento

Status: aceito (02/09/2026)

## Contexto
Latência previsível exige ausência de contenção. Um investidor nunca precisa cruzar partição para posição, liquidação ou eventos corporativos.

## Decisão
Uma thread pinada por core, uma partição por thread, estado e WAL próprios, comunicação por SPSC rings; partição fixa por hash(CPF/CNPJ).

## Alternativas consideradas
Pool de threads com locks por conta; ator por conta sobre runtime genérico.

## Consequências
Rebalanceamento por replay do log da partição em outro core; mensagens entre partições (raras) gateadas por durable_lsn.
