# 11 — Subscrição exercida parcialmente

Exerce I4 (preço médio: média ponderada com o preço de emissão) e I6.

## Contexto

Posição: 143 ações, `preco_medio` `3132440559` (R$ 31,32440559), `caixa` R$ 5.704,36 (`57043600`).
Direito de subscrição 1:4 (uma ação nova para cada quatro possuídas) a R$ 25,00.

    direitos = 143 / 4 = 35,75  → 35 direitos inteiros + 0,75 de direito

O investidor exerce **20** dos 35.

## Por que o motor não decide quantos exercer

Exercer é ato do investidor: chega como campo do evento (`qty_exercida`), nunca como cálculo. O
motor valida apenas `0 ≤ qty_exercida ≤ direitos_inteiros` e rejeita fora disso (I5, mesma
mecânica do cenário 04). A fração de direito (0,75) segue para `sobras` ou caduca conforme o
edital — também campo do evento.

## Efeito

    disponivel : 143 + 20 = 163       → 16300000000
    caixa      : −20 × R$ 25,00 = −R$ 500,00 → 57043600 − 5000000 = 52043600

    custo antes  = 143 × 31,32440559 = R$ 4.479,3900  → 44793900
    custo emissão= 20 × 25,00        = R$   500,0000  →  5000000
    custo total  =                     R$ 4.979,3900  → 49793900
    quantidade   = 163

    preco_medio = 49793900 × 1e12 / 16300000000 = 3.054.840.490,7975
                → HALF_EVEN                      = 3054840491   (R$ 30,54840491)

O preço médio cai porque R$ 25,00 é menor que R$ 31,32 — subscrição abaixo do preço médio dilui o
custo. Se o preço de emissão fosse maior, subiria. O teste cobre os dois lados exatamente para que
ninguém "otimize" a fórmula assumindo que subscrição sempre barateia.

## Ordem das operações e a razão de ser dos `__int128`

    custo_total(1e-4) × 1e12 / qty(1e-8)

O numerador aqui é `49793900 × 10^12 ≈ 5 × 10^19` — passa de `int64` (≈ 9,2 × 10^18) por um fator
de cinco. Numa posição institucional de 10^9 ações, passaria por dez ordens de grandeza. É o
motivo concreto de CODING_RULES §2 exigir `__int128` nos intermediários: não é preciosismo, é o
segundo caso de teste que estoura.

O teste inclui uma posição de 2 × 10^9 ações a R$ 900,00 e verifica que o resultado continua exato —
e um `static_assert` sobre os limites declarados de `Qty` e `Money` prova a margem em compilação.
