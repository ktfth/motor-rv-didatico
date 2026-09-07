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
os dois lados **intercalados, cinco vezes cada, no mesmo runner** — o único arranjo em que a
comparação isola a mudança de código do hardware. O relatório vai para o resumo do job e para os
artefatos.

O que esse gate pega e o que não pega, **medido nesta máquina, com os parâmetros DESTE job** —
`--suites base,nucleo,snapshot --repeticoes 9`, 1 pregão, 5000 negócios/dia, 500 investidores,
5 execuções por lado. Duas campanhas: 15 rodadas de PR inocente (o MESMO binário dos dois lados) e
5 rodadas com uma **regressão real de código** — uma segunda árvore com `#pragma GCC optimize("O0")`
em `src/core/apply.cpp`, compilada de verdade. Regressão sintética não serve como prova: ela
preserva o ruído exatamente e superestima a detecção; a real traz o ruído dela junto.

| versão do gate | fechamento `nucleo.loop` | fechamento `snapshot.salva` | vermelho falso | cego | regressão real reprovada |
|---|---|---|---|---|---|
| antes de B1 (`48af6c1`) | 6/15 | 2/15 | 0/15 | **14/15** | 4/5 |
| B1 aplicado (`41517d0`) | 15/15 | 15/15 | **1/15** | 0/15 | 5/5 |
| **hoje** | **15/15** | **15/15** | **0/15** | **0/15** | **5/5** |

As três linhas são o mesmo dado, lido por três versões do script. A do meio é o portão que fechava
mas deixava **qualquer** série votar; o vermelho falso dela foi uma série não contratual. A de hoje
só deixa votar quem o contrato nomeia.

Uma segunda campanha independente repetiu 15 rodadas inocentes e 5 reais: somadas às primeiras, são
**30 rodadas com o mesmo binário dos dois lados** e **10 regressões reais reprovadas 10/10**. Uma
terceira campanha trocou o método — dois BINÁRIOS de código idêntico, que é o que o job faz — e está
na seção seguinte: é ela que decide quem pode reprovar um PR.

Na regressão real, `nucleo.loop.eventos_por_s_por_core` caiu entre 37,7 % e 50,0 % e foi marcada nas
5 rodadas. `snapshot.salva.duracao_ms` não foi marcada em nenhuma, e está certo: `-O0` no `apply`
não torna o snapshot mais lento.

O limiar exigido nas 15 rodadas inocentes ficou entre **9,3 % e 31,5 %** (núcleo) e **6,6 % e
31,2 %** (snapshot) — inteiramente abaixo do teto de 50 %. Com **três** execuções por lado, o mesmo
teto era estourado em 3 das 15 rodadas (exigido chegava a 53,8 %) e o job ficava vermelho por
cegueira sem ninguém ter mudado código; foi essa medição que fixou o job em cinco execuções.

Ou seja: o gate pega **regressão grossa** — um fator, um `O(n²)` acidental, uma flag de otimização
que caiu. Não pega 5 %, e prometer que pegaria seria construir um gate que reprova PR inocente até
alguém desligá-lo.

**O teto de 50 %** (`TETO_EXIGIDO_PCT`, em `scripts/relatorio-bench.py`). Acima dele a série
bloqueante conta como não comparada (código 2), e não como aprovada. O número não é gosto: ele
espelha a frase "**pega regressão grossa — um fator**" do parágrafo acima. Um fator é 2×, isto é um
Δ de −50 %; se o limiar exigido passa de 50 %, uma regressão de 2× cabe dentro dele e "sem
regressão" vira uma afirmação que a medição não sustenta. As duas coisas mudam juntas: se um dia o
gate prometer pegar 1,5×, o teto vai a 33 %. O teto é o ponto onde ele admite que não enxerga o que
prometeu; o remédio é mais execuções por lado, não um teto mais alto.

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

## Quem reprova um PR não é quem vira baseline

