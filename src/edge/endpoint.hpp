#pragma once
// Definições de endpoints e etapas do pipeline da borda (FAPI-BR / Open Finance Brasil).
//
// Conforme ADR-0003 e contratos-internos.md:
// A borda roteia as seis rotas regulatórias de Renda Variável, classificando
// as etapas de validação para métricas de latência (R17) e disponibilidade (R18).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace rv::edge {

// Os seis endpoints da API de Renda Variável Open Finance Brasil (API RV 1.3.0).
enum class Endpoint : uint8_t {
  InvestmentsList = 0,      // GET /investments
  InvestmentDetail = 1,     // GET /investments/{id}
  Balances = 2,             // GET /investments/{id}/balances
  Transactions = 3,         // GET /investments/{id}/transactions
  TransactionsCurrent = 4,  // GET /investments/{id}/transactions-current (D-6..D)
  BrokerNote = 5,           // GET /broker-notes/{brokerNoteId}
  Count = 6
};

inline constexpr size_t kEndpointCount = static_cast<size_t>(Endpoint::Count);

// Limites operacionais mensais por consentimento (R16): 30/4/30/4/30/30
inline constexpr uint16_t kMonthlyLimit[kEndpointCount] = {30, 4, 30, 4, 30, 30};

// Etapas do pipeline em ordem de custo de rejeição (pipeline.hpp).
enum class Stage : uint8_t {
  Tls = 1,            // Verificação de certificado cliente (mTLS), ciphersuite e ALPN
  InteractionId = 2,  // x-fapi-interaction-id obrigatório e válido (UUID v4)
  Token = 3,          // Validação JWS PS256, exp, iat, thumbprint x5t#S256
  Consent = 4,        // Validação de consentimento ativo, titular e permissão
  Quota = 5,          // Limite operacional mensal (423) e rate limit (429)
  Lookup = 6,         // Localização da conta/posição no snapshot de exposição
  Application = 7,    // Execução e geração da resposta
  Count = 8
};

inline constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);

[[nodiscard]] constexpr std::string_view endpoint_name(Endpoint ep) noexcept {
  switch (ep) {
    case Endpoint::InvestmentsList:
      return "investments_list";
    case Endpoint::InvestmentDetail:
      return "investment_detail";
    case Endpoint::Balances:
      return "balances";
    case Endpoint::Transactions:
      return "transactions";
    case Endpoint::TransactionsCurrent:
      return "transactions_current";
    case Endpoint::BrokerNote:
      return "broker_note";
    case Endpoint::Count:
      break;
  }
  return "unknown";
}

[[nodiscard]] constexpr std::string_view endpoint_path(Endpoint ep) noexcept {
  switch (ep) {
    case Endpoint::InvestmentsList:
      return "/investments";
    case Endpoint::InvestmentDetail:
      return "/investments/{id}";
    case Endpoint::Balances:
      return "/investments/{id}/balances";
    case Endpoint::Transactions:
      return "/investments/{id}/transactions";
    case Endpoint::TransactionsCurrent:
      return "/investments/{id}/transactions-current";
    case Endpoint::BrokerNote:
      return "/broker-notes/{brokerNoteId}";
    case Endpoint::Count:
      break;
  }
  return "/unknown";
}

[[nodiscard]] constexpr std::string_view stage_name(Stage st) noexcept {
  switch (st) {
    case Stage::Tls:
      return "tls";
    case Stage::InteractionId:
      return "interaction_id";
    case Stage::Token:
      return "token";
    case Stage::Consent:
      return "consent";
    case Stage::Quota:
      return "quota";
    case Stage::Lookup:
      return "lookup";
    case Stage::Application:
      return "application";
    case Stage::Count:
      break;
  }
  return "unknown";
}

}  // namespace rv::edge
