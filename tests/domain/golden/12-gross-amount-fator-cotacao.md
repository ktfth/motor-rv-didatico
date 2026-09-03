# 12 — `grossAmount` do snapshot com fator de cotação ≠ 1

Exerce I7: `grossAmount == qty × closingPrice / priceFactor` com o arredondamento **declarado**.
É o único invariante do núcleo que a borda Open Finance enxerga diretamente (R10).

## Por que este cenário existe separado

Os cenários 01–11 vivem dentro do motor, onde a unidade é sempre a escala interna (1e-8 para
quantidade e preço, 1e-4 para BRL). O `grossAmount` é a primeira vez que um número **sai** do
motor com um contrato de terceiro: o schema da API Renda Variável v1.3.0. Se o arredondamento
não for o mesmo do teste de conformidade do Open Finance Brasil, o motor está certo por dentro e
errado no que importa.

## Contexto

Instrumento com `priceFactor = 1000` (papel cotado por lote de mil). `closingPrice` de D-1
vem do evento `CotacaoFechada`, não de leitura externa no momento da resposta (ADR-0003, I12).

| Campo | Valor humano | Interno |
|---|---|---|
| `qty` | 137 unidades | `13700000000` (1e-8) |
| `closingPrice` | R$ 1.234,567 | `123456700000` (1e-8) |
| `priceFactor` | 1000 | `1000` (inteiro, não é ponto fixo) |

## Aritmética, com as escalas explícitas

    qty(1e-8) × closingPrice(1e-8) = produto em 1e-16, em __int128
      13700000000 × 123456700000 = 1_691_356_790_000_000_000_000

    dividir por priceFactor (inteiro puro, não muda escala)
      1_691_356_790_000_000_000_000 / 1000 = 1_691_356_790_000_000_000

    converter 1e-16 → 1e-4 (escala BRL do motor): dividir por 1e12
      1_691_356_790_000_000_000 / 1_000_000_000_000 = 1_691_356,79
                                                      → não é inteiro

| Política | Valor em 1e-4 | R$ |
|---|---|---|
| `TRUNC` | `1691356` | 169,1356 |
| `HALF_UP` | `1691357` | 169,1357 |

**Decisão: `HALF_UP`**, e a função chama-se `gross_amount_half_up`. Motivo: é o arredondamento
comercial usual e o mesmo que a apuração de posição da B3 aplica; truncar produz, na soma de uma
carteira grande, um viés sistemático para baixo no total exibido ao investidor.

**O intermediário é `__int128` e a divisão acontece uma única vez, no fim.** Dividir por
`priceFactor` antes de multiplicar por `qty` daria `123456700000/1000 = 123456700` — exato neste
caso, mas em `priceFactor = 3` perderia casas antes da multiplicação e o erro seria multiplicado
por `qty`. A ordem é parte do invariante, não um detalhe de implementação.

## O que a resposta da API carrega

    "grossAmount": { "amount": "169.1357", "currency": "BRL" }

Quatro casas decimais: é a escala interna de BRL (1e-4), sem conversão adicional. Escolher a
escala interna igual à precisão que a API expõe elimina uma classe inteira de erro de borda.

## Teste

O teste percorre uma tabela de `(qty, closingPrice, priceFactor)` que inclui, de propósito:

| Caso | Por que está na tabela |
|---|---|
| fator 1, valores exatos | caminho comum, sem arredondamento |
| fator 1000, resto 0,5 exato | a fronteira do `HALF_UP` (empate) |
| fator 1000, resto 0,4999… | logo abaixo do empate |
| `qty` de uma posição inteira da B3 (≈ 10⁹ ações) | prova que `__int128` não estoura |
| `closingPrice` no máximo representável em 1e-8 | idem |
| `priceFactor = 0` | entrada inválida: `apply` rejeita o instrumento no cadastro, não na resposta |

O caso de estouro merece a conta explícita: o maior produto plausível é
`10^9 ações × 10^6 reais`, ou seja `10^17 × 10^14 = 10^31` em escala 1e-16. `__int128` vai até
≈ `1,7 × 10^38`. Sobram sete ordens de grandeza — o teste afirma isso com um `static_assert` sobre
os limites declarados de `qty` e `price`, para que a margem seja verificada pelo compilador e não
por confiança.
