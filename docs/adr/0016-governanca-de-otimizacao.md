# ADR-0016 — Governança de otimização: pré-aprovadas vs. experimentos

Status: aceito (02/09/2026)

## Contexto
Otimização prematura disfarçada de engenharia é o risco principal de um projeto 'com otimizações na veia'. A distinção precisa ser mecânica, não de bom senso.

## Decisão
Pré-aprovadas (implementar e medir para baseline): thread-per-core, SPSC rings, SoA, ponto fixo, SBE, group commit com O_DSYNC, snapshot pré-serializado, JSON só na borda, JWKS/token cache, -O3/LTO. Experimentos (atrás de flag, com baseline → medição → replay intacto → ADR): SQPOLL, reescrita de cauda, kTLS, huge pages, PGO, snapshot fuzzy, buffer rings. Ganho abaixo do limiar de bench/baseline.json ou complexidade fora do hot path → rejeitado e registrado.

## Alternativas consideradas
Deixar a critério do implementador; revisar caso a caso sem baseline.

## Consequências
desempenho é o único a escrever bench/baseline.json e tem veto sobre PRs de hot path; toda rejeição vira ADR para não voltar.
