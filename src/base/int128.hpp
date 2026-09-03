#pragma once
// O inteiro de 128 bits, isolado — e o porquê deste arquivo existir.
//
// CODING_RULES §2 exige `__int128` nos intermediários de dinheiro. CODING_RULES §9 exige
// `-Wall -Wextra -Wpedantic -Werror`. As duas regras se contradizem: `__int128` é extensão de
// compilador, e `-Wpedantic` recusa extensão. Descobrimos isso na primeira compilação do
// `mul_div` — o tipo de conflito que só aparece quando o build é levado a sério.
//
// Três saídas eram possíveis:
//   1. Desligar `-Wpedantic` no projeto. Barato e errado: perderíamos o aviso em todo o resto do
//      código por causa de dois tipos.
//   2. Usar uma biblioteca de inteiro largo. Custo de desempenho no caminho mais quente do motor,
//      para resolver um problema de aviso.
//   3. Isolar a extensão em UM arquivo, silenciar o aviso SÓ nele, e usar o alias em toda parte.
//
// A terceira. O resto do projeto compila com `-Wpedantic` de verdade, e a única concessão à
// extensão está aqui, com o motivo escrito. Se um dia o padrão adotar inteiros largos
// (`std::wide_int` ou equivalente), muda-se este arquivo e nada mais.

#include <cstdint>

#if defined(__SIZEOF_INT128__)

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

namespace rv {
using i128 = __int128;
using u128 = unsigned __int128;
}  // namespace rv

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#else
#error "motor-rv exige inteiro de 128 bits (CODING_RULES §2): compile em x86-64 com GCC ou Clang."
#endif

namespace rv {
inline constexpr i128 kI128Max = (static_cast<i128>(INT64_MAX) << 64) | static_cast<i128>(UINT64_MAX);
static_assert(sizeof(i128) == 16);
static_assert(static_cast<i128>(INT64_MAX) * 2 > static_cast<i128>(INT64_MAX),
              "i128 precisa mesmo ter o dobro do alcance; senão nada disto vale");
}  // namespace rv
