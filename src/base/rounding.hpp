#pragma once
// Todas as operações que PERDEM informação. A política de arredondamento está no nome.
//
// Regra do projeto: nenhuma multiplicação ou divisão de valor monetário acontece fora deste
// arquivo. Ler o nome da função é ler a decisão. `notional_half_even` diz o que faz e como
// arredonda; `calc(...)` com um comentário ao lado não diz.
//
// A tabela de políticas, e por que cada uma:
//
//   HALF_EVEN  números que serão SOMADOS — `grossAmount` (I7) e preço médio (I4).
//              Truncar tem viés numa direção; meio-para-cima tem viés na outra. Só half-even tem
//              erro esperado ZERO na soma, e é a soma que o investidor confere contra o extrato.
//   HALF_UP    tarifas (corretagem, emolumentos, taxa de liquidação). Convenção do mercado: o
//              meio centavo vai para cima.
//   TRUNC      retenção na fonte e grupamento. Reter a maior seria cobrar imposto não devido;
//              grupamento não pode criar quantidade do nada.

#include <cstdint>

#include "base/fixed.hpp"
#include "base/int128.hpp"
#include "base/panic.hpp"

namespace rv {

enum class Rounding : uint8_t { TowardZero, TowardNegInf, HalfUp, HalfEven };

// (a × b) / d, com o produto em i128 e a política explícita. Exige d > 0.
//
// Por que i128 não é preciosismo: o cálculo do preço médio é
// `custo(1e-4) × 10^12 / quantidade(1e-8)`. Com um custo de R$ 4.979,39 o numerador já é
// 5 × 10^19 — cinco vezes o teto de int64. Numa posição institucional passa por dez ordens de
// grandeza. É o segundo caso de teste que estoura, não o milésimo.
template <Rounding R>
[[nodiscard]] constexpr int64_t mul_div(int64_t a, int64_t b, int64_t d) noexcept {
  if (d <= 0) panic_precondition("mul_div", "d > 0");

  const i128 n = static_cast<i128>(a) * static_cast<i128>(b);
  i128 q = n / d;              // trunca em direção a zero
  const i128 r = n % d;        // o sinal segue n
  if (r == 0) return static_cast<int64_t>(q);

  // `TowardZero` sai aqui: `q` já está truncado, e o resto do cálculo — sinal e dobro do resto —
  // só serve às políticas que arredondam. Antes isso era um ramo VAZIO no `if constexpr` mais
  // abaixo; o clang-tidy o apontou como corpo repetido, e sair cedo é ao mesmo tempo mais claro e
  // mais barato.
  if constexpr (R == Rounding::TowardZero) {
    return static_cast<int64_t>(q);
  } else {
  const i128 sinal = (n < 0) ? -1 : 1;
  const i128 dobro = (r < 0 ? -r : r) * 2;   // < 2d, nunca estoura

  if constexpr (R == Rounding::TowardNegInf) {
    if (n < 0) q -= 1;
  } else if constexpr (R == Rounding::HalfUp) {
    if (dobro >= d) q += sinal;                  // empate vai para longe do zero
  } else {  // HalfEven
    // Passou do meio, sobe. Empatou exatamente no meio, sobe só se isso levar ao PAR — que é o
    // que dá erro esperado zero na soma. As duas condições, escritas separadas, tinham o mesmo
    // corpo, e o clang-tidy tinha razão: uma expressão só diz melhor a mesma regra.
    if (dobro > d || (dobro == d && q % 2 != 0)) q += sinal;
  }

  const i128 kMax = static_cast<i128>(INT64_MAX);
  const i128 kMin = static_cast<i128>(INT64_MIN);
  if (q > kMax || q < kMin) panic_overflow("mul_div", a, b);
  return static_cast<int64_t>(q);
  }
}

// ---------------------------------------------------------------- fatores de escala
// Produto de dois valores em 1e-8 tem escala 1e-16; para chegar a BRL (1e-4) divide-se por 1e12.
inline constexpr int64_t kQtyPriceToMoney = 1'000'000'000'000LL;  // 10^12
inline constexpr int64_t kMoneyToQtyPrice = 1'000'000'000'000LL;  // o caminho de volta

// ================================================================ operações do domínio

// I7 — `grossAmount = qty × closingPrice / priceFactor`.
//
// A divisão acontece UMA vez, no fim. Dividir `closingPrice` por `priceFactor` antes de
// multiplicar por `qty` perderia casas antes da multiplicação, e o erro sairia multiplicado por
// `qty`. A ordem das operações é parte do invariante, não detalhe de implementação.
[[nodiscard]] constexpr Money notional_half_even(Qty q, Price p, uint32_t price_factor) noexcept {
  if (price_factor == 0) panic_precondition("notional_half_even", "price_factor > 0");
  int64_t d = 0;
  if (__builtin_mul_overflow(kQtyPriceToMoney, static_cast<int64_t>(price_factor), &d)) {
    panic_overflow("notional_half_even", kQtyPriceToMoney, static_cast<int64_t>(price_factor));
  }
  return Money::from_raw(mul_div<Rounding::HalfEven>(q.raw(), p.raw(), d));
}

// Tarifas em partes por milhão. `rate_ppm = 3000` são 0,30 %.
[[nodiscard]] constexpr Money fee_half_up(Money base, uint32_t rate_ppm) noexcept {
  return Money::from_raw(mul_div<Rounding::HalfUp>(base.raw(), static_cast<int64_t>(rate_ppm),
                                                   1'000'000));
}

// Retenção na fonte em pontos-base. `rate_bp = 1500` são 15 % (IRRF sobre JCP).
[[nodiscard]] constexpr Money withhold_trunc(Money gross, uint32_t rate_bp) noexcept {
  return Money::from_raw(mul_div<Rounding::TowardZero>(gross.raw(), static_cast<int64_t>(rate_bp),
                                                       10'000));
}

// O custo total de uma posição, a partir da quantidade e do preço médio guardados.
[[nodiscard]] constexpr Money position_cost_half_even(Qty q, Price p) noexcept {
  return Money::from_raw(mul_div<Rounding::HalfEven>(q.raw(), p.raw(), kQtyPriceToMoney));
}

// I4 — preço médio depois de somar `q_add` ações que custaram `cost_add`.
//
// Repare que o custo antigo é RECONSTRUÍDO de `q_old × p_old` em vez de ser carregado. É uma
// escolha: guardar `custo_total` dispensaria a reconstrução e o arredondamento dela, mas
// obrigaria a mantê-lo coerente em toda operação de quantidade — inclusive nas de evento
// corporativo, onde a quantidade muda e o custo não. Guardamos o preço médio porque é o número
// que a API expõe e o que a apuração de IR consome; o erro da reconstrução é de 1 ulp por
// operação, e o teste de 1.000 compras sucessivas fixa quanto disso é tolerável.
[[nodiscard]] constexpr Price average_price_half_even(Qty q_old, Price p_old, Qty q_add,
                                                      Money cost_add) noexcept {
  const Money custo_antigo = position_cost_half_even(q_old, p_old);
  const Money custo_total = custo_antigo + cost_add;
  const Qty q_nova = q_old + q_add;

  // Posição líquida não positiva não tem preço médio NESTE modelo, e devolver zero é a resposta,
  // não uma falha.
  //
  // A primeira versão chamava `panic_precondition` aqui — e o cenário que a dispara é
  // perfeitamente válido: uma venda a descoberto autorizada (I3 permite `disponivel` negativo)
  // liquida antes da recompra, a posição possuída fica negativa, e a compra seguinte a leva de
  // −63 para −53. Abortar o processo por um evento de mercado legítimo é indisponibilidade
  // autoinfligida — foi o simulador completo, emitindo liquidação de verdade, que expôs isso na
  // primeira execução.
  //
  // O custo de uma posição descoberta é assunto da apuração de ganho (ADR-0011, módulo separado),
  // que consome o log e tem regra própria. O motor guarda a quantidade; o preço médio volta a
  // existir quando a posição volta a ser positiva.
  if (q_nova.raw() <= 0) return Price{};

  return Price::from_raw(
      mul_div<Rounding::HalfEven>(custo_total.raw(), kMoneyToQtyPrice, q_nova.raw()));
}

// Desdobramento e grupamento usam o MESMO par de fatores: `new_qty = qty × num / den`.
//   desdobramento 1:10  → num=10, den=1   (quantidade ×10, preço ÷10)
//   grupamento    10:1  → num=1,  den=10  (quantidade ÷10, preço ×10)
// Um par só evita a pergunta "o fator é o de multiplicar ou o de dividir?" em cada chamada.

// Quantidade depois do evento, truncada para ação INTEIRA; a fração vai para `sobras`.
//
// `sobras` fica na unidade CORRENTE do instrumento (cenário golden 08). O motivo é I1: ele soma
// `disponivel + a_liquidar_venda + bloqueado + sobras` e compara com a depositária. Uma soma só
// faz sentido com todas as parcelas na mesma unidade.
[[nodiscard]] constexpr Qty scale_qty_trunc(Qty q, uint32_t num, uint32_t den,
                                            Qty& leftover) noexcept {
  if (num == 0 || den == 0) panic_precondition("scale_qty_trunc", "num > 0 && den > 0");
  const int64_t total = mul_div<Rounding::TowardZero>(q.raw(), static_cast<int64_t>(num),
                                                      static_cast<int64_t>(den));
  const int64_t inteiras = (total / Qty::kOne) * Qty::kOne;
  leftover = Qty::from_raw(total - inteiras);
  return Qty::from_raw(inteiras);
}

// Escala uma quantidade MANTENDO a fração. Para buckets em voo (`a_liquidar_compra`,
// `a_liquidar_venda`, vencidos), que não vão a leilão.
//
// Por que eles não podem usar `scale_qty_trunc`: a fração que ela devolve vai para `sobras`, e
// `sobras` entra em I1 — a quantidade guardada HOJE na depositária. Uma compra ainda em voo não
// está lá. Mandar a fração dela para `sobras` produz divergência de reconciliação todos os dias,
// que é exatamente o alarme falso que `tests/domain/golden/08` diz que não pode existir.
//
// E há um segundo motivo: `scale_qty_trunc` trunca em direção a zero, então sobre bucket NEGATIVO
// — o caso que I3 permite, venda a descoberto autorizada — ela devolveria uma fração negativa, e
// `sobras` negativo I3 não permite em hipótese alguma. Aqui não há fração devolvida, logo não há
// como envenenar o bucket.
[[nodiscard]] constexpr Qty scale_qty_keep_fraction(Qty q, uint32_t num, uint32_t den) noexcept {
  if (num == 0 || den == 0) panic_precondition("scale_qty_keep_fraction", "num > 0 && den > 0");
  return Qty::from_raw(mul_div<Rounding::TowardZero>(q.raw(), static_cast<int64_t>(num),
                                                     static_cast<int64_t>(den)));
}

// Preço médio depois do evento: o inverso exato do fator da quantidade, para que o valor da
// posição se conserve. `qty × num/den` e `price × den/num` — o produto dos dois é o mesmo.
[[nodiscard]] constexpr Price scale_price_half_even(Price p, uint32_t num, uint32_t den) noexcept {
  if (num == 0 || den == 0) panic_precondition("scale_price_half_even", "num > 0 && den > 0");
  return Price::from_raw(mul_div<Rounding::HalfEven>(p.raw(), static_cast<int64_t>(den),
                                                     static_cast<int64_t>(num)));
}

}  // namespace rv
