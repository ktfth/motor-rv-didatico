#pragma once
// Andaime dos testes de domínio: monta uma partição em memória e aplica eventos.
//
// Nada aqui toca disco, relógio ou rede — é a mesma restrição do `apply` (D2), e é o que faz cada
// teste ser reproduzível bit a bit. O `Journal` não aparece: `apply` não conhece o log, só recebe
// um `EventView` com um LSN. Essa separação é o que permite testar o domínio inteiro antes de o
// WAL existir.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "base/arena.hpp"
#include "base/metrics.hpp"
#include "codec/events.hpp"
#include "core/apply.hpp"
#include "core/outbox.hpp"
#include "codec/sbe_runtime.hpp"
#include "core/partition_state.hpp"

namespace rv::testing {

inline constexpr uint64_t kCpfA = 11122233344ULL;
inline constexpr uint64_t kCpfB = 55566677788ULL;
inline constexpr uint32_t kPetr4 = 1;
inline constexpr uint32_t kXpto11 = 12;  // fator de cotação 1000 (cenário 12)

class Engine {
 public:
  Engine() {
    memoria_ = std::make_unique<std::byte[]>(kBytes);
    arena_ = std::make_unique<Arena>(memoria_.get(), kBytes);
    core::PartitionCapacity cap{};
    cap.accounts = 1024;
    cap.instruments = 64;
    cap.positions = 4096;
    cap.trades = 8192;
    cap.corporate_actions_per_gen = 1024;
    cap.exceptions = 256;
    cap.outbox_slots = 256;
    cap.outbox_payload_bytes = 1 << 16;
    EXPECT_TRUE(estado_.init(*arena_, PartitionId{0}, cap));
    EXPECT_TRUE(outbox_.init(*arena_, cap.outbox_slots, cap.outbox_payload_bytes));
    arena_->seal();  // a partir daqui, alocar é erro — é assim que CODING_RULES §1 vira mecanismo
    ctx_ = core::ApplyContext{&outbox_, &metricas_};
  }

  // Aplica uma mensagem POD. O payload aponta direto para o struct: é exatamente o que o WAL
  // entrega — a única cópia do evento já está no buffer de commit.
  template <class T>
  Status aplica(const T& msg) {
    const core::EventView ev{Lsn{++lsn_}, 0, reinterpret_cast<const std::byte*>(&msg),
                             T::kTemplateId, T::kBlockLength, 0};
    return core::apply(estado_, ev, ctx_);
  }

  // Para o único evento com grupo repetido.
  Status aplica_reconciliacao(const codec::CustodyReconciled& cab,
                              const codec::CustodyReconciledDivergence* div, uint16_t n) {
    alignas(8) static std::byte buf[4096];
    std::memcpy(buf, &cab, sizeof cab);
    codec::GroupHeader gh{static_cast<uint16_t>(sizeof(codec::CustodyReconciledDivergence)), n};
    std::memcpy(buf + sizeof cab, &gh, sizeof gh);
    std::memcpy(buf + sizeof cab + sizeof gh, div,
                sizeof(codec::CustodyReconciledDivergence) * n);
    const auto len = static_cast<uint16_t>(sizeof cab + sizeof gh +
                                           sizeof(codec::CustodyReconciledDivergence) * n);
    const core::EventView ev{Lsn{++lsn_}, 0, buf, codec::CustodyReconciled::kTemplateId, len, 0};
    return core::apply(estado_, ev, ctx_);
  }

  core::PartitionState& estado() { return estado_; }
  core::Outbox& outbox() { return outbox_; }
  Metrics& metricas() { return metricas_; }

  // ---- consultas que os testes usam o tempo todo ----
  // Posição inexistente FALHA o teste em vez de devolver kEmpty. A primeira versão devolvia
  // kEmpty e os acessores indexavam a coluna com ele — leitura fora dos limites, que só apareceu
  // porque um teste consultou o preço médio de um papel que a conta nunca negociou. Um andaime de
  // teste que corrompe memória é pior que teste nenhum: ele transforma um erro do TESTE num
  // sintoma que parece do MOTOR.
  uint32_t pos(uint64_t documento, uint32_t instrumento) {
    const uint32_t a = estado_.account_index.find(documento);
    const uint32_t i = estado_.instrument_index.find(instrumento);
    EXPECT_NE(a, DenseIndex::kEmpty) << "conta " << documento << " não existe no estado";
    EXPECT_NE(i, DenseIndex::kEmpty) << "instrumento " << instrumento << " não existe no estado";
    if (a == DenseIndex::kEmpty || i == DenseIndex::kEmpty) return 0;
    const uint32_t p = estado_.find_position(a, i);
    EXPECT_NE(p, DenseIndex::kEmpty) << "posição (" << documento << ", " << instrumento << ") não existe";
    return p == DenseIndex::kEmpty ? 0 : p;
  }
  uint32_t conta(uint64_t documento) {
    const uint32_t a = estado_.account_index.find(documento);
    EXPECT_NE(a, DenseIndex::kEmpty) << "conta " << documento << " não existe no estado";
    return a == DenseIndex::kEmpty ? 0 : a;
  }

  Qty disponivel(uint64_t doc, uint32_t inst) { return estado_.custody.available[pos(doc, inst)]; }
  Price preco_medio(uint64_t doc, uint32_t inst) { return estado_.custody.avg_price[pos(doc, inst)]; }
  Qty sobras(uint64_t doc, uint32_t inst) { return estado_.custody.leftovers[pos(doc, inst)]; }
  Money caixa(uint64_t doc) { return estado_.cash.cash[conta(doc)]; }
  Money proventos(uint64_t doc) { return estado_.cash.income_receivable[conta(doc)]; }
  Qty i1(uint64_t doc, uint32_t inst) { return estado_.custody.custody_today(pos(doc, inst)); }
  Qty i13(uint64_t doc, uint32_t inst) { return estado_.custody.custody_projected(pos(doc, inst)); }
  Money a_liquidar(uint64_t doc, uint32_t data) {
    const uint32_t k = estado_.window.slot_of(DateYmd{data});
    return k == core::SettlementWindow::kNoSlot
               ? Money{}
               : estado_.cash.pending[conta(doc) * core::kSettlementSlots + k];
  }

