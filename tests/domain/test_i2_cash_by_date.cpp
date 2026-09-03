// I2 — para cada conta e data D, `a_liquidar[D]` é EXATAMENTE a soma dos negócios pendentes
// para essa data.
//
// A afirmação é sobre uma identidade, não sobre um número: o teste recomputa o lado direito
// varrendo a tabela de negócios e compara com o bucket que o motor manteve incrementalmente.
// É a única forma de o teste ter valor — se ele lesse o bucket dos dois lados, provaria apenas
// que o bucket é igual a si mesmo.

#include <gtest/gtest.h>

#include "cenario.hpp"
#include "engine_fixture.hpp"

using namespace rv;
using namespace rv::testing;
using codec::SettleOutcome;
using codec::Side;

namespace {
// As datas vêm de tests/support/cenario.hpp — redefini-las aqui criava ambiguidade de nome, que é
// o compilador avisando que existiam duas fontes para o mesmo fato.

// O lado direito de I2, recomputado do zero a partir dos negócios registrados.
Money soma_dos_negocios_pendentes(const core::PartitionState& s, uint32_t conta, DateYmd data) {
  Money total{};
  for (uint32_t t = s.account_first_trade[conta]; t != core::TradeTable::kNil;
       t = s.trades.next_of_account[t]) {
    if (s.trades.settlement_date[t] != data) continue;
    const auto estado = static_cast<core::TradeState>(s.trades.state[t]);
    if (estado == core::TradeState::Settled || estado == core::TradeState::Closed) continue;
    total += s.trades.cash[t];
  }
  return total;
}
}  // namespace

TEST(I2, ALiquidarBateComOsNegociosPendentes) {
  Engine m;
  ASSERT_TRUE(m.aplica(dia(kD0, kD1, kD2)).is_ok());
  for (uint32_t i = 1; i <= 4; ++i) {
    ASSERT_TRUE(m.aplica(cadastro(i, "TICK", 1, 2'000'000'000, kD0)).is_ok());
  }

  Lcg r{20260908};
  const uint64_t docs[] = {kCpfA, kCpfB};
  uint64_t id = 1;

  // Só compras: assim toda venda é coberta e nenhum negócio é rejeitado por I3, o que deixa a
  // identidade de I2 exata sobre TODOS os negócios registrados.
  for (uint32_t i = 0; i < 200; ++i) {
    const uint64_t doc = docs[r.below(2)];
    const uint32_t inst = 1 + r.below(4);
    const int64_t qty = Qty::from_units(static_cast<int64_t>(1 + r.below(100))).raw();
    const int64_t preco = 1'000'000'000 + static_cast<int64_t>(r.below(3'000'000'000u));
    const int64_t custos = static_cast<int64_t>(r.below(50'000));
    ASSERT_TRUE(m.aplica(negocio(id++, doc, inst, Side::Buy, qty, preco, custos, kD2)).is_ok());
  }

  for (uint64_t doc : docs) {
    const uint32_t c = m.conta(doc);
    EXPECT_EQ(m.a_liquidar(doc, kD2).raw(),
              soma_dos_negocios_pendentes(m.estado(), c, DateYmd{kD2}).raw())
        << "I2 para a conta " << doc;
    EXPECT_LT(m.a_liquidar(doc, kD2).raw(), 0) << "só compras: o net é devedor";
  }

  // Depois de liquidar cada negócio pelo SEU valor, o bucket zera — que é a mesma identidade
  // vista do outro lado.
  for (uint64_t doc : docs) {
    const uint32_t c = m.conta(doc);
    for (uint32_t t = m.estado().account_first_trade[c]; t != core::TradeTable::kNil;
         t = m.estado().trades.next_of_account[t]) {
      // `trades.instrument[t]` é o SLOT interno; o evento fala o id EXTERNO. Confundir os dois
      // foi o que este teste pegou na primeira execução — e o motivo de `external_id` existir.
      const uint32_t externo = m.estado().instruments.external_id[m.estado().trades.instrument[t]];
      const Status st = m.aplica(liquidacao(1, doc, externo, Side::Buy,
                                            m.estado().trades.qty[t].raw(),
                                            m.estado().trades.cash[t].raw(), 0, kD2,
                                            SettleOutcome::Settled));
      ASSERT_TRUE(st.is_ok()) << "negócio " << t << " instrumento externo " << externo << ": "
                              << to_string(st.code()) << " detalhe=" << st.detail();
    }
    EXPECT_EQ(m.a_liquidar(doc, kD2).raw(), 0) << "conta " << doc;
  }
}

TEST(I2, NetDaCamaraDivergenteEhRecusadoERegistrado) {
  Engine m;
  ASSERT_TRUE(m.aplica(dia(kD0, kD1, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(cadastro(1, "PETR4", 1, 3'245'000'000, kD0)).is_ok());
  ASSERT_TRUE(m.aplica(negocio(1, kCpfA, 1, Side::Buy, Qty::from_units(100).raw(), 3'245'000'000,
                               52'000, kD2)).is_ok());

  // O net que a câmara mandou não bate com o que o motor calculou: divergência de verdade.
  const uint32_t antes = m.estado().exception_count;
  EXPECT_EQ(m.aplica(net(1, kCpfA, -30'000'000, kD2, 1)).code(), Err::AmountMismatch);
  EXPECT_EQ(m.estado().exception_count, antes + 1) << "vai para a fila de exceção";
  EXPECT_EQ(m.a_liquidar(kCpfA, kD2).raw(), -32'502'000) << "o ledger fica com o número do motor";

  // Com o número certo, passa e a máquina de estados avança.
  ASSERT_TRUE(m.aplica(alocacao(1, 1, kCpfA, kCpfA, 1, Side::Buy, Qty::from_units(100).raw(),
                                -32'502'000, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(net(2, kCpfA, -32'502'000, kD2, 1)).is_ok());
  EXPECT_EQ(static_cast<core::TradeState>(m.estado().trades.state[0]), core::TradeState::Netted);
}
