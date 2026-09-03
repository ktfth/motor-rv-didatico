#include "base/crc32c.hpp"

#include <cstring>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace rv {
namespace {

// Castagnoli refletido. A tabela é construída em compilação: nada de inicialização em runtime,
// nada de "primeira chamada é lenta".
constexpr uint32_t kPoly = 0x82F63B78U;

constexpr auto build_table() noexcept {
  struct Tabela {
    uint32_t v[256];
  } t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1U) ? (kPoly ^ (c >> 1)) : (c >> 1);
    t.v[i] = c;
  }
  return t;
}
constexpr auto kTabela = build_table();

bool detecta_hardware() noexcept {
#if defined(__x86_64__)
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2");
#else
  return false;
#endif
}

// Resolvido uma vez, na inicialização estática, antes de qualquer partição existir.
const bool g_hw = detecta_hardware();

}  // namespace

uint32_t crc32c_table(uint32_t crc, const void* data, size_t len) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;
  for (size_t i = 0; i < len; ++i) c = kTabela.v[(c ^ p[i]) & 0xFFU] ^ (c >> 8);
  return ~c;
}

#if defined(__x86_64__)
#if defined(__clang__)
__attribute__((target("sse4.2")))
#else
__attribute__((target("sse4.2")))
#endif
uint32_t crc32c_hw(uint32_t crc, const void* data, size_t len) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  uint64_t c = ~crc;

  // Alinha em 8 bytes byte a byte; depois consome de oito em oito. O laço de 8 bytes é a razão
  // de existir do caminho de hardware: uma instrução por palavra, com latência de três ciclos.
  while (len && (reinterpret_cast<uintptr_t>(p) & 7U)) {
    c = _mm_crc32_u8(static_cast<uint32_t>(c), *p++);
    --len;
  }
  while (len >= 8) {
    uint64_t w = 0;
    std::memcpy(&w, p, 8);
    c = _mm_crc32_u64(c, w);
    p += 8;
    len -= 8;
  }
  while (len--) c = _mm_crc32_u8(static_cast<uint32_t>(c), *p++);
  return ~static_cast<uint32_t>(c);
}
#else
uint32_t crc32c_hw(uint32_t crc, const void* data, size_t len) noexcept {
  return crc32c_table(crc, data, len);
}
#endif

uint32_t crc32c(uint32_t crc, const void* data, size_t len) noexcept {
  return g_hw ? crc32c_hw(crc, data, len) : crc32c_table(crc, data, len);
}

bool crc32c_uses_hardware() noexcept { return g_hw; }

}  // namespace rv
