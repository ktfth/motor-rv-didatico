# Leitura guiada — o motor em doze paradas

Este documento existe porque um motor que ninguém consegue ler não pode ser mantido. Ele é um
roteiro: doze paradas, na ordem em que fazem sentido, cada uma com o arquivo, a ideia e a pergunta
que aquela parada responde. Ao fim você deve conseguir explicar o sistema inteiro para outra
pessoa — que é um teste bem mais duro que compilar.

Se você tem vinte minutos, leia as paradas 1, 4, 6 e 9. Se tem uma tarde, leia todas e faça os
exercícios do fim.

---

## Antes de tudo: o problema

O motor faz **pós-negociação de renda variável**. Quando alguém compra uma ação na B3, o negócio é
executado hoje (D+0) e liquidado dois pregões depois (D+2). Entre esses dois momentos existe uma
obrigação: o comprador deve dinheiro, o vendedor deve ações, e alguém precisa saber exatamente
quanto, de quem, para quando.

Esse "alguém" é este motor. Ele mantém dois ledgers — **custódia** (quantas ações, em que estado) e
**financeiro** (quanto dinheiro, a receber ou a pagar em cada data) — para cada investidor, e
publica a posição de D-1 pela API de Open Finance.

Três restrições moldam tudo o que vem a seguir:

1. **Não pode errar dinheiro.** Nem um centavo, nem por arredondamento acumulado.
2. **Não pode perder o que confirmou.** Se o motor disse a alguém que algo aconteceu, aquilo
   sobrevive a uma queda de energia.
3. **Tem de ser rápido.** Milhões de eventos por dia, latência de microssegundos.

Quase toda decisão do repositório é uma dessas três restrições levada a sério.

---

## 1. `docs/dominio.md` — os dois ledgers e a máquina de estados

**A pergunta:** o que exatamente o motor guarda?

Custódia, por conta × instrumento: `disponivel`, `a_liquidar_compra[D]`, `a_liquidar_venda[D]`,
`bloqueado`, `sobras`, `preco_medio`. Financeiro, por conta: `caixa`, `a_liquidar[D]`,
`proventos_a_receber`.

E um grafo: `Executado → Alocado → Compensado → Liquidado`, com um desvio para `FalhaEntrega`.

**Leia junto:** `docs/invariantes.md`, em especial a seção "Correções". O enunciado original de I1
divergia da posição real da depositária em dois dos cinco estados do ciclo — os dois erros de sinal
se cancelavam exatamente no caso em que alguém provavelmente o conferiu. A tabela lá mostra o
percurso completo. É o melhor exemplo do repositório de por que um invariante precisa ser
*percorrido*, não apenas escrito.

---

## 2. `src/base/fixed.hpp` — por que dinheiro é `int64`

**A pergunta:** por que não `double`?

    >>> 7 * 31.40
    219.79999999999998

Em `int64` com escala 1e-4, sete ações a `314000` dão `2198000` — exato, sempre, em qualquer
máquina. O arquivo tem uma decisão que costuma surpreender: `Qty` e `Price` têm a **mesma escala**
(1e-8) mas são **tipos diferentes**, separados por uma tag vazia. Custa zero byte e transforma
trocar quantidade por preço num erro de compilação.

E uma ausência deliberada: **não existe `operator*`**. Multiplicar dois pontos fixos muda a escala e
obriga a arredondar; toda multiplicação mora em `rounding.hpp`, com a política no nome da função.
Se você conseguiu escrever `preco * quantidade`, o desenho falhou.

---

## 3. `src/base/rounding.hpp` — três políticas, e por quê

**A pergunta:** para onde vai o meio centavo?

| Política | Onde | Por quê |
|---|---|---|
| `HALF_EVEN` | `grossAmount` (I7), preço médio (I4) | são números **somados**; só half-even tem erro esperado zero na soma |
| `HALF_UP` | tarifas | convenção de mercado |
| `TRUNC` | retenção na fonte, grupamento | reter a maior seria cobrar imposto não devido; grupamento não cria quantidade |

Repare em `mul_div`: o produto vai para `i128` e a divisão acontece **uma vez, no fim**. O cenário
`tests/domain/golden/12` explica por que a ordem faz parte do invariante, e o teste
`MulDiv.IntermediarioPassaDeInt64` mostra o segundo caso de teste que já estoura `int64`.

---

## 4. `schema/events.xml` — o vocabulário do log

**A pergunta:** o que o motor considera um fato?

Dez mensagens, dez `templateId`, campo a campo. Três regras governam o arquivo:

- **`templateId` é imutável.** Mudar o significado de um template existente é proibido;
  comportamento novo pede template novo (contrato de determinismo D7).
- **Datas vêm resolvidas.** `DayOpened` traz D, D+1 e D+2 já calculados contra o calendário de
  pregões. O motor nunca calcula dia útil — calcular exigiria ler arquivo durante o replay.
