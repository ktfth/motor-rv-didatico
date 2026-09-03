# Ambiente de referência — medido em 02/09/2026

Máquina onde o motor é construído e medido nesta sessão. Todo agente lê este arquivo antes de
escrever build system, código de plataforma ou benchmark. Números de `bench/baseline.json` só
valem para esta máquina; outra máquina exige novo baseline.

## Hardware

| Item | Valor | Consequência |
|---|---|---|
| CPU | Intel Core i7-2600 (Sandy Bridge, 4C/8T, 3.4 GHz) | — |
| ISA | SSE4.2 + PCLMULQDQ; **sem AVX2, sem BMI2** | `-march=x86-64-v3` gera SIGILL aqui. Baseline é `x86-64-v2` (ADR-0022). |
| CRC32C | `_mm_crc32_u64` disponível (SSE4.2) | caminho de hardware ativo; fallback por tabela obrigatório mesmo assim |
| Threads | 8 | no máximo 4 partições pinadas em cores físicos sem HT |
| FS do WAL | btrfs | ver `STATX_DIOALIGN` abaixo |

## Kernel e I/O

| Item | Valor | Consequência |
|---|---|---|
| Kernel | 7.1.9-arch1-2 | acima do mínimo (6.1) de `DEFER_TASKRUN` |
| liburing | 2.15 | `io_uring_prep_write_fixed`, buffer/file registration, buffer rings disponíveis |
| `IORING_SETUP_SINGLE_ISSUER \| DEFER_TASKRUN` | **OK** (probe) | usar por padrão; manter fallback para ring simples |
| `open(O_DIRECT\|O_DSYNC)` em btrfs | **OK** (probe) | escrita direta + FUA viável |
| `statx(STATX_DIOALIGN)` | **não suportado** (bit ausente da máscara) | não dá para "nunca assumir 4 KiB": quando o bit não vem, cai para 4096 e registra a origem do valor em log e no cabeçalho do segmento |
| write O_DIRECT 4 KiB via io_uring | `res=4096` | caminho completo validado |

## Toolchain

| Item | Valor |
|---|---|
| GCC | 16.2.1 (padrão) |
| Clang | 22.1.8 (segundo compilador dos presets) |
| CMake | 4.4.3 — **local**, em `.toolchain/bin` (não há cmake no sistema) |
| Ninja | 1.13.2 — idem |
| Bootstrap | `scripts/bootstrap-toolchain.sh` recria `.toolchain/` (venv Python + pip) |

## Bibliotecas do sistema

Disponíveis: OpenSSL 3.6.3, liburing 2.15, fmt 12.2.0, zstd, GoogleTest/GoogleMock 1.17.0.

**Ausentes** — cada ausência virou decisão registrada, não gambiarra:

| Faltando | Substituto | ADR |
|---|---|---|
| `sbe-tool` (não há JVM) | gerador próprio em Python sobre `schema/*.xml` | ADR-0017 |
| glaze / simdjson | serializador e parser JSON próprios, no diretório da borda | ADR-0018 |
| Asio / Beast | servidor HTTP próprio sobre io_uring/epoll | ADR-0019 |
| jwt-cpp | JOSE direto sobre `EVP_*` do OpenSSL | ADR-0020 |
| Google Benchmark | harness de medição próprio que escreve `bench/baseline.json` | ADR-0021 |
| CPU com AVX2 | baseline `x86-64-v2`; `-march=native` só em preset opcional | ADR-0022 |

## Como reproduzir os probes

`scripts/probe-ambiente.sh` — recompila e reexecuta as sondas de io_uring, O_DIRECT e statx e
regenera a tabela acima. Rode ao trocar de máquina antes de confiar em qualquer número.
