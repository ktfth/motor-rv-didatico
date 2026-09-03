#include "base/panic.hpp"

#include <cstdio>
#include <cstdlib>

namespace rv {
namespace {
// Escreve em stderr sem formatação de biblioteca: em pânico, o processo pode estar com a arena
// corrompida, e `fmt` alocaria. `fprintf` com formato constante é o que sobra de confiável.
void grita(const char* titulo, const char* onde, const char* extra) noexcept {
  std::fprintf(stderr, "\n=== motor-rv PANIC: %s ===\n  onde : %s\n  %s\n", titulo, onde, extra);
  std::fflush(stderr);
}
}  // namespace

void panic_overflow(const char* onde, int64_t a, int64_t b) noexcept {
  char buf[128];
  std::snprintf(buf, sizeof buf, "valores: a=%lld b=%lld", static_cast<long long>(a),
                static_cast<long long>(b));
  grita("overflow de ponto fixo", onde, buf);
  std::abort();
}

void panic_precondition(const char* onde, const char* condicao) noexcept {
  grita("pré-condição violada", onde, condicao);
  std::abort();
}
}  // namespace rv
