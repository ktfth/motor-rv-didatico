# ADR-0023 — Backend de I/O do WAL plugável (io_uring | pwrite), escolhido em compilação

Status: aceito (02/09/2026). Complementa ADR-0012 e ADR-0008.

## Contexto
I8–I12 exigem testar completions fora de ordem, escrita curta, falha de grupo N com N+1 completo e
`kill -9` no meio de um grupo. Com io_uring de verdade, esses cenários dependem de sorte. Além
disso, um teste de invariante que precisa de kernel ≥ 6.1 e de um dispositivo específico deixa de
rodar onde mais importa: no CI de quem for manter isto.

## Decisão
O WAL fala com um *backend* de I/O de interface estreita — submeter escrita alinhada em offset,
colher completions, `fail_stop` — resolvido em **tempo de compilação** (parâmetro de template, não
função virtual: CODING_RULES §1 e o hot path não pagam indireção). Três implementações:
`UringBackend` (produção), `PwriteBackend` (portabilidade e depuração, síncrono) e `FaultBackend`
(testes: reordena completions, encurta escritas, injeta erro em grupo escolhido, congela a
conclusão para simular queda).

## Alternativas consideradas
- Interface virtual: mais simples de escrever, mas coloca uma chamada indireta no caminho de commit
  e impede o compilador de embutir o `reap`.
- Testar só com io_uring e `dm-flakey`: cobre o dispositivo, não cobre a lógica de ordenação FIFO do
  `durable_lsn`, que é onde I9 e I10 podem quebrar.

## Consequências
O `FaultBackend` passa a ser parte da definição de "suíte de crash verde" do critério de saída da
fase 2 — junto, não no lugar, dos testes com `kill -9` reais. A interface estreita é um compromisso
público: mudá-la exige revisitar este ADR.
