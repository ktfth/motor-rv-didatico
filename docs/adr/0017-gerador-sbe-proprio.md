# ADR-0017 — Gerador de codecs SBE próprio, em Python, no lugar do `sbe-tool`

Status: aceito (02/09/2026). Complementa ADR-0006 (não o substitui: a codificação continua SBE).

## Contexto
ADR-0006 fixou SBE como codificação interna e CODING_RULES §6 exige codecs gerados, nunca escritos
à mão. O `sbe-tool` de referência é um jar: exige JVM. A máquina de referência (`docs/ambiente.md`)
não tem `java`, e adicionar uma JVM ao build de um motor C++ para gerar cabeçalhos é uma dependência
de peso desproporcional ao que usamos do SBE — mensagens de campo fixo, sem grupos repetidos
aninhados nem `varData` além de strings curtas.

## Decisão
`scripts/sbe_gen.py` lê `schema/*.xml` (mesmo dialeto do `sbe-tool`: `messageSchema`, `types`,
`message`, `field`, `enum`, `set`, `composite`) e gera cabeçalhos C++ em `src/codec/generated/`.
O gerado continua sendo artefato de build: não é versionado e ninguém o edita. O gerador emite
`static_assert` de tamanho e deslocamento de cada campo, atendendo CODING_RULES §8.

## Alternativas consideradas
- Empacotar a JVM: contamina o build e a imagem de execução por um passo de geração.
- Escrever os structs à mão: viola CODING_RULES §6 e perde o `schemaId`/`version` como contrato.
- Outro formato (FlatBuffers, Cap'n Proto): romperia ADR-0006 e a paridade com o UMDF da B3.

## Consequências
Assumimos a manutenção do gerador. Ele cobre um subconjunto do SBE — o subconjunto que usamos — e
**falha ruidosamente** em construção não suportada, em vez de gerar código silenciosamente errado.
Um teste compara o layout gerado com uma tabela de deslocamentos escrita à mão em
`tests/core/sbe_layout_test.cpp`: se o gerador regredir, o teste quebra antes do motor.
Se um dia houver JVM, trocar o gerador é trocar um passo do CMake — o resto do código não sabe
quem gerou o cabeçalho.
