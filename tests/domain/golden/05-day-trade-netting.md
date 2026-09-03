# 05 — Compra e venda do mesmo papel no mesmo dia: netting por data de liquidação

Exerce I2 (o `a_liquidar[D]` financeiro é exatamente a soma dos negócios pendentes para D).

## Contexto

Posição inicial: 137 ações disponíveis, `preco_medio` `3217072993`, `caixa` R$ 5.592,61.
Dia 20260908; ambos os negócios liquidam em 20260910.

## Eventos

| # | Evento | qty | preço | custos |
|---|---|---:|---:|---:|
| 1 | `NegocioExecutado` COMPRA | 100 | R$ 30,00 | R$ 4,00 |
| 2 | `NegocioExecutado` VENDA | 100 | R$ 31,20 | R$ 4,25 |
| 3 | `LoteCompensado{data=20260910}` | — | — | — |
| 4 | `Liquidado{data=20260910}` | — | — | — |

## Custódia — o ponto que costuma escapar

    após 1: disponivel 137, a_liq_compra[10] = 100, a_liq_venda[10] = 0
    após 2: disponivel  37, a_liq_compra[10] = 100, a_liq_venda[10] = 100

Note que o passo 2 debita `disponivel` das ações **antigas** (137 → 37), e não das 100 que estão
chegando pela compra do passo 1. Isso é deliberado e é a regra da custódia: só se entrega o que já
está depositado. Se o investidor tivesse 0 disponíveis, a venda do passo 2 seria descoberta
(cenário 04) mesmo com a compra do passo 1 pendente para a mesma data.

    I1  = 37 + 100 = 137  ✔ a depositária continua com 137 até D+2
    I13 = 37 + 100 = 137  ✔ a posição projetada também é 137: comprou 100, vendeu 100

## Financeiro e I2

    compra: 100 × 30,00 = 3.000,00 + 4,00 → débito  R$ 3.004,00 → -30040000
    venda : 100 × 31,20 = 3.120,00 − 4,25 → crédito R$ 3.115,75 →  31157500
    a_liquidar[20260910] = −30040000 + 31157500 = 1117500 = +R$ 111,75

I2 afirma que `a_liquidar[20260910]` é **exatamente** a soma dos dois negócios pendentes para essa
data. O `LoteCompensado` do passo 3 é o registro do net que a câmara calculou; ele **não recalcula**
o número — ele confirma. O teste compara o valor que a câmara mandou com o que o motor já tinha:
divergência aqui é divergência de verdade, e vai para a fila de exceção, não para o ledger.

**Por que o motor calcula se a câmara também calcula**: porque o motor precisa do número antes de
D+1 para mostrar posição intradiária, e porque uma diferença entre os dois é o sinal mais barato de
que um negócio se perdeu no caminho. Se o motor apenas copiasse o net da câmara, um negócio faltando
no drop copy passaria despercebido até a reconciliação da depositária.

## Após a liquidação (passo 4)

    a_liq_compra[10] → 0, disponivel += 100  → 137
    a_liq_venda[10]  → 0
    caixa += 1117500 → 55926100 + 1117500 = 57043600 = R$ 5.704,36

## Preço médio (I4)

Recalculado **uma vez**, na perna de compra:

    custo antes  = 137 × 32,17072993 = R$ 4.407,39
    custo compra = R$ 3.004,00
    quantidade   = 237
    preco_medio  = 7.411,39 / 237 = 3127168776  (HALF_EVEN, igual ao cenário 03)

A perna de venda não toca o preço médio, mesmo sendo day trade. O ganho de day trade
(R$ 3.115,75 − 100 × 31,27168776 = R$ −11,42) é assunto do módulo de IR (ADR-0011), que tem regra
própria — 20 % e sem a isenção de R$ 20 mil — e consome o log sem escrever nele.
