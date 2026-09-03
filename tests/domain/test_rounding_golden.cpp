// Os números dos cenários de tests/domain/golden/ passados pelas funções de rounding.hpp.
//
// Este é o teste mais importante do repositório e o mais chato de escrever: cada valor esperado
// foi calculado À MÃO no arquivo de cenário correspondente, ANTES de existir código. Um teste
// golden cujo valor esperado saiu do próprio motor só prova que o motor concorda consigo mesmo.
// Por isso cada EXPECT abaixo cita o arquivo de onde o número veio.

#include <gtest/gtest.h>

#include "base/fixed.hpp"
#include "base/int128.hpp"
#include "base/rounding.hpp"

using namespace rv;

namespace {
// Açúcar para os testes lerem como o cenário: reais com quatro casas, ações com oito.
constexpr Money brl(int64_t centesimos_de_milesimo) { return Money::from_raw(centesimos_de_milesimo); }
constexpr Qty acoes(int64_t n) { return Qty::from_units(n); }
constexpr Price preco(int64_t raw) { return Price::from_raw(raw); }
}  // namespace

// ---------------------------------------------------------------- mul_div: a base de tudo
TEST(MulDiv, PoliticasNoEmpateExato) {
  // 0,5 → as quatro políticas discordam. É o caso que define cada uma.
  EXPECT_EQ((mul_div<Rounding::TowardZero>(1, 5, 10)), 0);
  EXPECT_EQ((mul_div<Rounding::HalfUp>(1, 5, 10)), 1);
  EXPECT_EQ((mul_div<Rounding::HalfEven>(1, 5, 10)), 0);   // 0 é par: fica
  EXPECT_EQ((mul_div<Rounding::HalfEven>(1, 15, 10)), 2);  // 1,5 → 1 é ímpar: sobe para 2
  EXPECT_EQ((mul_div<Rounding::HalfEven>(1, 25, 10)), 2);  // 2,5 → 2 é par: fica
  EXPECT_EQ((mul_div<Rounding::HalfEven>(1, 35, 10)), 4);  // 3,5 → 3 é ímpar: sobe para 4
}

TEST(MulDiv, NegativosArredondamSimetricamente) {
  // "meio para cima" significa para longe do zero, não para o infinito positivo.
  EXPECT_EQ((mul_div<Rounding::HalfUp>(-1, 5, 10)), -1);
  EXPECT_EQ((mul_div<Rounding::TowardZero>(-1, 7, 10)), 0);
  EXPECT_EQ((mul_div<Rounding::TowardNegInf>(-1, 7, 10)), -1);
  EXPECT_EQ((mul_div<Rounding::HalfEven>(-1, 5, 10)), 0);
  EXPECT_EQ((mul_div<Rounding::HalfEven>(-1, 15, 10)), -2);
}

