# 01 — Compra simples, liquidação em D+2, preço médio

Exerce I1 (identidade de custódia), I2 (financeiro a liquidar bate com os negócios pendentes)
e I4 (preço médio só muda em `Liquidado(compra)`).

## Contexto

Conta `11122233344` (CPF), instrumento `PETR4` (`instrument_id` interno 1, fator de cotação 1,
lote padrão 100). Posição inicial zerada; `caixa` = R$ 10.000,00 (`100000000` em escala 1e-4).

## Eventos

| # | Evento | Campos |
|---|---|---|
| 1 | `AberturaDia` | `data=20260902` |
| 2 | `NegocioExecutado` | `conta=11122233344`, `instrumento=1`, `lado=COMPRA`, `qty=100` (`10000000000`), `preco=R$ 32,45` (`3245000000`), `data_liq=20260904`, `custos=R$ 5,20` (`52000`), `broker_note_id=7001` |
| 3 | `Alocado` | negócio 1 → conta final (mesma conta; alocação direta) |
| 4 | `LoteCompensado` | `data=20260904`, net financeiro da conta |
| 5 | `AberturaDia` | `data=20260904` |
| 6 | `Liquidado` | `data=20260904`, lado COMPRA |

## Aritmética, passo a passo

Financeiro do negócio (escala 1e-4, intermediário em `__int128`):

    bruto  = qty × preco = 100 × 32,45 = R$ 3.245,00        → 32450000
    custos =                                  R$     5,20   →    52000
    débito = bruto + custos = R$ 3.250,20                   → 32502000

A multiplicação `qty(1e-8) × preco(1e-8)` dá escala 1e-16; converter para 1e-4 divide por 1e-12.
`100 × 32,45` é exato: **não há arredondamento neste cenário** — é de propósito, para que o
primeiro teste isole a mecânica dos buckets do problema de arredondamento (que é o cenário 12).

## Estado esperado

Após o evento 2 (`Executado`, compra):

| Bucket | Valor | Escala |
|---|---|---|
| `custodia[conta,1].a_liquidar_compra[20260904]` | 100 | `10000000000` |
| `custodia[conta,1].disponivel` | 0 | `0` |
| `custodia[conta,1].preco_medio` | 0 | `0` — **ainda não mudou** (I4) |
| `financeiro[conta].a_liquidar[20260904]` | −R$ 3.250,20 | `-32502000` |
| `financeiro[conta].caixa` | R$ 10.000,00 | `100000000` |

Após o evento 6 (`Liquidado`, compra):

| Bucket | Valor | Escala |
|---|---|---|
| `a_liquidar_compra[20260904]` | 0 | `0` |
| `disponivel` | 100 | `10000000000` |
| `preco_medio` | R$ 32,502 | `3250200000` |
| `financeiro.a_liquidar[20260904]` | 0 | `0` |
| `financeiro.caixa` | R$ 6.749,80 | `67498000` |

**Por que `preco_medio` = 32,502 e não 32,45**: o custo de aquisição inclui os custos da operação.
`(3.245,00 + 5,20) / 100 = 32,502`. Divisão exata; o cenário 02 é quem força o arredondamento.

## Invariante I1 no fechamento

    disponivel + Σ a_liquidar_compra − Σ a_liquidar_venda + bloqueado + sobras
      = 100 + 0 − 0 + 0 + 0 = 100 ações  == posição na depositária
