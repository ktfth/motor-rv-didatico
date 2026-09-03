#pragma once
// O estado de UMA partição: tudo o que o `apply` pode ler e escrever, e nada mais.
//
// Uma partição é um core, um loop, um escritor. Nada aqui é compartilhado, nada aqui tem lock
// (ADR-0005, CODING_RULES §5). Toda a memória vem de uma arena selada no fim do warm-up: depois
// disso, uma alocação é um erro fatal e não um pico de latência que ninguém explica.
//
// A regra que decide o que entra nesta struct: **se muda a decisão do `apply`, é estado e vai
// para o snapshot.** O conjunto de idempotência de evento corporativo é o exemplo que o cenário
// golden 13 usa — se ele fosse tratado como cache e ficasse de fora do snapshot, um evento
// duplicado depois de uma recuperação seria aplicado duas vezes e I11 quebraria em silêncio.
// Cache é só aquilo que se reconstrói sem mudar decisão nenhuma.

#include <cstdint>

#include "base/arena.hpp"
#include "base/dense_index.hpp"
#include "base/fixed.hpp"
#include "base/ids.hpp"
#include "base/metrics.hpp"
#include "base/pair_index.hpp"
#include "core/ledger.hpp"
#include "core/trade_state.hpp"

namespace rv::core {

// ---------------------------------------------------------------- instrumentos
// O cadastro chega junto com a cotação de fechamento (`ClosingPriceSet`): um evento por
// instrumento por dia traz ticker, ISIN, tipo, fator de cotação e preço. É o que permite ao
// replay reconstruir a tabela inteira sem abrir arquivo nenhum — o requisito de I12.
struct InstrumentTable {
  // O id que o INGRESS atribuiu, guardado ao lado do slot interno.
  //
  // São dois espaços de nome diferentes e é fácil confundi-los — um teste os confundiu, e o
  // sintoma foi um `NotFound` obscuro. O log fala `external_id`; a memória fala `slot`. A
  // tradução acontece uma vez, em `intern_instrument`, e esta coluna é o caminho de volta: quem
  // percorre as colunas SoA (o escritor do snapshot, uma ferramenta de depuração, um teste)
  // precisa saber a que instrumento do log cada linha corresponde.
  uint32_t* external_id = nullptr;
  char (*symbol)[12] = nullptr;
  char (*isin)[12] = nullptr;
  uint8_t* type = nullptr;
  uint32_t* price_factor = nullptr;  // divisor de grossAmount (I7)
  uint32_t* lot_size = nullptr;
  Price* closing_price = nullptr;
  Price* previous_close = nullptr;
  DateYmd* closing_date = nullptr;
  uint32_t count = 0;
  uint32_t capacity = 0;

  [[nodiscard]] bool init(Arena& a, uint32_t cap) noexcept;
};

// ---------------------------------------------------------------- negócios
// A tabela é densa e em ordem de chegada. A lista encadeada por conta (`next_of_account`) existe
// para que `BatchNetted` — que fala de uma conta e uma data — não precise varrer todos os
// negócios da partição. Encadear pelo ÍNDICE, e não por ponteiro, é o que permite copiar a
// tabela inteira no snapshot com um `memcpy`.
struct TradeTable {
  uint64_t* trade_id = nullptr;
  uint64_t* broker_note_id = nullptr;
  uint32_t* account = nullptr;     // AccountId interno
  uint32_t* instrument = nullptr;  // slot interno
  Qty* qty = nullptr;
  Price* price = nullptr;
  Money* cash = nullptr;  // financeiro do negócio, com sinal (negativo = débito)
  DateYmd* settlement_date = nullptr;
  uint8_t* side = nullptr;
  uint8_t* state = nullptr;  // TradeState
  uint32_t* next_of_account = nullptr;
  uint32_t count = 0;
  uint32_t capacity = 0;

  static constexpr uint32_t kNil = 0xFFFFFFFFU;
  [[nodiscard]] bool init(Arena& a, uint32_t cap) noexcept;
};

// ---------------------------------------------------------------- fila de exceção
// Divergência de reconciliação e negócio rejeitado. É ESTADO (vai para o snapshot), não log:
// o replay tem de reproduzi-la idêntica, senão I11 falha de um jeito que nenhum teste de
// "aplica duas vezes" pega — ver cenário golden 14.
struct ExceptionRecord {
  uint64_t document;
  int64_t qty_delta_raw;  // depositária − motor
  uint32_t instrument;
  uint32_t date;
  uint16_t err_code;
  uint16_t reserved;
};
static_assert(sizeof(ExceptionRecord) == 32);

// ---------------------------------------------------------------- dimensionamento
// Todos os limites em um lugar. Quem opera o motor dimensiona aqui e o warm-up falha
// imediatamente se a arena não comportar — em vez de a tabela encher em produção.
struct PartitionCapacity {
  uint32_t accounts = 1u << 16;
  uint32_t instruments = 4096;
  uint32_t positions = 1u << 18;
  uint32_t trades = 1u << 20;
  uint32_t corporate_actions_per_gen = 1u << 16;
  uint32_t exceptions = 1u << 12;
  uint32_t outbox_slots = 1u << 14;
  uint32_t outbox_payload_bytes = 1u << 22;

