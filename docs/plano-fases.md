# Plano de fases

Uma fase por vez. Critério de saída = gates dos agentes envolvidos + relatório do verificador
aprovado. O orquestrador registra status e data ao fim de cada fase.

## Fase 1 — tipos, SBE, invariantes
Entram: toolchain → dominio-pos-negociacao → nucleo → verificador.
Entregas: presets de build e sanitizers; `docs/invariantes.md` com I1–I7 testados em
`tests/domain`; `schema/*.xml` v1 e codecs gerados; tipos de ponto fixo com testes de
arredondamento; estrutura de `src/core` sem partição ainda.
Saída: build zero warnings em todos os presets; I1–I7 com teste; verificador aprovado.
Status: pendente.

## Fase 2 — partição e WAL
Entram: nucleo → persistencia → desempenho → verificador.
Entregas: loop single-writer com SPSC ring; `apply` para eventos de negócio; WAL completo
(formato, group commit, io_uring, recuperação); suíte de crash; baseline inicial.
Saída: I8–I12 com teste; suíte de crash verde; `bench/baseline.json` preenchido; verificador
aprovado.
Status: pendente.

## Fase 3 — liquidação e eventos corporativos
Entram: dominio-pos-negociacao → nucleo → verificador.
Entregas: máquina de estados completa (D+0 a D+2, falha de entrega); eventos corporativos;
reconciliação; cenários golden com datas reais.
Saída: todos os cenários verdes; sem regressão contra baseline; verificador aprovado.
Status: pendente.

## Fase 4 — snapshot e resource server
Entram: persistencia → borda-fapi → regulatorio-open-finance → verificador.
Entregas: snapshot EOD (recuperação + exposição), manifesto, publicação atômica; resource
server com pipeline FAPI; contract tests; suíte negativa e fuzz.
Saída: R1–R19 com teste; snapshot versionado; verificador aprovado.
Status: pendente.

## Fase 5 — conformance
Entram: regulatorio-open-finance → borda-fapi → verificador.
Entregas: execução da suíte funcional do OFB contra o resource server (com AS de prateleira
em ambiente de teste); evidências arquivadas.
Saída: relatório de conformance sem falhas bloqueantes.
Status: pendente.
