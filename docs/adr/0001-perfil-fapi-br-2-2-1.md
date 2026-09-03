# ADR-0001 — Perfil de segurança: FAPI-BR v2.2.1 (subconjunto do FAPI 1.0 Advanced)

Status: aceito (02/09/2026)

## Contexto
O ecossistema exige certificação no perfil vigente do Open Finance Brasil. O perfil FAPI-BR é um subconjunto obrigatório do FAPI 1.0 Advanced; o FAPI 2.0 da OIDF não é o perfil certificável hoje.

## Decisão
Implementar contra FAPI-BR v2.2.1 (private_key_jwt, PAR, PKCE, code id_token/fragment, id_token JWE, PS256, tokens vinculados a certificado, TLS restrito).

## Alternativas consideradas
FAPI 2.0 Security Profile da OIDF (DPoP/mTLS, sem JAR/JARM obrigatórios).

## Consequências
Migração para FAPI 2.0 só quando o GT de Segurança publicar cronograma; o pipeline de validação deve isolar o que muda (formato do token, binding) do que não muda (consentimento, limites).