  // Janela de retenção do conjunto de idempotência, em dias de calendário. Sessenta é folgado
  // para qualquer reentrega de arquivo da B3; a consequência de reaplicar depois disso está
  // documentada e testada em tests/domain/golden/13.
  int32_t idempotency_window_days = 60;
};

class Outbox;

struct PartitionState {
  PartitionId id{};
  DateYmd business_date{};
  DateYmd prev_business_date{};
  SettlementWindow window{};
  Lsn applied_lsn{};

  CustodyLedger custody;
  CashLedger cash;
  InstrumentTable instruments;
  TradeTable trades;

  DenseIndex account_index;      // documento (uint64 exato) -> AccountId
  DenseIndex position_index;     // (account << 32 | instrument) -> linha da custódia
  DenseIndex instrument_index;   // id externo do instrumento -> slot interno
  DenseIndex trade_index;        // trade_id -> linha da tabela de negócios
  TwoGenSet applied_actions;     // (action_id, account) -> evento corporativo já aplicado (I6)
  TwoGenSet applied_income;      // (action_id, account) -> provento já creditado (I6)

  uint32_t* account_first_trade = nullptr;  // cabeça da lista encadeada, por conta
  uint64_t* account_document = nullptr;     // AccountId -> documento (para o snapshot e a exceção)

  ExceptionRecord* exceptions = nullptr;
  uint32_t exception_count = 0;     // quantas entraram, desde sempre
  uint32_t exception_dropped = 0;   // quantas foram sobrescritas por a fila ser circular
  uint32_t exception_capacity = 0;

  // `kFlagReconDivergence` só sobe por divergência de RECONCILIAÇÃO de custódia — é o que o
  // cabeçalho do snapshot de exposição publica com esse nome. Falha de entrega e net divergente
  // entram na fila de exceção sem ligá-la: uma flag que sobe por tudo não distingue nada.
  static constexpr uint32_t kFlagReconDivergence = 1U << 0;
  uint32_t flags = 0;

  PartitionCapacity cap{};

  // Quantos bytes da arena o ESTADO ocupa. É a fronteira da imagem de recuperação: o que vem
  // depois na arena (outbox, buffers do WAL) é transitório e NÃO entra no snapshot — o que estava
  // empilhado no outbox e não era durável nunca foi visto por ninguém, então perdê-lo é o
  // resultado correto. Marcar a fronteira aqui é o que impede a imagem de arrastar junto memória
  // que não é estado.
  uint64_t arena_bytes = 0;

  // Constrói tudo a partir da arena e SELA. Falha se não couber — e falhar aqui é o objetivo:
  // é o teste de dimensionamento, rodando no warm-up de todo processo.
  [[nodiscard]] bool init(Arena& a, PartitionId pid, const PartitionCapacity& c) noexcept;

  // ---- internação (determinística: por ordem de primeira aparição no log) ----
  [[nodiscard]] uint32_t intern_account(DocumentId doc) noexcept;
  [[nodiscard]] uint32_t intern_instrument(uint32_t external_id) noexcept;
  [[nodiscard]] uint32_t intern_position(uint32_t account, uint32_t instrument) noexcept;

  // Reconstrói os quatro índices densos a partir das colunas. Usado pela recuperação: o índice é
  // cache de uma relação que já está nos dados, e gravá-lo seria gravar a mesma verdade duas
  // vezes — com o custo do fator de carga.
  [[nodiscard]] bool rebuild_indexes() noexcept;

  // Baixa os negócios que terminaram e COMPACTA a tabela.
  //
  // Sem isto, `TradeTable::count` só cresce: a 10 milhões de eventos por dia, o limite de quantos
  // dias o motor roda não é o disco, é esta tabela. E as listas encadeadas por conta crescem
  // junto, o que faz `BatchNetted` e `TradeSettled` percorrerem a história inteira de um
  // investidor a cada liquidação.
  //
  // Um negócio termina quando está `Liquidado` e sua data de liquidação já saiu da janela — a
  // aresta `Liquidado --> [*]` de docs/dominio.md, que até aqui era inalcançável porque nada
  // emitia o gatilho `Close`. Falha de entrega NÃO termina: ela fica, com o bucket vencido.
  //
  // A compactação é estável (preserva a ordem original), portanto determinística — e é chamada de
  // dentro do `apply` de `DayOpened`, isto é, dirigida por evento e não por relógio (I12).
  // Depois dela, a lista de uma conta contém apenas negócios em voo: no máximo os três dias da
  // janela. É o que transforma o percurso por conta de "história inteira" em "o que está aberto".
  [[nodiscard]] uint32_t close_and_compact_trades() noexcept;

  [[nodiscard]] uint32_t find_position(uint32_t account, uint32_t instrument) const noexcept {
    return position_index.find((static_cast<uint64_t>(account) << 32) | instrument);
  }

  // ---- âncoras de I11: dois uint64 que o replay confere contra `EodMarked` ----
  [[nodiscard]] uint64_t custody_checksum() const noexcept;
  [[nodiscard]] uint64_t cash_checksum() const noexcept;

  // `marca_divergencia` só é verdadeiro para divergência de reconciliação de custódia.
  void push_exception(const ExceptionRecord& r, bool marca_divergencia = false) noexcept;
};

}  // namespace rv::core
