#include "core/state_image.hpp"

#include <cstring>

#include "base/crc32c.hpp"

namespace rv::core {
namespace {
[[nodiscard]] uint32_t crc_do_cabecalho(const StateImageHeader& h) noexcept {
  StateImageHeader copia = h;
  copia.header_crc32c = 0;
  return crc32c(0, &copia, sizeof copia);
}
}  // namespace

Status save_state_image(const PartitionState& s, const std::byte* arena_base, MutBytes out,
                        uint64_t* written) noexcept {
  const uint64_t arena_used = s.arena_bytes;
  const uint64_t total = state_image_bytes(arena_used);
  if (out.size() < total) return Status::fail(Err::OutOfRange, static_cast<uint32_t>(total));

  StateImageHeader h{};
  h.magic = kStateImageMagic;
  h.version = kStateImageVersion;
  h.partition_id = s.id.v;
  h.arena_bytes = arena_used;
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
  h.account_index_size = s.account_index.size();
  h.position_index_size = s.position_index.size();
  h.instrument_index_size = s.instrument_index.size();
  h.trade_index_size = s.trade_index.size();
  const auto c = s.applied_actions.counters();
  h.actions_size0 = c.size0;
  h.actions_size1 = c.size1;
  h.actions_current = c.atual;
  h.actions_rotation_date = c.rotacao.v;
  const auto ci = s.applied_income.counters();
  h.income_size0 = ci.size0;
  h.income_size1 = ci.size1;
  h.income_current = ci.atual;
  h.income_rotation_date = ci.rotacao.v;
  h.cap = s.cap;
  h.header_crc32c = crc_do_cabecalho(h);

  std::memcpy(out.data(), &h, sizeof h);
  std::memcpy(out.data() + sizeof h, arena_base, arena_used);
  if (written != nullptr) *written = total;
  return kOk;
}

Status load_state_image(PartitionState& s, Arena& arena, ByteSpan image) noexcept {
  if (image.size() < sizeof(StateImageHeader)) return Status::fail(Err::ShortPayload);
  StateImageHeader h{};
  std::memcpy(&h, image.data(), sizeof h);
  if (h.magic != kStateImageMagic) return Status::fail(Err::BadMagic);
  if (h.version != kStateImageVersion) return Status::fail(Err::BadBlockLength, h.version);
  if (crc_do_cabecalho(h) != h.header_crc32c) return Status::fail(Err::BadCrc);
  if (image.size() < state_image_bytes(h.arena_bytes)) return Status::fail(Err::ShortPayload);

  // `init` percorre a MESMA sequência de alocações que produziu a imagem, e por isso atribui os
  // mesmos deslocamentos. Depois disso os ponteiros já apontam para o lugar certo, e só falta
  // trazer o conteúdo de volta.
  if (!s.init(arena, PartitionId{h.partition_id}, h.cap)) return Status::fail(Err::ArenaExhausted);
  if (s.arena_bytes != h.arena_bytes) {
    // Se a ordem de alocação mudou (campo novo, capacidade diferente), o deslocamento de cada
    // coluna mudou junto, e copiar os bytes produziria estado silenciosamente errado. Recusar é
    // a única resposta segura — e é o que o bump de versão de formato existe para evitar.
    return Status::fail(Err::StateCorrupt, static_cast<uint32_t>(arena.used()));
  }
  std::memcpy(arena.base(), image.data() + sizeof h, h.arena_bytes);

  s.id = PartitionId{h.partition_id};
  s.applied_lsn = Lsn{h.applied_lsn};
  s.business_date = DateYmd{h.business_date};
  s.prev_business_date = DateYmd{h.prev_business_date};
  for (uint32_t k = 0; k < kSettlementSlots; ++k) s.window.dates[k] = DateYmd{h.window_dates[k]};
  s.flags = h.flags;
  s.custody.count = h.custody_count;
  s.cash.count = h.cash_count;
  s.instruments.count = h.instrument_count;
  s.trades.count = h.trade_count;
  s.exception_count = h.exception_count;
  s.exception_dropped = h.exception_dropped;
  s.account_index.restore_size(h.account_index_size);
  s.position_index.restore_size(h.position_index_size);
  s.instrument_index.restore_size(h.instrument_index_size);
  s.trade_index.restore_size(h.trade_index_size);
  s.applied_actions.restore(
      TwoGenSet::Counters{h.actions_size0, h.actions_size1, h.actions_current,
                          DateYmd{h.actions_rotation_date}});
  s.applied_income.restore(
      TwoGenSet::Counters{h.income_size0, h.income_size1, h.income_current,
                          DateYmd{h.income_rotation_date}});
  return kOk;
}

}  // namespace rv::core
