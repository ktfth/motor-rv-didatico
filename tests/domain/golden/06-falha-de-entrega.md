# 06 — Falha de entrega em D+2 e recompra

Exerce I5 (toda transição segue o grafo de `docs/dominio.md`) e a persistência da pendência.

## O grafo, na parte que interessa

    Compensado --> Liquidado    : janela da câmara (D+2)
    Compensado --> FalhaEntrega : entrega não honrada (D+2)
    FalhaEntrega --> Compensado : recompra
    FalhaEntrega --> [*]        : multa / tratamento B3

Duas coisas que o grafo diz e que é fácil violar por descuido:

1. **`FalhaEntrega` não vai direto para `Liquidado`.** A recompra devolve o negócio para
   `Compensado`, e só de lá ele liquida. Um `apply` que atalhasse `FalhaEntrega → Liquidado`
   produziria estado certo e histórico errado — e o histórico é o que a API de movimentações
   expõe (R11).
2. **`Executado → Compensado` não existe.** Todo negócio passa por `Alocado`, mesmo quando a
   alocação é trivial (conta final == conta de execução). O evento `Alocado` é o que fixa quem é
   o titular, e sem ele a partição não sabe de quem é a posição.

## Cenário

Venda de 50 ações que falha na entrega em 20260910 e é recomprada em 20260911.

| # | Evento | Estado do negócio | Efeito |
|---|---|---|---|
| 1 | `NegocioExecutado` VENDA 50 @ R$ 35,00 | `Executado` | `disponivel −= 50`; `a_liq_venda[0910] += 50` |
| 2 | `Alocado` | `Alocado` | titular fixado |
| 3 | `LoteCompensado{0910}` | `Compensado` | net financeiro consolidado |
| 4 | `FalhaEntrega{negocio, 0910}` | `FalhaEntrega` | **`a_liq_venda[0910]` permanece**; marca de falha |
| 5 | `NegocioExecutado` COMPRA 50 @ R$ 36,10 (recompra) | novo negócio, `Executado` | `a_liq_compra[0913] += 50` |
| 6 | `LoteCompensado{0913}` liga a recompra à falha | `Compensado` | — |
| 7 | `Liquidado{0913}` | `Liquidado` | `a_liq_venda[0910] −= 50`; `a_liq_compra[0913] −= 50` |

## O bucket que não se mexe (passo 4)

Depois da falha, `a_liquidar_venda[20260910]` continua com 50. A tentação é zerá-lo — afinal, a
data passou. Mas o bucket é indexado pela **data de liquidação contratada**, não pela data
corrente, e a obrigação não desapareceu por não ter sido honrada. Zerá-lo faria I1 mentir:

    I1 = disponivel + Σ a_liq_venda = 87 + 50 = 137   ✔ a depositária ainda tem 137
                                                        (as ações não foram entregues!)

Se o motor zerasse o bucket, I1 daria 87 contra 137 da depositária, e a reconciliação do EOD
acusaria uma divergência que não existe — pior, acusaria todos os dias, transformando o alarme em
ruído. **O bucket de uma data vencida e não liquidada é a própria definição da falha.**

## Custo da recompra

    venda original: 50 × 35,00 = R$ 1.750,00 (crédito, pendente)
    recompra      : 50 × 36,10 = R$ 1.805,00 (débito)
    diferença     : −R$ 55,00 por conta de quem falhou

O motor **registra** a diferença como movimento; quem decide se ela é multa, custo do investidor
ou do intermediário é regra da B3, e entra como campo do evento `FalhaEntrega`, não como cálculo
do motor. Regra de terceiro que muda por ofício não pode virar `if` dentro do `apply` — vira
campo de evento, e o replay continua determinístico quando a regra mudar.

## Teste de propriedade de I5

O teste enumera **todas** as transições possíveis do produto (estado × tipo de evento) — não só as
do grafo — e afirma:

- transição no grafo → aceita e o estado avança;
- transição fora do grafo → `ApplyResult::Rejected{InvariantId::I5}`, estado byte a byte idêntico.

São 6 estados × 10 tipos de evento = 60 combinações; 7 estão no grafo. As 53 restantes são o teste.
Um grafo escrito em `docs/dominio.md` e uma tabela de transição no código que discordem é o tipo de
divergência que só aparece em produção — por isso a tabela do código é **gerada** a partir de uma
única declaração e o teste percorre a declaração inteira.
