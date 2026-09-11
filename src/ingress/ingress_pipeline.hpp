#pragma once
// IngressPipeline — Decodificação de framing SBE de rede e roteamento para partições.
//
// Recebe bytes contínuos de stream de rede (TCP Drop Copy B3 / UMDF), lida com fragmentação,
// valida SbeMessageHeader, extrai o DocumentId de cada mensagem e despacha IngressFrame
// para o Inbox (SpscRing) da partição destino com contrapressão em caso de anel saturado.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "base/bytes.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"
#include "core/partition.hpp"
#include "ingress/partitioner.hpp"
#include "ingress/sbe_message_header.hpp"

namespace rv::ingress {

struct IngressStats {
  uint64_t bytes_received{0};
  uint64_t events_received{0};
  uint64_t events_routed{0};
  uint64_t events_broadcast{0};
  uint64_t backpressure_stalls{0};
  uint64_t parse_errors{0};
};

class IngressPipeline {
 public:
  static constexpr size_t kBufferSize = 64 * 1024;  // 64 KiB buffer de recepção
  static constexpr uint32_t kMaxPartitions = 64;

  explicit IngressPipeline(std::span<core::Inbox*> inboxes) noexcept;

  // Alimenta o pipeline com um chunk de bytes recebidos da rede.
  // now_ns: timestamp de auditoria de chegada do evento (D2).
  [[nodiscard]] Status feed(ByteSpan chunk, uint64_t now_ns) noexcept;

  // Encerra um stream. Bytes residuais significam frame TCP truncado.
  [[nodiscard]] Status finish() noexcept;

  // Reseta o estado do buffer e contadores.
  void reset() noexcept;

  [[nodiscard]] const IngressStats& stats() const noexcept { return stats_; }
  [[nodiscard]] size_t pending_bytes() const noexcept { return buffered_; }
  [[nodiscard]] uint32_t partition_count() const noexcept { return num_inboxes_; }

 private:
  [[nodiscard]] Status route_and_dispatch(const SbeMessageHeader& hdr, const std::byte* payload,
                                          uint64_t now_ns) noexcept;

  Partitioner partitioner_;
  std::array<core::Inbox*, kMaxPartitions> inboxes_{};
  uint32_t num_inboxes_{0};

  alignas(64) std::byte buffer_[kBufferSize]{};
  size_t buffered_{0};

  IngressStats stats_{};
};

}  // namespace rv::ingress
