# ADR-0009 — Access token validado localmente; consentimento em cache com invalidação push

Status: aceito (02/09/2026)

## Contexto
Introspecção por requisição adiciona um RTT ao AS em todo acesso. O status do consentimento precisa refletir revogação imediatamente.

## Decisão
JWT (PS256) validado contra JWKS em cache; cnf verificado contra o certificado; status do consentimento em cache replicado no RS, invalidado por evento do serviço de consentimento; tokens validados cacheados até exp (≤ 900 s).

## Alternativas consideradas
Introspecção síncrona por chamada; cache por TTL sem push.

## Consequências
Se o AS escolhido só emitir token opaco, cair para introspecção com cache ≤ TTL do token e registrar o desvio em ADR.
