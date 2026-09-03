# 02 — Duas compras a preços diferentes: preço médio ponderado e arredondamento

Exerce I4 e a política de arredondamento do preço médio.

## Contexto

Mesma conta e instrumento do cenário 01, posição inicial zerada.

## Eventos e aritmética

    compra A: 100 ações a R$ 32,45, custos R$ 5,20  →  custo total  R$ 3.250,20
    compra B:  37 ações a R$ 31,17, custos R$ 3,90  →  custo total  R$ 1.157,19
                                                        (37 × 31,17 = 1.153,29; + 3,90)

Após liquidar as duas:

    quantidade = 137 ações
    custo      = 3.250,20 + 1.157,19 = R$ 4.407,39
    preco_medio = 4.407,39 / 137 = 32,17072992700729927...

Em escala 1e-8, `3217072992,700...` **não é exato**: é aqui que a política manda.

| Política | Valor em escala 1e-8 | R$ |
|---|---|---|
| `TRUNC` (em direção a zero) | `3217072992` | 32,17072992 |
| `HALF_EVEN` | `3217072993` | 32,17072993 |

**Decisão do domínio**: preço médio usa `HALF_EVEN` (arredondamento bancário), e a função
chama-se `average_price_half_even`. Duas razões, nesta ordem:

1. **Truncar tem viés de direção única.** O preço médio é base de custo e alimenta a apuração de
   ganho (ADR-0011). Truncar sempre baixa o custo, ou seja, sempre aumenta o ganho tributável —
   um erro que nunca se compensa.
2. **Meio-para-cima tem viés de magnitude.** `HALF_UP` empurra todo empate para cima; somando
   milhões de posições, o erro esperado é positivo. `HALF_EVEN` manda metade dos empates para
   cima e metade para baixo, e o erro esperado da soma é zero.

Aqui as duas dão `3217072993` — a parte descartada é `0,7`, não é empate. O caso que distingue as
políticas está na tabela de testes: `…992,5` vai para `…992` em `HALF_EVEN` (992 é par) e para
`…993` em `HALF_UP`. O teste inclui os dois lados do empate justamente porque, se incluísse só
este cenário, trocar a política não quebraria nada.

**O que NÃO se faz**: recomputar `preco_medio` a partir de `custo_total / quantidade` guardando
apenas `preco_medio`. O estado guarda `preco_medio` (escala 1e-8) e o teste verifica que aplicar a
segunda compra sobre o `preco_medio` da primeira dá o mesmo resultado que a fórmula acima dentro
de 1 ulp — a diferença é o erro acumulado, e o teste fixa quanto dele é tolerável (1 ulp por
operação, verificado no cenário de 1.000 compras sucessivas).

## Estado esperado

| Bucket | Valor |
|---|---|
| `disponivel` | 137 ações → `13700000000` |
| `preco_medio` | `3217072993` |
| `caixa` | 10.000,00 − 3.250,20 − 1.157,19 = R$ 5.592,61 → `55926100` |