- **Dinheiro decidido por terceiro é campo, não conta.** O valor de um provento vem do pagador; o
  motor **verifica** dentro de uma tolerância declarada e registra a divergência (cenário 10).

Os codecs C++ são **gerados** por `scripts/sbe_gen.py` (ADR-0017) e nunca versionados. O gerador
emite `static_assert` do tamanho, do alinhamento e do deslocamento de **cada campo** — e recusa,
com mensagem explicativa, schema mal ordenado, `templateId` repetido e ponto flutuante.

---

## 5. `src/core/ledger.hpp` — a memória do dia

**A pergunta:** por que colunas em vez de objetos?

SoA (*structure of arrays*): uma coluna por campo, não uma struct por posição. O `apply` de um
negócio toca três campos de uma posição; o percurso do EOD lê **um** campo de todas as posições.
Com objetos, o EOD arrastaria a posição inteira para o cache a cada linha.

E leia com atenção o comentário sobre a **janela de liquidação**. A tentação é indexar os buckets
por `dia % 3`, já que o ciclo é D+2. Está errado: D+2 conta *pregões*, e três pregões consecutivos
podem estar a cinco dias de calendário de distância. A solução — as três datas em voo vivem na
partição, não na posição — e o bucket `overdue`, que existe porque uma venda que falhou na entrega
não tem mais data na janela mas **continua** contando em I1.

---

## 6. `src/core/apply.cpp` — o coração

**A pergunta:** o que acontece quando um evento chega?

Uma função. Dez casos. E um padrão que se repete em todos:

    1. decodifica e valida tamanho
    2. resolve identidades (documento → conta, instrumento → slot)
    3. CHECA tudo — sem mutar nada
    4. muta
    5. verifica os invariantes

O passo 3 antes do 4 é o que faz um evento rejeitado deixar o estado **byte a byte idêntico**. O
teste `ApplyGolden04` afirma exatamente isso.

Duas ideias no topo do arquivo merecem parada:

**O log é a verdade do que CHEGOU, não do que foi aceito.** `append` acontece antes de `apply`, e
por isso eventos rejeitados estão no log — e o replay os rejeita de novo, pelo mesmo código, com o
mesmo `Status`. Não existe um "caminho de validação" separado do caminho de aplicação; existiriam
duas verdades.

**Duas classes de falha, e só duas.** Rejeição (entrada externa inválida: métrica sobe, o motor
segue) e fatal (corrupção: a partição para). A classificação vem da *faixa numérica* de `Err`, não
de uma tabela paralela que alguém esquece de atualizar. Derrubar um core inteiro porque uma
corretora mandou lixo transformaria o erro do outro em indisponibilidade nossa.

---

## 7. `src/core/partition.hpp` — o motor inteiro, em dezoito linhas

**A pergunta:** qual é o laço principal?

    drena e aplica  →  maybe_submit  →  reap  →  libera o outbox até durable_lsn

A ordem não é arbitrária. Aplicar **antes** de ser durável é o que dá latência baixa (estado
otimista); liberar **só depois** é o que dá correção. As duas juntas são a ideia central do group
commit.

E `poll(now_ns)` em vez de `run()`: o relógio entra como parâmetro, uma vez, no lugar mais raso
possível, e **nunca** cruza a fronteira do `apply`. É a mesma função em produção, em teste e no
replay — o teste chama com o tempo que quiser, o replay chama com zero.

---

## 8. `src/core/outbox.hpp` — um portão, e por isso um invariante

**A pergunta:** como se prova que nada sai antes de estar durável (I10)?

Fazendo com que exista **um** lugar por onde a saída passa. O `apply` não escreve em ring de saída,
não loga conteúdo de negócio, não chama nada que publique: ele empilha aqui, com o LSN do evento.
`ready(durable)` percorre da cabeça enquanto `lsn <= durable` e para. Não existe caminho que
ultrapasse.

Duas correções que este arquivo carrega, e que valem mais que o código: a volta do buffer circular
sobrescrevia payload de entrada pendente (invisível em teste curto, corrompe sob carga); e o outbox
cheio era fail-stop quando deveria ser **contrapressão** — fila de saída cheia significa consumidor
atrasado, não corrupção.

---

## 9. `src/core/state_image.hpp` — como o estado volta

**A pergunta:** o que é, exatamente, um snapshot?

A resposta é mais simples do que parece, e a simplicidade vem de uma propriedade que já existia por
outra razão: **a arena aloca em ordem determinística**. Logo, o deslocamento de cada coluna dentro
da arena é sempre o mesmo. Salvar é copiar os bytes da arena mais os poucos escalares que moram no
struct; restaurar é rodar `init` numa arena nova — que atribui os mesmos deslocamentos, logo
ponteiros válidos — e copiar os bytes por cima.