 private:
  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> memoria_;
  std::unique_ptr<Arena> arena_;
  core::PartitionState estado_;
  core::Outbox outbox_;
  Metrics metricas_;
  core::ApplyContext ctx_{nullptr, nullptr};
  uint64_t lsn_ = 0;
};

// ---------------------------------------------------------------- construtores de evento
inline codec::DayOpened dia(uint32_t d, uint32_t d1, uint32_t d2, uint32_t anterior = 0) {
  codec::DayOpened e{};
  e.business_date = d;
  e.prev_business_date = anterior;
  e.settle_d1 = d1;
  e.settle_d2 = d2;
  e.schema_version = codec::kSchemaVersion;
  return e;
}

inline codec::ClosingPriceSet cadastro(uint32_t inst, const char* ticker, uint32_t fator,
                                       int64_t fechamento, uint32_t data) {
  codec::ClosingPriceSet e{};
  e.instrument = inst;
  e.date = data;
  e.price_factor = fator;
  e.lot_size = 100;
  e.closing_price = fechamento;
  e.previous_close = fechamento;
  std::strncpy(e.symbol, ticker, sizeof e.symbol);
  e.type = static_cast<uint8_t>(codec::InstrumentType::Stock);
  return e;
}

inline codec::TradeExecuted negocio(uint64_t id, uint64_t doc, uint32_t inst, codec::Side lado,
                                    int64_t qty, int64_t preco, int64_t custos, uint32_t data_liq,
                                    uint16_t flags = 0) {
  codec::TradeExecuted e{};
  e.trade_id = id;
  e.broker_note_id = 7000 + id;
  e.account = doc;
  e.qty = qty;
  e.price = preco;
  e.brokerage_fee = custos;
  e.instrument = inst;
  e.settlement_date = data_liq;
  e.flags = flags;
  e.side = static_cast<uint8_t>(lado);
  e.market = static_cast<uint8_t>(codec::Market::RoundLot);
  return e;
}

inline codec::TradeAllocated alocacao(uint64_t id, uint64_t trade, uint64_t de, uint64_t para,
                                      uint32_t inst, codec::Side lado, int64_t qty, int64_t dinheiro,
                                      uint32_t data_liq) {
  codec::TradeAllocated e{};
  e.allocation_id = id;
  e.trade_id = trade;
  e.from_account = de;
  e.to_account = para;
  e.qty = qty;
  e.cash_amount = dinheiro;
  e.instrument = inst;
  e.settlement_date = data_liq;
  e.side = static_cast<uint8_t>(lado);
  return e;
}

inline codec::BatchNetted net(uint64_t id, uint64_t doc, int64_t liquido, uint32_t data,
                              uint32_t n = 0) {
  codec::BatchNetted e{};
  e.batch_id = id;
  e.account = doc;
  e.net_amount = liquido;
  e.settlement_date = data;
  e.trade_count = n;
  return e;
}

inline codec::TradeSettled liquidacao(uint64_t lote, uint64_t doc, uint32_t inst, codec::Side lado,
                                      int64_t qty, int64_t dinheiro, int64_t custo_base,
                                      uint32_t data, codec::SettleOutcome resultado) {
  codec::TradeSettled e{};
  e.batch_id = lote;
  e.account = doc;
  e.qty = qty;
  e.cash_amount = dinheiro;
  e.cost_basis = custo_base;
  e.instrument = inst;
  e.settlement_date = data;
  e.side = static_cast<uint8_t>(lado);
  e.outcome = static_cast<uint8_t>(resultado);
  return e;
}

inline codec::CorporateActionApplied corporativo(uint64_t id, uint64_t doc, uint32_t inst,
                                                 codec::ActionType tipo, uint32_t num, uint32_t den,
                                                 uint32_t ex) {
  codec::CorporateActionApplied e{};
  e.action_id = id;
  e.account = doc;
  e.instrument = inst;
  e.result_instrument = inst;
  e.ex_date = ex;
  e.com_date = ex;
  e.factor_num = num;
  e.factor_den = den;
  e.type = static_cast<uint8_t>(tipo);
  return e;
}

inline codec::DividendPaid provento(uint64_t id, uint64_t doc, uint32_t inst, codec::IncomeKind k,
                                    int64_t base, int64_t taxa, int64_t bruto, int64_t retido,
                                    uint32_t ex, codec::IncomeStage estagio) {
  codec::DividendPaid e{};
  e.action_id = id;
  e.account = doc;
  e.qty_basis = base;
  e.rate_per_share = taxa;
  e.gross_amount = bruto;
  e.withheld_tax = retido;
  e.net_amount = bruto - retido;
  e.instrument = inst;
  e.ex_date = ex;
  e.payment_date = ex;
  e.kind = static_cast<uint8_t>(k);
  e.stage = static_cast<uint8_t>(estagio);
  return e;
}

inline codec::EodMarked eod(uint32_t data, uint64_t soma_custodia, uint64_t soma_caixa,
                            uint32_t flags = 0) {
  codec::EodMarked e{};
  e.custody_checksum = soma_custodia;
  e.cash_checksum = soma_caixa;
  e.date = data;
  e.flags = flags;
  return e;
}

}  // namespace rv::testing
