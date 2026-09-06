# Medição inicial — 06/09/2026

Autor: papel `desempenho`. Harness: `bench/`, ADR-0021. Arquivos de dados desta medição:
`2026-09-06-medicao-inicial.json` (um pregão) e `2026-09-06-tres-pregoes.json` (três pregões).

> **Estes números NÃO foram promovidos a `bench/baseline.json`, e não devem ser.** A máquina onde
> eles foram tirados não é a máquina de referência de `docs/ambiente.md`: é um Xeon virtualizado
> de 4 vCPUs sobre KVM, com o WAL em ext4 num disco virtual compartilhado, enquanto a referência é
> um i7-2600 físico com btrfs. ADR-0022 é explícito: número medido em outra máquina não se compara.
> O que este relatório entrega é o **harness**, exercitado ponta a ponta, mais o que ele já
> descobriu. Fixar o baseline é `motor-rv-bench --gravar-baseline bench/baseline.json` rodado na
> máquina de referência — um comando, depois de `scripts/probe-ambiente.sh`.

## Ambiente desta medição

| | |
|---|---|
| commit | `2873563` (árvore suja: o próprio `bench/` ainda não estava commitado) |
| máquina | Intel Xeon @ 2.10 GHz, 4 vCPUs, KVM |
| kernel | 6.18.44 |
| flags | GCC 13.3.0, Release, `-march=x86-64-v2 -flto`, asserts de invariante desligados |
| WAL | `/tmp` → `/dev/vda`, ext4; `O_DIRECT\|O_DSYNC` aceito; bloco 4096 **assumido** (o fs não respondeu ao `statx`) |
| io_uring | `SINGLE_ISSUER` e `DEFER_TASKRUN` negociados com sucesso |
| carga | 1 pregão, 20 000 negócios, 2 000 investidores, 1 partição → **42 015 eventos**; semente 20260902 |
| harness | 3 aquecimentos, 15 repetições, descarte de série acima de 5 % de CV, até 3 tentativas |

Antes de qualquer número: `ctest` verde nos presets `debug`, `release`, `clang-release` e `asan`;
o **próprio harness** roda limpo sob ASan+UBSan e sob TSan (o bench de SPSC com duas threads é o
que dá sentido ao segundo). É CODING_RULES §9 — "sanitizers verdes antes de qualquer benchmark
contar" — aplicado ao medidor, e não só ao medido.

## 1. Primitivas de `src/base/`

| série | mediana | CV |
|---|---:|---:|
| `base.relogio.custo_ns` | 28,90 ns | 1,8 % |
| `base.crc32c_hw.vazao` | **7 224 MiB/s** | 0,6 % |
| `base.crc32c_tabela.vazao` | **336 MiB/s** | 0,3 % |
| `base.spsc_1thread.mensagens_por_s` | 296,3 M/s | 1,9 % |
| `base.spsc_2threads.mensagens_por_s` | 131,5 M/s | 11,5 % (instável) |
| `base.dense_index.insercoes_por_s` | 96,0 M/s | 1,9 % |
| `base.dense_index.buscas_por_s` | 84,1 M/s | 2,2 % |
| `base.arredondamento.negocios_por_s` | 22,4 M/s | 0,6 % |

**O CRC32C de hardware é 21,5× o da tabela.** É a confirmação numérica de ADR-0022: o motivo de o
baseline exigir `x86-64-v2` é o `_mm_crc32_u64`, e a diferença justifica a exigência. Também
justifica manter o fallback: 336 MiB/s é lento, não impraticável — uma máquina sem SSE4.2 roda o
motor, mais devagar, em vez de não subir.

**O ponto fixo custa 44 ns por negócio** (financeiro + corretagem + custo de posição + preço
médio, os quatro em `__int128`). Contra ~7 M eventos/s do núcleo, é ~30 % do orçamento de um
evento — caro em proporção, e ainda assim o preço certo: CODING_RULES §2 existe porque dinheiro em
`double` erra, e errar de graça não é barato.

`base.spsc_2threads` saiu instável (11,5 %) e por isso **não vira baseline**: com 4 vCPUs
compartilhadas e sem pinagem de core, o par produtor/consumidor migra. Na máquina de referência,
com os cores pinados (ADR-0005), esta série deve fechar bem abaixo do limiar.

## 2. Núcleo — a métrica obrigatória

