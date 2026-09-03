# ADR-0024 — Backend de I/O híbrido: `WalT<B>` por template, base virtual só para os testes

Status: aceito (03/09/2026). **Substitui a mecânica de ADR-0023**; mantém a decisão de fundo.

## Contexto

ADR-0023 decidiu que o backend de I/O do WAL é plugável e que a escolha se resolve em **tempo de
compilação**, com parâmetro de template, "porque uma função virtual colocaria uma chamada indireta
no caminho de commit". A justificativa registrada lá — "`submit`/`reap` acontecem uma vez por
grupo, a cada ~10 mil eventos" — está **errada para `reap`**: o loop da partição chama `reap()` em
toda volta do busy-poll, mesmo quando não há nada a colher. Numa partição ociosa isso é milhões de
chamadas por segundo.

Ao mesmo tempo, a implementação precisou de uma base virtual por uma razão que ADR-0023 não
antecipou: o `FaultBackend` é um **decorador** sobre qualquer backend real, e o harness de caos
guarda um `IoBackend&` escolhido em runtime. Com template puro, cada combinação
(backend × cenário de falha) viraria uma instanciação, e a suíte de caos deixaria de poder
escolher o cenário por configuração.

## Decisão

As duas coisas, cada uma no seu lugar:

- **Produção**: `WalT<UringBackend>` — o backend é parâmetro de template, o compilador enxerga
  `submit` e `reap` e os embute. **Zero chamada indireta no caminho de commit**, que é o que
  ADR-0023 queria garantir.
- **Testes e composição por configuração**: existe uma base `IoBackend` com métodos virtuais.
  Quem guarda um `IoBackend&` — o `FaultBackend`, o harness de caos, um binário que escolhe o
  backend por opção de linha de comando — paga a vtable. Nenhum deles está no caminho medido.

A regra geral do projeto, agora escrita: **`concept` (ou template) onde a chamada é por evento ou
por volta de loop; virtual onde a chamada é por operação rara e a escolha é de runtime.** O
`concept Journal` de `src/core/journal.hpp` é o primeiro caso; este ADR é o segundo.

## Alternativas consideradas

- **Só template** (ADR-0023 literal): obrigaria o harness de caos a instanciar cada combinação e
  a escolher o cenário em compilação. A suíte de crash perderia a capacidade de variar o cenário
  por configuração, que é justamente o que a torna útil.
- **Só virtual**: colocaria uma indireção em `reap()`, chamado a cada volta do busy-poll.
- **Erasure com ponteiro de função**: mesma indireção da virtual, com menos legibilidade.

## Consequências

`src/wal/io_backend.hpp` declara a base virtual e documenta, no próprio cabeçalho, que produção não
passa por ela. Um teste de desempenho que instanciasse `WalT<IoBackend&>` mediria a coisa errada —
e é por isso que o harness de bench (ADR-0021) fixa `UringBackend` como parâmetro de template, sem
opção.

A afirmação factualmente errada de ADR-0023 ("`reap` roda uma vez por grupo") fica registrada aqui
em vez de corrigida lá: ADR aceito é imutável, e apagar o erro apagaria a informação de que a
premissa foi conferida tarde demais.
