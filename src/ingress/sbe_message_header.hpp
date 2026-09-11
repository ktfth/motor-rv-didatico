#pragma once
// Cabeçalho canônico de mensagens SBE (Simple Binary Encoding).
//
// Conforme ADR-0006 e schema/events.xml, cada quadro na rede ou em arquivo
// é prefixado pelo MessageHeader padrão de 8 bytes antes do bloco de payload.

#include <cstdint>
#include <type_traits>

namespace rv::ingress {

struct alignas(2) SbeMessageHeader {
  uint16_t block_length{0};  // Tamanho do bloco de dados fixo (payload)
  uint16_t template_id{0};   // TemplateId do evento (codec::Tmpl)
  uint16_t schema_id{0};     // Deve ser 1 (kSchemaId)
  uint16_t version{0};       // Deve ser 1 (kSchemaVersion)
};

static_assert(sizeof(SbeMessageHeader) == 8, "SbeMessageHeader deve ter exatamente 8 bytes");
static_assert(alignof(SbeMessageHeader) == 2, "Alinhamento do SbeMessageHeader é 2 bytes");
static_assert(std::is_standard_layout_v<SbeMessageHeader>);
static_assert(std::is_trivially_copyable_v<SbeMessageHeader>);

}  // namespace rv::ingress