| série | 1 pregão (42 015 eventos) | 3 pregões (143 402 eventos) |
|---|---:|---:|
| `nucleo.loop.eventos_por_s_por_core` | **7 296 216 /s** | 5 370 118 /s |
| `nucleo.apply.eventos_por_s` | 7 410 291 /s | 5 394 187 /s |
| `nucleo.apply.latencia_ns` (média) | 229,95 ns | 306,63 ns |
| latência p50 / p99 / p999 / máx | 182 / 460 / 744 / 56 968 ns | 236 / 672 / 1 392 / 502 087 ns |
| `nucleo.outbox.ciclos_por_s` | 243,0 M/s | 245,1 M/s |

Três leituras:

**(a) O ring custa ~1,5 %.** `nucleo.loop` (motor completo, com SPSC ring) contra `nucleo.apply`
(as mesmas quatro etapas sem o ring): 7,30 M/s contra 7,41 M/s. O transporte praticamente não
aparece — o que é o resultado esperado de um ring de slot fixo com cursores em linhas de cache
separadas, e é a primeira vez que isso é um número e não uma expectativa.

**(b) A vazão cai 26 % do primeiro para o terceiro pregão.** Não é ruído: o CV é 2,2 % nos dois. O
estado cresce (mais contas, mais posições, mais negócios em voo), a liquidação percorre listas
encadeadas por conta, e a virada do dia executa `close_and_compact_trades`. A cauda mostra o mesmo:
p999 vai de 744 ns para 1 392 ns e o máximo de 57 µs para 502 µs.

Isso tem uma consequência de processo, e é a razão de o harness ter mudado no meio desta medição:
**a carga faz parte do número**. Um baseline de 7,3 M/s comparado com uma execução de três pregões
acusaria 26 % de regressão sem que nada tivesse regredido. O JSON agora grava o bloco `carga`
(dias, negócios, investidores, partições, semente, eventos) e `--comparar` **recusa** confrontar
medições de cargas diferentes, saindo com código 4.

**(c) O máximo é 250× o p999.** Um punhado de eventos custa dezenas de microssegundos: `DayOpened`
(que compacta a tabela de negócios), `EodMarked`, `CustodyReconciled`. Não é defeito — é trabalho
em lote disfarçado de evento — mas é a razão de a métrica publicada ser o P999 e não o máximo, e é
o que o group commit vai encontrar pela frente quando existir.

## 3. Snapshot — e um defeito achado pela medição

| série | 1 pregão | 3 pregões |
|---|---:|---:|
| `snapshot.bytes` | 2 703 624 | 5 912 424 |
| `snapshot.salva.duracao_ms` (o **stall**) | **1,13 ms** | 1,83 ms |
| `snapshot.salva.vazao` | 2 806 MiB/s | 3 125 MiB/s |
| `snapshot.carrega.duracao_ms` | **9,31 ms** | 10,07 ms |
| `snapshot.carrega_capacidade_justa.duracao_ms` | **1,22 ms** | 2,69 ms |
| `snapshot.bytes_capacidade_justa` | 2 703 624 | 5 912 424 |

A gravação está confortavelmente dentro do alvo: `docs/wal.md` mira 20–40 ms de stall, e um pregão
de 20 mil negócios custa **1,1 ms**. A imagem cresce com o dado (2,7 MB → 5,9 MB quando o volume
triplica), o que confirma que o conserto da revisão de 03/09 continua de pé — e agora tem série
própria (`snapshot.bytes`, direção "menor é melhor") para não voltar atrás em silêncio.

**O achado é a leitura.** As duas últimas linhas são a MESMA imagem, byte a byte — 2 703 624 nos
dois casos —, restaurada com duas capacidades configuradas diferentes:

- capacidade de medição (1 M negócios, 256 K posições, 64 K contas): **9,31 ms**
- capacidade ajustada ao que o dia teve: **1,22 ms**

**7,6× de diferença sem nenhum byte de dado a mais.** A causa está em `load_state_image`, que
começa chamando `PartitionState::init`: `init` aloca e **zera** as colunas inteiras, porque é o que
`PartitionCapacity` manda — não o que a imagem contém. É exatamente a classe de defeito que a
revisão de 03/09 corrigiu do lado da **escrita**, viva do lado da **leitura**: o custo depende de
quanto alguém dimensionou, e não de quanto o dia teve. Dobrar a capacidade para caber num pico
dobra o tempo de recuperação de todos os dias.

Não corrigi aqui — `src/core/` é de `nucleo`, e a correção precisa decidir uma coisa que não é do
medidor: zerar preguiçosamente (só até `count`) muda o contrato de "estado virgem" de `init`, e
esse contrato é o que faz o replay ser determinístico. Fica como o próximo passo sugerido, com o
número e o experimento já prontos para conferir o conserto.

## 4. WAL — o piso físico do dispositivo

