# 09 — Bonificação de 5 % com custo atribuído pela companhia

Exerce I4 (preço médio muda em evento corporativo), I6 (idempotência) e a mesma regra de unidade
de `sobras` do cenário 08.

## Contexto

Posição: 137 ações, `preco_medio` `3217072993` (R$ 32,17072993). Custo da posição:

    137 × 32,17072993 = R$ 4.407,39000041  → em 1e-4, R$ 4.407,3900 (44073900)

A companhia bonifica 5 % e **atribui** custo de R$ 12,00 por ação bonificada.

## Por que a companhia atribui o custo

Bonificação é capitalização de reservas: o investidor recebe ações sem desembolsar nada, mas para
o fisco elas têm custo — o valor que a companhia declara na deliberação. Esse número é dado
externo, vem no evento, e o motor **não o calcula**. Se o motor inventasse um custo (zero, ou
rateio do preço médio), a apuração de ganho do módulo de IR (ADR-0011) sairia errada e não haveria
como saber, porque estaria internamente consistente.

## Quantidade

    137 × 0,05 = 6,85 ações
    inteiras : 6      → entram em `disponivel`
    fração   : 0,85   → vai para `sobras`, na unidade corrente (mesma ação; cenário 08)

    disponivel = 137 + 6 = 143   → 14300000000
    sobras     = 0,85            →    85000000

## Preço médio

O custo atribuído entra só sobre as ações que entraram na posição. A fração fica com `sobras`, e o
custo dela é resolvido no leilão pelo módulo de IR — o motor guarda a quantidade, não a decisão
fiscal.

    custo antes    = R$ 4.407,3900          → 44073900 (1e-4)
    custo atribuído= 6 × R$ 12,00 = R$ 72,00 →   720000
    custo total    = R$ 4.479,3900          → 44793900
    quantidade     = 143 ações

    preco_medio = 44793900 × 1e12 / 14300000000 = 3.132.440.559,44  (1e-8)
                → HALF_EVEN                      = 3132440559       (R$ 31,32440559)

O preço médio **cai**, como tem de cair: mais ações pelo mesmo custo mais R$ 72,00.

## I1 e I13

    depositária = 137 × 1,05 = 143,85 ações
    I1  = disponivel + sobras = 143 + 0,85 = 143,85  ✔
    I13 = disponivel + a_liq_compra + sobras = 143 + 0 + 0,85 = 143,85  ✔

## I6 — idempotência

Chave `(evento_id, conta)`. Reentregar `EventoCorporativoAplicado{evento_id=903, conta}` não pode
somar outras 6 ações. Onde a chave vive é o detalhe que importa e está no cenário 13.
