# ADR-0012 — WAL com O_DIRECT|O_DSYNC; sem fsync encadeado

Status: aceito (02/09/2026)

## Contexto
Com O_DSYNC, completar a escrita é ser durável (write com FUA no NVMe). Um fsync encadeado dobra os pedidos por grupo.

## Decisão
fd do segmento aberto com O_DIRECT|O_DSYNC; um WRITE_FIXED por grupo; segmentos pré-alocados e pré-zerados para que não haja metadado pendente.

## Alternativas consideradas
write + IORING_OP_FSYNC(DATASYNC) encadeado; buffered I/O + fsync.

## Consequências
Exige NVMe enterprise com PLP no volume do WAL (sem PLP, FUA custa milissegundos). Toolchain/desempenho medem e registram o dispositivo.
