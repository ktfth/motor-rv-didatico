#pragma once
// Dinheiro, quantidade e preço. ADR-0007, CODING_RULES §2.
//
// A ideia inteira em três frases:
//   1. Um valor é um `int64` que conta múltiplos de 10^-Escala. Nada de ponto flutuante.
//   2. Só existem aqui as operações que NÃO perdem informação: soma, subtração, comparação.
//   3. Tudo que perde informação — multiplicação, divisão — mora em `rounding.hpp`, com a
//      política de arredondamento no NOME da função.
//
// Se você conseguiu escrever `preco * quantidade` neste código, o desenho falhou: a expressão não
// compila de propósito, porque ela esconde uma decisão de arredondamento que alguém precisa tomar.

#include <compare>
#include <cstdint>
#include <type_traits>

#include "base/panic.hpp"

namespace rv {

// ---------------------------------------------------------------- unidades
// Tags vazias. Custam zero byte e zero instrução, e transformam trocar quantidade por preço em
// erro de compilação. É o único lugar do projeto onde preferimos um parâmetro de template a um
// comentário: `Qty` e `Price` têm a MESMA escala (1e-8, ADR-0007), então sem a tag eles seriam
// literalmente o mesmo tipo e `notional(qty, price)` aceitaria os argumentos trocados.
namespace unit {
struct Share {};        // quantidade de ativo
struct BrlPerShare {};  // preço unitário
struct Brl {};          // dinheiro
}  // namespace unit

namespace detail {
constexpr int64_t pow10(int n) noexcept {
  int64_t r = 1;
  for (int i = 0; i < n; ++i) r *= 10;
  return r;
}
}  // namespace detail

template <int Scale, class Unit>
class Fixed {
 public:
  using Raw = int64_t;
  static constexpr int kScale = Scale;
  static constexpr Raw kOne = detail::pow10(Scale);  // uma unidade inteira, em raw

  constexpr Fixed() noexcept = default;

  // Construção explícita nos dois sentidos: `from_raw` para quem vem do disco ou do SBE,
  // `from_units` para quem escreve "100 ações" num teste. Não existe conversão implícita de
  // `int64` — um número solto virando dinheiro é a origem clássica do erro de escala.
  static constexpr Fixed from_raw(Raw r) noexcept { return Fixed{r}; }
  static constexpr Fixed from_units(int64_t u) noexcept {
    Raw r = 0;
    if (__builtin_mul_overflow(u, kOne, &r)) panic_overflow("Fixed::from_units", u, kOne);
    return Fixed{r};
  }

  [[nodiscard]] constexpr Raw raw() const noexcept { return raw_; }

  // A parte inteira, truncando em direção a zero. Serve para formatação e para o cálculo de
  // sobras no grupamento — nunca para aritmética de dinheiro.
  [[nodiscard]] constexpr int64_t whole() const noexcept { return raw_ / kOne; }
  [[nodiscard]] constexpr Raw fraction() const noexcept { return raw_ % kOne; }

  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }

  // ---- exatas ----
  friend constexpr Fixed operator+(Fixed a, Fixed b) noexcept {
    Raw r = 0;
    if (__builtin_add_overflow(a.raw_, b.raw_, &r)) panic_overflow("Fixed::+", a.raw_, b.raw_);
    return Fixed{r};
  }
  friend constexpr Fixed operator-(Fixed a, Fixed b) noexcept {
    Raw r = 0;
    if (__builtin_sub_overflow(a.raw_, b.raw_, &r)) panic_overflow("Fixed::-", a.raw_, b.raw_);
    return Fixed{r};
  }
  friend constexpr Fixed operator-(Fixed a) noexcept {
    Raw r = 0;
    if (__builtin_sub_overflow(Raw{0}, a.raw_, &r)) panic_overflow("Fixed::neg", a.raw_, 0);
    return Fixed{r};
  }
  constexpr Fixed& operator+=(Fixed b) noexcept { return *this = *this + b; }
  constexpr Fixed& operator-=(Fixed b) noexcept { return *this = *this - b; }

  friend constexpr auto operator<=>(Fixed, Fixed) noexcept = default;
  friend constexpr bool operator==(Fixed, Fixed) noexcept = default;

  // Versão que devolve o estouro em vez de parar o processo. Usada onde a recusa é decisão de
  // negócio (um evento malformado do ingress) e não corrupção interna: derrubar a partição
  // porque uma corretora mandou lixo transformaria o erro do outro em indisponibilidade nossa.
  [[nodiscard]] static constexpr bool checked_add(Fixed a, Fixed b, Fixed& out) noexcept {
    Raw r = 0;
    if (__builtin_add_overflow(a.raw_, b.raw_, &r)) return false;
    out = Fixed{r};
    return true;
  }
  [[nodiscard]] static constexpr bool checked_sub(Fixed a, Fixed b, Fixed& out) noexcept {
    Raw r = 0;
    if (__builtin_sub_overflow(a.raw_, b.raw_, &r)) return false;
    out = Fixed{r};
    return true;
  }

 private:
  explicit constexpr Fixed(Raw r) noexcept : raw_(r) {}
  Raw raw_{0};
};

// ADR-0007: 1e-8 para quantidade e preço, 1e-4 para BRL.
//
// Por que 1e-8 na QUANTIDADE, se ação se compra inteira: porque grupamento e bonificação geram
// frações (cenários golden 08 e 09), e uma fração precisa ser representada exatamente no momento
// em que nasce — senão o motor teria de arredondá-la ou jogá-la fora, e a reconciliação com a
// depositária acusaria divergência todo dia.
//
// Por que 1e-4 no DINHEIRO: é a precisão que a API Renda Variável expõe (`"169.1357"`). Escolher
// a escala interna igual à precisão publicada elimina uma conversão na borda e, com ela, uma
// classe inteira de erro de arredondamento na saída.
using Qty = Fixed<8, unit::Share>;
using Price = Fixed<8, unit::BrlPerShare>;
using Money = Fixed<4, unit::Brl>;

static_assert(sizeof(Qty) == 8 && std::is_trivially_copyable_v<Qty>);
static_assert(sizeof(Money) == 8 && std::is_trivially_copyable_v<Money>);
static_assert(!std::is_same_v<Qty, Price>, "a tag de unidade é o que impede trocar os dois");
static_assert(Qty::kOne == 100'000'000);
static_assert(Money::kOne == 10'000);

}  // namespace rv
