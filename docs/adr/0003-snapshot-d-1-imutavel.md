# ADR-0003 — Open Finance lê de snapshot D-1 imutável, não do motor vivo

Status: aceito (02/09/2026)

## Contexto
A API Renda Variável expõe dados do fechamento do dia anterior (D-1, ou D-2 com atraso da B3). Servir do motor vivo acoplaria tráfego regulatório ao hot path sem ganho funcional.

## Decisão
Snapshot de exposição gerado no EOD, versionado por data e número de versão, pré-serializado, mapeado em memória pelo resource server e trocado atomicamente.

## Alternativas consideradas
Servir do read model intradiário com filtro de data; consultar o motor por RPC.

## Consequências
Correções da B3 geram nova versão para a mesma data; se o EOD atrasa, o RS continua em D-2. Histórico de movimentos fica em segmentos mensais, não no snapshot diário.
