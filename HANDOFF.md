# HANDOFF — motor-rv

Pacote de transferência do desenho (chat de setembro/2026) para o Claude Code. Contém o
contrato do projeto, oito subagentes com donos e gates, hook de fronteira de diretórios, ADRs
das decisões fechadas, especificações e os dois diagramas.

## Como iniciar

1. Descompacte na raiz do repositório (novo ou vazio) e faça o commit inicial.
2. `claude` na raiz. `.claude/settings.json` já define `agent: orquestrador`, então a sessão
   principal nasce como orquestrador (prompt próprio, sem editar `src/`). Aceite o diálogo de
   confiança do diretório — sem ele os hooks de frontmatter dos agentes não rodam.
3. Confirme os agentes carregados: `/agents`. Devem aparecer os oito mais `orquestrador`.
4. Primeiro prompt sugerido:

   > Leia HANDOFF.md, CLAUDE.md e docs/plano-fases.md. Estamos na fase 1. Delegue ao
   > toolchain a criação dos presets de build e ao dominio-pos-negociacao o fechamento de
   > docs/invariantes.md com testes vinculados. Traga os dois relatórios antes de seguir.

5. A partir daí, uma fase por vez, sempre com relatório no formato de
   `docs/relatorio-template.md` antes de delegar a próxima etapa.

## O que está aqui

| Caminho | Conteúdo | Dono |
|---|---|---|
| `CLAUDE.md` | Contrato mínimo, carregado por todo agente | orquestrador |
| `CODING_RULES.md` | Regras do hot path e gerais | orquestrador |
| `.claude/agents/*.md` | Nove agentes (orquestrador + oito) | orquestrador |
| `.claude/ownership.json` | Donos de diretório e arquivos protegidos | orquestrador |
| `.claude/hooks/guard_paths.py` | Hook `PreToolUse` que aplica `ownership.json` | orquestrador |
| `docs/adr/` | ADR-0001 a ADR-0016 | orquestrador (imutáveis após aceitos) |
| `docs/invariantes.md` | I1…I12, todos ainda sem teste | dominio-pos-negociacao |
| `docs/rastreabilidade.md` | Requisitos FAPI-BR/API RV/RNF → teste | regulatorio-open-finance |
| `docs/arquitetura.md` | Planos, pipeline FAPI, otimizações, diagrama Mermaid | orquestrador |
| `docs/dominio.md` | Ledgers, máquina de estados, eventos corporativos | dominio-pos-negociacao |
| `docs/wal.md` | WAL, group commit, io_uring, recuperação, snapshot | persistencia |
| `docs/open-finance.md` | Perfil FAPI-BR, endpoints, SLA, limites, fontes | regulatorio-open-finance |
| `docs/plano-fases.md` | Cinco fases, quem entra, critério de saída | orquestrador |
| `docs/diagrams/` | Arquitetura e máquina de estados (SVG + PNG 2x) | orquestrador |
| `bench/baseline.json` | Vazio até a fase 2 | desempenho |
| `schema/` | SBE (`*.xml`) | dominio + nucleo |

## O que ainda não existe

- Nenhum código C++. O desenho está fechado; a implementação começa na fase 1.
- Nenhum teste vinculado aos invariantes — é o primeiro gate do `dominio-pos-negociacao`.
- Baseline de desempenho — fixado pelo `desempenho` na fase 2, nunca antes.
- Verificação da versão vigente do FAPI-BR e da API RV na data de início — primeiro ato do
  `regulatorio-open-finance` (as versões aqui são de 02/09/2026).

## Limites conhecidos do hook

O hook bloqueia `Edit`/`Write` fora do diretório do papel e escrita em arquivos protegidos, e
inspeciona comandos `Bash` por redirecionamento ou `sed -i`/`tee`/`rm` sobre caminhos
protegidos. Não é sandbox: um agente que quisesse contornar conseguiria. A instrução no prompt
de cada agente é a regra; o hook é o freio e o registro.
