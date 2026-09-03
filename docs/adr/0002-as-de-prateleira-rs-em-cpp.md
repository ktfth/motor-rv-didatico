# ADR-0002 — Servidor de autorização de prateleira certificado; resource server em C++

Status: aceito (02/09/2026)

## Contexto
O AS não está no hot path e exige certificação OpenID (suíte da OIDF) e DCR/DCM contra o Diretório. Escrever um AS em C++ multiplica superfície criptográfica sem ganho de desempenho.

## Decisão
Usar AS certificado (produto de mercado ou open source certificado); este repositório entrega apenas o resource server em C++, que valida tokens localmente e aplica consentimento e limites.

## Alternativas consideradas
AS próprio em C++.

## Consequências
Dependência de um AS externo em ambiente de teste (fase 5); o RS precisa de JWKS do AS e de canal de eventos de consentimento.
