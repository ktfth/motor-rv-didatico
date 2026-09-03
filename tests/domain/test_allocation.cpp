// `TradeAllocated` com conta de origem diferente da de destino — a alocação de verdade.
//
// Este arquivo existe porque uma revisão independente encontrou QUATRO bugs neste caminho, e todos
// pela mesma razão: nenhum teste o exercitava. Todos alocavam `doc → doc`, que não move bucket
// nenhum. É o lembrete de que cobertura de linha não é cobertura de caminho — as linhas eram
// executadas pelo ramo `de == para`, que pula o corpo inteiro.

#include <gtest/gtest.h>

#include "engine_fixture.hpp"

using namespace rv;
using namespace rv::testing;
using codec::SettleOutcome;
using codec::Side;

namespace {
constexpr uint32_t kD0 = 20260908, kD1 = 20260909, kD2 = 20260910;
constexpr uint64_t kCorretora = 12345678000199ULL;  // CNPJ

void abre(Engine& m) {
  ASSERT_TRUE(m.aplica(dia(kD0, kD1, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(cadastro(kPetr4, "PETR4", 1, 3'000'000'000, kD0)).is_ok());
}

// Percorre a lista encadeada de uma conta e devolve os negócios que ela contém.
std::vector<uint32_t> lista_de(const core::PartitionState& s, uint32_t conta) {
  std::vector<uint32_t> v;
  for (uint32_t t = s.account_first_trade[conta]; t != core::TradeTable::kNil;
       t = s.trades.next_of_account[t]) {
    v.push_back(t);
    if (v.size() > 64) break;  // proteção contra ciclo
  }
  return v;
}
}  // namespace

TEST(Alocacao, NegocioSaiDaListaDaOrigemAoMudarDeDono) {
  Engine m;
  abre(m);
  // A corretora executa dois negócios; só o segundo é alocado ao investidor.
  ASSERT_TRUE(m.aplica(negocio(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(negocio(2, kCorretora, kPetr4, Side::Buy, Qty::from_units(200).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  ASSERT_EQ(lista_de(m.estado(), m.conta(kCorretora)).size(), 2u);

  ASSERT_TRUE(m.aplica(alocacao(1, 2, kCorretora, kCpfA, kPetr4, Side::Buy,
                                Qty::from_units(200).raw(), -60'000'000, kD2)).is_ok());

  const auto na_corretora = lista_de(m.estado(), m.conta(kCorretora));
  const auto no_investidor = lista_de(m.estado(), m.conta(kCpfA));
  EXPECT_EQ(na_corretora.size(), 1u) << "o negócio alocado tem de SAIR da lista da origem";
  EXPECT_EQ(no_investidor.size(), 1u);
  EXPECT_NE(na_corretora.at(0), no_investidor.at(0));
  // E o negócio que ficou é mesmo o da corretora.
  EXPECT_EQ(m.estado().trades.account[na_corretora.at(0)], m.conta(kCorretora));
  EXPECT_EQ(m.estado().trades.account[no_investidor.at(0)], m.conta(kCpfA));
}

TEST(Alocacao, LoteDaOrigemNaoAvancaNegociosDoDestino) {
  // O sintoma que a revisão mediu: com o nó nas duas listas, um `BatchNetted` da corretora
  // avançava a máquina de estados de negócios que já eram do investidor.
  Engine m;
  abre(m);
  ASSERT_TRUE(m.aplica(negocio(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(negocio(2, kCorretora, kPetr4, Side::Buy, Qty::from_units(200).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(alocacao(1, 2, kCorretora, kCpfA, kPetr4, Side::Buy,
                                Qty::from_units(200).raw(), -60'000'000, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(alocacao(2, 1, kCorretora, kCorretora, kPetr4, Side::Buy,
                                Qty::from_units(100).raw(), -30'000'000, kD2)).is_ok());

  const uint32_t t_investidor = lista_de(m.estado(), m.conta(kCpfA)).at(0);
  ASSERT_EQ(static_cast<core::TradeState>(m.estado().trades.state[t_investidor]),
            core::TradeState::Allocated);

  // Lote da CORRETORA: só o negócio dela pode avançar.
  ASSERT_TRUE(m.aplica(net(1, kCorretora, m.a_liquidar(kCorretora, kD2).raw(), kD2)).is_ok());
  EXPECT_EQ(static_cast<core::TradeState>(m.estado().trades.state[t_investidor]),
            core::TradeState::Allocated)
      << "o negócio do investidor NÃO pode ter sido compensado pelo lote da corretora";
}

TEST(Alocacao, VendaParaDestinoSemPosicaoEhRejeitadaPorI3) {
  Engine m;
  abre(m);
  // A corretora tem posição e vende coberto em nome próprio.
  ASSERT_TRUE(m.aplica(negocio(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(500).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(liquidacao(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(500).raw(),
                                  -150'000'000, 150'000'000, kD2, SettleOutcome::Settled)).is_ok());
  ASSERT_TRUE(m.aplica(negocio(2, kCorretora, kPetr4, Side::Sell, Qty::from_units(200).raw(),
                               3'500'000'000, 0, kD2)).is_ok());

  // Alocar a venda para um investidor SEM posição: I3 seria violado no destino.
  const Status st = m.aplica(alocacao(1, 2, kCorretora, kCpfA, kPetr4, Side::Sell,
                                      Qty::from_units(200).raw(), 70'000'000, kD2));
  EXPECT_EQ(st.code(), Err::ShortSaleNotAllowed);
  EXPECT_EQ(core::classify(st), core::ApplyClass::Rejected);
  // E, sendo rejeitado, não moveu nada.
  EXPECT_EQ(m.disponivel(kCorretora, kPetr4).raw(), Qty::from_units(300).raw());
  EXPECT_EQ(lista_de(m.estado(), m.conta(kCorretora)).size(), 2u);
}

TEST(Alocacao, AlocacaoParcialEhRejeitadaExplicitamente) {
  // v1 não fatia negócio (ADR-0025). Aceitar movendo só os buckets quebraria I2 nas duas contas.
  Engine m;
  abre(m);
  ASSERT_TRUE(m.aplica(negocio(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  const Money antes_origem = m.a_liquidar(kCorretora, kD2);
  const Status st = m.aplica(alocacao(1, 1, kCorretora, kCpfA, kPetr4, Side::Buy,
                                      Qty::from_units(40).raw(), -12'000'000, kD2));
  EXPECT_EQ(st.code(), Err::QtyMismatch);
  EXPECT_EQ(m.a_liquidar(kCorretora, kD2).raw(), antes_origem.raw()) << "I2 intacto na origem";
}

TEST(Alocacao, CompraAlocadaMoveBucketEFinanceiroInteiros) {
  Engine m;
  abre(m);
  ASSERT_TRUE(m.aplica(negocio(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  ASSERT_TRUE(m.aplica(alocacao(1, 1, kCorretora, kCpfA, kPetr4, Side::Buy,
                                Qty::from_units(100).raw(), -30'000'000, kD2)).is_ok());

  EXPECT_EQ(m.i13(kCorretora, kPetr4).raw(), 0) << "a corretora não fica com a posição";
  EXPECT_EQ(m.i13(kCpfA, kPetr4).raw(), Qty::from_units(100).raw());
  EXPECT_EQ(m.a_liquidar(kCorretora, kD2).raw(), 0);
  EXPECT_EQ(m.a_liquidar(kCpfA, kD2).raw(), -30'000'000);
  EXPECT_EQ(m.i1(kCpfA, kPetr4).raw(), 0) << "compra pendente não está na depositária";
}

TEST(Alocacao, QuantidadeMaiorQueOBucketDaOrigemEhRejeitada) {
  Engine m;
  abre(m);
  ASSERT_TRUE(m.aplica(negocio(1, kCorretora, kPetr4, Side::Buy, Qty::from_units(100).raw(),
                               3'000'000'000, 0, kD2)).is_ok());
  // O evento mente sobre a quantidade: mais do que o negócio tem.
  auto a = alocacao(1, 1, kCorretora, kCpfA, kPetr4, Side::Buy, Qty::from_units(500).raw(),
                    -150'000'000, kD2);
  EXPECT_EQ(m.aplica(a).code(), Err::QtyMismatch);
}
