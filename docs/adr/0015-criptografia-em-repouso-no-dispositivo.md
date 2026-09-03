# ADR-0015 — Criptografia em repouso no dispositivo, não por registro

Status: aceito (02/09/2026)

## Contexto
O WAL contém CPF (dado pessoal, LGPD). Cifrar por registro no hot path desloca custo para o lugar errado e complica replay e fuzz.

## Decisão
dm-crypt/LUKS ou NVMe autocriptografado (SED) nos volumes de WAL, snapshot e arquivo; chaves fora do repositório.

## Alternativas consideradas
AES por registro; cifrar só campos de identificação.

## Consequências
Backups e arquivo frio seguem a mesma regra; auditoria de acesso ao volume faz parte da observabilidade.
