# ADR-0004 — Event sourcing com replay determinístico como fonte de verdade

Status: aceito (02/09/2026)

## Contexto
Reconciliação com a B3, trilha de auditoria e reconstrução de qualquer data exigem que o estado seja derivável do histórico. CRUD sobre posição perde a causa de cada saldo.

## Decisão
Log append-only de eventos por partição; estado em memória derivado por apply puro; snapshots como cache do prefixo. Todo não-determinismo entra como evento.

## Alternativas consideradas
CRUD com tabela de posição + histórico separado.

## Consequências
Regras de determinismo em CODING_RULES (sem relógio/RNG/I/O no apply); custo de replay limitado por snapshots; WAL como trilha de auditoria com retenção regulatória.
