# Rastreabilidade — requisito regulatório → teste

Dono: `regulatorio-open-finance`. Uma linha por requisito; fonte obrigatória (URL/seção em
`docs/open-finance.md`). Coluna "Teste" preenchida antes do fim da fase 4; "—" significa
pendente, nunca "não se aplica" sem justificativa na coluna Nota.

| Id | Requisito | Fonte | Camada | Teste | Nota |
|---|---|---|---|---|---|
| R1 | Recusar requisição sem `x-fapi-interaction-id`; ecoar o valor na resposta | FAPI-BR 5.1 item 18 | RS | — | |
| R2 | Access token: PS256 via JWKS do AS; `exp` respeitado (300–900 s) | FAPI-BR 5.1 item 11; 6.2 | RS | — | |
| R3 | Token vinculado ao certificado: `cnf.x5t#S256` == thumbprint do cert mTLS | RFC 8705 (ref. normativa) | RS | — | confirmar exigência na 2.2.1 |
| R4 | Compartilhar só com consentimento AUTHORISED; token inválido → 401 | FAPI-BR 8.1 itens 2, 2.1 | RS | — | |
| R5 | Escopo parametrizável `consent:urn:…` e permissão do produto | FAPI-BR 7.1 | RS | — | |
| R6 | TLS: cipher suites obrigatórios; resumption e renegotiation off | FAPI-BR 6.4 | RS/TLS | — | |
| R7 | Rejeitar `alg` ≠ PS256; `kid` desconhecido; assinatura inválida → 400 BAD_SIGNATURE (JWS de mensagem) | FAPI-BR 6.1, 6.2 | RS | — | |
| R8 | `GET /investments` — schema, paginação, cabeçalhos | API RV 1.3.0 | RS | — | |
| R9 | `GET /investments/{id}` — schema | API RV 1.3.0 | RS | — | |
| R10 | `GET /investments/{id}/balances` — schema; `grossAmount` (I7) | API RV 1.3.0 | RS/snapshot | — | |
| R11 | `GET /investments/{id}/transactions` — filtros de data, paginação | API RV 1.3.0 | RS/histórico | — | |
| R12 | `GET /investments/{id}/transactions-current` — janela D-6..D | API RV 1.3.0 | RS/snapshot | — | |
| R13 | `GET /broker-notes/{brokerNoteId}` — schema | API RV 1.3.0 | RS/snapshot | — | caminho a confirmar |
| R14 | Dados de D-1 (ou D-2 com atraso da B3), nunca intradiários | Orientações [DC] Investimentos | snapshot | — | |
| R15 | `resourceId` (API Recursos) == `investmentId` | Orientações [DC] Investimentos | RS | — | |
| R16 | Limites operacionais mensais por consentimento (30/4/30/4/30/30) | Manual de limites | RS | — | confirmar código 423 |
| R17 | P95 diário por endpoint dentro do SLA da classe de frequência | Tempo de resposta das APIs | RS/observabilidade | — | métrica por endpoint |
| R18 | Disponibilidade medida (2XX/422 sucesso; 5XX/408 erro) | Monitoramento | RS/observabilidade | — | |
| R19 | Trilha de auditoria de consentimentos e acessos | FAPI-BR 8.1 item 7; Res. Conjunta 1/2020 | RS/log | — | |
