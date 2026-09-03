# 13 — O mesmo evento corporativo entregue duas vezes

Exerce I6 e, de quebra, I11 (equivalência de replay) — porque o lugar onde a chave de idempotência
mora é o que decide se I6 sobrevive a um snapshot.

## O cenário raso

    EventoCorporativoAplicado{evento_id=903, conta=A, tipo=BONIFICACAO, fator=0,05}
    EventoCorporativoAplicado{evento_id=903, conta=A, ...}   ← duplicata

A segunda aplicação não pode somar mais 6 ações. Fácil: um conjunto de `(evento_id, conta)` já
vistos; se a chave está lá, o `apply` devolve `ApplyResult::Duplicado` e não toca em bucket algum.

## O cenário que realmente testa alguma coisa

    1. aplica o evento 903 na conta A
    2. tira snapshot no LSN k
    3. entrega a duplicata do evento 903
    4. **reinicia** e recupera: snapshot em k + replay de k+1 em diante

Se o conjunto de chaves vistas viver só na memória e **não** estiver no snapshot, o passo 4
esquece que 903 já foi aplicado, a duplicata do passo 3 passa a valer, e o investidor ganha 6 ações
que não existem. O estado depois de recuperar seria diferente do estado antes de cair — I11
violado, e violado de um jeito que nenhum teste de "aplica duas vezes seguidas" pega.

**Portanto**: o conjunto de idempotência é parte do estado da partição, mora nas arenas SoA junto
com os ledgers e é copiado no *stall-and-copy* do EOD como qualquer outro bucket. Não é cache. A
regra geral que ele ilustra: **se uma estrutura muda a decisão do `apply`, ela é estado e vai para
o snapshot.** Cache é o que dá para reconstruir sem mudar decisão nenhuma.

## Como o conjunto não cresce para sempre

Guardar todo `(evento_id, conta)` desde o começo dos tempos é inviável. O conjunto é podado por
`AberturaDia`: eventos corporativos com data-ex anterior a `D − 60` saem. Sessenta dias é folgado
para qualquer reentrega de arquivo da B3 e cabe em memória.

A poda é feita **por evento** (`AberturaDia` carrega a data), nunca por relógio — se fosse por
relógio, o replay podaria em pontos diferentes da execução original e o estado divergiria. É I12
protegendo I6: o determinismo não é uma propriedade que se verifica no fim, é uma restrição que
molda cada estrutura de dados.

## Teste

| Passo | Afirmação |
|---|---|
| aplica 903, aplica 903 | posição igual à de uma aplicação só; `ApplyResult::Duplicado` na segunda |
| aplica 903 na conta A e na conta B | as duas recebem — a chave inclui a conta |
| 903 e 904 na mesma conta, mesma data-ex | as duas aplicam — a chave é o evento, não a data |
| snapshot em k, duplicata, recupera, compara | estado idêntico ao de antes da queda (I11) |
| `AberturaDia` avançando 61 dias, reentrega 903 | reaplica — a chave saiu da janela, e o teste **documenta** isso como comportamento aceito, não como bug |

A última linha é desconfortável de propósito. A janela de 60 dias é uma escolha de engenharia com
consequência real; o teste existe para que a consequência esteja escrita em algum lugar em vez de
ser descoberta por um investidor.
