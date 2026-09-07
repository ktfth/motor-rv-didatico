# O portão de regressão — quatro rodadas até ele funcionar

07/09/2026. Autor: `desempenho`. Revisão: `verificador` (rodadas 1 a 3; a rodada 4 **não** foi
revisada — ver §5). Máquina: Xeon @2.10 GHz, 4 vCPU, KVM, kernel 6.18.44 — **não** é a máquina de
referência de `docs/ambiente.md`.

## 1. O que estava errado no que foi entregue em 06/09

A entrega de 06/09 trazia um gate de regressão no CI e a documentação afirmava que ele pegava
"regressão grossa — um fator, um `O(n²)` acidental". Uma revisão independente mediu:

- **taxa de fechamento 0/5** — em cinco execuções com o mesmo binário dos dois lados, nenhuma das
  duas métricas obrigatórias chegou a ser comparada;
- **uma regressão de 2× passou 5/5**, e o job não falhou: saiu `::warning::`.

O gate era decorativo. Pior que gate nenhum, porque dava confiança falsa — exatamente o defeito que
`HANDOFF.md` registra como o pior da revisão de 03/09 (o LTO que nunca ligava, o preset `tsan` que
selecionava zero testes).

**Causa raiz:** ele reusava o filtro de estabilidade do harness — série com CV intra-execução acima
de 5 % é descartada — como pré-condição para comparar. Aquele filtro existe para decidir se um
número vira *baseline*, onde ser conservador é certo. Aplicado à comparação, ele jogava fora
justamente a métrica que o portão existe para olhar.

## 2. As quatro rodadas

| rodada | veredito | o que caiu |
|---|---|---|
| 1 | **DEVOLVIDO** | portão vacuoso (0/5), cego para um fator de 2 |
| 2 | aprovado com ressalvas | fecha (5/5), mas três fail-open: rename saía verde; medição que falhou virava "melhor"; sem teto de ruído |
| 3 | **APROVADO** | 15/15 fechamento, 0/15 vermelho falso, 5/5 regressão real — e três portas novas |
| 4 | não revisada | `--exigir` vazio saía verde sobre regressão de 2×; só 2 de 18 séries podiam reprovar |

O padrão vale mais que a lista: **cada conserto abriu uma porta nova**, e três delas eram da mesma
classe do defeito original — o portão ficando verde tendo comparado nada. A mais instrutiva:
renomear uma série contratual, que é a edição de uma linha que o próprio desenho incentiva, fazia a
base "não ter" a métrica; isso era lido como *métrica nova* e liberava verde.

## 3. Onde chegou

O gate mede os dois lados **na mesma execução**, no mesmo runner, intercalados, cinco vezes cada
(ADR-0027). Declara regressão quando a diferença passa do limiar do projeto (5 %) **e** de 3× o erro
padrão da mediana das duas séries. Teto de 50 % no limiar exigido — o teto é igual ao menor efeito
que o gate promete enxergar.

Medido, com os parâmetros do job:

| | resultado |
|---|---|
| fechamento da métrica obrigatória | **15/15** |
| vermelho falso | **0/15** |
| cegueira (não deu para dizer) | **0/15** |
| regressão **real** de código reprovada | **10/10** |

A regressão real é `#pragma GCC optimize("O0")` em `src/core/apply.cpp`, com as duas árvores
compiladas de verdade — não amostras escaladas num arquivo.

## 4. Os dois achados de método

**Regressão sintética não é prova.** Escalar as amostras de um JSON preserva o ruído exatamente e
**superestima** a detecção. Medido: a mesma métrica foi detectada 5/5 na regressão sintética e
**4/5** na real. Toda mudança neste critério exige uma segunda árvore compilada.

**Consistência não é correção.** Numa execução, todas as séries de io_uring vieram com p50 de
exatamente 4,0 ms — 55× o valor normal — com CV de 0,8 %, isto é, perfeitamente "estáveis" pelo
filtro. O harness aprovou. Não se reproduziu; era a máquina virtual passando mal. O coeficiente de
variação mede dispersão dentro de uma série e uma anomalia estável passa por ele sem tocar.

## 5. O que NÃO foi verificado, e o que isso implica

A rodada 4 (commits `6323293` e `4c7a534`) **não passou pelo `verificador`** — a revisão foi
interrompida. A regra do projeto é que autor nunca verifica o próprio trabalho, então **estes dois
commits estão em dívida de revisão** e não deveriam ser mergeados sem ela.

Uma afirmação da rodada 4 já não se sustentou numa conferência rápida. O autor atribuiu a
desqualificação de `snapshot.salva.duracao_ms` a **viés de layout de binário** — dois binários de
código idêntico rendendo diferente. Conferido com três execuções por lado: a série oscila **±13 %
entre execuções do mesmo binário**, e duas compilações distintas diferiram **1,6 %** na mediana, ou
seja, dentro daquela oscilação. O fato que sustenta a decisão — a série é ruidosa demais para gatear
a 6–8 % de limiar — está medido e vale. A **causa** não está estabelecida. Está registrado assim em
ADR-0028.

## 6. O que continua aberto

1. **Fixar `bench/baseline.json`** na máquina de referência. Nenhum número deste relatório vale como
   baseline (ADR-0022).
2. **Uma única série decide** a reprovação hoje. As outras três candidatas voltam com um mecanismo
   que as sustente — a reexecução de confirmação antes de reprovar é a mais barata —, e cada uma
   precisa da campanha de 15 rodadas de ADR-0028.
3. **`load_state_image` proporcional à capacidade configurada**: a mesma imagem carrega em 9,31 ms
   ou 1,22 ms conforme o dimensionamento. É o defeito de `src/core/` que a medição de 06/09 achou e
   que ninguém corrigiu ainda — o experimento que prova o conserto já está no harness.
4. **`CLAUDE.md` §Idioma** contra a prática de `bench/`, `src/app/` e `src/ingress/`: a regra diz
   "identificadores em inglês" e as três camadas são português inteiro. Uma tentativa de resolver
   isso dentro de um commit de desempenho foi revertida (era decisão de contrato, não de
   implementação, e o próprio commit a violava). Continua sem decisão escrita.
