# HANDOFF — motor-rv

## Atualização de 11/09/2026 — rejeição do clearing paralelo

O experimento local `src/clearing/` foi removido após revisão cética. Ele duplicava a máquina de
estados de `src/core`, criava um segundo formato de snapshot, usava um vetor como falso WAL e
comparava duas implementações quase idênticas como se fossem oráculos independentes. Endurecer esse
caminho aumentaria a superfície de manutenção sem elevar a confiança no motor.

A proposição mantida é uma só: liquidação e recuperação evoluem no caminho existente
`core -> wal -> recovery`, com os cenários fechados à mão em `tests/domain/golden/` e falhas físicas
em `tests/chaos/`. Após a remoção, os **18/18 testes** passam em `debug`, ASan/UBSan e
`clang-release`. A próxima ação deve nascer de uma lacuna reproduzida nesse caminho, não da criação
de um subsistema concorrente.

Estado em **06/09/2026** (o corpo é de 03/09; a medição inicial e o harness de bench entraram em
06/09). Este arquivo substitui o pacote de transferência original (o desenho sem código), que está
preservado em `../raw/`.

## O que existe e funciona

Sete presets de build configuram, compilam e passam os testes:

| preset | o que é |
|---|---|
| `debug` | asserts de invariante ligados |
| `release` | baseline oficial: `x86-64-v2` + LTO (ADR-0022) |
| `clang-release` | segundo compilador, mesmo baseline |
| `asan` / `tsan` / `fuzz` | sanitizers e libFuzzer |
| `nativo` | `-march=native`, só para experimento |

O CI tem **dois workflows**, porque correção e desempenho falham por motivos diferentes e em
ritmos diferentes — misturá-los faz um teste quebrado e um benchmark barulhento aparecerem como a
mesma coisa vermelha:

- `.github/workflows/ci.yml` — matriz de presets, os gates do projeto e o clang-tidy.
- `.github/workflows/bench.yml` — medição. O job `verificacao` prova que o harness e o comparador
  funcionam (veredito determinístico, sem depender da velocidade do runner); o job `medicao`
  compila as DUAS árvores de um PR e mede os dois lados intercalados, **cinco vezes cada**, no
  mesmo runner, publicando o relatório no resumo do job e nos artefatos. O critério de regressão e
  quem pode reprovar estão em ADR-0027 e ADR-0028, com os números que os calibraram.

```sh
./scripts/bootstrap-toolchain.sh && export PATH="$PWD/.toolchain/bin:$PATH"
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
python3 scripts/check_invariants.py build/debug     # 13/13 invariantes com teste
./build/release/src/app/motor-rv-sim --dias 3 --negocios 20000 --investidores 2000
./build/release/bench/motor-rv-bench --repeticoes 15   # medição (ADR-0021)
```

O último comando roda três pregões por quatro partições — ~120 mil eventos — e imprime aceitos,
rejeitados por motivo (com conferência de soma), os checksums de cada partição e o tamanho da
imagem de recuperação. Mesma semente, mesmo resultado.

### Camadas prontas

