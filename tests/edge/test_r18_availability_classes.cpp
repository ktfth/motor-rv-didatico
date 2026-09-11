// test_r18_availability_classes.cpp — Teste da apuração de disponibilidade (R18).
//
// Requisito R18:
// Disponibilidade medida: 2XX e 422 contam como SUCESSO (rejeição de negócio não é
// indisponibilidade). 5XX e 408 contam como ERRO de infraestrutura / indisponibilidade. Rejeições
// de cliente (400, 401, 403, 404, 423, 429) são classificadas por Stage sem penalizar a
// disponibilidade.

#include <gtest/gtest.h>

#include "edge/audit.hpp"
#include "edge/endpoint.hpp"

namespace {

using rv::edge::AuditMetrics;
using rv::edge::Endpoint;
using rv::edge::Stage;

TEST(AuditR18Test, SuccessClassification2xxAnd422) {
  AuditMetrics audit;
  const auto ep = Endpoint::Balances;

  // 200 OK -> Sucesso
  audit.record_response(ep, Stage::Application, 200);
  // 201 Created -> Sucesso
  audit.record_response(ep, Stage::Application, 201);
  // 422 Unprocessable Entity -> Sucesso regulatório R18
  audit.record_response(ep, Stage::Application, 422);

  EXPECT_EQ(audit.count_success(ep), 3U);
  EXPECT_EQ(audit.count_error(ep), 0U);
  EXPECT_EQ(audit.count_client_reject(ep), 0U);
  EXPECT_DOUBLE_EQ(audit.availability_ratio(ep), 1.0);
}

TEST(AuditR18Test, ErrorClassification5xxAnd408) {
  AuditMetrics audit;
  const auto ep = Endpoint::Transactions;

  // 500 Internal Error -> Erro de disponibilidade
  audit.record_response(ep, Stage::Application, 500);
  // 503 Service Unavailable -> Erro de disponibilidade
  audit.record_response(ep, Stage::Lookup, 503);
  // 408 Request Timeout -> Erro de disponibilidade conforme R18
  audit.record_response(ep, Stage::Tls, 408);

  EXPECT_EQ(audit.count_success(ep), 0U);
  EXPECT_EQ(audit.count_error(ep), 3U);
  EXPECT_EQ(audit.count_client_reject(ep), 0U);
  EXPECT_DOUBLE_EQ(audit.availability_ratio(ep), 0.0);

  // Verifica contagem por etapa
  EXPECT_EQ(audit.rejections_by_stage(Stage::Application), 1U);
  EXPECT_EQ(audit.rejections_by_stage(Stage::Lookup), 1U);
  EXPECT_EQ(audit.rejections_by_stage(Stage::Tls), 1U);
}

TEST(AuditR18Test, ClientRejectionsDoNotDegradeAvailability) {
  AuditMetrics audit;
  const auto ep = Endpoint::InvestmentsList;

  // 90 sucessos (200 OK)
  for (int i = 0; i < 90; ++i) {
    audit.record_response(ep, Stage::Application, 200);
  }
  // 10 sucessos de validação de negócio (422)
  for (int i = 0; i < 10; ++i) {
    audit.record_response(ep, Stage::Application, 422);
  }
  // 50 rejeições de cliente por token inválido ou consentimento
  for (int i = 0; i < 30; ++i) {
    audit.record_response(ep, Stage::Token, 401);
  }
  for (int i = 0; i < 20; ++i) {
    audit.record_response(ep, Stage::Consent, 403);
  }

  // Como não houve 5XX nem 408, a disponibilidade deve permanecer 100% (1.0)
  EXPECT_EQ(audit.count_success(ep), 100U);
  EXPECT_EQ(audit.count_error(ep), 0U);
  EXPECT_EQ(audit.count_client_reject(ep), 50U);
  EXPECT_DOUBLE_EQ(audit.availability_ratio(ep), 1.0);

  EXPECT_EQ(audit.rejections_by_stage(Stage::Token), 30U);
  EXPECT_EQ(audit.rejections_by_stage(Stage::Consent), 20U);
}

TEST(AuditR18Test, AvailabilityRatioWithErrors) {
  AuditMetrics audit;
  const auto ep = Endpoint::TransactionsCurrent;

  // 95 sucessos (90x 200 + 5x 422)
  for (int i = 0; i < 90; ++i) {
    audit.record_response(ep, Stage::Application, 200);
  }
  for (int i = 0; i < 5; ++i) {
    audit.record_response(ep, Stage::Application, 422);
  }
  // 5 erros de infraestrutura (500)
  for (int i = 0; i < 5; ++i) {
    audit.record_response(ep, Stage::Application, 500);
  }

  // 95 / (95 + 5) = 0.95 (95% disponibilidade)
  EXPECT_EQ(audit.count_success(ep), 95U);
  EXPECT_EQ(audit.count_error(ep), 5U);
  EXPECT_DOUBLE_EQ(audit.availability_ratio(ep), 0.95);
}

TEST(AuditR18Test, GlobalAvailabilityRatio) {
  AuditMetrics audit;

  // Endpoint 1: 100% (50 / 50)
  for (int i = 0; i < 50; ++i) {
    audit.record_response(Endpoint::Balances, Stage::Application, 200);
  }

  // Endpoint 2: 80% (40 / 50)
  for (int i = 0; i < 40; ++i) {
    audit.record_response(Endpoint::Transactions, Stage::Application, 200);
  }
  for (int i = 0; i < 10; ++i) {
    audit.record_response(Endpoint::Transactions, Stage::Application, 500);
  }

  // Total: 90 sucessos, 10 erros -> 90% (0.90)
  EXPECT_DOUBLE_EQ(audit.availability_ratio_global(), 0.90);
}

}  // namespace
