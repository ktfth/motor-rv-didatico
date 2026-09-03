#pragma once
// Fatias de bytes e leitura little-endian. A camada mais fina possível sobre `std::span`.
//
// Por que não usar `std::span<const std::byte>` direto em toda parte: porque `ByteSpan` é um nome
// que aparece em centenas de assinaturas, e porque os dois helpers de leitura alinhada abaixo
// precisam morar em algum lugar. Nenhum comportamento novo — só vocabulário.

#include <cstddef>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "base/status.hpp"

namespace rv {

using ByteSpan = std::span<const std::byte>;
using MutBytes = std::span<std::byte>;

// O formato do WAL e do snapshot é little-endian (docs/wal.md). Na máquina de referência isso é
// a ordem nativa, então estas funções compilam para um `mov`. Elas existem para que, num dia em
// que alguém tente portar para big-endian, haja UM lugar para mexer.
template <class T>
[[nodiscard]] inline T load_le(const std::byte* p) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  static_assert(std::endian::native == std::endian::little,
                "motor-rv assume little-endian; ver docs/wal.md");
  T v{};
  std::memcpy(&v, p, sizeof(T));
  return v;
}

template <class T>
inline void store_le(std::byte* p, T v) noexcept {
  static_assert(std::is_trivially_copyable_v<T>);
  std::memcpy(p, &v, sizeof(T));
}

[[nodiscard]] inline bool is_aligned(const void* p, size_t a) noexcept {
  return (reinterpret_cast<uintptr_t>(p) & (a - 1)) == 0;
}

// Reinterpretação sem cópia, com as duas checagens que a tornam segura: a fatia comporta o tipo e
// o endereço está alinhado. É o caminho do decode SBE e do leitor do snapshot — os dois únicos
// lugares do projeto onde aritmética de ponteiro é o trabalho, não um atalho.
template <class T>
[[nodiscard]] inline Result<const T*> view_bytes(ByteSpan b) noexcept {
  if (b.size() < sizeof(T)) return Status::fail(Err::ShortPayload, static_cast<uint32_t>(b.size()));
  if (!is_aligned(b.data(), alignof(T))) return Status::fail(Err::Misaligned);
  return reinterpret_cast<const T*>(b.data());  // NOLINT: é o trabalho deste arquivo
}

}  // namespace rv