| Camada | Conteúdo |
|---|---|
| `src/base/` | ponto fixo com unidades tipadas, três políticas de arredondamento, `Status`/`Result` sem exceção, ids fortes, `DateYmd` puro, CRC32C (hardware + tabela), arena selável, SPSC ring, índice denso, índice de par exato, conjunto de idempotência de duas gerações, métricas com histograma log-linear, `i128` isolado |
| `src/codec/` | runtime SBE + **gerador próprio** (`scripts/sbe_gen.py`, ADR-0017) que emite `static_assert` de tamanho, alinhamento e deslocamento de cada campo, e recusa schema mal formado com mensagem que ensina |
| `src/core/` | ledgers SoA com bucket vencido, máquina de estados gerada de uma declaração única, `apply()` completo dos dez eventos, loop da partição, outbox com contrapressão, imagem de recuperação por stall-and-copy |
| `src/format/` | snapshot de exposição D-1: cabeçalho de 4096 bytes exatos, 27 seções, só offsets |
| `src/ingress/` | particionador congelado com valores golden, framing SBE canônico, streaming TCP chunked e simulador determinístico que lê o calendário real da B3 |
| `src/edge/` | **borda de observabilidade e SLA**: endpoints Open Finance (`endpoint.hpp`), auditoria regulatória R17 (latência P95 com handshake isolado da requisição), R18 (disponibilidade com 2XX/422 sucesso vs 5XX/408 erro), R19 (trilha de auditoria), coletor de métricas Prometheus 0.0.4 e status JSON zero-allocation (`observability.hpp`), e servidor HTTP/1.1 ultraleve não-bloqueante (`metrics_server.hpp`) atendendo `/metrics`, `/healthz`, `/ready` e `/status` |
| `src/app/` | `motor-rv` (servidor integrado com N partições, io_uring, ingress pipeline e servidor HTTP de métricas/healthcheck), `motor-rv-sim` e `motor-rv-wal-inspect` (inspeção e diagnóstico forense de integridade de segmentos WAL) |
| `src/wal/` | **completo**: formato (`WalHdr` 32 B, `SegmentHdr`), alinhamento dinâmico via `statx`, três backends de I/O (`io_uring`, `pwrite`, injeção de falhas `fault_backend`), `GroupCommit` com coalescência, leitor sequencial `SegmentReader`, recuperação e replay determinístico a quente (`wal::recover`) com tolerância a corrupção e cauda rasgada |
| `tests/chaos/` | suíte `test_recovery_chaos` com 9 cenários de falhas extremas (replay determinístico, cauda rasgada, snapshot + replay, falha de IO, corrupção de CRC, saltos de LSN, fallback de snapshot corrompido, replay de rejeições, blocos parciais truncados) |
| `tests/ingress/` | suíte `test_ingress_pipeline` cobrindo fragmentação TCP, roteamento de investidor por DocumentId, broadcast global e contrapressão |
| `tests/edge/` | suítes `test_r17_latency_slo` (isolamento de handshake e P95), `test_r18_availability_classes` (sucessos 2XX/422 vs falhas 5XX/408 e classificação por Stage) e `test_observability` (renderização de métricas Prometheus, status JSON e servidor HTTP com requisições reais via socket) |
| `bench/` | harness próprio (ADR-0021): aquecimento, descarte de série por CV, histograma sem alocação, bloco `ambiente` e `carga` preenchidos pelo programa, comparador que sai != 0 em regressão e recusa comparar cargas diferentes; métricas contratuais de WAL e Ingress conectadas |
| `scripts/` | `gate-local.sh` (pré-voo rápido e full), `quick-bench.sh`, `setup-hooks.sh`, `commit-and-handoff.sh`, hooks de `pre-commit` e `post-commit`, `check_invariants.py`, `gera-calendario.py`, `sbe_gen.py`, `relatorio-bench.py` |
| `bench/contrato.hpp` | a ÚNICA tabela de métricas: alimenta o JSON, o comparador e o relatório, e declara à parte quem pode reprovar um PR (ADR-0026, ADR-0028) |

### Testes — 18 testes, 13/13 invariantes

| Suíte | Cobre |
|---|---|
| `test_rounding_golden` | 18 casos: os números dos cenários golden pela aritmética |
| `test_apply_golden` | 16 casos: os mesmos cenários **ponta a ponta** pelo motor |
| `test_i2_cash_by_date` | I2 recomputando o lado direito da identidade |
| `test_replay_equivalence` | I11 com imagem de estado em 6 pontos de corte; I12 |
| `test_outbox_gate` | I10 nas três frentes (portão, loop, congelamento) |
| `test_format` | formato do WAL e equivalência entre backends |
| `test_ingress_pipeline` | framing SBE, decodificação streaming, particionamento e broadcast |
| `test_r17_latency_slo` | R17: medição P95 de latência com handshake TLS separado da requisição |
| `test_r18_availability_classes` | R18: classificação de disponibilidade (2XX/422 sucesso vs 5XX/408 erro) |
| `test_observability` | Prometheus exposition 0.0.4, status JSON, healthz e servidor HTTP TCP |

