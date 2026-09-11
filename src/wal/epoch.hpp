#pragma once
// Geração e injeção de epoch para segmentos do WAL (docs/wal.md).
//
// O epoch é um uint32_t aleatório por segmento gravado no SegmentHdr e em cada WalHdr.
// Impede que registros residuais de um segmento reciclado sejam confundidos com dados válidos.
// O valor nunca é 0 (região pré-zerada).

#include <cstdint>

namespace rv::wal {

inline constexpr uint32_t kDefaultEpoch = 0xC0FFEE01u;

[[nodiscard]] uint32_t random_epoch() noexcept;

struct EpochSource {
  uint32_t (*fn)(void*) noexcept = nullptr;
  void* ctx = nullptr;

  [[nodiscard]] uint32_t next() const noexcept {
    const uint32_t v = (fn != nullptr) ? fn(ctx) : random_epoch();
    return v != 0 ? v : 1u;
  }
};

}  // namespace rv::wal
