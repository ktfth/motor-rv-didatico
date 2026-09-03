# ADR-0008 — Asio + io_uring + OpenSSL 3 no lugar de Seastar/DPDK

Status: aceito (02/09/2026)

## Contexto
O SLA do Open Finance (P95 de 1.500 ms) não pede kernel bypass. O ganho do núcleo vem do desenho (single writer, WAL), não da pilha de rede.

## Decisão
Boost.Asio/Beast com backend io_uring para o resource server; liburing direto no WAL; OpenSSL 3 para mTLS (kTLS como experimento).

## Alternativas consideradas
Seastar; DPDK; frameworks HTTP de mais alto nível.

## Consequências
Kernel ≥ 6.1 exigido (SINGLE_ISSUER/DEFER_TASKRUN); toolchain verifica em configure.
