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
| `--conferir-esquema ARQ` | confere as chaves de um baseline contra `bench/contrato.hpp`; não mede nada |

Códigos de saída de `motor-rv-bench`: **0** ok · **1** erro de execução · **2** uso inválido, recusa
de gravar baseline, série contratual ausente ou esquema divergente · **3** regressão acima do limiar
· **4** a carga do baseline é outra e nada foi comparado · **5** comparou sem poder conferir a carga,
porque o baseline não a declara · **6** métrica contratual não comparada (sem número no baseline, ou
medida como instável/pulada nesta execução).

A precedência é **3 → 4 → 6 → 5**, e ela é o conserto de um defeito real: enquanto os códigos eram
atribuídos por `if`s independentes, qualquer baseline sem bloco `carga` — todos os anteriores a esta
mudança — transformava uma regressão medida em 5, com `REGRESSÃO` impresso no terminal e o
`bench.yml` conferindo `test $? -eq 3`. Uma regressão medida não é mascarada por uma conferência que
faltou.

O 5 e o 6 existem pelo mesmo motivo: "as cargas batem" e "não deu para saber" produziam a mesma
saída; "a métrica não regrediu" e "a métrica não foi comparada" também. Contra o
`bench/baseline.json` versionado de hoje — que não tem número nenhum — o comando sai **6**, e não 0.

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

## Uma métrica, um lugar: `bench/contrato.hpp`

Qual série alimenta cada chave de `bench/baseline.json`, que forma o valor tem no JSON, por que uma
métrica ainda é nula e como ela se chama em português corrente — tudo isso é UMA tabela,
`kEsquemaMetricas`. Acrescentar uma métrica ao esquema é acrescentar uma linha ali, e ela aparece
sozinha no JSON, no `metricas_ausentes`, no comparador e no resumo do relatório.

Essa correspondência já esteve escrita três vezes (no emissor do JSON, no comparador e num default
digitado no script de relatório). Renomear uma série atualizava uma das três; as outras passavam a
procurar um nome inexistente — e procurar um nome que não existe é silêncio, não erro. O comparador
saía **verde** tendo comparado coisa nenhuma. As duas defesas contra a volta disso:

- o nome da série é `constexpr` na tabela e usado no ponto de registro, então renomear é uma linha;
- `contrato_quebrado()` roda antes de qualquer publicação: se a suíte rodou e a série contratual não
  apareceu, o harness sai com 2 em vez de gravar um JSON com a métrica obrigatória nula.

O `scripts/relatorio-bench.py` **não copia** a tabela: ele a recebe no bloco `contrato` do próprio
JSON de medição, e é dali que saem as séries que ele exige comparar, os rótulos do resumo e os
campos que definem a carga.

O quarto lugar onde as mesmas chaves aparecem é `bench/baseline.json`, que é versionado e editado à
mão — ele não pode nascer da tabela (é o alvo, e quem o escreve de verdade é `--gravar-baseline`),
então é conferido contra ela por `--conferir-esquema`, nos dois sentidos: chave que falta e chave
que sobra.

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

O que esse gate pega e o que não pega, **medido nesta máquina e não suposto**. O procedimento: 5
rodadas independentes, cada uma com 3 execuções por lado (`--suites nucleo,snapshot --repeticoes 9`,
1 pregão, 1500 negócios/dia, 200 investidores), o MESMO binário dos dois lados — um PR inocente. Nas
mesmas 5 rodadas, uma cópia do lado da cabeça com regressão de 2× injetada (metade da vazão do
núcleo, dobro da duração do snapshot), escalando todas as amostras para preservar o ruído medido:

| | fechamento (série comparada) | regressão de 2× pega | falso positivo |
|---|---|---|---|
| `nucleo.loop.eventos_por_s_por_core` | 5/5 | 5/5 | 0/5 |
| `snapshot.salva.duracao_ms` | 5/5 | 5/5 | 0/5 |

O limiar exigido ficou entre 9,2 % e 49,1 % conforme a série e a rodada, e os Δ do PR inocente
chegaram a −19,4 % sem reprovar. Ou seja: o gate pega **regressão grossa** — um fator, um `O(n²)`
acidental, uma flag de otimização que caiu. Não pega 5 %, e prometer que pegaria seria construir um
gate que reprova PR inocente até alguém desligá-lo.

Estas duas linhas já foram **0/5 e 0/5** — o portão não fechava e não pegava nada, e o job só
avisava. As duas causas, e as duas correções, estão na seção "A regra de ruído" de
`scripts/relatorio-bench.py`: o filtro de estabilidade do baseline usado como pré-condição da
comparação, e a dispersão usada onde cabia o erro padrão da mediana. Com o gate fechando, o código 2
("não deu para olhar") passou a **reprovar** o job: um gate cego que avisa é um gate que se aprende
a ignorar.

Quem pega 5 % é o outro gate: `motor-rv-bench --comparar bench/baseline.json`, rodado por
`desempenho` na máquina de referência, onde o baseline vale (ADR-0022).

O relatório abre por um **Resumo**: passou ou não, e as métricas contratuais em uma tabela com
rótulo em português corrente — é o que o resumo do job do GitHub mostra primeiro. O ambiente, as 24
séries e as métricas ainda não medidas continuam abaixo, inteiros. `--apendice` gera o documento sem
resumo e com o título em nível 2, para o relatório informativo do WAL, que é concatenado embaixo do
principal: dois "## Resumo" no mesmo resumo de job davam dois vereditos, e o de baixo listava como
"não medida" métricas que aquela execução nem tenta medir.

As séries que ele EXIGE comparar vêm do bloco `contrato` do JSON (`--exigir` só sobrepõe). JSON sem
esse bloco não fica verde: sem contrato não há o que exigir, e um veredito "sem regressão" com zero
métricas obrigatórias conferidas é o gate falhando ABERTO.

Códigos de saída de `relatorio-bench.py`: **0** sem regressão, **1** regressão, **2** não deu para
olhar — uma série obrigatória que existe nos DOIS lados não foi comparada, o JSON não declara
contrato, ou as duas cargas são diferentes. **O job falha em 1 e em 2.** O 2 já foi só aviso, e o
motivo de não ser mais está medido na seção abaixo.

Série que a base não tem (uma métrica nova introduzida pelo PR) não cai no 2: não há cegueira, há
primeira medição. Ela aparece no resumo e não reprova.

## O baseline versionado e o esquema

`bench/baseline.json` traz os quatro blocos que um baseline precisa ter — `ambiente`, `harness`,
`carga` e `metricas` — hoje todos nulos, porque o baseline ainda não foi fixado (ver `status` no
arquivo). Ele NÃO traz os blocos que só uma medição tem (`contrato`, `metricas_ausentes`, `series`):
um baseline é um alvo, não um relatório. Os blocos nulos não são enfeite: sem `carga` declarada, o
comparador não tem contra o que conferir a carga desta execução, e é isso que ele diz.

Um baseline de verdade nunca é escrito à mão — sai de `--gravar-baseline`. Mas o arquivo versionado
é editado à mão, e por isso ele era a QUARTA cópia da tabela de métricas: renomear uma chave só ali
fazia o comparador procurar o que não existe, imprimir `SEM BASELINE` e sair 0. `--conferir-esquema`
confronta as chaves do arquivo com `bench/contrato.hpp` e reprova na divergência; o CI o roda a cada
PR, inclusive contra uma cópia com a chave propositalmente renomeada, para provar que a conferência
morde.

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