TEST(MulDiv, IntermediarioPassaDeInt64) {
  // O caso que justifica i128: numerador de 5e19 contra um teto de 9,2e18.
  // (cenário golden 11 — subscrição)
  const int64_t custo = 49'793'900;              // R$ 4.979,39 em 1e-4
  const int64_t resultado = mul_div<Rounding::HalfEven>(custo, 1'000'000'000'000LL, 16'300'000'000LL);
  EXPECT_EQ(resultado, 3'054'840'491);
  EXPECT_GT(static_cast<i128>(custo) * 1'000'000'000'000LL, static_cast<i128>(INT64_MAX));
}

// ---------------------------------------------------------------- cenário 01
TEST(Golden01, PrecoMedioDaPrimeiraCompra) {
  // golden/01: 100 ações a R$ 32,45 + R$ 5,20 de custos → R$ 32,502 por ação.
  const Money custo = brl(32'502'000);  // R$ 3.250,20
  const Price pm = average_price_half_even(Qty{}, Price{}, acoes(100), custo);
  EXPECT_EQ(pm.raw(), 3'250'200'000);
}

// ---------------------------------------------------------------- cenário 02
TEST(Golden02, PrecoMedioPonderadoDeDuasCompras) {
  // golden/02: 100 @ custo 3.250,20 e depois 37 @ custo 1.157,19 → 32,17072993
  const Price pm1 = average_price_half_even(Qty{}, Price{}, acoes(100), brl(32'502'000));
  const Price pm2 = average_price_half_even(acoes(100), pm1, acoes(37), brl(11'571'900));
  EXPECT_EQ(pm2.raw(), 3'217'072'993);
  // e o custo reconstruído bate com a conta do cenário: R$ 4.407,39
  EXPECT_EQ(position_cost_half_even(acoes(137), pm2).raw(), 44'073'900);
}

// ---------------------------------------------------------------- cenário 03 e 05
TEST(Golden03, PrecoMedioAposCompraDeCemAtrinta) {
  // golden/03 e golden/05 chegam ao mesmo número por caminhos diferentes: 31,27168776
  const Price pm = average_price_half_even(acoes(137), preco(3'217'072'993), acoes(100),
                                           brl(30'040'000));
  EXPECT_EQ(pm.raw(), 3'127'168'776);
}

// ---------------------------------------------------------------- cenário 07
TEST(Golden07, DesdobramentoUmPorDez) {
  // golden/07: 137 ações a 32,17072993; desdobramento 1:10 → num=10 den=1
  Qty sobras{};
  const Qty nova = scale_qty_trunc(acoes(137), 10, 1, sobras);
  EXPECT_EQ(nova.raw(), Qty::from_units(1370).raw());
  EXPECT_EQ(sobras.raw(), 0) << "desdobramento não gera fração";

  const Price pm = scale_price_half_even(preco(3'217'072'993), 10, 1);
  EXPECT_EQ(pm.raw(), 321'707'299);

  // conservação de valor: a diferença é o arredondamento de uma casa vezes a quantidade.
  const i128 antes = static_cast<i128>(137) * 3'217'072'993LL;
  const i128 depois = static_cast<i128>(1370) * pm.raw();
  EXPECT_EQ(static_cast<int64_t>(antes - depois), 411) << "número fechado à mão em golden/07";
}

// ---------------------------------------------------------------- cenário 08
TEST(Golden08, GrupamentoDezPorUmComSobras) {
  // golden/08: 1.377 ações; grupamento 10:1 → num=1 den=10; sobra 0,7 ação NOVA
  Qty sobras{};
  const Qty nova = scale_qty_trunc(acoes(1377), 1, 10, sobras);
  EXPECT_EQ(nova.raw(), Qty::from_units(137).raw());
  EXPECT_EQ(sobras.raw(), 70'000'000) << "0,7 ação na unidade CORRENTE (I1 soma sobras)";

  const Price pm = scale_price_half_even(preco(321'707'299), 1, 10);
  EXPECT_EQ(pm.raw(), 3'217'072'990);

  // o leilão da sobra: 0,7 ação nova a R$ 314,00 = R$ 219,80 — exato, sem o 219,79999 do double.
  const Money leilao = notional_half_even(sobras, preco(31'400'000'000), 1);
  EXPECT_EQ(leilao.raw(), 2'198'000);
}

// ---------------------------------------------------------------- cenário 09
TEST(Golden09, BonificacaoDeCincoPorCento) {
  // golden/09: 137 ações + 5 % → 6,85; 6 inteiras entram, 0,85 vai para sobras
  Qty sobras{};
  const Qty nova = scale_qty_trunc(acoes(137), 105, 100, sobras);
  EXPECT_EQ(nova.raw(), Qty::from_units(143).raw());
  EXPECT_EQ(sobras.raw(), 85'000'000);

  // custo atribuído pela companhia: R$ 12,00 por ação bonificada, sobre as 6 que entraram
  const Price pm = average_price_half_even(acoes(137), preco(3'217'072'993), acoes(6),
                                           brl(720'000));
  EXPECT_EQ(pm.raw(), 3'132'440'559);
}

// ---------------------------------------------------------------- cenário 11
TEST(Golden11, SubscricaoExercidaParcialmente) {
  // golden/11: 143 ações a 31,32440559; exerce 20 a R$ 25,00 → 30,54840491
  const Price pm = average_price_half_even(acoes(143), preco(3'132'440'559), acoes(20),
                                           brl(5'000'000));
  EXPECT_EQ(pm.raw(), 3'054'840'491);
}

// ---------------------------------------------------------------- cenário 12 (I7)
TEST(Golden12, GrossAmountComFatorDeCotacao) {
  // golden/12: 137 unidades, fechamento R$ 1.234,567, fator 1000 → R$ 169,1357
  const Money g = notional_half_even(acoes(137), preco(123'456'700'000), 1000);
  EXPECT_EQ(g.raw(), 1'691'357);

  // fator 1: o caminho comum
  EXPECT_EQ(notional_half_even(acoes(100), preco(3'245'000'000), 1).raw(), 32'450'000);
}

TEST(Golden12, GrossAmountNoEmpateSegueHalfEven) {
  // Construído para cair exatamente em 0,5: qty=1, price tal que o produto/1e12 termine em ,00005
  // 1 ação × R$ 0,00005 = R$ 0,00005 → em 1e-4 é 0,5 → half-even manda para 0 (par).
  EXPECT_EQ(notional_half_even(acoes(1), preco(5'000), 1).raw(), 0);
  // 3 ações × R$ 0,00005 = R$ 0,00015 → 1,5 → half-even manda para 2 (1 é ímpar).
  EXPECT_EQ(notional_half_even(acoes(3), preco(5'000), 1).raw(), 2);
}

// ---------------------------------------------------------------- tarifas e retenção
TEST(Tarifas, HalfUpNoMeioCentavo) {
  // 0,25 % sobre R$ 1.000,00 = R$ 2,50 exato — o caso comum, sem arredondamento.
  EXPECT_EQ(fee_half_up(brl(10'000'000), 2'500).raw(), 25'000);

  // O empate. 50 % é taxa irreal, e é justamente por isso que serve: é a forma mais direta de
  // colocar a conta exatamente em ,5 sem depender de um valor "bonito". 21 × 0,5 = 10,5.
  EXPECT_EQ(fee_half_up(brl(21), 500'000).raw(), 11) << "empate sobe";
  EXPECT_EQ(fee_half_up(brl(-21), 500'000).raw(), -11) << "para cima = para longe do zero";
  // O mesmo empate sob a outra política, para que a diferença fique registrada num teste:
  EXPECT_EQ((mul_div<Rounding::HalfEven>(21, 500'000, 1'000'000)), 10) << "10 é par";
}

TEST(Retencao, TruncaAFavorDoContribuinte) {
  // IRRF de 15 % sobre R$ 11,6165 = R$ 1,742475 → trunca para R$ 1,7424
  EXPECT_EQ(withhold_trunc(brl(116'165), 1'500).raw(), 17'424);
  // day trade: 1 % sobre o ganho
  EXPECT_EQ(withhold_trunc(brl(999), 100).raw(), 9);
}

// ---------------------------------------------------------------- propriedades
TEST(Propriedade, ErroDoPrecoMedioNaoAcumulaAlemDeUmUlpPorOperacao) {
  // Mil compras sucessivas de 1 ação a R$ 10,00 devem manter o preço médio em 10,00000000.
  // Se o erro de reconstrução do custo acumulasse, ele apareceria aqui.
  Qty q{};
  Price pm{};
  for (int i = 0; i < 1000; ++i) {
    pm = average_price_half_even(q, pm, acoes(1), brl(100'000));  // R$ 10,00
    q += acoes(1);
  }
  EXPECT_EQ(q.raw(), Qty::from_units(1000).raw());
  EXPECT_EQ(pm.raw(), 1'000'000'000) << "R$ 10,00000000 exato depois de mil operações";
}

TEST(Propriedade, ConservacaoDeValorNoDesdobramento) {
  // Para qualquer fator, |valor antes − valor depois| < qty ulps de preço.
  for (uint32_t fator : {2u, 3u, 4u, 5u, 10u, 100u}) {
    const Qty q = acoes(1'234);
    const Price p = preco(1'234'567'891);
    Qty sobras{};
    const Qty q2 = scale_qty_trunc(q, fator, 1, sobras);
    const Price p2 = scale_price_half_even(p, fator, 1);
    const i128 antes = static_cast<i128>(q.raw()) * p.raw();
    const i128 depois = static_cast<i128>(q2.raw()) * p2.raw();
    const i128 diff = antes > depois ? antes - depois : depois - antes;
    EXPECT_LT(diff, static_cast<i128>(q2.raw())) << "fator " << fator;
    EXPECT_EQ(sobras.raw(), 0) << "desdobramento por inteiro não gera fração; fator " << fator;
  }
}

TEST(Propriedade, SobraDoGrupamentoMaisInteirasReconstroiOTotal) {
  for (int64_t n : {1'377, 1'000, 999, 7, 1}) {
    Qty sobras{};
    const Qty inteiras = scale_qty_trunc(acoes(n), 1, 10, sobras);
    const i128 total = static_cast<i128>(inteiras.raw()) + sobras.raw();
    EXPECT_EQ(total, static_cast<i128>(acoes(n).raw()) / 10) << "n=" << n;
    EXPECT_LT(sobras.raw(), Qty::kOne) << "a sobra é sempre menor que uma ação";
    EXPECT_GE(sobras.raw(), 0);
  }
}

// ---------------------------------------------------------------- tipos
TEST(Tipos, UnidadesNaoSeMisturam) {
  // Estas linhas não compilam de propósito — a tag de unidade é o que as impede:
  //   Qty q = Price::from_units(1);          // erro: tipos diferentes
  //   auto x = acoes(1) + preco(1);          // erro: operator+ exige o mesmo tipo
  static_assert(!std::is_same_v<Qty, Price>);
  static_assert(!std::is_convertible_v<Qty, Price>);
  static_assert(!std::is_convertible_v<Money, Qty>);
  SUCCEED();
}
