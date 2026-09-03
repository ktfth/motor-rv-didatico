# 04 — Venda maior que a posição: rejeitada, ou autorizada com flag

Exerce I3 (nenhum bucket de quantidade negativo, **exceto** `disponivel` em venda a descoberto
autorizada) e a semântica de rejeição do `apply`.

## Contexto

Posição: 137 ações disponíveis. Chega `NegocioExecutado{VENDA, qty=200}`.

## Caso A — sem flag de descoberto: o evento é rejeitado

`apply` não estoura exceção (CODING_RULES §4) e não altera bucket nenhum. Ele devolve
`ApplyResult::Rejected{InvariantId::I3, ...}` e o motor:

1. **registra o evento no log assim mesmo** — o log é a verdade do que aconteceu, e "chegou um
   negócio impossível" é um fato que aconteceu. O que não acontece é a mutação de estado.
2. incrementa a métrica `apply.rejeitado{motivo=I3}`;
3. empurra o negócio para a fila de exceção, junto com a divergência de reconciliação.

**Por que registrar no log um evento rejeitado**: se o evento não entrasse, o replay a partir do
snapshot anterior não reproduziria a fila de exceção, e I11 (equivalência de replay) quebraria de
um jeito silencioso — o estado "principal" bateria e o estado auxiliar não. A regra geral: tudo o
que muda qualquer parte observável do processo entra no log, inclusive a decisão de rejeitar.

**Por que não é fail-stop**: fail-stop da partição é para violação de *durabilidade* ou de
*ledger* (CODING_RULES §4) — coisas em que continuar significa corromper. Um negócio malformado
vindo do ingress é entrada externa inválida, não corrupção interna. Derrubar a partição de um core
inteiro porque uma corretora mandou lixo seria transformar um erro do outro em indisponibilidade
nossa.

### Estado após o caso A

Idêntico ao anterior: `disponivel = 137`, todos os demais buckets inalterados. O teste afirma
igualdade byte a byte do estado da partição antes e depois, não só dos campos que ele lembrou de
conferir.

## Caso B — com flag de descoberto autorizada

O mesmo evento com `short_allowed = true` no cadastro da conta × instrumento:

    disponivel        : 137 → −63          (13700000000 → -6300000000)
    a_liquidar_venda[D+2] : 0 → 200        (          0 →  20000000000)

`disponivel` negativo é o **único** bucket que I3 permite negativo, e só com a flag. O assert de
I3 em build debug consulta a flag antes de disparar; em release, a métrica registra a posição
descoberta para que o risco a veja.

### I1 e I13 no caso B

    I1  = disponivel + a_liq_venda = −63 + 200 = 137   ✔ a depositária ainda tem 137
    I13 = disponivel + a_liq_compra = −63 + 0  = −63   ✔ a posição projetada é short em 63

I13 negativo é o resultado correto e informativo: depois de entregar as 200, o investidor deve 63
ações. Um invariante que proibisse I13 negativo esconderia exatamente a informação que o risco
precisa.

## Fronteira do teste de propriedade (I5 e I3 juntos)

O teste de propriedade gera sequências aleatórias de negócios sobre uma posição inicial e afirma,
depois de cada `apply`:

- todo bucket é ≥ 0, **exceto** `disponivel` quando `short_allowed`;
- I1 e I13 valem;
- o conjunto de eventos rejeitados é exatamente o conjunto que violaria as regras acima —
  ou seja, o motor não rejeita a mais nem a menos.

A terceira afirmação é a que tem valor: um motor que rejeitasse tudo passaria nas duas primeiras.
