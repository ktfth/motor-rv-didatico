# Invariantes — motor-rv

Dono: `dominio-pos-negociacao` (I1–I7) e `persistencia` via relatório (I8–I12). Numeração
nunca é reutilizada. Invariante sem teste não existe: coluna "Teste" obrigatória antes do fim
da fase 1 (I1–I7) e da fase 2 (I8–I12).

| Id | Invariante | Verificação | Teste |
|---|---|---|---|
| I1 | Para cada conta × instrumento: `disponivel + Σ a_liquidar_compra[D] − Σ a_liquidar_venda[D] + bloqueado + sobras` == posição na depositária no fechamento | `ReconciliacaoDepositaria` (EOD) e assert em debug | — |
| I2 | Para cada conta e data D: `a_liquidar[D]` financeiro == soma dos negócios pendentes para D (net da câmara) | assert em debug após cada apply | — |
| I3 | Nenhum bucket de quantidade negativo, exceto `disponivel` em venda a descoberto autorizada (flag explícita) | assert | — |
| I4 | `preco_medio` só muda em `Liquidado(compra)` e `EventoCorporativoAplicado`; nunca em venda | assert por tipo de evento | — |
| I5 | Toda transição da máquina de estados do negócio segue o grafo de `docs/dominio.md`; transição fora do grafo é rejeitada | teste de propriedade | — |
| I6 | Evento corporativo aplica-se uma única vez por (evento, conta) | idempotência por chave | — |
| I7 | `grossAmount` do snapshot == `qty × closingPrice / priceFactor` com arredondamento declarado | teste de golden | — |
| I8 | `lsn` é estritamente crescente por partição; sem lacunas no log válido | recuperação | — |
| I9 | `durable_lsn ≤ last_lsn` e avança apenas em ordem FIFO de grupos | teste com completions embaralhadas | — |
| I10 | Nenhuma saída (outbox) é liberada com `lsn > durable_lsn` | teste de crash (kill -9) | — |
| I11 | Estado após N eventos == estado após snapshot em k + replay k+1..N, para todo k | equivalência de replay | — |
| I12 | Replay não lê relógio, RNG nem I/O externo | grep + teste com relógio congelado | — |
