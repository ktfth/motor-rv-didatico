# Invariantes — motor-rv

Dono: `dominio-pos-negociacao` (I1–I7) e `persistencia` via relatório (I8–I12). Numeração
nunca é reutilizada. Invariante sem teste não existe: coluna "Teste" obrigatória antes do fim
da fase 1 (I1–I7) e da fase 2 (I8–I12).

| Id | Invariante | Verificação | Teste |
|---|---|---|---|
| I1 | Para cada conta × instrumento: `disponivel + Σ a_liquidar_venda[D] + bloqueado + sobras` == posição na depositária no fechamento (**enunciado corrigido — ver "Correções"**) | `ReconciliacaoDepositaria` (EOD) e assert em debug | `test_apply_golden` |
| I2 | Para cada conta e data D: `a_liquidar[D]` financeiro == soma dos negócios pendentes para D (net da câmara) | assert em debug após cada apply | `test_i2_cash_by_date` |
| I3 | Nenhum bucket de quantidade negativo, exceto `disponivel` em venda a descoberto autorizada (flag explícita) | assert | `test_apply_golden` |
| I4 | `preco_medio` só muda em `Liquidado(compra)` e `EventoCorporativoAplicado`; nunca em venda | assert por tipo de evento | `test_rounding_golden`, `test_apply_golden` |
| I5 | Toda transição da máquina de estados do negócio segue o grafo de `docs/dominio.md`; transição fora do grafo é rejeitada | teste de propriedade | `test_apply_golden`, `test_i2_cash_by_date` |
| I6 | Evento corporativo aplica-se uma única vez por (evento, conta) | idempotência por chave | `test_apply_golden` |
| I7 | `grossAmount` do snapshot == `qty × closingPrice / priceFactor` com arredondamento declarado | teste de golden | `test_rounding_golden` |
| I8 | `lsn` é estritamente crescente por partição; sem lacunas no log válido | recuperação | `test_format` |
| I9 | `durable_lsn ≤ last_lsn` e avança apenas em ordem FIFO de grupos | teste com completions embaralhadas | `test_format` |
| I10 | Nenhuma saída (outbox) é liberada com `lsn > durable_lsn` | teste de crash (kill -9) | `test_outbox_gate` |
| I11 | Estado após N eventos == estado após snapshot em k + replay k+1..N, para todo k | equivalência de replay | `test_replay_equivalence` |
| I12 | Replay não lê relógio, RNG nem I/O externo | grep + teste com relógio congelado | `test_replay_equivalence` |
| I13 | Para cada conta × instrumento: `disponivel + Σ a_liquidar_compra[D] + bloqueado + sobras` == posição projetada depois de liquidar tudo que está pendente | assert em debug após cada apply | `test_apply_golden` |

## Correções

### I1 — enunciado original inconsistente com `docs/dominio.md` (corrigido em 02/09/2026)

O enunciado original era:

> `disponivel + Σ a_liquidar_compra[D] − Σ a_liquidar_venda[D] + bloqueado + sobras` == posição na
> depositária no fechamento

Ele não fecha com a tabela de transições de `docs/dominio.md`. Percorrendo o ciclo completo de uma
posição de 137 ações que compra 100 e vende 50, ambas liquidando em D+2:

| Passo | `disponivel` | `a_liq_compra` | `a_liq_venda` | I1 original | Depositária real |
|---|---:|---:|---:|---:|---:|
| inicial | 137 | 0 | 0 | 137 | 137 |
| `Executado(compra 100)` | 137 | 100 | 0 | **237** | 137 |
| `Executado(venda 50)` | 87 | 100 | 50 | 137 | 137 |
| `Liquidado(compra)` | 187 | 0 | 50 | **137** | 237 |
| `Liquidado(venda)` | 187 | 0 | 0 | 187 | 187 |

Duas divergências, e as duas pela mesma razão — o enunciado trocou o sinal dos dois buckets:

1. **Compra pendente não está na depositária.** As ações compradas em D chegam em D+2. Somar
   `a_liquidar_compra` conta ações que ainda não são suas.
2. **Venda pendente ainda está na depositária.** A tabela de `docs/dominio.md` já debita
   `disponivel` no `Executado(venda)` — as ações saem do bucket livre e ficam reservadas para
   entrega, mas continuam depositadas até D+2. Subtrair `a_liquidar_venda` desconta de novo o que
   `disponivel` já descontou.

Os dois erros se cancelam exatamente nos estados em que há uma compra e uma venda pendentes de
tamanho compensatório — que é, muito provavelmente, o caso em que o enunciado foi conferido.

**Enunciado corrigido (I1):** o que a depositária guarda hoje é o que está livre mais o que está
reservado para entrega:

    disponivel + Σ a_liquidar_venda[D] + bloqueado + sobras == posição na depositária

**Invariante novo (I13):** a identidade que o enunciado original *tentava* expressar — a posição
depois que tudo pendente liquidar — é útil e passou a ser invariante próprio, com numeração nova
porque numeração nunca é reutilizada:

    disponivel + Σ a_liquidar_compra[D] + bloqueado + sobras == posição projetada

As duas juntas são o par que a reconciliação usa: I1 confronta o extrato da depositária de hoje,
I13 confronta a projeção que o investidor vê no aplicativo. Um motor que só verificasse uma delas
passaria a semana inteira certo e erraria exatamente nos dois dias em que houvesse liquidação
pendente de um lado só.

Ambos são verificados no cenário `tests/domain/golden/03-venda-preco-medio-inalterado.md`, que
percorre os cinco estados da tabela acima e afirma as duas identidades em cada um.
