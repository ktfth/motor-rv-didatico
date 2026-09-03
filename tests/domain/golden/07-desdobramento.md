# 07 — Desdobramento 1:10

Exerce I4 (preço médio muda em evento corporativo) e I6 (idempotência por chave).

## Contexto

Posição de 137 ações a `preco_medio` `3217072993` (fim do cenário 02). A companhia anuncia
desdobramento na proporção 1:10, data-com 20260910, data-ex 20260911.

## Regra

`docs/dominio.md`: desdobramento multiplica a quantidade por `fator` e divide o preço médio por
`fator`. O valor da posição não muda — é a propriedade que o teste verifica primeiro.

    quantidade: 137 × 10 = 1.370 ações        → 137000000000
    preco_medio: 3217072993 / 10 = 321707299,3

`321707299,3` não é inteiro em escala 1e-8.

| Política | Valor |
|---|---|
| `TRUNC` | `321707299` |
| `HALF_UP` | `321707299` (a parte fracionária é 0,3 → arredonda para baixo) |

As duas coincidem aqui; o cenário 08 escolhe números onde não coincidem, de propósito.

## Conservação de valor

    antes:  137   × 3217072993 = 440.739.000.041  (escala 1e-16)
    depois: 1370  ×  321707299 = 440.738.999.630
    diferença: 411 unidades de 1e-16 = R$ 0,0000000000000411

A perda é o arredondamento de uma casa em 1e-8 multiplicado pela quantidade. O teste fixa o
limite: **a diferença de valor da posição por evento corporativo é menor que `qty` ulps de
preço**, e o resíduo NÃO é lançado em nenhum bucket financeiro — desdobramento não move dinheiro.

## Idempotência (I6)

O mesmo `EventoCorporativoAplicado{evento_id=901, conta}` entregue duas vezes deve aplicar uma
vez só. A chave é `(evento_id, conta)`, não `(instrumento, data_ex)`: a mesma companhia pode ter
dois eventos na mesma data-ex (ex.: dividendo + desdobramento), e a conta é parte da chave porque
a aplicação é por posição.

## Estado esperado

| Bucket | Valor |
|---|---|
| `disponivel` | `137000000000` (1.370 ações) |
| `preco_medio` | `321707299` |
| `caixa` | inalterado |
| `sobras` | 0 — desdobramento não gera fração |
