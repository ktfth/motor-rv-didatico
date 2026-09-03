# WAL, group commit, io_uring e snapshot EOD

Dono: `persistencia`. Invariantes I8–I12. ADRs 0004, 0012, 0013, 0014, 0015.

## Papel

Um log por partição, um único escritor, LSN monotônico por partição. O estado em memória é
derivado do log — nunca o contrário. Todo não-determinismo entra como evento
(`AberturaDia{data}`, cotação de fechamento, resultado de reconciliação). O replay nunca
consulta relógio, arquivo externo ou RNG.

## Formato de registro

```cpp
struct alignas(8) WalHdr {   // 32 bytes, little-endian
  uint32_t magic;    // 'RVW1'
  uint32_t crc32c;   // header (crc=0) + payload
  uint64_t lsn;      // monotônico por partição
  uint64_t ts_ns;    // auditoria; ignorado no replay
  uint32_t epoch;    // aleatório por segmento
  uint16_t tmpl;     // templateId SBE
  uint16_t len;      // bytes do payload SBE (≤ 64 KiB)
};                   // payload, padding até múltiplo de 8
```

Payload é o buffer SBE vindo do ingress, copiado uma vez para o buffer de commit; `apply` lê
dali. CRC32C por hardware (`_mm_crc32_u64`).

## Segmentos

1 GiB, nomeados pelo primeiro LSN, cabeçalho com partição, epoch, versão de formato. Criados
por thread de fundo, sempre dois à frente: `fallocate` seguido de escrita real de zeros (extent
não inicializado vira metadado na primeira escrita e encarece o `fdatasync`). fd aberto com
`O_DIRECT|O_DSYNC`. Alinhamento de buffer, offset e tamanho vem de `statx(STATX_DIOALIGN)`.

## Group commit

- Primeiro append não durável abre janela `W`; o grupo é submetido quando `now ≥ deadline`
  ou o buffer chega a `kMaxGroup`.
- Buffers de 64 KiB, alinhados a 4 KiB, registrados no ring (`WRITE_FIXED`). Último bloco
  preenchido com zeros (ADR-0013); reescrita de cauda é experimento.
- `O_DSYNC`: completar a escrita é ser durável (NVMe: write com FUA). Um pedido por grupo.
- `apply` roda imediatamente (estado otimista). Toda saída — read model intradiário,
  confirmações, mensagens entre partições — vai para um outbox liberado só até `durable_lsn`.
- Até `kMaxInflight` grupos em voo. Completions podem chegar fora de ordem; `durable_lsn`
  avança em ordem FIFO. Grupo N falho e N+1 completo → N+1 é truncado na recuperação, correto
  porque nada além de N foi externalizado.
- Erro ou short write em CQE é fail-stop da partição.

## io_uring

Um ring por thread de partição. `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`
(kernel ≥ 6.1); arquivos e buffers registrados.

```cpp
void Wal::maybe_submit(uint64_t now) {
  if (!cur_.len || (now < cur_.deadline && cur_.len < kMaxGroup)) return;
  pad_to_block(cur_);
  auto* sqe = io_uring_get_sqe(&ring_);
  io_uring_prep_write_fixed(sqe, seg_idx_, cur_.buf, cur_.len, seg_off_, cur_.buf_idx);
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, cur_.last_lsn);
  inflight_.push({cur_.last_lsn, cur_.buf_idx});   // FIFO, cap kMaxInflight
  seg_off_ += cur_.len; cur_ = next_free_buffer();
  io_uring_submit(&ring_);
}
void Wal::reap() {
  io_uring_cqe* cqe;
  while (io_uring_peek_cqe(&ring_, &cqe) == 0) {
    if (cqe->res != inflight_len(cqe)) fail_stop(cqe->res);
    mark_done(io_uring_cqe_get_data64(cqe));       // durable_lsn avança em ordem
    io_uring_cqe_seen(&ring_, cqe);
  }
}
```

Loop da partição: drena SPSC ring em lote → `append` + `apply` → `maybe_submit` → `reap` →
libera outbox até `durable_lsn`. Busy-poll por padrão. Se a partição puder dormir,
`io_uring_wait_cqe_timeout` cobre CQEs, o deadline do grupo como timeout e um `POLL_ADD` num
eventfd que o ingress sinaliza quando encontra a flag "consumidor dormindo". SQPOLL é
experimento.

## Recuperação

1. Carrega o snapshot mais recente (`snapshot_lsn`).
2. Percorre segmentos a partir de `snapshot_lsn + 1`: valida magic, `len` dentro do limite,
   `epoch` igual ao do cabeçalho do segmento, CRC e `lsn == esperado`. Primeira falha encerra o
   log ali (cauda de torn write ou zeros de pré-alocação).
