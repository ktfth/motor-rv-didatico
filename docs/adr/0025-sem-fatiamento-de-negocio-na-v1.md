# ADR-0025 — v1 não fatia negócio na alocação; alocação parcial é rejeitada

Status: aceito (03/09/2026)

## Contexto

`schema/events.xml` declara que `TradeAllocated` "pode fatiar o negócio", e a mesa faz isso na vida
real: a corretora executa um negócio grande e o distribui entre vários investidores finais.

A implementação movia apenas os buckets (`qty`, `cash_amount`) entre as contas e reapontava o dono
do negócio inteiro. Uma revisão independente mediu a consequência com números: corretora compra 100
ações a R$ 30,00 e aloca 40. Depois disso, o `a_liquidar` da corretora é −R$ 1.800,00 enquanto a
soma dos negócios pendentes dela ainda é −R$ 3.000,00; o investidor tem −R$ 1.200,00 de
`a_liquidar` contra −R$ 3.000,00 de negócios pendentes. **I2 falha nas duas contas**, e ninguém vê:
o teste de I2 não emitia alocação nenhuma.

Fatiar de verdade exige criar uma linha nova em `TradeTable` para a parcela, reduzir a original,
derivar um `trade_id` para a parcela e manter as duas listas encadeadas coerentes.

## Decisão

Na v1, `apply_trade_allocated` **rejeita** com `Err::QtyMismatch` quando `e.qty` difere de
`trades.qty[trade_id]`. A alocação total — inclusive a trivial, de uma conta para ela mesma —
continua funcionando, e é o que o simulador e o fluxo de investidor direto usam.

## Alternativas consideradas

- **Aceitar movendo só os buckets** (o que estava lá): errado em silêncio, e o erro é em I2, que é a
  identidade que amarra o financeiro aos negócios.
- **Implementar o fatiamento agora**: é a decisão certa para a v2 e exige mexer na tabela de
  negócios, na chave de idempotência do `allocation_id` e no histórico que a API de movimentações
  expõe (R11). Fazer isso sob a pressa de fechar uma revisão produziria a terceira versão errada.

## Consequências

O motor recusa uma entrada que o schema declara válida — uma divergência entre formato e
comportamento, registrada aqui em vez de escondida. Quem implementar o fatiamento deve: criar a
linha nova, reduzir a original, ligar as duas por `allocation_id`, e estender
`tests/domain/test_allocation.cpp`, que hoje afirma a rejeição e passará a afirmar o fatiamento.
