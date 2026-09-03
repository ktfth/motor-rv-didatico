# Cenários golden — pós-negociação de renda variável

Dono: `dominio-pos-negociacao`. Cada cenário é um par (sequência de eventos, estado esperado),
com os números **fechados à mão** neste documento antes de existir código. É isso que faz um
teste golden valer alguma coisa: se o número sai do próprio motor, o teste só prova que o motor
concorda consigo mesmo.

Convenções (ADR-0007, `docs/dominio.md`):
- quantidade e preço: `int64` escala 1e-8 → `100` ações = `10000000000`; `R$ 32,45` de preço =
  `3245000000`.
- BRL financeiro: `int64` escala 1e-4 → `R$ 3.245,00` = `32450000`.
- datas: `uint32` AAAAMMDD.
- arredondamento: toda operação que perde casa declara a política no nome da função. Nos cenários
  abaixo, `HALF_UP` significa meio para cima em valor absoluto; `TRUNC` significa em direção a zero.

Cada arquivo `NN-*.md` traz: contexto, eventos de entrada, estado esperado e **qual invariante
(I1..I7) o cenário exerce**. O teste correspondente vive em `tests/domain/` e cita o arquivo.

| # | Cenário | Invariantes exercidos |
|---|---|---|
| 01 | Compra simples, liquidação em D+2, preço médio | I1, I2, I4 |
| 02 | Compra em dois lotes com preços diferentes, preço médio ponderado | I4 |
| 03 | Venda com posição disponível; preço médio **não** muda | I1, I3, I4 |
| 04 | Venda a descoberto sem flag → rejeitada; com flag → `disponivel` negativo autorizado | I3 |
| 05 | Compra e venda do mesmo papel no mesmo dia (day trade), netting por data | I2 |
| 06 | Falha de entrega em D+2 e recompra | I5 |
| 07 | Desdobramento 1:10 com preço médio dividido | I4, I6 |
| 08 | Grupamento 10:1 com fração indo para `sobras` | I1, I4, I6 |
| 09 | Bonificação 5 % com custo atribuído pela companhia | I4, I6 |
| 10 | Dividendo e JCP com IRRF 15 % | I6 |
| 11 | Subscrição exercida parcialmente | I4, I6 |
| 12 | `grossAmount` do snapshot com fator de cotação ≠ 1 (BDR) | I7 |
| 13 | Idempotência: mesmo evento corporativo entregue duas vezes | I6 |
| 14 | Reconciliação com divergência: marca o snapshot, não bloqueia o EOD | I1 |
