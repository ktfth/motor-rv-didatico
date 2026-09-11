// test_r17_latency_slo.cpp — Teste de conformidade do SLA de latência P95 (R17).
//
// Requisito R17:
// P95 diário por endpoint dentro do SLA da classe de frequência,
// com medição de handshake TLS SEPARADA do tempo de atendimento da requisição.

#include <gtest/gtest.h>

#include "edge/audit.hpp"
#include "edge/endpoint.hpp"

namespace {

using rv::edge::AuditMetrics;
using rv::edge::Endpoint;
using rv::edge::kEndpointCount;

TEST(AuditR17Test, SeparateHandshakeAndRequestLatency) {
  AuditMetrics audit;

  // Endpoint: Balances (/investments/{id}/balances)
  const auto ep = Endpoint::Balances;

  // Simula 100 conexões onde:
  // - Handshake TLS leva ~30.000.000 ns (30 ms)
  // - Requisição leva ~5.000.000 ns (5 ms)
  for (int i = 0; i < 100; ++i) {
    audit.record_handshake(ep,
                           static_cast<uint64_t>(30'000'000) + static_cast<uint64_t>(i * 10'000));
    audit.record_request(ep, static_cast<uint64_t>(5'000'000) + static_cast<uint64_t>(i * 10'000));
  }

  // Se o handshake fosse somado à requisição, o P95 da requisição seria > 35 ms.
  // Como o contrato R17 exige separação:
  const uint64_t req_p95 = audit.p95_request_ns(ep);
  const uint64_t hs_p95 = audit.p95_handshake_ns(ep);

  EXPECT_GT(req_p95, 0U);
  EXPECT_LT(req_p95, 10'000'000ULL);  // < 10 ms
  EXPECT_GE(req_p95, 5'000'000ULL);   // >= 5 ms

  EXPECT_GE(hs_p95, 30'000'000ULL);  // >= 30 ms
  EXPECT_LT(hs_p95, 40'000'000ULL);

  // Verifica que os histogramas internos refletem 100 amostras cada
  EXPECT_EQ(audit.request_histogram(ep).count(), 100U);
  EXPECT_EQ(audit.handshake_histogram(ep).count(), 100U);
}

TEST(AuditR17Test, EndpointIsolation) {
  AuditMetrics audit;

  // Registra latências para Balances
  audit.record_request(Endpoint::Balances, 1'000'000ULL);
  // Registra latências para Transactions
  audit.record_request(Endpoint::Transactions, 50'000'000ULL);

  EXPECT_EQ(audit.request_histogram(Endpoint::Balances).count(), 1U);
  EXPECT_EQ(audit.request_histogram(Endpoint::Transactions).count(), 1U);
  EXPECT_EQ(audit.request_histogram(Endpoint::InvestmentsList).count(), 0U);

  EXPECT_LT(audit.p95_request_ns(Endpoint::Balances), 2'000'000ULL);
  EXPECT_GE(audit.p95_request_ns(Endpoint::Transactions), 45'000'000ULL);
  EXPECT_EQ(audit.p95_request_ns(Endpoint::InvestmentsList), 0U);
}

TEST(AuditR17Test, ResetClearsHistograms) {
  AuditMetrics audit;
  audit.record_request(Endpoint::BrokerNote, 2'000'000ULL);
  audit.record_handshake(Endpoint::BrokerNote, 15'000'000ULL);

  EXPECT_EQ(audit.request_histogram(Endpoint::BrokerNote).count(), 1U);
  EXPECT_EQ(audit.handshake_histogram(Endpoint::BrokerNote).count(), 1U);

  audit.reset();

  EXPECT_EQ(audit.request_histogram(Endpoint::BrokerNote).count(), 0U);
  EXPECT_EQ(audit.handshake_histogram(Endpoint::BrokerNote).count(), 0U);
  EXPECT_EQ(audit.p95_request_ns(Endpoint::BrokerNote), 0U);
}

}  // namespace
