# ADRs — motor-rv

Template: ADR-NNNN — título; Status; Contexto; Decisão; Alternativas; Consequências.
ADR aceito é imutável (hook). Para mudar uma decisão: ADR novo com 'substitui ADR-NNNN'.

| Id | Decisão |
|---|---|
| ADR-0001 | [Perfil de segurança: FAPI-BR v2.2.1 (subconjunto do FAPI 1.0 Advanced)](0001-perfil-fapi-br-2-2-1.md) |
| ADR-0002 | [Servidor de autorização de prateleira certificado; resource server em C++](0002-as-de-prateleira-rs-em-cpp.md) |
| ADR-0003 | [Open Finance lê de snapshot D-1 imutável, não do motor vivo](0003-snapshot-d-1-imutavel.md) |
| ADR-0004 | [Event sourcing com replay determinístico como fonte de verdade](0004-event-sourcing-replay-deterministico.md) |
| ADR-0005 | [Thread-per-core, shared-nothing, partição por hash do documento](0005-thread-per-core-shared-nothing.md) |
| ADR-0006 | [SBE como codificação interna de eventos](0006-sbe-codificacao-interna.md) |
| ADR-0007 | [Dinheiro, quantidade e preço em ponto fixo int64](0007-ponto-fixo-int64.md) |
| ADR-0008 | [Asio + io_uring + OpenSSL 3 no lugar de Seastar/DPDK](0008-asio-io-uring-openssl.md) |
| ADR-0009 | [Access token validado localmente; consentimento em cache com invalidação push](0009-jwt-local-cache-consentimento.md) |
| ADR-0010 | [Escopo v1 sem opções, termo ou BTC](0010-escopo-v1-sem-derivativos.md) |
| ADR-0011 | [Apuração de IR (RFB) como módulo separado](0011-ir-modulo-a-parte.md) |
| ADR-0012 | [WAL com O_DIRECT|O_DSYNC; sem fsync encadeado](0012-wal-o-dsync.md) |
| ADR-0013 | [Padding de bloco antes de reescrita de cauda](0013-wal-padding-antes-de-reescrita.md) |
| ADR-0014 | [Snapshot EOD por stall-and-copy; fuzzy por chunks como experimento; fork() descartado](0014-snapshot-stall-and-copy.md) |
| ADR-0015 | [Criptografia em repouso no dispositivo, não por registro](0015-criptografia-em-repouso-no-dispositivo.md) |
| ADR-0016 | [Governança de otimização: pré-aprovadas vs. experimentos](0016-governanca-de-otimizacao.md) |
| ADR-0017 | [Gerador de codecs SBE próprio, em Python, no lugar do `sbe-tool`](0017-gerador-sbe-proprio.md) |
| ADR-0018 | [Serializador e parser JSON próprios na borda](0018-json-proprio-na-borda.md) |
| ADR-0019 | [Servidor HTTP próprio sobre io_uring/epoll, no lugar de Asio/Beast](0019-http-proprio-sobre-io-uring.md) |
| ADR-0020 | [JOSE direto sobre `EVP_*` do OpenSSL, no lugar de jwt-cpp](0020-jose-direto-openssl.md) |
| ADR-0021 | [Harness de medição próprio que escreve `bench/baseline.json`](0021-harness-de-bench-proprio.md) |
| ADR-0022 | [Baseline `x86-64-v2`; `-march=native` só em preset opcional](0022-baseline-x86-64-v2.md) |
| ADR-0023 | [Backend de I/O do WAL plugável (io_uring \| pwrite), escolhido em compilação](0023-backend-de-io-plugavel.md) |
| ADR-0024 | [Backend de I/O híbrido: `WalT<B>` por template, base virtual só para os testes](0024-io-backend-hibrido.md) — substitui a mecânica de ADR-0023 |

## Nota sobre 0017–0023

Os sete últimos ADRs nasceram do encontro do desenho com a máquina real (`docs/ambiente.md`): cada
biblioteca que o desenho pressupunha e não existe virou decisão explícita, com alternativa
considerada e consequência assumida, em vez de improviso dentro do código. Nenhum deles substitui
ADR anterior; 0019 e 0022 ajustam um ponto de 0008 e da linha de build de `docs/arquitetura.md`.
