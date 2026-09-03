# 08 — Grupamento 10:1 com fração para `sobras`

Exerce I1 (a identidade de custódia inclui `sobras`), I4 e I6.

## Contexto

Posição de 1.377 ações (número escolhido para não ser múltiplo de 10), `preco_medio`
`321707299`. Grupamento 10:1, data-ex 20260915.

## Regra

Grupamento divide a quantidade por `fator` e multiplica o preço médio por `fator`. A parte da
posição que não forma ação inteira vira `sobras` — **em quantidade da ação antiga**, porque é isso
que a B3 leva a leilão.

    1.377 / 10 = 137 ações inteiras, resto 7 ações antigas
    disponivel = 137 ações        → 13700000000
    sobras     =   7 ações antigas →   700000000
    preco_medio = 321707299 × 10 = 3217072990

## Por que `sobras` fica na escala antiga

Se convertêssemos a sobra para a ação nova, ela seria 0,7 ação — e o invariante I3 ("nenhum
bucket de quantidade negativo") passaria a conviver com bucket fracionário cuja unidade muda no
meio do evento. Guardar em quantidade antiga mantém uma única unidade por bucket e deixa o leilão
(`Sobras` → `caixa +=`) ser um evento comum, com preço vindo do log e não de uma conversão
implícita.

## I1 depois do grupamento

A identidade de I1 compara com a posição na depositária. Entre o grupamento e o leilão de sobras,
a depositária também carrega a fração. O teste roda `ReconciliacaoDepositaria` com a posição
esperada `137 ações novas + 7 ações antigas em sobras` e exige divergência **zero** — se o motor
tivesse jogado a fração fora, a divergência apareceria aqui, que é o ponto do cenário.

## Fecho: leilão de sobras

    evento Sobras{evento_id=902, conta, qty=700000000, preco=R$ 31,40}
    caixa += 7 × 31,40 = R$ 219,80  → 2198000
    sobras = 0

## Nota didática: por que ponto fixo, em uma linha

O leilão acima, calculado em `double`:

    >>> 7 * 31.40
    219.79999999999998

Em `int64` escala 1e-4, `7` ações a `314000` centésimos de milésimo dão `2198000` — exato, sempre,
em qualquer máquina. É por isso que CODING_RULES §2 proíbe `double` em dinheiro, e por isso que
o erro acima nunca aparece no motor: ele não tem onde acontecer.
