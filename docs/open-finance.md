# Open Finance — perfil, API e requisitos não funcionais

Dono: `regulatorio-open-finance`. Verificado em 02/09/2026 nas fontes abaixo; reverificar a
cada fase e registrar aqui a data e as divergências.

## Perfil de segurança — FAPI-BR

Fonte: Área do Desenvolvedor do Open Finance Brasil → Segurança → FAPI. Versão vigente na
verificação: FAPI-BR v2.2.1 (histórico: 2.2.0, 2.1.0, 2.0.0, 1.0.0). O texto abaixo foi
extraído da v2.1.0; conferir o diff da 2.2.1 antes da fase 4.

O perfil é uma implementação/subconjunto obrigatório do FAPI 1.0 Advanced (OIDF), não do FAPI
2.0. O "FAPI-Único" unificou os perfis: PAR obrigatório e, por consequência, PKCE obrigatório.

Servidor de autorização (resumo dos "deve"):
- autenticação de cliente `private_key_jwt`; PAR obrigatório; PKCE obrigatório;
- discovery (`.well-known`) com os escopos obrigatórios declarados, entre eles
  `variable-incomes`, mesmo sem oferecer o produto;
- `response_type` `code id_token`; `response_mode` `fragment`; `subject_type` `public`;
- id_token criptografado (JWE RSA-OAEP + A256GCM) com chave `enc` do JWKS do cliente;
- access token com expiração entre 300 e 900 s; refresh token sem rotação e sem prazo quando
  necessário; `request_uri` com expiração mínima de 60 s;
- recusar requisições sem `x-fapi-interaction-id` em recursos protegidos;
- escopo parametrizável `consent:urn:<org>:<id>`; claims `acr` `urn:brasil:openbanking:loa2`
  (loa3 recomendado para pagamentos);
- só compartilhar recurso com token vinculado a consentimento AUTHORISED; token inválido → 401;
  revogar tokens quando o consentimento for apagado; trilha de auditoria de consentimentos.

Assinatura e transporte:
- JWS com PS256; JWE com RSA-OAEP/A256GCM; ICP-Brasil emite só RSA (sem EC);
- mensagens JWS: `aud`, `iss`, `jti` (UUID v4, único por clientId em 24 h), `iat` (±60 s),
  `kid` obrigatório, `typ: JWT`, content-type `application/jwt`; erro de assinatura → 400
  `BAD_SIGNATURE`;
- TLS: suportar TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 e TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384;
  TLS Session Resumption e Renegotiation desabilitados;
- referências normativas incluem RFC 8705 (mTLS e tokens vinculados a certificado) e BCP 195.

## API Renda Variável ([DC] Investimentos)

Versão vigente na verificação: v1.3.0. Base `/open-banking/variable-incomes/v1`:
- `GET /investments` — lista
- `GET /investments/{investmentId}` — detalhe
- `GET /investments/{investmentId}/balances` — posição
- `GET /investments/{investmentId}/transactions` — movimentações históricas
- `GET /investments/{investmentId}/transactions-current` — até 7 dias (D-6 a D)
- `GET /broker-notes/{brokerNoteId}` — nota de negociação (verificar o caminho exato na 1.3.0;
  versões antigas o aninhavam sob `/investments/{investmentId}`)

Orientações do GT: posição e movimentações expostas são do fechamento do dia anterior (D-1),
podendo refletir D-2 pela janela de processamento dos arquivos da B3; `resourceId` da API
Recursos == `investmentId`. Permissão associada: `VARIABLE_INCOMES_READ` (confirmar nome no
consentimento v3).

## Requisitos não funcionais

Tempo de resposta (monitoramento): P95 diário por endpoint ≤ 1.500 ms (alta e média-alta
frequência), ≤ 2.000 ms (média), ≤ 4.000 ms (baixa); apuração mensal com tolerância de 20 %
em dias isolados. Disponibilidade calculada empiricamente em janelas de 30 e 90 dias
(2XX e 422 = sucesso; 5XX e 408 = erro).

Limites operacionais mensais por consentimento (RV): `/investments` 30; `/{id}` 4;
`/balances` 30; `/transactions` 4; `/transactions-current` 30; `/broker-notes` 30.
Códigos a confirmar no manual vigente: 423 (limite operacional) e 429 (rate limit).

## Certificação

Transmissoras precisam de certificação de segurança (perfil FAPI e DCR, suíte da OpenID
Foundation) e funcional (conformance suite do OFB). É o motivo do ADR-0002: AS de prateleira
certificado; o resource server em C++ é o que este repositório entrega.

## Fontes consultadas (02/09/2026)

- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/1334149137 (FAPI-BR v2.1.0 PT)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/1957625885 (FAPI-BR v2.2.1)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/346652711 (convivência FAPI-Único)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/103284839 (APIs Investimentos)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/102957060 (orientações D-1)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/801996803 (tempo de resposta)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/1069350926 (endpoints PCM)
- https://openfinancebrasil.atlassian.net/wiki/spaces/OF/pages/155910145 (certificação)
