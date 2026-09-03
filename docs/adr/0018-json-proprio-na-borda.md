# ADR-0018 — Serializador e parser JSON próprios na borda, no lugar de glaze/simdjson

Status: aceito (02/09/2026)

## Contexto
`docs/arquitetura.md` previa glaze para serializar e simdjson para entrada. Nenhuma das duas existe
na máquina de referência. Mais relevante que a ausência: o desenho já decidiu que os corpos de
resposta são **pré-serializados no snapshot** (ADR-0003, `docs/wal.md`), então o caminho quente da
borda não serializa nada — ele recorta bytes prontos. E a entrada JSON da API RV é mínima: os seis
endpoints são `GET`, sem corpo; o que se analisa são cabeçalhos, query string e JWS.

## Decisão
Dois arquivos pequenos em `src/edge`: um escritor JSON com escape correto e limites de tamanho,
usado **fora** do caminho quente (na geração do snapshot), e um leitor JSON restrito, usado só para
JWKS e para o payload de token — com limite de profundidade, limite de tamanho e recusa de duplicata
de chave antes de qualquer alocação (CODING_RULES §14).

## Alternativas consideradas
- Vendorizar glaze/simdjson como submódulo: peso e superfície de ataque grandes para o uso real.
- Gerar JSON com `fmt` solto: perde escape e limites, exatamente onde a segurança importa.

## Consequências
Menos código de terceiros no processo que recebe tráfego externo, e o parser é pequeno o bastante
para ser coberto por fuzz de verdade (`tests/edge`). Em troca, se um dia o motor precisar serializar
JSON no caminho quente, este ADR deve ser revisto — o escritor próprio não foi desenhado para isso.
