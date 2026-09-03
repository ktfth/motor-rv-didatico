#include "core/state_image.hpp"

#include <cstring>
#include <vector>

#include "base/crc32c.hpp"

namespace rv::core {
namespace {

// A descrição de uma seção: onde estão os bytes na memória viva, o tamanho do elemento e quantos.
// Save e load percorrem a MESMA tabela, na mesma ordem — é o que impede os dois de divergirem.
struct Descritor {
  void* dados;
  uint32_t elem_size;
  uint32_t count;
};

[[nodiscard]] uint64_t alinha8(uint64_t v) noexcept { return (v + 7U) & ~uint64_t{7}; }

// Monta a tabela de descritores a partir do estado. `const_cast` porque a mesma função serve ao
// save (que só lê) e ao load (que escreve) — duplicá-la seria criar a segunda verdade que este
// arquivo inteiro existe para evitar.
void descreve(const PartitionState& s, Descritor* d) noexcept {
  const uint32_t np = s.custody.count;
  const uint32_t na = s.cash.count;
  const uint32_t ni = s.instruments.count;
  const uint32_t nt = s.trades.count;
  const uint32_t nps = np * kSettlementSlots;
  const uint32_t nas = na * kSettlementSlots;
  auto& c = const_cast<CustodyLedger&>(s.custody);
  auto& x = const_cast<CashLedger&>(s.cash);
  auto& i = const_cast<InstrumentTable&>(s.instruments);
  auto& t = const_cast<TradeTable&>(s.trades);
  auto at = [&](StateSection sec) -> Descritor& { return d[static_cast<std::size_t>(sec)]; };

  at(StateSection::CustodyAvailable)   = {c.available, 8, np};
  at(StateSection::CustodyPendingBuy)  = {c.pending_buy, 8, nps};
  at(StateSection::CustodyPendingSell) = {c.pending_sell, 8, nps};
  at(StateSection::CustodyOverdueBuy)  = {c.overdue_buy, 8, np};
  at(StateSection::CustodyOverdueSell) = {c.overdue_sell, 8, np};
  at(StateSection::CustodyBlocked)     = {c.blocked, 8, np};
  at(StateSection::CustodyLeftovers)   = {c.leftovers, 8, np};
  at(StateSection::CustodyAvgPrice)    = {c.avg_price, 8, np};
  at(StateSection::CustodyAccount)     = {c.account, 4, np};
  at(StateSection::CustodyInstrument)  = {c.instrument, 4, np};
  at(StateSection::CustodyFlags)       = {c.flags, 1, np};

  at(StateSection::CashCash)           = {x.cash, 8, na};
  at(StateSection::CashPending)        = {x.pending, 8, nas};
  at(StateSection::CashOverdue)        = {x.overdue, 8, na};
  at(StateSection::CashIncome)         = {x.income_receivable, 8, na};
  at(StateSection::AccountDocument)    = {const_cast<uint64_t*>(s.account_document), 8, na};
  at(StateSection::AccountFirstTrade)  = {const_cast<uint32_t*>(s.account_first_trade), 4, na};

  at(StateSection::InstExternalId)     = {i.external_id, 4, ni};
  at(StateSection::InstSymbol)         = {i.symbol, 12, ni};
  at(StateSection::InstIsin)           = {i.isin, 12, ni};
  at(StateSection::InstType)           = {i.type, 1, ni};
  at(StateSection::InstPriceFactor)    = {i.price_factor, 4, ni};
  at(StateSection::InstLotSize)        = {i.lot_size, 4, ni};
  at(StateSection::InstClosingPrice)   = {i.closing_price, 8, ni};
  at(StateSection::InstPreviousClose)  = {i.previous_close, 8, ni};
  at(StateSection::InstClosingDate)    = {i.closing_date, 4, ni};

  at(StateSection::TradeId)             = {t.trade_id, 8, nt};
  at(StateSection::TradeBrokerNote)     = {t.broker_note_id, 8, nt};
  at(StateSection::TradeAccount)        = {t.account, 4, nt};
  at(StateSection::TradeInstrument)     = {t.instrument, 4, nt};
  at(StateSection::TradeQty)            = {t.qty, 8, nt};
  at(StateSection::TradePrice)          = {t.price, 8, nt};
  at(StateSection::TradeCash)           = {t.cash, 8, nt};
  at(StateSection::TradeSettlementDate) = {t.settlement_date, 4, nt};
  at(StateSection::TradeSide)           = {t.side, 1, nt};
  at(StateSection::TradeState)          = {t.state, 1, nt};
  at(StateSection::TradeNextOfAccount)  = {t.next_of_account, 4, nt};

  const uint32_t ne = s.exception_count < s.exception_capacity ? s.exception_count
                                                               : s.exception_capacity;
  at(StateSection::Exceptions) = {const_cast<ExceptionRecord*>(s.exceptions),
                                  sizeof(ExceptionRecord), ne};
  // Os dois conjuntos de idempotência são preenchidos à parte: eles não são colunas.
  at(StateSection::AppliedActions) = {nullptr, sizeof(AppliedKeyRecord), s.applied_actions.size()};
  at(StateSection::AppliedIncome)  = {nullptr, sizeof(AppliedKeyRecord), s.applied_income.size()};
}

[[nodiscard]] uint32_t crc_do_cabecalho(const StateImageHeader& h) noexcept {
  StateImageHeader copia = h;
  copia.header_crc32c = 0;
  return crc32c(0, &copia, sizeof copia);
}

void extrai(const TwoGenSet& s, std::vector<AppliedKeyRecord>& out) {
  s.for_each([&out](uint64_t hi, uint32_t lo, uint32_t g) {
    out.push_back(AppliedKeyRecord{hi, lo, g});
  });
}

}  // namespace

uint64_t state_image_bytes(const PartitionState& s) noexcept {
  Descritor d[static_cast<std::size_t>(StateSection::Count)]{};
  descreve(s, d);
  uint64_t total = alinha8(sizeof(StateImageHeader));
  for (const auto& sec : d) total = alinha8(total + uint64_t{sec.elem_size} * sec.count);
  return total;
}

Status save_state_image(const PartitionState& s, MutBytes out, uint64_t* written) noexcept {
  Descritor d[static_cast<std::size_t>(StateSection::Count)]{};
  descreve(s, d);

  std::vector<AppliedKeyRecord> acoes, proventos;
  extrai(s.applied_actions, acoes);
  extrai(s.applied_income, proventos);
  d[static_cast<std::size_t>(StateSection::AppliedActions)].dados = acoes.data();
  d[static_cast<std::size_t>(StateSection::AppliedIncome)].dados = proventos.data();

  StateImageHeader h{};
  h.magic = kStateImageMagic;
  h.version = kStateImageVersion;
  h.partition_id = s.id.v;
  h.applied_lsn = s.applied_lsn.v;
  h.business_date = s.business_date.v;
  h.prev_business_date = s.prev_business_date.v;
  for (uint32_t k = 0; k < kSettlementSlots; ++k) h.window_dates[k] = s.window.dates[k].v;
  h.flags = s.flags;
  h.custody_count = s.custody.count;
  h.cash_count = s.cash.count;
  h.instrument_count = s.instruments.count;
  h.trade_count = s.trades.count;
  h.exception_count = s.exception_count;
  h.exception_dropped = s.exception_dropped;
  const auto ca = s.applied_actions.counters();
  h.actions_current = ca.atual;
  h.actions_rotation_date = ca.rotacao.v;
  const auto ci = s.applied_income.counters();
  h.income_current = ci.atual;
  h.income_rotation_date = ci.rotacao.v;
  h.custody_checksum = s.custody_checksum();
  h.cash_checksum = s.cash_checksum();
  h.cap = s.cap;

  uint64_t off = alinha8(sizeof(StateImageHeader));
  for (std::size_t k = 0; k < static_cast<std::size_t>(StateSection::Count); ++k) {
    const uint64_t bytes = uint64_t{d[k].elem_size} * d[k].count;
    h.sections[k] = StateSectionRef{off, d[k].elem_size, d[k].count, 0, 0};
    off = alinha8(off + bytes);
  }
  h.image_bytes = off;
  if (out.size() < off) return Status::fail(Err::OutOfRange, static_cast<uint32_t>(off));

  std::memset(out.data(), 0, off);
  for (std::size_t k = 0; k < static_cast<std::size_t>(StateSection::Count); ++k) {
    const uint64_t bytes = uint64_t{d[k].elem_size} * d[k].count;
    if (bytes != 0 && d[k].dados != nullptr) {
      std::memcpy(out.data() + h.sections[k].offset, d[k].dados, bytes);
      h.sections[k].crc32c = crc32c(0, d[k].dados, bytes);
    }
  }
  h.header_crc32c = crc_do_cabecalho(h);
  std::memcpy(out.data(), &h, sizeof h);
  if (written != nullptr) *written = off;
  return kOk;
}

Status load_state_image(PartitionState& s, Arena& arena, ByteSpan image) noexcept {
  if (image.size() < sizeof(StateImageHeader)) return Status::fail(Err::ShortPayload);
  StateImageHeader h{};
  std::memcpy(&h, image.data(), sizeof h);
  if (h.magic != kStateImageMagic) return Status::fail(Err::BadMagic);
  if (h.version != kStateImageVersion) return Status::fail(Err::BadBlockLength, h.version);
  if (crc_do_cabecalho(h) != h.header_crc32c) return Status::fail(Err::BadCrc);
  if (image.size() < h.image_bytes) return Status::fail(Err::ShortPayload);

  if (!s.init(arena, PartitionId{h.partition_id}, h.cap)) return Status::fail(Err::ArenaExhausted);

  // Os contadores primeiro: eles são o que diz quantos elementos cada seção tem.
  s.custody.count = h.custody_count;
  s.cash.count = h.cash_count;
  s.instruments.count = h.instrument_count;
  s.trades.count = h.trade_count;
  s.exception_count = h.exception_count;
  s.exception_dropped = h.exception_dropped;
  if (s.custody.count > s.custody.capacity || s.cash.count > s.cash.capacity ||
      s.instruments.count > s.instruments.capacity || s.trades.count > s.trades.capacity) {
    return Status::fail(Err::StateCorrupt);
  }

  Descritor d[static_cast<std::size_t>(StateSection::Count)]{};
  descreve(s, d);

  for (std::size_t k = 0; k < static_cast<std::size_t>(StateSection::Count); ++k) {
    const StateSectionRef& ref = h.sections[k];
    const auto sec = static_cast<StateSection>(k);
    if (sec == StateSection::AppliedActions || sec == StateSection::AppliedIncome) continue;
    if (ref.elem_size != d[k].elem_size || ref.count != d[k].count) {
      return Status::fail(Err::StateCorrupt, static_cast<uint32_t>(k));
    }
    const uint64_t bytes = uint64_t{ref.elem_size} * ref.count;
    if (bytes == 0) continue;
    if (ref.offset + bytes > image.size()) return Status::fail(Err::ShortPayload);
    const std::byte* origem = image.data() + ref.offset;
    if (crc32c(0, origem, bytes) != ref.crc32c) {
      return Status::fail(Err::BadCrc, static_cast<uint32_t>(k));
    }
    std::memcpy(d[k].dados, origem, bytes);
  }

  // Os conjuntos de idempotência: pares gravados, reinseridos na geração de origem.
  s.applied_actions.set_state(h.actions_current, DateYmd{h.actions_rotation_date});
  s.applied_income.set_state(h.income_current, DateYmd{h.income_rotation_date});
  for (const auto sec : {StateSection::AppliedActions, StateSection::AppliedIncome}) {
    const StateSectionRef& ref = h.sections[static_cast<std::size_t>(sec)];
    const uint64_t bytes = uint64_t{ref.elem_size} * ref.count;
    if (bytes == 0) continue;
    if (ref.elem_size != sizeof(AppliedKeyRecord)) return Status::fail(Err::StateCorrupt);
    if (ref.offset + bytes > image.size()) return Status::fail(Err::ShortPayload);
    const std::byte* origem = image.data() + ref.offset;
    if (crc32c(0, origem, bytes) != ref.crc32c) return Status::fail(Err::BadCrc);
    TwoGenSet& destino = sec == StateSection::AppliedActions ? s.applied_actions : s.applied_income;
    for (uint32_t j = 0; j < ref.count; ++j) {
      AppliedKeyRecord rec{};
      std::memcpy(&rec, origem + j * sizeof(AppliedKeyRecord), sizeof rec);
      if (!destino.insert_into(rec.generation, rec.action_id, rec.account)) {
        return Status::fail(Err::ArenaExhausted);
      }
    }
  }

  s.id = PartitionId{h.partition_id};
  s.applied_lsn = Lsn{h.applied_lsn};
  s.business_date = DateYmd{h.business_date};
  s.prev_business_date = DateYmd{h.prev_business_date};
  for (uint32_t k = 0; k < kSettlementSlots; ++k) s.window.dates[k] = DateYmd{h.window_dates[k]};
  s.flags = h.flags;

  // Os índices densos NÃO são gravados: são reconstruídos das colunas. Uma tabela de hash é cache
  // de uma relação que já está nos dados.
  if (!s.rebuild_indexes()) return Status::fail(Err::ArenaExhausted);

  // E a conferência final: as âncoras de `EodMarked` gravadas têm de bater com o estado que
  // acabou de ser reconstruído. CRC prova que os BYTES chegaram; isto prova que eles significam
  // a mesma coisa.
  if (s.custody_checksum() != h.custody_checksum || s.cash_checksum() != h.cash_checksum) {
    return Status::fail(Err::StateCorrupt);
  }
  return kOk;
}

}  // namespace rv::core
