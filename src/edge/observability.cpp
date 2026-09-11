#include "edge/observability.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "base/status.hpp"

namespace rv::edge {

namespace {

// Formatador minimalista para buffer estático sem alocação dinâmica.
class FastBufferWriter {
 public:
  explicit FastBufferWriter(std::span<char> buf) noexcept
      : start_(buf.data()), ptr_(buf.data()), end_(buf.data() + buf.size()) {}

  bool append(std::string_view s) noexcept {
    if (static_cast<size_t>(end_ - ptr_) < s.size()) return false;
    std::memcpy(ptr_, s.data(), s.size());
    ptr_ += s.size();
    return true;
  }

  bool append_u64(uint64_t v) noexcept {
    auto res = std::to_chars(ptr_, end_, v);
    if (res.ec != std::errc{}) return false;
    ptr_ = res.ptr;
    return true;
  }

  bool append_i64(int64_t v) noexcept {
    auto res = std::to_chars(ptr_, end_, v);
    if (res.ec != std::errc{}) return false;
    ptr_ = res.ptr;
    return true;
  }

  bool append_hex64(uint64_t v) noexcept {
    if (!append("0x")) return false;
    char hex[17];
    int len = std::snprintf(hex, sizeof(hex), "%016lx", v);
    if (len <= 0) return false;
    return append(std::string_view(hex, static_cast<size_t>(len)));
  }

  bool append_double(double v) noexcept {
    char tmp[32];
    int len = std::snprintf(tmp, sizeof(tmp), "%.6f", v);
    if (len <= 0) return false;
    return append(std::string_view(tmp, static_cast<size_t>(len)));
  }

  [[nodiscard]] size_t size() const noexcept { return static_cast<size_t>(ptr_ - start_); }
  [[nodiscard]] char* data() noexcept { return ptr_; }

 private:
  char* start_{nullptr};
  char* ptr_{nullptr};
  char* end_{nullptr};
};

std::string_view clean_reason_label(uint16_t code) noexcept {
  const char* str = rv::to_string(static_cast<rv::Err>(code));
  switch (static_cast<rv::Err>(code)) {
    case Err::Ok:
      return "Ok";
    case Err::OutOfRange:
      return "OutOfRange";
    case Err::ArenaExhausted:
      return "ArenaExhausted";
    case Err::Overflow:
      return "Overflow";
    case Err::NotFound:
      return "NotFound";
    case Err::WouldBlock:
      return "WouldBlock";
    case Err::InvalidArgument:
      return "InvalidArgument";
    case Err::UnknownTemplate:
      return "UnknownTemplate";
    case Err::ShortPayload:
      return "ShortPayload";
    case Err::BadBlockLength:
      return "BadBlockLength";
    case Err::GroupTooLarge:
      return "GroupTooLarge";
    case Err::Misaligned:
      return "Misaligned";
    case Err::InvalidTransition:
      return "InvalidTransition_I5";
    case Err::AlreadyApplied:
      return "AlreadyApplied_I6";
    case Err::NegativeBucket:
      return "NegativeBucket_I3";
    case Err::QtyMismatch:
      return "QtyMismatch";
    case Err::UnknownBatch:
      return "UnknownBatch";
    case Err::AmountMismatch:
      return "AmountMismatch";
    case Err::ShortSaleNotAllowed:
      return "ShortSaleNotAllowed_I3";
    case Err::InstrumentNotDescribed:
      return "InstrumentNotDescribed";
    case Err::OutsideSettlementWindow:
      return "OutsideSettlementWindow";
    case Err::UnknownInstrument:
      return "UnknownInstrument_FATAL";
    case Err::UnknownAccount:
      return "UnknownAccount_FATAL";
    case Err::LedgerOverflow:
      return "LedgerOverflow_FATAL";
    case Err::StateCorrupt:
      return "StateCorrupt_FATAL";
    case Err::WalFull:
      return "WalFull";
    case Err::ShortWrite:
      return "ShortWrite_FATAL";
    case Err::IoError:
      return "IoError_FATAL";
    case Err::BadCrc:
      return "BadCrc_FATAL";
    case Err::BadMagic:
      return "BadMagic_FATAL";
    case Err::LsnGap:
      return "LsnGap_FATAL";
    case Err::EpochMismatch:
      return "EpochMismatch_FATAL";
    case Err::SegmentFull:
      return "SegmentFull_FATAL";
    case Err::MissingInteractionId:
      return "MissingInteractionId_R1";
    case Err::BadSignature:
      return "BadSignature_R7";
    case Err::TokenExpired:
      return "TokenExpired_R2";
    case Err::CertBindingMismatch:
      return "CertBindingMismatch_R3";
    case Err::ConsentNotAuthorised:
      return "ConsentNotAuthorised_R4";
    case Err::ScopeMissing:
      return "ScopeMissing_R5";
    case Err::OperationalLimit:
      return "OperationalLimit_R16";
    case Err::RateLimited:
      return "RateLimited_R16";
    case Err::MalformedRequest:
      return "MalformedRequest";
    case Err::ResourceNotFound:
      return "ResourceNotFound";
    case Err::TlsProfileViolation:
      return "TlsProfileViolation_R6";
  }
  return str ? str : "Unknown";
}

}  // namespace

ObservabilityCollector::ObservabilityCollector() noexcept = default;

void ObservabilityCollector::set_partition_count(uint32_t count) noexcept {
  num_partitions_ = (count <= kMaxPartitions) ? count : kMaxPartitions;
}

void ObservabilityCollector::update_partition(const PartitionMetricsSnapshot& snapshot) noexcept {
  if (snapshot.partition_id < kMaxPartitions) {
    partitions_[snapshot.partition_id] = snapshot;
    if (snapshot.partition_id >= num_partitions_) {
      num_partitions_ = snapshot.partition_id + 1;
    }
  }
}

void ObservabilityCollector::update_ingress(const IngressMetricsSnapshot& snapshot) noexcept {
  ingress_ = snapshot;
}

bool ObservabilityCollector::is_healthy() const noexcept {
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    if (partitions_[i].halted || partitions_[i].apply_fatal > 0) {
      return false;
    }
  }
  return true;
}

