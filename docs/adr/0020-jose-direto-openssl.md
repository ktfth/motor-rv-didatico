# ADR-0020 — JOSE direto sobre `EVP_*` do OpenSSL, no lugar de jwt-cpp

Status: aceito (02/09/2026)

## Contexto
A borda valida JWS PS256 contra o JWKS do AS (R2, R7) e compara `cnf.x5t#S256` (R3). `jwt-cpp` não
existe na máquina de referência. O que precisamos é pequeno e muito específico: **verificar**
assinatura PS256, nunca assinar; recusar qualquer `alg` diferente de PS256 (a defesa clássica contra
troca de algoritmo é recusar o campo, não confiá-lo); resolver `kid` em `EVP_PKEY` já construído e
mantido em cache, porque construir a chave por requisição é o custo real (ADR-0009).

## Decisão
`src/edge/jose` usa `EVP_DigestVerify*` com `RSA_PKCS1_PSS_PADDING` e `RSA_PSS_SALTLEN_DIGEST`
diretamente. O cache de JWKS guarda `EVP_PKEY*` por `kid`; o token validado é memorizado até `exp`.
O `alg` do cabeçalho é comparado a uma constante e a chave vem do `kid` — nunca do token.

## Alternativas consideradas
- Vendorizar jwt-cpp: traz um parser JSON próprio e uma API que aceita mais algoritmos do que
  queremos que existam no binário.
- Delegar a validação ao AS por *introspection*: uma ida à rede por requisição, contra o SLA de R17
  e contra ADR-0009.

## Consequências
Ficamos donos do código que decide se um token vale — o ponto mais sensível da borda. Ele é coberto
por suíte negativa explícita (assinatura trocada, `alg` `none`, `alg` RS256 com chave PS256, `kid`
desconhecido, `exp` vencido, `cnf` divergente) e por fuzz do decodificador base64url antes do parse.