`tests/domain/golden/` traz **14 cenários** com todos os números fechados **à mão antes de existir
código**, cada um citando o invariante que exercita. É a especificação executável do domínio.

## O que falta

Em ordem de dependência:

1. **WAL — escritor.** `segment.hpp/.cpp`, `group_commit.hpp/.cpp`, `wal.hpp/.cpp` satisfazendo
   `core::Journal`. O formato e os backends já existem. Ver ADR-0012, 0013, 0024.
2. **Recuperação.** `wal/recovery.cpp`, único ponto de `wal` que inclui `core/apply.hpp`.
3. **Suíte de crash.** `tests/chaos/`: cauda rasgada, CRC corrompido, epoch trocado, `kill -9` no
   meio de um grupo. O `FaultBackend` já está pronto para reordenar completions (I9).
4. **Snapshot de exposição.** `src/expose/`: construtor a partir de `PartitionState`, leitor com
   `mmap`, `investmentId` determinístico (UUID v5), corpos JSON pré-serializados, manifesto.
   O formato (`src/format/exposure.hpp`) já está fechado.
5. **Borda FAPI.** `src/edge/`: HTTP próprio (ADR-0019), JOSE sobre OpenSSL (ADR-0020),
   consentimento, limites, os seis endpoints, R1–R19 com teste.
6. **Baseline fixado.** O harness de ADR-0021 **existe** (`bench/`, exercitado ponta a ponta em
   06/09/2026, limpo sob ASan/UBSan e TSan). O que falta é rodá-lo na MÁQUINA DE REFERÊNCIA e
   promover os números: `motor-rv-bench --repeticoes 15 --gravar-baseline bench/baseline.json`.
   A medição de 06/09 foi feita em outra máquina e ADR-0022 proíbe promover número de outra
   máquina — os números e o que eles já revelaram estão em
   `bench/reports/2026-09-06-medicao-inicial.md`. **Nenhuma otimização é aprovável antes disto**
   (ADR-0016).

O script `/tmp/.../wf-borda.js` (referenciado nos logs de workflow) tem a especificação detalhada
dos itens 4 e 5, escrita e pronta para reexecução.

## Revisão de 03/09 e o que ela mudou

`docs/revisao-2026-09-03.md` traz 45 achados de quatro revisores independentes (nenhum deles autor
do código que revisou — é a regra de `CLAUDE.md`). **Quarenta e um dos 45 foram corrigidos** em duas rodadas, cada um com teste ou
gate; o que ficou está na tabela "O que ficou, e por quê" do mesmo arquivo.

A segunda rodada fechou os grandes: a imagem de recuperação passou de 126 MiB (proporcional à
capacidade configurada) para 0,70 MiB com 20 mil negócios (proporcional ao dado); a tabela de
negócios ganhou baixa diária e **estabiliza** em vez de crescer para sempre; e o gate de
invariantes passou a exigir que o teste **mencione** o invariante, o que imediatamente acusou I5 e
I6 rotulados sem verificação.

O padrão dos achados vale mais que a lista: o pior deles — quatro bugs em `apply_trade_allocated` —
existia porque **nenhum teste exercitava aquele caminho**. Todos alocavam `doc → doc`, o ramo que
não move bucket nenhum. Cobertura de linha não é cobertura de caminho.

E dois gates reportavam **verde sem ter feito nada**: o LTO nunca ligava (`PARENT_SCOPE` dentro de
arquivo `include()`d vai para um escopo que não existe) e o preset `tsan` filtrava por um rótulo que
nenhum `CMakeLists` declarava, selecionando zero testes. Os dois estão consertados e o CI agora
**confere que fizeram alguma coisa**, não só que saíram com código zero.

