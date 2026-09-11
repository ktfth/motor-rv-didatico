#include "ingress/ingress_pipeline.hpp"

#include <cstring>

#include "codec/template_ids.hpp"

namespace rv::ingress {

IngressPipeline::IngressPipeline(std::span<core::Inbox*> inboxes) noexcept
    : partitioner_(static_cast<uint32_t>(inboxes.size())) {
  num_inboxes_ = static_cast<uint32_t>(inboxes.size());
  for (uint32_t i = 0; i < num_inboxes_ && i < kMaxPartitions; ++i) {
    inboxes_[i] = inboxes[i];
  }
}

void IngressPipeline::reset() noexcept {
  buffered_ = 0;
  stats_ = IngressStats{};
}

Status IngressPipeline::feed(ByteSpan chunk, uint64_t now_ns) noexcept {
  stats_.bytes_received += chunk.size();

  if (buffered_ + chunk.size() > kBufferSize) {
    stats_.parse_errors++;
    return Status::fail(Err::OutOfRange);
  }

  std::memcpy(buffer_ + buffered_, chunk.data(), chunk.size());
  buffered_ += chunk.size();

  Status last_status = kOk;

  while (buffered_ >= sizeof(SbeMessageHeader)) {
    SbeMessageHeader hdr{};
    std::memcpy(&hdr, buffer_, sizeof(SbeMessageHeader));

    // Validação de schema do SBE
    if (hdr.schema_id != codec::kSchemaId || hdr.version != codec::kSchemaVersion) {
      stats_.parse_errors++;
      buffered_ = 0;  // Descarta fluxo desordenado/inválido
      return Status::fail(Err::UnknownTemplate, hdr.template_id);
    }

    if (hdr.block_length > core::kMaxIngressPayload) {
      stats_.parse_errors++;
      buffered_ = 0;
      return Status::fail(Err::ShortPayload, hdr.block_length);
    }

    const size_t total_msg_bytes = sizeof(SbeMessageHeader) + hdr.block_length;
    if (buffered_ < total_msg_bytes) {
      // Mensagem fragmentada na camada TCP; aguardar mais bytes
      break;
    }

    const std::byte* payload = buffer_ + sizeof(SbeMessageHeader);
    Status st = route_and_dispatch(hdr, payload, now_ns);
    if (st.is_error()) {
      last_status = st;
    }

    stats_.events_received++;

    // Desliza os bytes consumidos
    const size_t rem = buffered_ - total_msg_bytes;
    if (rem > 0) {
      std::memmove(buffer_, buffer_ + total_msg_bytes, rem);
    }
    buffered_ = rem;
  }

  return last_status;
}

Status IngressPipeline::route_and_dispatch(const SbeMessageHeader& hdr, const std::byte* payload,
                                           uint64_t now_ns) noexcept {
  if (!codec::is_known_template(hdr.template_id)) {
    stats_.parse_errors++;
    return Status::fail(Err::UnknownTemplate, hdr.template_id);
  }

  const auto tmpl = static_cast<codec::Tmpl>(hdr.template_id);

  // 1. Mensagens de Broadcast (enviadas para todas as partições)
  // DayOpened (1), ClosingPriceSet (8), CustodyReconciled (9), EodMarked (10)
  if (tmpl == codec::Tmpl::DayOpened || tmpl == codec::Tmpl::ClosingPriceSet ||
      tmpl == codec::Tmpl::CustodyReconciled || tmpl == codec::Tmpl::EodMarked) {
    bool all_ok = true;
    for (uint32_t i = 0; i < num_inboxes_; ++i) {
      if (inboxes_[i] == nullptr) continue;

      core::IngressFrame* slot = inboxes_[i]->claim();
      if (slot == nullptr) {
        stats_.backpressure_drops++;
        all_ok = false;
        continue;
      }

      slot->arrival_ts_ns = now_ns;
      slot->tmpl = hdr.template_id;
      slot->len = hdr.block_length;
      slot->reserved = 0;
      std::memcpy(slot->payload, payload, hdr.block_length);
      inboxes_[i]->publish();
    }
    if (all_ok) {
      stats_.events_broadcast++;
      return kOk;
    }
    return Status::fail(Err::WouldBlock);
  }

  // 2. Mensagens particionadas por DocumentId
  uint64_t doc_raw = 0;
  switch (tmpl) {
    case codec::Tmpl::TradeExecuted:
      // account em offset 16
      doc_raw = load_le<uint64_t>(payload + 16);
      break;
    case codec::Tmpl::TradeAllocated:
      // to_account em offset 24
      doc_raw = load_le<uint64_t>(payload + 24);
      break;
    case codec::Tmpl::BatchNetted:
      // account em offset 8
      doc_raw = load_le<uint64_t>(payload + 8);
      break;
    case codec::Tmpl::TradeSettled:
      // account em offset 8
      doc_raw = load_le<uint64_t>(payload + 8);
      break;
    case codec::Tmpl::CorporateActionApplied:
      // account em offset 8
      doc_raw = load_le<uint64_t>(payload + 8);
      break;
    case codec::Tmpl::DividendPaid:
      // account em offset 8
      doc_raw = load_le<uint64_t>(payload + 8);
      break;
    default:
      doc_raw = 0;
      break;
  }

  const PartitionId p = partitioner_.of(DocumentId{doc_raw});
  if (p.v >= num_inboxes_ || inboxes_[p.v] == nullptr) {
    stats_.parse_errors++;
    return Status::fail(Err::OutOfRange, p.v);
  }

  core::IngressFrame* slot = inboxes_[p.v]->claim();
  if (slot == nullptr) {
    stats_.backpressure_drops++;
    return Status::fail(Err::WouldBlock, p.v);
  }

  slot->arrival_ts_ns = now_ns;
  slot->tmpl = hdr.template_id;
  slot->len = hdr.block_length;
  slot->reserved = 0;
  std::memcpy(slot->payload, payload, hdr.block_length);
  inboxes_[p.v]->publish();

  stats_.events_routed++;
  return kOk;
}

}  // namespace rv::ingress
