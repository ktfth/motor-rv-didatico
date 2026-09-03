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

## Em que unidade `sobras` é guardado — e por que a resposta é forçada por I1

    1.377 / 10 = 137 ações novas inteiras, sobrando o equivalente a 7 ações antigas

A fração pode ser escrita de dois jeitos: **7 ações antigas** ou **0,7 ação nova**. Parece
escolha de gosto. Não é: I1 soma `disponivel + Σ a_liquidar_venda + bloqueado + sobras` e compara
com a depositária. Uma soma só faz sentido se todas as parcelas estiverem na mesma unidade — e
depois do grupamento a unidade de `disponivel` é a ação **nova**. Guardar `sobras` em ação antiga
tornaria I1 uma soma de coisas diferentes, e o motor passaria a comparar 137 + 7 = 144 com uma
depositária que tem 137,7.

**Regra, então: `sobras` está sempre na unidade corrente do instrumento.**

    disponivel  = 137 ações novas   → 13700000000
    sobras      = 0,7 ação nova     →    70000000
    preco_medio = 321707299 × 10    →  3217072990

A escala 1e-8 da quantidade existe exatamente para isto: 0,7 ação é representável de forma exata,
e nenhuma fração precisa ser arredondada no momento em que nasce. Guardar quantidade em inteiro de
unidades — a simplificação óbvia — obrigaria a jogar a fração fora ou a inventar um bucket em
outra unidade, e é por isso que ADR-0007 fixou 1e-8 também para quantidade, e não só para preço.

## I1 depois do grupamento

A identidade de I1 compara com a posição na depositária. Entre o grupamento e o leilão de sobras,
a depositária também carrega a fração:

    I1 = disponivel + sobras = 137 + 0,7 = 137,7 ações novas   == 1.377 ações antigas ✔

O teste roda `ReconciliacaoDepositaria` com essa posição e exige divergência **zero** — se o motor
tivesse jogado a fração fora, a divergência apareceria aqui, que é o ponto do cenário.

## Fecho: leilão de sobras

O preço do leilão vem no evento, na unidade nova (uma ação nova vale dez antigas):

    evento Sobras{evento_id=902, conta, qty=70000000, preco=R$ 314,00}
    caixa += 0,7 × 314,00 = R$ 219,80  → 2198000
    sobras = 0

O mesmo dinheiro que 7 ações antigas a R$ 31,40 dariam — como tem de ser, já que o grupamento não
cria nem destrói valor. O teste afirma as duas contas e a igualdade entre elas.

## Nota didática: por que ponto fixo, em uma linha

O leilão acima, calculado em `double`:

    >>> 7 * 31.40
    219.79999999999998

Em `int64` escala 1e-4, `7` ações a `314000` centésimos de milésimo dão `2198000` — exato, sempre,
em qualquer máquina. É por isso que CODING_RULES §2 proíbe `double` em dinheiro, e por isso que
o erro acima nunca aparece no motor: ele não tem onde acontecer.
