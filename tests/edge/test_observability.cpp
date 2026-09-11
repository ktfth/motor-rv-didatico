// test_observability.cpp — Teste da borda de observabilidade, Prometheus e servidor HTTP.

#include <array>
#include <cstring>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "edge/metrics_server.hpp"
#include "edge/observability.hpp"

namespace {

using rv::edge::IngressMetricsSnapshot;
using rv::edge::MetricsServer;
using rv::edge::ObservabilityCollector;
using rv::edge::PartitionMetricsSnapshot;

std::string http_get(uint16_t port, const std::string& path) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return {};
  }

  std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);

  std::string resp;
  char buf[1024];
  while (true) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    resp.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);
  return resp;
}

TEST(ObservabilityCollectorTest, RendersPrometheusMetrics) {
  ObservabilityCollector collector;
  collector.set_partition_count(2);

  PartitionMetricsSnapshot p0{};
  p0.partition_id = 0;
  p0.apply_accepted = 1200;
  p0.apply_rejected = 5;
  p0.apply_fatal = 0;
  p0.durable_lsn = 1205;
  p0.last_lsn = 1205;
  p0.custody_checksum = 0xabcdef0123456789ULL;
  p0.rejected_by_code[201] = 5;  // AlreadyApplied
  p0.wal_appends = 1205;
  p0.wal_lat_p99_ns = 50'000;
  collector.update_partition(p0);

  PartitionMetricsSnapshot p1{};
  p1.partition_id = 1;
  p1.apply_accepted = 800;
  p1.apply_rejected = 0;
  p1.durable_lsn = 800;
  p1.last_lsn = 800;
  p1.custody_checksum = 0x1122334455667788ULL;
  collector.update_partition(p1);

  IngressMetricsSnapshot ing{};
  ing.bytes_received = 500'000;
  ing.events_received = 2005;
  ing.events_routed = 2005;
  collector.update_ingress(ing);

  std::array<char, 32 * 1024> buf{};
  size_t len = collector.render_prometheus(buf);
  EXPECT_GT(len, 0U);

  std::string_view prom(buf.data(), len);

  // Valida métricas esperadas no output do Prometheus
  EXPECT_NE(prom.find("# HELP motor_rv_events_applied_total"), std::string_view::npos);
  EXPECT_NE(prom.find("# TYPE motor_rv_events_applied_total counter"), std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_events_applied_total{partition=\"0\",status=\"accepted\"} 1200"),
            std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_events_applied_total{partition=\"1\",status=\"accepted\"} 800"),
            std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_wal_durable_lsn{partition=\"0\"} 1205"), std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_wal_durable_lsn{partition=\"1\"} 800"), std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_custody_checksum{partition=\"0\"} 12379813738877118345"),
            std::string_view::npos);
  EXPECT_NE(
      prom.find(
          "motor_rv_rejections_total{partition=\"0\",code=\"201\",reason=\"AlreadyApplied_I6\"} 5"),
      std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_ingress_bytes_total 500000"), std::string_view::npos);
  EXPECT_NE(prom.find("motor_rv_ingress_events_total{type=\"routed\"} 2005"),
            std::string_view::npos);
}

TEST(ObservabilityCollectorTest, RendersJsonStatus) {
  ObservabilityCollector collector;
  collector.set_partition_count(1);
  collector.set_ready(true);

  PartitionMetricsSnapshot p0{};
  p0.partition_id = 0;
  p0.durable_lsn = 500;
  p0.last_lsn = 500;
  p0.custody_checksum = 0xfeedfacecafebeefULL;
  p0.apply_accepted = 499;
  p0.apply_rejected = 1;
  collector.update_partition(p0);

  std::array<char, 8 * 1024> buf{};
  size_t len = collector.render_json_status(buf);
  EXPECT_GT(len, 0U);

  std::string_view json(buf.data(), len);

  EXPECT_NE(json.find("\"status\": \"HEALTHY\""), std::string_view::npos);
  EXPECT_NE(json.find("\"ready\": true"), std::string_view::npos);
  EXPECT_NE(json.find("\"partitions_count\": 1"), std::string_view::npos);
  EXPECT_NE(json.find("\"durable_lsn\": 500"), std::string_view::npos);
  EXPECT_NE(json.find("\"apply_accepted\": 499"), std::string_view::npos);
}

TEST(ObservabilityCollectorTest, HealthStatusReportsHaltAndFatal) {
  ObservabilityCollector collector;
  collector.set_partition_count(2);

  PartitionMetricsSnapshot p0{};
  p0.partition_id = 0;
  p0.halted = false;
  p0.apply_fatal = 0;
  collector.update_partition(p0);

  PartitionMetricsSnapshot p1{};
  p1.partition_id = 1;
  p1.halted = false;
  p1.apply_fatal = 0;
  collector.update_partition(p1);

  EXPECT_TRUE(collector.is_healthy());

  // Se partição 1 entrar em halt por divergência contábil (fail-stop I1..I13):
  p1.halted = true;
  collector.update_partition(p1);
  EXPECT_FALSE(collector.is_healthy());

  // Se recuperar halt mas tiver apply_fatal:
  p1.halted = false;
  p1.apply_fatal = 1;
  collector.update_partition(p1);
  EXPECT_FALSE(collector.is_healthy());
}

TEST(MetricsServerTest, ServeHttpEndpoints) {
  ObservabilityCollector collector;
  collector.set_partition_count(1);
  collector.set_ready(true);

  PartitionMetricsSnapshot p0{};
  p0.partition_id = 0;
  p0.apply_accepted = 50;
  p0.durable_lsn = 50;
  collector.update_partition(p0);

  MetricsServer server;
  ASSERT_TRUE(server.start(0, collector));  // Porta efêmera do SO
  const uint16_t port = server.bound_port();
  ASSERT_GT(port, 0U);

  // 1. GET /metrics
  std::string resp_metrics = http_get(port, "/metrics");
  EXPECT_NE(resp_metrics.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(resp_metrics.find("Content-Type: text/plain; version=0.0.4"), std::string::npos);
  EXPECT_NE(
      resp_metrics.find("motor_rv_events_applied_total{partition=\"0\",status=\"accepted\"} 50"),
      std::string::npos);

  // 2. GET /healthz (quando saudável)
  std::string resp_health = http_get(port, "/healthz");
  EXPECT_NE(resp_health.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(resp_health.find("ok\n"), std::string::npos);

  // 3. GET /ready
  std::string resp_ready = http_get(port, "/ready");
  EXPECT_NE(resp_ready.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(resp_ready.find("ready\n"), std::string::npos);

  // 4. GET /status
  std::string resp_status = http_get(port, "/status");
  EXPECT_NE(resp_status.find("HTTP/1.1 200 OK"), std::string::npos);
  EXPECT_NE(resp_status.find("Content-Type: application/json"), std::string::npos);
  EXPECT_NE(resp_status.find("\"status\": \"HEALTHY\""), std::string::npos);

  // 5. GET /healthz quando degradado
  p0.halted = true;
  collector.update_partition(p0);
  std::string resp_degraded = http_get(port, "/healthz");
  EXPECT_NE(resp_degraded.find("HTTP/1.1 503 Service Unavailable"), std::string::npos);
  EXPECT_NE(resp_degraded.find("degraded\n"), std::string::npos);

  // 6. Rota inexistente -> 404
  std::string resp_404 = http_get(port, "/not-found");
  EXPECT_NE(resp_404.find("HTTP/1.1 404 Not Found"), std::string::npos);

  server.stop();
  EXPECT_FALSE(server.is_running());
}

}  // namespace
