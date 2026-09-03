# ADR-0011 — Apuração de IR (RFB) como módulo separado

Status: aceito (02/09/2026)

## Contexto
IR sobre ganhos é regra da RFB, não do BCB nem da B3. Acoplar regime tributário ao ledger contamina o núcleo com regras que mudam por outro calendário.

## Decisão
Módulo consumidor do log (somente leitura) com suas próprias regras e testes; nunca escreve eventos no WAL.

## Alternativas consideradas
Calcular IR dentro do apply.

## Consequências
Fica fora das fases 1–5; entra quando houver demanda, com ADR próprio.
