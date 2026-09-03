# 03 — Venda com posição disponível: I1, I13 e o preço médio que não se move

Exerce I1 (custódia hoje), I13 (custódia projetada), I3 (nenhum bucket negativo) e I4.

Este é o cenário que expôs o erro de sinal do enunciado original de I1 — ver a seção "Correções"
de `docs/invariantes.md`. Ele percorre os cinco estados do ciclo e afirma as **duas** identidades
em cada um deles.

## Contexto

Conta `11122233344`, `PETR4` (`instrument_id` 1). Posição inicial: 137 ações disponíveis,
`preco_medio` `3217072993` (R$ 32,17072993 — fim do cenário 02), `caixa` R$ 5.592,61 (`55926100`).

## Eventos

| # | Evento | Campos |
|---|---|---|
| 1 | `AberturaDia` | `data=20260908` |
| 2 | `NegocioExecutado` | COMPRA 100 `PETR4` a R$ 30,00, custos R$ 4,00, `data_liq=20260910` |
| 3 | `NegocioExecutado` | VENDA 50 `PETR4` a R$ 35,00, custos R$ 4,10, `data_liq=20260910` |
| 4 | `AberturaDia` | `data=20260910` |
| 5 | `Liquidado` | `data=20260910`, lado COMPRA |
| 6 | `Liquidado` | `data=20260910`, lado VENDA |

## Buckets de custódia e as duas identidades

| Após | `disponivel` | `a_liq_compra` | `a_liq_venda` | **I1** = disp + aLV | **I13** = disp + aLC |
|---|---:|---:|---:|---:|---:|
| — inicial | 137 | 0 | 0 | 137 | 137 |
| 2 `Executado(compra)` | 137 | 100 | 0 | 137 | 237 |
| 3 `Executado(venda)` | 87 | 100 | 50 | 137 | 187 |
| 5 `Liquidado(compra)` | 187 | 0 | 50 | 237 | 187 |
| 6 `Liquidado(venda)` | 187 | 0 | 0 | 187 | 187 |

A coluna I1 é o que a depositária confirma no extrato do dia; a coluna I13 é o que o investidor vê
como "posição" no aplicativo. Elas convergem quando não há nada pendente — e é justamente entre
os passos 2 e 6 que um motor errado passa despercebido.

## Financeiro

    compra:  100 × 30,00 = 3.000,00 + 4,00 custos → débito R$ 3.004,00  → -30040000
    venda :   50 × 35,00 = 1.750,00 − 4,10 custos → crédito R$ 1.745,90 →  17459000
    net a_liquidar[20260910] = −3.004,00 + 1.745,90 = −R$ 1.258,10      → -12581000

Após o passo 6: `caixa` = 5.592,61 − 1.258,10 = **R$ 4.334,51** → `43345100`.

## Preço médio (I4)

    após passo 3 (venda executada) : 3217072993  — inalterado
    após passo 5 (compra liquidada): recalculado
    após passo 6 (venda liquidada) : igual ao de após o passo 5 — inalterado

Recálculo no passo 5, sobre a posição que a compra criou:

    custo antes  = 137 × 32,17072993   = R$ 4.407,390000410   (arredonda para R$ 4.407,39)
    custo compra = 3.000,00 + 4,00     = R$ 3.004,00
    quantidade   = 237 ações
    preco_medio  = 7.411,39 / 237      = 31,27168776371308...
                 → HALF_UP em 1e-8     = 3127168776

**O ponto sutil**: o preço médio é recalculado sobre 237 ações (137 + 100), **não** sobre 187
(137 + 100 − 50). A venda do passo 3 não retira custo da posição — retira quantidade de
`disponivel`. Quem baixa o custo é a apuração de ganho (ADR-0011, módulo separado), que consome o
log e não escreve nele. Se o motor abatesse o custo da venda aqui, I4 estaria violado e a apuração
de IR receberia um preço médio já contaminado.

## I3

Em nenhum passo qualquer bucket fica negativo. O passo 3 debita `disponivel` de 137 para 87 — se
a venda fosse de 200 ações sem a flag de descoberto, o `apply` rejeita o evento (cenário 04).
