# ADR-0021 — Harness de medição próprio que escreve `bench/baseline.json`

Status: aceito (02/09/2026)

## Contexto
Google Benchmark não existe na máquina de referência. Além disso, as métricas obrigatórias de
`docs/wal.md` não são "tempo médio de uma função": são distribuições ponta a ponta (append→durável
P50/P99/P999), tamanho de grupo, grupos em voo, duração do snapshot, tempo de recuperação medido.
Um framework de microbenchmark mede a coisa errada com muita cerimônia.

## Decisão
`bench/` traz um harness próprio: histograma de latência de precisão fixa (sem alocação durante a
medição), *warm-up* explícito, descarte de série quando a variação entre repetições passa do limiar,
e saída **direta** no formato de `bench/baseline.json`, incluindo o bloco `ambiente` (commit,
máquina, kernel, flags, dispositivo do WAL) preenchido pelo próprio harness.

## Alternativas consideradas
- Vendorizar Google Benchmark: resolveria microbenchmarks que não são o que precisamos medir.
- Medir com scripts externos: perderia o acesso aos contadores internos (tamanho de grupo, grupos
  em voo) que só existem dentro do processo.

## Consequências
`desempenho` continua sendo o único papel que escreve `bench/baseline.json`, agora por meio do
harness e não à mão — o arquivo passa a ser saída de programa, o que remove a classe de erro de
"número colado do terminal errado". O limiar de regressão de 5 % do arquivo é aplicado por um
comparador no próprio harness, que sai com código diferente de zero quando a regressão ocorre.
