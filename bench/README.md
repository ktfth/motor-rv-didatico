# bench/ — o harness de medição

Dono: `desempenho`. Ver ADR-0021 (por que um harness próprio), ADR-0016 (o que autoriza uma
otimização) e ADR-0022 (por que um número só vale para a máquina em que foi tirado).

## Rodar

```sh
cmake --preset release && cmake --build --preset release
./build/release/bench/motor-rv-bench
```

Sempre no preset `release`. O binário do `debug` roda, mede e **avisa em letras grandes** que a
medição não vale — nesta máquina ele dá 2,0 M eventos/s contra 6,8 M do `release`, o que é a razão
inteira de o aviso existir.

| opção | o que faz |
|---|---|
| `--suites base,nucleo,snapshot,wal` | escolhe os grupos (padrão: todos) |
| `--repeticoes N` / `--aquecimento N` | tamanho da série e quantas repetições vão para o lixo antes de medir |
| `--limiar-cv PCT` | acima disto a série é **descartada e refeita** (padrão 5 %) |
| `--tentativas N` | quantas séries refazer antes de desistir e marcar `INSTÁVEL` |
| `--rapido` | perfil de fumaça para CI: 1 aquecimento, 3 repetições, CV 25 % |
| `--dias / --negocios / --investidores / --semente` | a carga (a mesma sessão do `motor-rv-sim`) |
| `--dir-wal DIR` | onde criar o arquivo temporário de 256 MiB da suíte `wal` |
| `--out ARQ` | grava o JSON da medição |
| `--comparar ARQ` | confronta com um baseline; **sai != 0** se houver regressão |
| `--gravar-baseline ARQ` | fixa o baseline; recusa se a medição não for válida |

## O que o harness faz que um `for` com `clock_gettime` não faz

1. **Aquecimento explícito.** As primeiras repetições rodam e são jogadas fora: elas pagam
   primeiro toque de página da arena e cache frio, que o motor de produção paga uma vez no warm-up.
2. **Descarte de série.** Série com coeficiente de variação acima do limiar é refeita inteira. Se
   a instabilidade persistir, a série sai marcada `INSTÁVEL` e **a métrica correspondente sai
   `null` no JSON** — medição instável nunca vira baseline.
3. **Zero alocação dentro da região medida.** O corpo devolve `Amostra{operacoes, ns}` e decide o
   que está dentro do cronômetro; montar o cenário fica fora. Os histogramas são `rv::Histogram`
   (4 KiB fixos, sem alocação em `record`), e o `BenchJournal` existe justamente porque o
   `MemoryJournal` de teste dá `push_back` a cada `append`.
4. **O bloco `ambiente` é preenchido pelo programa** — commit (com marca `+sujo`), CPU, kernel,
   flags de compilação (por `-D` vindo do CMake) e o dispositivo do WAL com o alinhamento medido
   pelo mesmo código que o WAL usa. Número colado do terminal errado deixa de ser possível.
5. **Recusa de baseline inválido.** `--gravar-baseline` falha se o build não for `Release`, se
   houver sanitizer, se os asserts de invariante estiverem ligados ou se qualquer série estiver
   instável.

## As três séries do núcleo, e por que não uma

| série | o que mede |
|---|---|
| `nucleo.loop.eventos_por_s_por_core` | o motor **completo**: SPSC ring, `append`, `apply`, portão de I10. É a métrica de `baseline.json`. |
| `nucleo.apply.eventos_por_s` | as mesmas quatro etapas **sem o ring**. A diferença para a de cima é o que o transporte cobra. |
| `nucleo.apply.latencia_ns` | o mesmo laço com o relógio lido por evento: P50/P99/P999. O custo do relógio está em `base.relogio.custo_ns`. |

O laço "espelhado" das duas últimas reproduz as quatro etapas de `core/partition.hpp`. É uma
**cópia**, e cópia envelhece — por isso a série do loop de verdade fica ao lado: uma diferença que
não seja explicável pelo ring é sinal de que a cópia divergiu.

## O que este diretório NÃO mede, e por quê

`wal.append_para_duravel_us`, `wal.tamanho_grupo_bytes`, `wal.grupos_em_voo`, `wal.recuperacao_s` e
`rs.latencia_por_endpoint_ms` continuam `null` em `bench/baseline.json`. Elas exigem o escritor do
WAL e a borda, que não existem (HANDOFF, "O que falta"). O JSON de medição traz o motivo de cada
uma em `metricas_ausentes` — a alternativa era deixar cinco `null` sem explicação, que é como um
baseline vira folclore.

As séries `wal.*_commit` **não são** `append_para_duravel_us`: são o **piso físico** do
dispositivo — uma escrita alinhada, submetida e colhida, com `O_DIRECT|O_DSYNC` quando o
filesystem aceita (a `nota` da série diz quando não aceitou). Nenhum group commit pode ficar
abaixo disso, e é contra isso que ele será julgado quando existir.

## No CI: dois gates diferentes, e nenhum substitui o outro

`.github/workflows/bench.yml` tem dois jobs.

**`verificacao`** responde "o harness funciona?". Nada ali depende de quão rápido é o runner: o
harness roda e o JSON é conferido campo a campo; o comparador é confrontado com baselines
sintéticos e tem de sair 3 (regressão) e 4 (carga diferente); o `--gravar-baseline` tem de recusar
o preset `debug`; e `scripts/relatorio-bench.py` tem de aprovar 3 % e reprovar 50 %. Veredito
determinístico em qualquer máquina.

**`medicao`** responde "este PR está mais lento que a base?". Ele compila as DUAS árvores e mede
os dois lados **intercalados, três vezes cada, no mesmo runner** — o único arranjo em que a
comparação isola a mudança de código do hardware. O relatório vai para o resumo do job e para os
artefatos.

O que esse gate pega e o que não pega, medido e não suposto: no runner deste projeto o ruído entre
execuções idênticas do mesmo binário fica entre 3 % e 25 % conforme a série. Por isso a regressão
só é declarada quando a diferença passa do limiar do projeto **e** de três vezes o ruído que as
próprias séries mediram. Na prática ele pega **regressão grossa** — um fator, um `O(n²)` acidental,
uma flag de otimização que caiu. Não pega 5 %, e prometer que pegaria seria construir um gate que
reprova PR inocente até alguém desligá-lo.

Quem pega 5 % é o outro gate: `motor-rv-bench --comparar bench/baseline.json`, rodado por
`desempenho` na máquina de referência, onde o baseline vale (ADR-0022).

Códigos de saída de `relatorio-bench.py`: **0** sem regressão, **1** regressão (o job falha),
**2** uma série contratual não pôde ser comparada por instabilidade (o job avisa e não falha —
"está mais lento" e "não deu para olhar" são fatos diferentes).

## Rodar o relatório à mão

```sh
./build/release/bench/motor-rv-bench --repeticoes 15 --out /tmp/m1.json
python3 scripts/relatorio-bench.py --medicao /tmp/m1.json --saida /tmp/relatorio.md
# comparando dois lados (várias execuções por lado melhoram a estimativa de ruído):
python3 scripts/relatorio-bench.py --medicao /tmp/h1.json /tmp/h2.json /tmp/h3.json \
                                   --contra  /tmp/b1.json /tmp/b2.json /tmp/b3.json
```

## Relatórios

`bench/reports/AAAA-MM-DD-tema.md`, escritos por `desempenho`; notas de revisão, por `verificador`.