const PartitionMetricsSnapshot* ObservabilityCollector::partition(uint16_t id) const noexcept {
  if (id < num_partitions_) {
    return &partitions_[id];
  }
  return nullptr;
}

size_t ObservabilityCollector::render_prometheus(std::span<char> out) const noexcept {
  if (out.empty()) return 0;
  FastBufferWriter w(out);

  // 1. Eventos aplicados por status e partição
  w.append(
      "# HELP motor_rv_events_applied_total Total de eventos do log aplicados pela maquina de "
      "estados\n");
  w.append("# TYPE motor_rv_events_applied_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_events_applied_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\",status=\"accepted\"} ");
    w.append_u64(p.apply_accepted);
    w.append("\n");

    w.append("motor_rv_events_applied_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\",status=\"rejected\"} ");
    w.append_u64(p.apply_rejected);
    w.append("\n");

    w.append("motor_rv_events_applied_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\",status=\"fatal\"} ");
    w.append_u64(p.apply_fatal);
    w.append("\n");
  }

  // 2. Negócios encerrados e compactados
  w.append(
      "# HELP motor_rv_trades_closed_total Negocios fechados e compactados na virada de pregao\n");
  w.append("# TYPE motor_rv_trades_closed_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_trades_closed_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.trades_closed);
    w.append("\n");
  }

  // 3. Rejeições por código de erro
  w.append(
      "# HELP motor_rv_rejections_total Contagem exata de rejeicoes por particao, codigo e "
      "razao\n");
  w.append("# TYPE motor_rv_rejections_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    for (uint16_t code = 0; code < rv::Metrics::kMaxErrCode; ++code) {
      const uint32_t count = p.rejected_by_code[code];
      if (count == 0) continue;
      w.append("motor_rv_rejections_total{partition=\"");
      w.append_u64(p.partition_id);
      w.append("\",code=\"");
      w.append_u64(code);
      w.append("\",reason=\"");
      w.append(clean_reason_label(code));
      w.append("\"} ");
      w.append_u64(count);
      w.append("\n");
    }
  }

  // 4. WAL LSN durável e último LSN
  w.append(
      "# HELP motor_rv_wal_durable_lsn Ultimo LSN confirmado gravado e duravel em disco "
      "(O_DSYNC)\n");
  w.append("# TYPE motor_rv_wal_durable_lsn gauge\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_wal_durable_lsn{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.durable_lsn);
    w.append("\n");
  }

  w.append("# HELP motor_rv_wal_last_lsn Ultimo LSN submetido ao log da particao\n");
  w.append("# TYPE motor_rv_wal_last_lsn gauge\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_wal_last_lsn{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.last_lsn);
    w.append("\n");
  }

  // 5. Checksum de custódia e status de halt
  w.append(
      "# HELP motor_rv_custody_checksum Checksum 64-bit da custodia total da particao (I1..I13)\n");
  w.append("# TYPE motor_rv_custody_checksum gauge\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_custody_checksum{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.custody_checksum);
    w.append("\n");
  }

  w.append(
      "# HELP motor_rv_partition_halted Indicador booleano (1=fail-stop por divergencia contabil, "
      "0=ok)\n");
  w.append("# TYPE motor_rv_partition_halted gauge\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_partition_halted{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.halted ? 1 : 0);
    w.append("\n");
  }

  // 6. Contas e posições ativas
  w.append(
      "# HELP motor_rv_active_accounts Total de contas alocadas no indice denso da particao\n");
  w.append("# TYPE motor_rv_active_accounts gauge\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_active_accounts{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.active_accounts);
    w.append("\n");
  }

  w.append("# HELP motor_rv_active_positions Total de posicoes em custodia abertas na particao\n");
  w.append("# TYPE motor_rv_active_positions gauge\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_active_positions{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.active_positions);
    w.append("\n");
  }

  // 7. WAL métricas de I/O
  w.append("# HELP motor_rv_wal_appends_total Total de appends submetidos ao WAL\n");
  w.append("# TYPE motor_rv_wal_appends_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_wal_appends_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.wal_appends);
    w.append("\n");
  }

  w.append("# HELP motor_rv_wal_groups_total Grupos de commit persistidos no WAL\n");
  w.append("# TYPE motor_rv_wal_groups_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_wal_groups_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.wal_groups);
    w.append("\n");
  }

  w.append("# HELP motor_rv_wal_bytes_total Bytes gravados em disco pelo WAL\n");
  w.append("# TYPE motor_rv_wal_bytes_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_wal_bytes_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.wal_bytes);
    w.append("\n");
  }

  w.append("# HELP motor_rv_wal_full_total Ocorrencias de WAL cheio (contrapressao de anel)\n");
  w.append("# TYPE motor_rv_wal_full_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_wal_full_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.wal_full);
    w.append("\n");
  }

  // 8. Latência de commit WAL (Summary)
  w.append(
      "# HELP motor_rv_wal_commit_latency_ns Latencia de persistencia e commit duravel do WAL\n");
  w.append("# TYPE motor_rv_wal_commit_latency_ns summary\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    const auto pid = p.partition_id;

    w.append("motor_rv_wal_commit_latency_ns{partition=\"");
    w.append_u64(pid);
    w.append("\",quantile=\"0.5\"} ");
    w.append_u64(p.wal_lat_p50_ns);
    w.append("\n");

    w.append("motor_rv_wal_commit_latency_ns{partition=\"");
    w.append_u64(pid);
    w.append("\",quantile=\"0.9\"} ");
    w.append_u64(p.wal_lat_p90_ns);
    w.append("\n");

    w.append("motor_rv_wal_commit_latency_ns{partition=\"");
    w.append_u64(pid);
    w.append("\",quantile=\"0.99\"} ");
    w.append_u64(p.wal_lat_p99_ns);
    w.append("\n");

    w.append("motor_rv_wal_commit_latency_ns{partition=\"");
    w.append_u64(pid);
    w.append("\",quantile=\"0.999\"} ");
    w.append_u64(p.wal_lat_p999_ns);
    w.append("\n");

    w.append("motor_rv_wal_commit_latency_ns{partition=\"");
    w.append_u64(pid);
    w.append("\",quantile=\"1.0\"} ");
    w.append_u64(p.wal_lat_max_ns);
    w.append("\n");

    w.append("motor_rv_wal_commit_latency_ns_sum{partition=\"");
    w.append_u64(pid);
    w.append("\"} ");
    w.append_u64(p.wal_lat_sum_ns);
    w.append("\n");

    w.append("motor_rv_wal_commit_latency_ns_count{partition=\"");
    w.append_u64(pid);
    w.append("\"} ");
    w.append_u64(p.wal_lat_count);
    w.append("\n");
  }

  // 9. Outbox
  w.append("# HELP motor_rv_outbox_staged_total Eventos preparados para saida\n");
  w.append("# TYPE motor_rv_outbox_staged_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_outbox_staged_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.outbox_staged);
    w.append("\n");
  }

  w.append("# HELP motor_rv_outbox_released_total Eventos liberados pelo loop da particao\n");
  w.append("# TYPE motor_rv_outbox_released_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_outbox_released_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.outbox_released);
    w.append("\n");
  }

  w.append("# HELP motor_rv_outbox_full_total Ocorrencias de outbox cheia\n");
  w.append("# TYPE motor_rv_outbox_full_total counter\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("motor_rv_outbox_full_total{partition=\"");
    w.append_u64(p.partition_id);
    w.append("\"} ");
    w.append_u64(p.outbox_full);
    w.append("\n");
  }

  // 10. Pipeline de Ingress
  w.append(
      "# HELP motor_rv_ingress_bytes_total Bytes de rede SBE recebidos pelo pipeline de ingress\n");
  w.append("# TYPE motor_rv_ingress_bytes_total counter\n");
  w.append("motor_rv_ingress_bytes_total ");
  w.append_u64(ingress_.bytes_received);
  w.append("\n");

  w.append(
      "# HELP motor_rv_ingress_events_total Eventos processados no pipeline de ingress por tipo\n");
  w.append("# TYPE motor_rv_ingress_events_total counter\n");
  w.append("motor_rv_ingress_events_total{type=\"received\"} ");
  w.append_u64(ingress_.events_received);
  w.append("\n");
  w.append("motor_rv_ingress_events_total{type=\"routed\"} ");
  w.append_u64(ingress_.events_routed);
  w.append("\n");
  w.append("motor_rv_ingress_events_total{type=\"broadcast\"} ");
  w.append_u64(ingress_.events_broadcast);
  w.append("\n");
  w.append("motor_rv_ingress_events_total{type=\"backpressure_drops\"} ");
  w.append_u64(ingress_.backpressure_drops);
  w.append("\n");
  w.append("motor_rv_ingress_events_total{type=\"parse_errors\"} ");
  w.append_u64(ingress_.parse_errors);
  w.append("\n");

  // 11. Borda Open Finance / SLA (R17, R18)
  w.append(
      "# HELP motor_rv_sla_availability_ratio R18: Disponibilidade medida (sucessos 2XX/422 vs "
      "erros 5XX/408)\n");
  w.append("# TYPE motor_rv_sla_availability_ratio gauge\n");
  for (size_t ep_i = 0; ep_i < kEndpointCount; ++ep_i) {
    const auto ep = static_cast<Endpoint>(ep_i);
    w.append("motor_rv_sla_availability_ratio{endpoint=\"");
    w.append(endpoint_name(ep));
    w.append("\"} ");
    w.append_double(audit_.availability_ratio(ep));
    w.append("\n");
  }

  w.append("# HELP motor_rv_sla_requests_total R18: Total de chamadas por endpoint e classe\n");
  w.append("# TYPE motor_rv_sla_requests_total counter\n");
  for (size_t ep_i = 0; ep_i < kEndpointCount; ++ep_i) {
    const auto ep = static_cast<Endpoint>(ep_i);
    const auto name = endpoint_name(ep);

    w.append("motor_rv_sla_requests_total{endpoint=\"");
    w.append(name);
    w.append("\",class=\"success\"} ");
    w.append_u64(audit_.count_success(ep));
    w.append("\n");

    w.append("motor_rv_sla_requests_total{endpoint=\"");
    w.append(name);
    w.append("\",class=\"error\"} ");
    w.append_u64(audit_.count_error(ep));
    w.append("\n");

    w.append("motor_rv_sla_requests_total{endpoint=\"");
    w.append(name);
    w.append("\",class=\"client_reject\"} ");
    w.append_u64(audit_.count_client_reject(ep));
    w.append("\n");
  }

  w.append(
      "# HELP motor_rv_sla_p95_request_latency_ns R17: Latencia P95 da requisicao por endpoint sem "
      "handshake\n");
  w.append("# TYPE motor_rv_sla_p95_request_latency_ns gauge\n");
  for (size_t ep_i = 0; ep_i < kEndpointCount; ++ep_i) {
    const auto ep = static_cast<Endpoint>(ep_i);
    w.append("motor_rv_sla_p95_request_latency_ns{endpoint=\"");
    w.append(endpoint_name(ep));
    w.append("\"} ");
    w.append_u64(audit_.p95_request_ns(ep));
    w.append("\n");
  }

  w.append(
      "# HELP motor_rv_sla_p95_handshake_latency_ns R17: Latencia P95 de handshake isolada da "
      "requisicao\n");
  w.append("# TYPE motor_rv_sla_p95_handshake_latency_ns gauge\n");
  for (size_t ep_i = 0; ep_i < kEndpointCount; ++ep_i) {
    const auto ep = static_cast<Endpoint>(ep_i);
    w.append("motor_rv_sla_p95_handshake_latency_ns{endpoint=\"");
    w.append(endpoint_name(ep));
    w.append("\"} ");
    w.append_u64(audit_.p95_handshake_ns(ep));
    w.append("\n");
  }

  return w.size();
}

