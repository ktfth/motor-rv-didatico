# ADR-0027 — O gate de regressão em CI compara os dois lados na mesma execução, com limiar calibrado pelo ruído medido

Status: aceito (07/09/2026). Complementa ADR-0016 e ADR-0021; não substitui nenhum.

## Contexto
ADR-0016 exige medição antes de qualquer otimização e ADR-0021 dá o harness. Falta a pergunta que um
PR faz: **este código está mais lento que a base?**

`bench/baseline.json` não responde isso no CI. Ele é fixado na máquina de referência (ADR-0022) e um
runner do GitHub tem outra CPU, outro disco e vizinhos barulhentos; comparado com aquele baseline,
todo PR reprovaria por hardware. Um gate que reprova por motivo errado é desligado, e junto com ele
vai o gate que funcionava.

A primeira tentativa de responder a pergunta **não funcionou, e o número disse isso**. Ela reusava o
filtro de estabilidade do harness — série com coeficiente de variação intra-execução acima de 5 % é
descartada — como pré-condição para comparar. Aquele filtro existe para decidir se um número vira
baseline, onde ser conservador é certo. Aplicado à comparação, ele descartava justamente a métrica
que o portão existe para olhar: **taxa de fechamento 0/5**, e uma regressão real de 2× passando
**5/5**, enquanto a documentação prometia pegar "um fator".

Tirar o filtro sozinho também não bastou. O estimador de ruído somava o **máximo** do CV
intra-execução com a dispersão entre execuções — dois estimadores conservadores, nenhum deles
encolhendo com o número de repetições. O limiar exigido ficava entre 56 % e 99 %: o portão passava a
opinar, e a opinião era "nada abaixo de 2,5× é regressão".

## Decisão
Quatro peças, cada uma com o número que a sustenta.

1. **Medir os dois lados na mesma execução.** O job compila a árvore da base e a do PR e mede as
   duas **intercaladas**, no mesmo runner. Intercalar controla a deriva da máquina: se ela fica
   lenta no meio, fica lenta para os dois.

2. **O ruído é o erro padrão da mediana**, não a dispersão bruta:
   `sqrt((mediana(cv_intra)/√repetições)² + (cv_entre/√execuções)²)`. Ele encolhe com `√n`, que é o
   que permite comprar precisão com execuções em vez de afrouxar o limiar. A regressão é declarada
   quando a diferença passa do limiar do projeto (5 %) **e** de 3× esse erro padrão.

3. **Teto de 50 % no limiar exigido**, e o número não é redondo por acaso: **o teto é igual ao menor
   efeito que o gate promete enxergar**. O gate promete pegar "um fator" — 2× é Δ −50 %. Acima disso
   uma regressão de 2× caberia dentro do limiar, e "sem regressão" seria afirmação que a medição não
   sustenta; então a série contratual passa a contar como **não comparada** (código 2), pelo mesmo
   princípio que faz "não comparar" ser diferente de "aprovar". A alternativa de 60 % foi medida e
   descartada: numa rodada de regressão real com Δ −46,6 % e exigido 55,9 %, o teto de 60 deixaria
   passar **verde uma regressão real de 2×**.

4. **Cinco execuções por lado, não três.** Com três, o exigido chegou a 65,7 % com a máquina quieta e
   56,0 % sob saturação, com 1/5 de vermelho falso; com cinco, o máximo sob saturação foi 44,7 % —
   5,3 pontos de folga abaixo do teto — e 0/5. É no runner compartilhado que a quinta execução
   compra o teto. Custo: quatro corridas de poucos segundos contra duas compilações com LTO, que
   dominam o job.

**Regressão sintética não é prova.** Escalar as amostras de um JSON preserva o ruído exatamente e
**superestima** a detecção; código realmente mais lento traz ruído próprio. Medido: a mesma métrica
contratual foi detectada 5/5 na regressão sintética e **4/5** na real. Toda mudança neste critério
tem de ser validada com uma segunda árvore compilada de verdade.

## Alternativas consideradas
- **Comparar com `bench/baseline.json` no CI**: reprovaria todo PR por diferença de hardware.
- **Manter o filtro de estabilidade como pré-condição**: medido, 0/5 de fechamento.
- **Limiar fixo mais frouxo (10 %, 20 %)**: não se adapta ao ruído do runner; ou reprova PR inocente
  numa máquina carregada, ou aprova regressão numa máquina quieta.
- **Teto de 60 %**: deixaria passar uma regressão de 2× medida.

## Consequências
Este gate pega **regressão grossa** — um fator, um `O(n²)` acidental, uma flag de otimização que
caiu — e não pega 5 %. Prometer 5 % num runner compartilhado seria construir o gate que se desliga
sozinho. O gate de 5 % continua existindo e é outro: `motor-rv-bench --comparar bench/baseline.json`,
rodado por `desempenho` na máquina de referência, onde o baseline vale.

Os números de calibração (teto, sigmas, execuções por lado) valem para o ruído medido em setembro de
2026. Revisá-los exige a mesma campanha: rodadas de PR inocente e rodadas com regressão **real** de
código, publicando fechamento, vermelho falso, cegueira e detecção — antes e depois.
