# ADR-0006 — SBE como codificação interna de eventos

Status: aceito (02/09/2026)

## Contexto
Mensagens de tamanho fixo e decodificação sem cópia importam no ingress e no WAL; a B3 usa SBE nos protocolos binários (UMDF/EntryPoint).

## Decisão
Schemas em schema/*.xml, codecs C++ gerados por sbe-tool; JSON só na borda Open Finance.

## Alternativas consideradas
Protobuf; JSON interno; structs C++ à mão.

## Consequências
Compatibilidade de schema versionada; codecs nunca editados à mão.
