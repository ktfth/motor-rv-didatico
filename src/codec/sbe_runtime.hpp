#pragma once
// A parte do SBE escrita à mão. Tudo o mais em `codec/` é gerado (ADR-0017).
//
// São três coisas: o cabeçalho de mensagem, o cabeçalho de grupo repetido, e a função que
// transforma uma fatia de bytes num ponteiro tipado com as duas checagens que tornam isso seguro.

#include <cstdint>

#include "base/bytes.hpp"
#include "base/status.hpp"

namespace rv::codec {

// Precede toda mensagem no fio. Dentro do WAL ele é redundante — `WalHdr` já carrega `tmpl` e
// `len` — e por isso o WAL NÃO o grava: o payload no log é o bloco puro. O cabeçalho existe para
// o caminho de ingresso, onde a mensagem chega sozinha e precisa se identificar.
struct MessageHeader {
  uint16_t block_length;
  uint16_t template_id;
  uint16_t schema_id;
  uint16_t version;
};
static_assert(sizeof(MessageHeader) == 8);

struct GroupHeader {
  uint16_t block_length;
  uint16_t num_in_group;
};
static_assert(sizeof(GroupHeader) == 4);

// Leitura sem cópia. As duas checagens não são paranoia: o payload vem de um arquivo que pode
// estar truncado (cauda de escrita rasgada) e de um buffer cujo alinhamento depende do offset do
// registro anterior. Sem elas, um log corrompido vira leitura fora dos limites.
template <class T>
[[nodiscard]] inline Result<const T*> view_as(ByteSpan b) noexcept {
  return view_bytes<T>(b);
}

// O grupo repetido, para o único evento que tem um (`CustodyReconciled`). Devolve a fatia de
// elementos já validada contra o teto declarado no schema.
template <class Elem>
[[nodiscard]] inline Result<std::span<const Elem>> view_group(ByteSpan apos_bloco,
                                                             uint16_t max_ocorrencias) noexcept {
  const auto gh = view_bytes<GroupHeader>(apos_bloco);
  if (!gh) return gh.status();
  if ((*gh)->block_length != sizeof(Elem)) return Status::fail(Err::BadBlockLength);
  if ((*gh)->num_in_group > max_ocorrencias) return Status::fail(Err::GroupTooLarge);
  const size_t n = (*gh)->num_in_group;
  const ByteSpan corpo = apos_bloco.subspan(sizeof(GroupHeader));
  if (corpo.size() < n * sizeof(Elem)) return Status::fail(Err::ShortPayload);
  return std::span<const Elem>{reinterpret_cast<const Elem*>(corpo.data()), n};  // NOLINT
}

}  // namespace rv::codec
