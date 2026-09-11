#pragma once
// audit.hpp — Trilha de auditoria (R19) e métricas regulatórias por endpoint (R17, R18).
//
// Regras:
// R17: Latência diária P95 por endpoint, com tempo de handshake SEPARADO do tempo de requisição.
// R18: Disponibilidade calculada como sucessos (2XX / 422) vs erros (5XX / 408), classificados por
// Stage. R19: Registro de auditoria com snapshot_lsn, base_date e revision para rastreabilidade de
// dados expostos.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "base/metrics.hpp"
#include "edge/endpoint.hpp"

namespace rv::edge {

struct InteractionId {
  char v[36]{};

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return std::string_view(v, sizeof(v));
  }
};

// R19: Trilha de auditoria da requisição atendida.
struct AuditRecord {
  InteractionId interaction_id{};
  std::string_view consent_urn{};
  std::string_view client_id{};
  Endpoint endpoint{Endpoint::InvestmentsList};
  Stage stage{Stage::Application};
  uint16_t http_status{200};
  uint64_t snapshot_lsn{0};
  uint32_t base_date{0};
  uint16_t revision{0};
  uint64_t now_s{0};
};

class AuditMetrics {
 public:
  AuditMetrics() noexcept = default;

  // R17: Registra a latência da requisição HTTP (excluindo handshake).
  void record_request(Endpoint ep, uint64_t latency_ns) noexcept;

  // R17: Registra a latência do handshake TLS (separada da requisição).
  void record_handshake(Endpoint ep, uint64_t latency_ns) noexcept;

  // R18: Registra o resultado da requisição e classifica disponibilidade por status HTTP e etapa.
  void record_response(Endpoint ep, Stage stage, uint16_t http_status) noexcept;

  // R17: Consulta P95 de latência de requisição por endpoint em nanossegundos.
  [[nodiscard]] uint64_t p95_request_ns(Endpoint ep) const noexcept;

  // R17: Consulta P95 de latência de handshake por endpoint em nanossegundos.
  [[nodiscard]] uint64_t p95_handshake_ns(Endpoint ep) const noexcept;

  [[nodiscard]] const Histogram& request_histogram(Endpoint ep) const noexcept {
    return request_lat_[static_cast<size_t>(ep)];
  }

  [[nodiscard]] const Histogram& handshake_histogram(Endpoint ep) const noexcept {
    return handshake_lat_[static_cast<size_t>(ep)];
  }

  // R18: Contadores de disponibilidade
  [[nodiscard]] uint64_t count_success(Endpoint ep) const noexcept {
    return success_counts_[static_cast<size_t>(ep)];
  }

  [[nodiscard]] uint64_t count_error(Endpoint ep) const noexcept {
    return error_counts_[static_cast<size_t>(ep)];
  }

  [[nodiscard]] uint64_t count_client_reject(Endpoint ep) const noexcept {
    return client_reject_counts_[static_cast<size_t>(ep)];
  }

  [[nodiscard]] uint64_t rejections_by_stage(Stage stage) const noexcept {
    const auto idx = static_cast<size_t>(stage);
    return idx < kStageCount ? stage_rejections_[idx] : 0;
  }

  // R18: Razão de disponibilidade por endpoint: sucessos / (sucessos + erros).
  [[nodiscard]] double availability_ratio(Endpoint ep) const noexcept;

  // R18: Razão de disponibilidade global de todos os endpoints.
  [[nodiscard]] double availability_ratio_global() const noexcept;

  // Limpa todos os contadores e histogramas.
  void reset() noexcept;

 private:
  std::array<Histogram, kEndpointCount> request_lat_{};
  std::array<Histogram, kEndpointCount> handshake_lat_{};

  std::array<uint64_t, kEndpointCount> success_counts_{};
  std::array<uint64_t, kEndpointCount> error_counts_{};
  std::array<uint64_t, kEndpointCount> client_reject_counts_{};

  std::array<uint64_t, kStageCount> stage_rejections_{};
};

}  // namespace rv::edge