size_t ObservabilityCollector::render_json_status(std::span<char> out) const noexcept {
  if (out.empty()) return 0;
  FastBufferWriter w(out);

  w.append("{\n");
  w.append("  \"status\": \"");
  w.append(is_healthy() ? "HEALTHY" : "DEGRADED");
  w.append("\",\n");

  w.append("  \"ready\": ");
  w.append(is_ready() ? "true" : "false");
  w.append(",\n");

  w.append("  \"partitions_count\": ");
  w.append_u64(num_partitions_);
  w.append(",\n");

  w.append("  \"partitions\": [\n");
  for (uint32_t i = 0; i < num_partitions_; ++i) {
    const auto& p = partitions_[i];
    w.append("    {\n");
    w.append("      \"id\": ");
    w.append_u64(p.partition_id);
    w.append(",\n");
    w.append("      \"halted\": ");
    w.append(p.halted ? "true" : "false");
    w.append(",\n");
    w.append("      \"durable_lsn\": ");
    w.append_u64(p.durable_lsn);
    w.append(",\n");
    w.append("      \"last_lsn\": ");
    w.append_u64(p.last_lsn);
    w.append(",\n");
    w.append("      \"custody_checksum\": \"");
    w.append_hex64(p.custody_checksum);
    w.append("\",\n");
    w.append("      \"active_accounts\": ");
    w.append_u64(p.active_accounts);
    w.append(",\n");
    w.append("      \"active_positions\": ");
    w.append_u64(p.active_positions);
    w.append(",\n");
    w.append("      \"apply_accepted\": ");
    w.append_u64(p.apply_accepted);
    w.append(",\n");
    w.append("      \"apply_rejected\": ");
    w.append_u64(p.apply_rejected);
    w.append(",\n");
    w.append("      \"apply_fatal\": ");
    w.append_u64(p.apply_fatal);
    w.append(",\n");
    w.append("      \"wal_bytes\": ");
    w.append_u64(p.wal_bytes);
    w.append(",\n");
    w.append("      \"wal_commit_p99_ns\": ");
    w.append_u64(p.wal_lat_p99_ns);
    w.append("\n");
    w.append("    }");
    if (i + 1 < num_partitions_) w.append(",");
    w.append("\n");
  }
  w.append("  ],\n");

  w.append("  \"ingress\": {\n");
  w.append("    \"bytes_received\": ");
  w.append_u64(ingress_.bytes_received);
  w.append(",\n");
  w.append("    \"events_received\": ");
  w.append_u64(ingress_.events_received);
  w.append(",\n");
  w.append("    \"events_routed\": ");
  w.append_u64(ingress_.events_routed);
  w.append(",\n");
  w.append("    \"events_broadcast\": ");
  w.append_u64(ingress_.events_broadcast);
  w.append(",\n");
  w.append("    \"backpressure_drops\": ");
  w.append_u64(ingress_.backpressure_drops);
  w.append(",\n");
  w.append("    \"parse_errors\": ");
  w.append_u64(ingress_.parse_errors);
  w.append("\n");
  w.append("  },\n");

  w.append("  \"sla\": {\n");
  w.append("    \"availability_ratio_global\": ");
  w.append_double(audit_.availability_ratio_global());
  w.append("\n");
  w.append("  }\n");
  w.append("}\n");

  return w.size();
}

}  // namespace rv::edge