Nenhum ponteiro é serializado. Nenhum é reajustado.

É isso que torna I11 verificável, e `tests/core/test_replay_equivalence.cpp` a verifica do jeito
duro: para **seis** valores de k, "estado após N eventos == estado após imagem em k + replay
k+1..N", comparando um CRC de **todas** as colunas dos dois ledgers, da tabela de instrumentos, da
tabela de negócios e da fila de exceção. Um campo novo que alguém esqueça de restaurar quebra o
teste — que é o ponto.

---

## 10. `src/wal/` — como um evento vira durável

**A pergunta:** por que group commit?

Porque `fsync` custa caro e o custo é por *chamada*, não por byte. Se cem eventos chegam em 100 µs,
uma escrita durável para os cem custa quase o mesmo que uma para um. A janela `W` é o quanto se
está disposto a esperar para juntar companhia.

O que segura o desenho: `durable_lsn` avança em ordem **FIFO** mesmo quando as completions chegam
fora de ordem (I9), e um grupo N falho com N+1 completo faz N+1 ser truncado na recuperação — o que
é correto porque nada além de N foi externalizado (I10 de novo).

E ADR-0023: o backend de I/O é plugável (`io_uring` | `pwrite` | `fault`), porque testar
"completions fora de ordem" com io_uring de verdade dependeria de sorte.

---

## 11. `src/format/exposure.hpp` — a única janela

**A pergunta:** como a borda enxerga o núcleo?

Por um arquivo mapeado, e por mais nada. `rs` (o resource server) **não linka** `rv_core`: um
`#include "core/..."` na borda quebra o *link* antes de qualquer revisão humana. É CODING_RULES §10
na forma executável.

Uma ideia estrutura o formato: toda seção é um array tipado ou um blob — instrumentos em SoA,
posições em CSR, movimentos em CSR, corpos JSON pré-serializados. Um acessor, uma checagem, um CRC
por seção.

E `grossAmount` já vem calculado. A borda não faz aritmética de dinheiro; ela recorta bytes. I7
vira um teste de golden do *construtor*, e não existem dois lugares onde a mesma conta possa
divergir.

---

## 12. `src/edge/pipeline.hpp` — as seis etapas, em ordem de custo

**A pergunta:** o que acontece numa requisição da API?

    1. mTLS com cadeia ICP-Brasil          (R6)
    2. x-fapi-interaction-id presente      (R1)
    3. access token PS256 + cnf.x5t#S256   (R2, R3, R7)
    4. consentimento AUTHORISED + escopo   (R4, R5)
    5. limites mensais e rate limit        (R16)
    6. lookup no snapshot D-1              (R8..R15)

A ordem é **requisito**: rejeitar antes de gastar. Por isso o pipeline é um array de ponteiros de
função e não um framework de middleware — ler o array é ler a especificação, e um teste afirma a
ordem.

---

## Como rodar

```sh
./scripts/bootstrap-toolchain.sh                 # CMake e Ninja locais ao projeto
export PATH="$PWD/.toolchain/bin:$PATH"

cmake --preset debug && cmake --build --preset debug
ctest --preset debug                             # todas as suítes
ctest --preset debug -L I11                      # quem cobre a equivalência de replay
python3 scripts/check_invariants.py build/debug   # invariante sem teste FALHA aqui

./build/debug/src/app/motor-rv-sim --dias 3 --negocios 5000
```

O último comando roda três pregões inteiros por quatro partições e imprime aceitos, rejeitados por
motivo, os checksums de cada partição e o tamanho da imagem de recuperação. **Mesma semente, mesmo
resultado** — é o determinismo do motor visível a olho nu.

---

## Exercícios

Fáceis de propor, difíceis de fazer sem entender:

1. **Quebre I1 de propósito.** Em `apply_trade_settled`, some `qty` a `available` sem tirar de
   `pending_buy`. Qual teste falha primeiro? Por que o de reconciliação não falha logo?
2. **Troque a política de arredondamento** do preço médio de `HALF_EVEN` para `HALF_UP`. Qual teste
   quebra? Por que só um? (Resposta em `tests/domain/golden/02`.)
3. **Adicione um evento.** `AccountRegistered`, para que um investidor possa nascer no meio do dia.
   Quantos arquivos você precisa tocar? Quantos deles são gerados?
4. **Faça a janela de liquidação ter dois slots.** Qual cenário golden falha, e por quê o de falha
   de entrega falha primeiro?
5. **Meça.** Compile no preset `release`, rode o simulador com um milhão de negócios e descubra
   onde o tempo vai. Depois leia ADR-0016 e explique por que você não pode otimizar ainda.

O exercício 5 é o mais importante do conjunto. A resposta é: porque ainda não existe baseline, e
otimização sem número é opinião.
