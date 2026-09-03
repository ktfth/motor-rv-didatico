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
  TwoGenSet applied_actions;     // (action_id, account) -> já aplicado (I6)

  uint32_t* account_first_trade = nullptr;  // cabeça da lista encadeada, por conta
  uint64_t* account_document = nullptr;     // AccountId -> documento (para o snapshot e a exceção)

  ExceptionRecord* exceptions = nullptr;
  uint32_t exception_count = 0;
  uint32_t exception_capacity = 0;

  static constexpr uint32_t kFlagReconDivergence = 1U << 0;
  uint32_t flags = 0;

  PartitionCapacity cap{};

  // Constrói tudo a partir da arena e SELA. Falha se não couber — e falhar aqui é o objetivo:
  // é o teste de dimensionamento, rodando no warm-up de todo processo.
  [[nodiscard]] bool init(Arena& a, PartitionId pid, const PartitionCapacity& c) noexcept;

  // ---- internação (determinística: por ordem de primeira aparição no log) ----
  [[nodiscard]] uint32_t intern_account(DocumentId doc) noexcept;
  [[nodiscard]] uint32_t intern_instrument(uint32_t external_id) noexcept;
  [[nodiscard]] uint32_t intern_position(uint32_t account, uint32_t instrument) noexcept;

  [[nodiscard]] uint32_t find_position(uint32_t account, uint32_t instrument) const noexcept {
    return position_index.find((static_cast<uint64_t>(account) << 32) | instrument);
  }

  // ---- âncoras de I11: dois uint64 que o replay confere contra `EodMarked` ----
  [[nodiscard]] uint64_t custody_checksum() const noexcept;
  [[nodiscard]] uint64_t cash_checksum() const noexcept;

  void push_exception(const ExceptionRecord& r) noexcept;
};

}  // namespace rv::core
