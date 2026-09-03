# data/ — dados de referência do estudo

Dono: `dominio-pos-negociacao`. Nada aqui é lido dentro do `apply` ou do replay — **isso violaria
I12**. Estes arquivos alimentam o *ingress* e os geradores de cenário, que transformam o conteúdo
em campos de evento. Depois disso, o motor só conhece o que está no log.

| Arquivo | Conteúdo | Quem lê |
|---|---|---|
| `calendario-b3-2026.csv` | 365 linhas, uma por dia: se há pregão, qual é o D+2 em pregões, feriado e tipo | ingress e simulador, para preencher `data_liq` dos negócios |
| `instrumentos.csv` | tabela de instrumentos com ISIN, tipo, fator de cotação e lote padrão | ingress, para internar `instrument_id`; snapshot de exposição, para a seção SoA de instrumentos |

## Sobre o calendário

Gerado por `scripts/gera-calendario.py`, que parte dos feriados nacionais fixos, dos móveis
(calculados a partir da Páscoa pelo algoritmo de Meeus/Jones/Butcher) e dos dias em que a B3 não
abre por decisão própria (24 e 31 de dezembro) ou por feriado estadual de São Paulo (9 de julho).

Em 2026 há **246 pregões**. O gerador carrega os feriados de dois anos de propósito: o D+2 de um
negócio de 30/12 cai em janeiro seguinte, e um mapa de um ano só o colocaria em 1º de janeiro. A Quarta-feira de Cinzas é pregão com abertura às 13h — conta como dia
útil para liquidação, e é o tipo de exceção que só aparece se o calendário for dado e não fórmula.

Conferências que o gerador imprime, e que os cenários golden usam:

| Negócio em | Liquida em | Por quê |
|---|---|---|
| qua 02/09 | sex 04/09 | dois pregões, sem feriado no meio |
| ter 08/09 | qui 10/09 | segunda 07/09 é Independência |
| sex 11/09 | ter 15/09 | pula o fim de semana |
| qua 30/12 | ter 05/01/2027 | pula 31/12 (B3 fechada), o feriado de 1º/01 e o fim de semana |
