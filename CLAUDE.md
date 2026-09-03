# motor-rv — contrato do projeto

Motor de renda variável (ações à vista, ETF/FII/BDR) em C++23: núcleo thread-per-core com
ledgers de custódia e financeiro, WAL com io_uring e group commit, snapshot EOD, e exposição
Open Finance (API Renda Variável v1.3.0) sob o perfil FAPI-BR v2.2.1.

Este arquivo é carregado por todo subagente. É o contrato mínimo; o detalhe está em `docs/`.

## Fontes de verdade — leia antes de agir, nesta ordem

1. `docs/adr/` — decisões fechadas. Mudar uma exige ADR novo; nunca commit direto.
2. `docs/invariantes.md` — I1…In. Invariante sem teste vinculado não existe.
3. `docs/rastreabilidade.md` — requisito regulatório → teste.
4. `CODING_RULES.md` — regras do hot path.
5. `bench/baseline.json` — números que qualquer PR respeita. Só `desempenho` escreve nele.
6. Especificações: `docs/arquitetura.md`, `docs/dominio.md`, `docs/wal.md`, `docs/open-finance.md`.
7. `docs/plano-fases.md` — ordem de construção e quem entra em cada fase.

## Papéis

- Orquestrador: sessão principal (`claude --agent orquestrador`, já configurado em
  `.claude/settings.json`). Delega, mantém ADRs e docs; não escreve código de produção.
- Subagentes: `dominio-pos-negociacao`, `regulatorio-open-finance`, `nucleo`, `persistencia`,
  `borda-fapi`, `desempenho`, `verificador`, `toolchain`.
- Donos de diretório em `.claude/ownership.json`, aplicado por hook `PreToolUse` em cada agente.
  O hook bloqueia com mensagem; a mensagem é a regra, não um obstáculo a contornar via Bash.

## Regras transversais

- Autor nunca verifica o próprio trabalho: todo merge passa por `verificador`.
- Vetos cruzados: hot path = `nucleo` + `desempenho`; camada FAPI = `borda-fapi` +
  `regulatorio-open-finance`; formato de WAL ou snapshot = `persistencia` + `verificador` com
  suíte de crash verde e bump de versão de formato.
- Otimização: a lista pré-aprovada (ADR-0016) implementa-se direto e mede-se para fixar baseline.
  Experimento entra atrás de flag: hipótese → baseline → medição no mesmo harness → replay e
  invariantes intactos → ADR com números, inclusive quando rejeitado.
- Determinismo: nenhum relógio, RNG ou leitura externa dentro do replay. Tudo entra como evento.
- Dinheiro e quantidade em ponto fixo `int64` (intermediários `__int128`). Nunca `double`.
- Idioma: documentos, ADRs e relatórios em português; identificadores de código em inglês.
- Fatos regulatórios: verificar a versão vigente na fonte oficial antes de cada fase
  (`regulatorio-open-finance` faz isso e registra em `docs/open-finance.md`).

## Relatório obrigatório ao terminar (todo subagente)

Formato em `docs/relatorio-template.md`. Cinco blocos: o que fez (arquivos), testes executados
com resultado, o que não fez, riscos e dúvidas, próximo passo sugerido. O orquestrador só delega
a etapa seguinte a partir desse relatório — nunca de memória.

## Comandos

Preenchidos por `toolchain` na fase 1 (presets de configure/build/test/bench). Até lá, nenhum
agente inventa comando de build.
