# Plano de fases

Uma fase por vez. Critério de saída = gates dos agentes envolvidos + relatório do verificador
aprovado. O orquestrador registra status e data ao fim de cada fase.

## Fase 0 — reconhecimento do terreno
Entrou: orquestrador.
Entregas: `docs/ambiente.md` com os fatos MEDIDOS da máquina (io_uring, O_DIRECT, ISA, bibliotecas
ausentes); ADR-0017 a ADR-0023 para cada divergência que o ambiente impôs; `scripts/probe-ambiente.sh`
e `scripts/bootstrap-toolchain.sh`; correção do enunciado de I1 e criação de I13; os 14 cenários
golden de `tests/domain/golden/` com números fechados à mão ANTES de existir código; calendário de
pregões da B3 2026 e tabela de instrumentos em `data/`.
Saída: build de C++23 verificado nos dois compiladores com `-Werror`; domínio especificado.
Status: **concluída (02/09/2026)**.

## Fase 1 — tipos, SBE, invariantes
Entram: toolchain → dominio-pos-negociacao → nucleo → verificador.
Entregas: presets de build e sanitizers; `docs/invariantes.md` com I1–I7 testados em
`tests/domain`; `schema/*.xml` v1 e codecs gerados; tipos de ponto fixo com testes de
arredondamento; estrutura de `src/core` sem partição ainda.
Saída: build zero warnings em todos os presets; I1–I7 com teste; verificador aprovado.
Status: **concluída (02/09/2026)**. Presets `debug`, `release`, `clang-release`, `asan`, `tsan`,
`fuzz` e `nativo` configuram e compilam; `scripts/sbe_gen.py` gera os dez codecs a partir de
`schema/events.xml` com `static_assert` de tamanho, alinhamento e deslocamento de cada campo;
ponto fixo e políticas de arredondamento com os números dos cenários golden verificados por teste;
`src/core` com ledgers SoA, máquina de estados gerada de uma declaração única e `apply` completo.
`scripts/check_invariants.py` confirma **13 de 13** invariantes com teste vinculado.

## Fase 2 — partição e WAL
Entram: nucleo → persistencia → desempenho → verificador.
Entregas: loop single-writer com SPSC ring; `apply` para eventos de negócio; WAL completo
(formato, group commit, io_uring, recuperação); suíte de crash; baseline inicial.
Saída: I8–I12 com teste; suíte de crash verde; `bench/baseline.json` preenchido; verificador
aprovado.
Status: **em andamento**. Já entregues: loop single-writer com SPSC ring (2 M mensagens/28 ms sem
erro de ordem), `apply` para todos os eventos de negócio, imagem de recuperação por stall-and-copy
com I11 verificado em seis pontos de corte, portão de saída de I10 com contrapressão. Pendentes:
WAL completo (formato, group commit, io_uring, recuperação), suíte de crash e baseline.

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
