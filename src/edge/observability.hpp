#pragma once
// observability.hpp — Agregador de observabilidade, exportador Prometheus e healthcheck.
//
// Coleta métricas de partições do motor, WAL, pipeline de ingress e borda Open Finance (R17, R18).
// Fornece renderização zero-allocation de:
//   - /metrics: formato de texto OpenMetrics / Prometheus 0.0.4
//   - /healthz: liveness probe (200 OK se partições ativas sem halt; 503 Service Unavailable se
//   degradado)
//   - /ready: readiness probe (200 OK quando pronto para receber eventos; 503 se indisponível)
//   - /status: JSON consolidado do estado do nó e de cada partição

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "base/metrics.hpp"
#include "edge/audit.hpp"
#include "edge/endpoint.hpp"

namespace rv::edge {

struct PartitionMetricsSnapshot {
  uint16_t partition_id{0};
  uint64_t last_lsn{0};
  uint64_t durable_lsn{0};
  uint64_t custody_checksum{0};
  uint32_t active_accounts{0};
  uint32_t active_positions{0};
  bool halted{false};

  // Contadores de apply
  uint64_t apply_accepted{0};
  uint64_t apply_rejected{0};
  uint64_t apply_fatal{0};
  uint64_t trades_closed{0};
  uint32_t rejected_by_code[rv::Metrics::kMaxErrCode]{};

  // WAL
  uint64_t wal_appends{0};
  uint64_t wal_groups{0};
  uint64_t wal_bytes{0};
  uint64_t wal_full{0};

  // Latência de commit WAL (quantis em nanossegundos)
  uint64_t wal_lat_p50_ns{0};
  uint64_t wal_lat_p90_ns{0};
  uint64_t wal_lat_p99_ns{0};
  uint64_t wal_lat_p999_ns{0};
  uint64_t wal_lat_max_ns{0};
  uint64_t wal_lat_sum_ns{0};
  uint64_t wal_lat_count{0};

  // Outbox
  uint64_t outbox_staged{0};
  uint64_t outbox_released{0};
  uint64_t outbox_full{0};
};

struct IngressMetricsSnapshot {
  uint64_t bytes_received{0};
  uint64_t events_received{0};
  uint64_t events_routed{0};
  uint64_t events_broadcast{0};
  uint64_t backpressure_drops{0};
  uint64_t parse_errors{0};
};

class ObservabilityCollector {
 public:
  static constexpr size_t kMaxPartitions = 64;

  ObservabilityCollector() noexcept;

  void set_partition_count(uint32_t count) noexcept;
  [[nodiscard]] uint32_t partition_count() const noexcept { return num_partitions_; }

  void update_partition(const PartitionMetricsSnapshot& snapshot) noexcept;
  void update_ingress(const IngressMetricsSnapshot& snapshot) noexcept;

  AuditMetrics& audit() noexcept { return audit_; }
  [[nodiscard]] const AuditMetrics& audit() const noexcept { return audit_; }

  void set_ready(bool ready) noexcept { is_ready_.store(ready, std::memory_order_release); }

  [[nodiscard]] bool is_ready() const noexcept { return is_ready_.load(std::memory_order_acquire); }

  // Verifica saúde das partições (I1..I13). Se qualquer partição der halt ou erro fatal contábil,
  // reporta não saudável (degraded) para liveness probe.
  [[nodiscard]] bool is_healthy() const noexcept;

  [[nodiscard]] const PartitionMetricsSnapshot* partition(uint16_t id) const noexcept;
  [[nodiscard]] const IngressMetricsSnapshot& ingress() const noexcept { return ingress_; }

  // Gera texto de métricas no padrão Prometheus 0.0.4. Retorna bytes escritos.
  size_t render_prometheus(std::span<char> out) const noexcept;

  // Gera JSON estruturado com status geral e de cada partição. Retorna bytes escritos.
  size_t render_json_status(std::span<char> out) const noexcept;

 private:
  std::array<PartitionMetricsSnapshot, kMaxPartitions> partitions_{};
  uint32_t num_partitions_{0};
  IngressMetricsSnapshot ingress_{};
  AuditMetrics audit_{};
  std::atomic<bool> is_ready_{false};
};

}  // namespace rv::edge