São duas perguntas, e elas estavam no mesmo campo. `kEsquemaMetricas` (em `bench/contrato.hpp`) diz
quais métricas `bench/baseline.json` publica — o alvo que `desempenho` fixa na máquina de
referência. O portão A/B do CI faz outra: quais séries, se piorarem, **reprovam este PR**. Enquanto
a segunda resposta era "as da primeira", só 2 das 18 séries medidas podiam reprovar, e uma piora de
195 % em `snapshot.carrega.duracao_ms` — o tempo de voltar ao ar depois de uma queda — saía como
"informativa, não vota".

A lista bloqueante é `kSeriesBloqueantes`, viaja no bloco `contrato` do JSON e hoje tem **uma**
série: `nucleo.loop.eventos_por_s_por_core`, o motor completo. As outras dezessete aparecem no
relatório, marcadas `informativa, não vota`.

**Quem entra na lista é decisão medida — e três candidatas saíram por causa do número.**

Antes dos números, o método, porque ele mudou no meio desta rodada. Um "PR inocente" **não** é o
mesmo binário medido dos dois lados: o job compila as DUAS árvores, e dois binários de código
idêntico têm layout diferente. Medir um binário contra si mesmo esconde esse viés por construção.
As duas primeiras campanhas fizeram isso, e por isso deram 0/30 a uma série que reprova 13 de 15 PRs
inocentes de verdade. A terceira campanha compara **dois binários** — o mesmo commit com uma edição
semanticamente neutra em `bench/harness.cpp` — e é ela que vale para decidir quem bloqueia.

| série | mesmo binário (30 rodadas) | dois binários (15 rodadas) | por que ficou de fora |
|---|---|---|---|
| `nucleo.loop.eventos_por_s_por_core` | 0/30 | **0/15** | — bloqueia |
| `snapshot.salva.duracao_ms` | 0/30 | **13/15** | repetível dentro de um binário (±3 %), anda +9,0 % a +18,5 % entre binários de mesmo código |
| `nucleo.apply.eventos_por_s` | 1/30 | não avaliada | um vermelho falso com os dois lados derivando (−13,8 %) e ruído estimado de ±2,8 % |
| `snapshot.carrega.duracao_ms` | 1/30 + 2 cegas | não avaliada | saltou de 9,09 para 16,89 ms (+85,8 %) entre execuções do MESMO binário; bimodal por HANDOFF §9 |

O caso de `snapshot.salva.duracao_ms` é o mais instrutivo: a mesma edição neutra move a série de
0,32 para 0,37 ms sem tocar em uma linha de `save_state_image`. Uma série assim — muito repetível
dentro de um binário e sensível a layout entre binários — é a pior combinação possível para um
portão A/B, porque o ruído que ela declara é pequeno demais para o viés que ela sofre. Ela continua
sendo métrica de **baseline** (é outra pergunta, e outra máquina) e continua no relatório.

O que traria cada uma de volta: a convergência das séries de carga (`carrega`), uma reexecução de
confirmação antes de reprovar (`apply`), ou um piso de limiar por série calibrado pelo viés de
layout medido (`salva`). Nenhuma dessas é decisão deste diretório.

Com o conjunto entregue: **0/15 de vermelho falso e 0/15 de cegueira** nas 15 rodadas de dois
binários, e **10/10** nas dez rodadas de regressão real das duas primeiras campanhas.

## O campo `schema`

Ele numera o formato dos dois documentos deste diretório, o de medição e o baseline. Um documento
declara a versão para a qual foi escrito; quem lê uma versão menor sabe quais blocos não existem lá
e diz isso, em vez de concluir que nada era obrigatório.

| versão | o que mudou |
|---|---|
| 1 | primeira: `ambiente` e `metricas` nos dois; o baseline sem `carga` e sem `harness` |
| 2 | o baseline passa a declarar `carga` e `harness`; a medição passa a trazer `contrato` (métricas de baseline, rótulos e campos da carga) |
| 3 | o `contrato` da medição passa a declarar `bloqueantes` — as séries que reprovam um PR |

Onde isso é conferido: `--conferir-esquema` recusa um baseline anterior ao **2** (é o que aquela
conferência depende: os blocos `carga` e `harness`), e o job `verificacao` exige **3** no documento
de medição, porque é onde `bloqueantes` entrou. Sem o número, "schema 1" designaria dois formatos
diferentes conforme a data.

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