Estas séries **não são** `wal.append_para_duravel_us`. O escritor do WAL não existe (HANDOFF, "O
que falta", item 1); o que se mede aqui é uma escrita alinhada submetida e colhida, com
`O_DIRECT|O_DSYNC`. É o piso que o group commit vai encontrar.

| tamanho | pwrite p50 | pwrite p99 | io_uring p50 | io_uring p99 |
|---|---:|---:|---:|---:|
| 4 KiB | 92 µs | 152 µs | **70 µs** | 111 µs |
| 64 KiB | 137 µs | 236 µs | **137 µs** | 270 µs |
| 256 KiB | 410 µs | 688 µs | **360 µs** | 549 µs |

**A hipótese do group commit se sustenta neste dispositivo.** 64 KiB custa 1,96× o que custa
4 KiB, carregando 16× mais dado; 256 KiB custa 5,2× por 64× o dado. Ou seja: agrupar dez mil
eventos numa escrita é quase de graça, que é a premissa inteira de ADR-0012 e de `docs/wal.md`.

**io_uring ganha de `pwrite` em 4 KiB (70 µs contra 92 µs, ~24 %)** e empata em 64 KiB. Faz
sentido: a economia é de syscall e de cópia, e ela pesa quando a escrita é pequena. Com
`DEFER_TASKRUN` e `SINGLE_ISSUER` negociados, é o caminho que ADR-0008 previu.

Quatro das seis séries de WAL saíram **instáveis** no run de 15 repetições e foram recusadas pelo
harness. É o comportamento correto num disco virtual compartilhado: os máximos (19 ms, 22 ms)
mostram que o dispositivo às vezes some por dezenas de milissegundos. Numa máquina com disco
dedicado, esta é a primeira suíte a refazer.

## 5. O que a medição descobriu sobre a própria medição

**O descarte por CV pega tremor, não pega desvio sistemático.** Numa das execuções, TODAS as séries
de io_uring vieram com p50 de exatamente 4,0 ms — 55× o valor normal — e com CV de 0,8 %, isto é,
perfeitamente "estáveis". O harness aprovou. O número não se reproduziu nas execuções seguintes
(voltou a 70–390 µs), e o que aconteceu foi uma condição transitória da máquina virtual, não do
código.

A lição é sobre o desenho do gate, e vale registrar: coeficiente de variação mede dispersão DENTRO
de uma série, e uma anomalia estável passa por ele sem tocar. Quem pega desvio sistemático é a
comparação com o baseline — os dois mecanismos são complementares, e é mais um motivo para o
baseline existir cedo. Enquanto ele não existir, **nenhum número deste relatório deve ser usado
sozinho para aprovar ou reprovar nada**.

**O limiar de 5 % é da ordem do ruído desta máquina.** Duas execuções idênticas da mesma
configuração deram 7,30 M/s e 7,63 M/s para `nucleo.loop` — 4,5 % de diferença —, com CV interno de
2,4 % e 1,5 %. A variância ENTRE execuções é maior que a variância dentro de uma. Na máquina de
referência, com cores pinados, isso deve encolher; se não encolher, o limiar de
`bench/baseline.json` precisa ser reconsiderado com número na mão, por ADR — não afrouxado no
susto do primeiro PR reprovado.

## 6. O que ficou de fora, e por quê

`bench/baseline.json` continua com cinco métricas `null`. O JSON de medição traz o motivo de cada
uma em `metricas_ausentes`:

| métrica | por que ainda não existe |
|---|---|
| `wal.append_para_duravel_us` | não há escritor de WAL; o piso do dispositivo está nas séries `wal.*_commit` |
| `wal.tamanho_grupo_bytes` | não há group commit para agrupar |
| `wal.grupos_em_voo` | idem |
| `wal.recuperacao_s` | não há `wal/recovery.cpp`; `nucleo.apply.eventos_por_s` é o limite superior (sem leitura de disco) |
| `rs.latencia_por_endpoint_ms` | `src/edge/` não existe (fase 4) |

## 7. Próximos passos, em ordem

1. **Rodar este harness na máquina de referência** (`scripts/probe-ambiente.sh`, depois
   `motor-rv-bench --repeticoes 15 --gravar-baseline bench/baseline.json`) com os cores pinados.
   Só então o gate de 5 % passa a significar alguma coisa.
2. **Corrigir a restauração proporcional à capacidade** (§3). O experimento que comprova o conserto
   já está no harness: as duas séries têm de convergir.
3. **`wal.append_para_duravel_us` quando o escritor existir.** O harness já tem o histograma, o
   descarte de série e o bloco de ambiente; falta o objeto a medir.