## Achados que mudaram o desenho

Estes valem mais que o código; estão registrados onde importam:

1. **I1 estava errado.** O enunciado original divergia da posição da depositária em 2 dos 5 estados
   do ciclo — dois erros de sinal que se cancelavam no caso provavelmente conferido. Corrigido, com
   a tabela do percurso, em `docs/invariantes.md` §Correções. Criou I13.
2. **`sobras` tem de ficar na unidade corrente**, porque I1 as soma com `disponivel` e uma soma
   exige unidade única (`tests/domain/golden/08`).
3. **A base do preço médio é o que se POSSUI, não o que está livre** — 137+100, não 87+100
   (`golden/03`). Errar isso não viola nenhum outro invariante.
4. **CODING_RULES §2 e §9 se contradiziam** (`__int128` vs `-Wpedantic`). Resolvido isolando a
   extensão em um arquivo; a seção "Conflitos entre regras" do CODING_RULES registra os três casos.
5. **`day_index % 3` não indexa liquidação**: D+2 conta pregões, e três pregões podem estar a cinco
   dias de calendário de distância. Daí a janela de datas na partição e o bucket `overdue`.
6. **Outbox cheio é contrapressão, não fail-stop.** E a volta do buffer circular sobrescrevia
   payload pendente — invisível em teste curto, corrompe sob carga.
7. **`-fno-exceptions` vazava por transitividade** para o ingress, que lê arquivo. Só quebrava no
   `release`. É a razão de a matriz de presets ser exercitada inteira.
8. **Limite de payload é 65535, não 64 KiB**: `len` é `uint16`, e 65536 gravaria `len == 0`.
9. **A RESTAURAÇÃO ainda é proporcional à capacidade configurada**, embora a imagem já não seja.
   Medido em 06/09: a MESMA imagem de 2 703 624 bytes carrega em 9,31 ms com a capacidade de
   medição e em 1,22 ms com a capacidade ajustada ao dado — 7,6× sem um byte de diferença. A causa
   é `load_state_image` começar por `PartitionState::init`, que aloca e zera as colunas inteiras.
   É a mesma classe de defeito que a revisão de 03/09 corrigiu do lado da escrita, viva do lado da
   leitura. O experimento que comprova o conserto já está no harness (as duas séries têm de
   convergir).
10. **A carga faz parte do número.** Um pregão dá 7,3 M eventos/s e três pregões dão 5,4 M — o
   estado cresce e a liquidação percorre listas mais longas. São dois números certos da MESMA
   métrica. Por isso o JSON de medição grava o bloco `carga` e o comparador RECUSA confrontar
   medições de cargas diferentes (código de saída 4) em vez de acusar 26 % de regressão inexistente.

## Armadilhas conhecidas

- **`docs/contratos-internos.md` é desenho, não descrição.** Leia o bloco de status no topo dele:
  há divergências deliberadas em relação ao código e incoerências internas ainda não corrigidas.
- **O CMake liga mais avisos que um `g++` direto** (`-Wuseless-cast`, `-Wold-style-cast`,
  `-Wconversion`). Use sempre `cmake --build`, nunca um `g++` à mão, para decidir se algo compila.
- **A imagem de recuperação exige a MESMA configuração de capacidade.** É verificado por CRC de
  cabeçalho; divergência é recusa explícita, não leitura de estado meio restaurado.
- **`intern_*` é por ordem de primeira aparição no log.** Qualquer mudança nessa ordem muda os ids
  internos e invalida snapshots.

## Por onde começar

`docs/leitura-guiada.md` — doze paradas, com exercícios no fim. O exercício 5 ("meça, depois
descubra por que você ainda não pode otimizar") é o que amarra o resto.
