#include "edge/audit.hpp"

namespace rv::edge {

void AuditMetrics::record_request(Endpoint ep, uint64_t latency_ns) noexcept {
  const auto idx = static_cast<size_t>(ep);
  if (idx < kEndpointCount) {
    request_lat_[idx].record(latency_ns);
  }
}

void AuditMetrics::record_handshake(Endpoint ep, uint64_t latency_ns) noexcept {
  const auto idx = static_cast<size_t>(ep);
  if (idx < kEndpointCount) {
    handshake_lat_[idx].record(latency_ns);
  }
}

void AuditMetrics::record_response(Endpoint ep, Stage stage, uint16_t http_status) noexcept {
  const auto ep_idx = static_cast<size_t>(ep);
  if (ep_idx >= kEndpointCount) return;

  const auto stage_idx = static_cast<size_t>(stage);

  // Regra R18 de Disponibilidade:
  // - 2XX e 422 (Unprocessable Entity) contam como SUCESSO.
  // - 5XX e 408 (Request Timeout) contam como ERRO de infraestrutura / indisponibilidade.
  // - Demais 4XX (400, 401, 403, 404, 423, 429) contam como rejeição de negócio / cliente.
  const bool is_2xx = (http_status >= 200 && http_status < 300);
  const bool is_422 = (http_status == 422);
  const bool is_5xx = (http_status >= 500 && http_status < 600);
  const bool is_408 = (http_status == 408);

  if (is_2xx || is_422) {
    ++success_counts_[ep_idx];
  } else if (is_5xx || is_408) {
    ++error_counts_[ep_idx];
    if (stage_idx < kStageCount) {
      ++stage_rejections_[stage_idx];
    }
  } else {
    ++client_reject_counts_[ep_idx];
    if (stage_idx < kStageCount) {
      ++stage_rejections_[stage_idx];
    }
  }
}

uint64_t AuditMetrics::p95_request_ns(Endpoint ep) const noexcept {
  const auto idx = static_cast<size_t>(ep);
  if (idx >= kEndpointCount) return 0;
  return request_lat_[idx].quantile(0.95);
}

uint64_t AuditMetrics::p95_handshake_ns(Endpoint ep) const noexcept {
  const auto idx = static_cast<size_t>(ep);
  if (idx >= kEndpointCount) return 0;
  return handshake_lat_[idx].quantile(0.95);
}

double AuditMetrics::availability_ratio(Endpoint ep) const noexcept {
  const auto idx = static_cast<size_t>(ep);
  if (idx >= kEndpointCount) return 1.0;

  const uint64_t ok = success_counts_[idx];
  const uint64_t err = error_counts_[idx];
  const uint64_t total = ok + err;
  if (total == 0) return 1.0;
  return static_cast<double>(ok) / static_cast<double>(total);
}

double AuditMetrics::availability_ratio_global() const noexcept {
  uint64_t total_ok = 0;
  uint64_t total_err = 0;
  for (size_t i = 0; i < kEndpointCount; ++i) {
    total_ok += success_counts_[i];
    total_err += error_counts_[i];
  }
  const uint64_t total = total_ok + total_err;
  if (total == 0) return 1.0;
  return static_cast<double>(total_ok) / static_cast<double>(total);
}

void AuditMetrics::reset() noexcept {
  for (auto& h : request_lat_) h.reset();
  for (auto& h : handshake_lat_) h.reset();
  success_counts_.fill(0);
  error_counts_.fill(0);
  client_reject_counts_.fill(0);
  stage_rejections_.fill(0);
}

}  // namespace rv::edge