3. Aplica cada registro. O estado é função pura do prefixo válido.

`epoch` + continuidade de LSN permitem reciclar segmentos sem que lixo antigo passe por válido.

## Snapshot EOD

Sequência (tudo como evento):
1. Arquivos B3 ingeridos (negócios finais, alocações, posição da depositária, proventos,
   cotações de fechamento).
2. `CotacaoFechada` por instrumento → balances marcados a fechamento.
3. `ReconciliacaoDepositaria{data, divergências}` — não bloqueia; marca o snapshot.
4. `EodMark{data}` → commit forçado → espera `durable_lsn ≥ eod_lsn`.
5. Snapshot exatamente em `eod_lsn`.

Método v1: stall-and-copy (pausa o loop, memcpy das arenas SoA para staging, retoma; exato no
LSN; ~20–40 ms para 200 MB, uma vez por dia com mercado fechado) — ADR-0014.
Experimento v2: fuzzy por chunks de 256 KiB com `chunk_lsn`; no replay, evento aplica ao slot
só se `lsn > chunk_lsn(slot)`; índices são chunks; alocação de slots determinística.
fork(): descartado (COW com huge pages copia 2 MiB por fault; filho não pode tocar malloc).

Dois artefatos do mesmo stall: snapshot de recuperação (estado completo) e snapshot de
exposição (visão D-1). Thread auxiliar por partição grava ambos com ring próprio
(`WRITE_FIXED` de 1 MiB, várias em voo), `fdatasync`, `rename` atômico, `fsync` do diretório.

### Layout do snapshot de exposição (mapeável pelo resource server)

- Cabeçalho 4 KiB: magic, versão, partição, `snapshot_lsn`, data-base, tabela de seções
  `(offset, len, crc32c)`, flag de divergência de reconciliação.
- Instrumentos SoA: ticker, ISIN, tipo, fator de cotação, fechamento de D.
- Índice de contas (open addressing sobre hash do documento) → slot.
- Posições em CSR por conta: `instrument_id[]`, buckets de quantidade, preço médio.
- Movimentos dos últimos 7 dias em CSR por conta (`transactions-current`).
- Notas de corretagem indexadas por `brokerNoteId`.
- Blobs JSON pré-serializados por `investmentId` (detalhe, balances) e por conta (lista).
- Rodapé com CRC global.

Só offsets, nunca ponteiros. Histórico completo de movimentos fica em segmentos mensais
imutáveis referenciados pelo cabeçalho.

### Publicação

`snapshot/p{k}/{data}/v{n}`, imutável. Manifesto global (tmp + rename) lista partição →
arquivo → LSN; o conjunto de D só é válido com `EodMark{D}` em todas as partições. O resource
server mapeia com `MAP_POPULATE` + `MADV_HUGEPAGE`, valida CRC, troca um
`std::atomic<Snapshot*>`; o mapeamento antigo é liberado quando o refcount de requisições em
voo zera. Correção da B3 na manhã seguinte → `v{n+1}` para a mesma data. Se o EOD de D atrasa,
o resource server continua em D-2 (a especificação da API admite).

### Arquivamento

Segmentos totalmente cobertos por snapshot durável são comprimidos (zstd) e enviados ao
armazenamento frio; o WAL é trilha de auditoria e segue o prazo regulatório de guarda.

## Parâmetros iniciais

| Parâmetro | Default | Razão |
|---|---|---|
| Janela W | 100 µs | latência de durabilidade ≈ W + escrita FUA |
| Grupo máx | 64 KiB | 16 blocos; amortiza sem inflar latência |
| Grupos em voo | 8 | cobre a latência do dispositivo sem reordenação lógica |
| Bloco | `statx` DIOALIGN | nunca assumir 4 KiB |
| Segmento | 1 GiB pré-zerado, 2 à frente | zero espera por criação/extensão |
| Chunk fuzzy | 256 KiB | ~25 µs de stall por chunk (v2) |
| NVMe do WAL | enterprise com PLP, dedicado | sem PLP, FUA custa milissegundos |

## Métricas obrigatórias

Distribuição de tamanho de grupo; latência append→durável (P50/P99/P999); grupos em voo;
bytes/s; duração do snapshot; tempo de recuperação medido. Referência: 1M eventos/s de replay
e 10M eventos/dia por partição → RTO ~10 s com snapshot só no EOD; é o número que decide se a
v2 fuzzy vale.

## Decisões fechadas

O_DSYNC no lugar de fsync encadeado (ADR-0012); padding antes de reescrita de cauda
(ADR-0013); stall-and-copy antes de fuzzy (ADR-0014); criptografia em repouso no dispositivo,
não por registro (ADR-0015).
