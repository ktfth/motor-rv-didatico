# ADR-0026 — Uma tabela só de métricas para o harness, o comparador e o relatório

Status: aceito (07/09/2026). Complementa ADR-0021.

## Contexto
A correspondência entre a série que o harness mede e a chave que `bench/baseline.json` publica
estava escrita em três lugares: `bench/saida.cpp` (que emite o JSON), `bench/comparador.cpp` (que
compara, com o prefixo `metricas.` digitado à parte) e o default de `--exigir` em
`scripts/relatorio-bench.py`. Renomear uma série atualizava uma cópia; as outras passavam a
procurar um nome que não existe mais.

Procurar um nome inexistente **não é erro** — é silêncio. Medido, com um baseline que nenhuma
máquina passaria (regressão de 160×): o comparador saía com código **0** e a mensagem "nenhuma
métrica desta execução tem chave no baseline", e o JSON gravava a métrica como `null`. O portão
reportava verde tendo comparado coisa nenhuma. É o mesmo defeito que `HANDOFF.md` registra como o
pior da revisão de 03/09 — o LTO que nunca ligava, o preset `tsan` que selecionava zero testes.

## Decisão
`bench/contrato.hpp` é a **única** descrição do esquema de métricas: para cada métrica, a chave do
baseline, a série que a alimenta, a forma do valor no JSON, o rótulo em português corrente e, quando
ela ainda não é medida, o motivo. `saida.cpp` gera o JSON percorrendo a tabela; `comparador.cpp`
compara por ela e monta o prefixo em vez de digitá-lo; o Python a recebe no bloco `contrato` do
próprio JSON de medição, em vez de repetir a lista.

Duas defesas, porque a tabela sozinha não impede a divergência:
- `static_assert` de `esquema_coerente()` — métrica sem série e sem motivo, série sem grupo, métrica
  de quantis sem subcampos: não compila.
- `contrato_quebrado()`, em execução: a suíte rodou e a série contratual não apareceu ⇒ o processo
  sai com código 2 e **não grava JSON**. É o que fecha o buraco do rename.
- `--conferir-esquema ARQ` confronta as chaves de um baseline com a tabela **nos dois sentidos**
  (chave que falta, chave que o esquema não conhece), e o CI o roda contra `bench/baseline.json` e
  contra uma cópia com chave renomeada de propósito.

## Alternativas consideradas
- **Convenção de nome** (a chave é derivada do nome da série): a chave do baseline é contrato
  versionado e o nome da série é organização interna. Amarrá-los por convenção quebra o baseline a
  cada renomeação de série, que é o oposto do que se quer.
- **Gerar o contrato em JSON no build e ler dos dois lados**: introduz artefato gerado no build para
  resolver um problema de duas dezenas de linhas de C++.

## Consequências
Acrescentar uma métrica ao esquema passou a ser uma linha, verificado acrescentando e removendo uma
métrica falsa. Uma métrica de quantis com série ligada quebra o build até o emissor aprender a
escrevê-la. `bench/baseline.json` deixou de poder divergir da tabela sem que o CI reclame. E o
`schema` do documento subiu para **2**, porque o bloco `contrato` passou a ser exigido: um número de
versão que não muda quando o formato muda é a mesma classe de silêncio que este ADR fecha.
