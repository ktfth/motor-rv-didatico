# HANDOFF — motor-rv

Estado em **03/09/2026**. Este arquivo substitui o pacote de transferência original (o desenho
sem código), que está preservado em `../raw/`.

## O que existe e funciona

Sete presets de build configuram, compilam e passam os testes:

| preset | o que é |
|---|---|
| `debug` | asserts de invariante ligados |
| `release` | baseline oficial: `x86-64-v2` + LTO (ADR-0022) |
| `clang-release` | segundo compilador, mesmo baseline |
| `asan` / `tsan` / `fuzz` | sanitizers e libFuzzer |
| `nativo` | `-march=native`, só para experimento |

```sh
./scripts/bootstrap-toolchain.sh && export PATH="$PWD/.toolchain/bin:$PATH"
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
python3 scripts/check_invariants.py build/debug     # 13/13 invariantes com teste
./build/release/src/app/motor-rv-sim --dias 3 --negocios 20000 --investidores 2000
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
| `src/ingress/` | particionador congelado com valores golden e simulador determinístico que lê o calendário real da B3 |
| `src/app/` | `motor-rv-sim` |
| `src/wal/` | **parcial**: formato (`WalHdr` 32 B, `SegmentHdr`), descoberta de alinhamento via `statx` com fallback, três backends de I/O (io_uring, pwrite, injeção de falhas) |

### Testes — 6 suítes, 13/13 invariantes

| Suíte | Cobre |
|---|---|
| `test_rounding_golden` | 18 casos: os números dos cenários golden pela aritmética |
| `test_apply_golden` | 16 casos: os mesmos cenários **ponta a ponta** pelo motor |
| `test_i2_cash_by_date` | I2 recomputando o lado direito da identidade |
| `test_replay_equivalence` | I11 com imagem de estado em 6 pontos de corte; I12 |
| `test_outbox_gate` | I10 nas três frentes (portão, loop, congelamento) |
| `test_format` | formato do WAL e equivalência entre backends |

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
6. **Baseline.** `bench/` com o harness de ADR-0021. **Nenhuma otimização é aprovável antes
   disto** (ADR-0016).

O script `/tmp/.../wf-borda.js` (referenciado nos logs de workflow) tem a especificação detalhada
dos itens 4 e 5, escrita e pronta para reexecução.

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
