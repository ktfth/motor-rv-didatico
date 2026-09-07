# ADR-0028 — Quem reprova um PR é um conjunto declarado, separado das métricas de baseline

Status: aceito (07/09/2026). Complementa ADR-0026 e ADR-0027.

## Contexto
`bench/contrato.hpp` (ADR-0026) lista as métricas do esquema de `bench/baseline.json`. Quando o
gate A/B de ADR-0027 passou a restringir a reprovação às "séries do contrato", ele reusou aquela
lista — e as duas coisas não são a mesma.

A lista do contrato responde "quais números viram baseline". A pergunta do gate é outra: "quais
números, ao piorarem, reprovam um PR". Reusar uma pela outra produziu os dois erros opostos, um de
cada vez:

- **Cobertura de menos.** Com o conjunto restrito a duas métricas, uma piora de **+195 %** em
  `snapshot.carrega.duracao_ms` — o tempo de voltar ao ar depois de uma queda — saiu impressa em
  negrito no relatório com o job **passando**.
- **Vermelho falso.** Antes disso, quando *qualquer* série votava, uma série não contratual
  reprovou 1 de 15 rodadas de PR inocente.

## Decisão
`bench/contrato.hpp` declara **duas** listas: as métricas do esquema (alvo de baseline) e
`kSeriesBloqueantes` (o que reprova um PR). `contrato_quebrado()` confere as duas, `saida.cpp`
publica ambas no bloco `contrato`, e o relatório lê de lá — a mesma disciplina de fonte única de
ADR-0026, aplicada a uma segunda política em vez de duplicada em cima da primeira.

**Critério de admissão ao conjunto bloqueante:** uma série só entra depois de medir **zero vermelho
falso em 15 rodadas de PR inocente**, com os parâmetros do job. Medido, hoje entra uma:

| série | vermelho falso em 15 rodadas | decisão |
|---|---|---|
| `nucleo.loop.eventos_por_s_por_core` | 0/15 | **bloqueia** |
| `snapshot.salva.duracao_ms` | 13/15 | fora |
| `nucleo.apply.eventos_por_s` | 1/30 | fora |
| `snapshot.carrega.duracao_ms` | 1/30 + 2 cegas; bimodal | fora |

As três excluídas continuam medidas, continuam no relatório e continuam alvo de baseline — saem
marcadas `informativa, não vota`. Elas voltam a bloquear quando existir um mecanismo que as sustente
(reexecução de confirmação antes de reprovar, piso de limiar por série, ou convergência das séries
de carga), e cada um desses mecanismos precisa da mesma campanha de 15 rodadas para entrar.

## Alternativas consideradas
- **Manter o conjunto do baseline como conjunto bloqueante:** foi o que produziu os dois erros
  acima. Uma lista para duas políticas é a duplicação de ADR-0026 com outra roupa.
- **Deixar todas as séries votarem:** 1/15 de vermelho falso, e um gate que reprova por motivo
  errado é desligado.
- **Admitir séries sem a campanha de 15 rodadas:** é como o conjunto errado entrou da primeira vez.

## Consequências
Hoje **uma única série decide** a reprovação, o que é menos cobertura do que se gostaria. É o que a
medição sustenta, e o número está aqui para que a próxima ampliação seja medida e não estimada.

Uma ressalva sobre a evidência, registrada porque ela não foi confirmada: a campanha que
desqualificou `snapshot.salva.duracao_ms` foi lida como **viés de layout de binário** — dois
binários de código idêntico rendendo diferente. Uma conferência independente com três execuções por
lado **não reproduziu** essa atribuição: a série oscila ±13 % entre execuções do MESMO binário, e
duas compilações distintas diferiram 1,6 % na mediana, ou seja, dentro daquela oscilação. O fato que
sustenta a decisão — a série é ruidosa demais para gatear a um limiar de 6–8 % — está medido e vale;
a **causa** não está estabelecida. Quem for reabrir isto precisa do experimento próprio: várias
compilações por lado, muitas execuções cada, antes de atribuir a diferença ao binário em vez ao
processo.
