// I5 — toda transição da máquina de estados do negócio segue o grafo de `docs/dominio.md`;
//      transição fora do grafo é rejeitada.
//
// Este arquivo existe porque o gate `scripts/check_invariants.py` passou a exigir que o teste
// MENCIONE o invariante, e não apenas carregue o rótulo — e I5 não tinha teste nenhum que o
// afirmasse diretamente. O rótulo estava em três suítes que exercitam a máquina de lado.
//
// A afirmação forte não é "as sete arestas funcionam": é "as 42 combinações restantes são
// recusadas". Um motor que aceitasse tudo passaria num teste que só testasse o caminho feliz.

#include <gtest/gtest.h>

#include "core/trade_state.hpp"
#include "engine_fixture.hpp"

using namespace rv;
using namespace rv::core;

namespace {
constexpr auto kEstados = static_cast<std::size_t>(TradeState::Count);
constexpr auto kGatilhos = static_cast<std::size_t>(TradeTrigger::Count);
}  // namespace

TEST(I5, ATabelaEhExatamenteOGrafoDeclarado) {
  // A tabela é construída em compilação a partir de `kEdges`. Percorrer o produto inteiro prova
  // que nenhuma aresta foi inventada pela construção nem perdida por ela.
  std::size_t validas = 0;
  for (std::size_t e = 0; e < kEstados; ++e) {
    for (std::size_t g = 0; g < kGatilhos; ++g) {
      const auto de = static_cast<TradeState>(e);
      const auto gat = static_cast<TradeTrigger>(g);
      const bool no_grafo = [&] {
        for (const Edge& a : kEdges) {
          if (a.de == de && a.gatilho == gat) return true;
        }
        return false;
      }();
      EXPECT_EQ(is_valid(de, gat), no_grafo)
          << "I5: (" << to_string(de) << ", gatilho " << g << ")";
      validas += is_valid(de, gat);
    }
  }
  // Sete estados × sete gatilhos = 49 combinações; `kEdges` declara 8. As outras 41 são o teste:
  // um motor que aceitasse tudo passaria em qualquer suíte que só exercitasse o caminho feliz.
  EXPECT_EQ(validas, kEdgeCount);
  EXPECT_EQ(kEstados * kGatilhos, 49u);
  EXPECT_EQ(kEstados * kGatilhos - validas, 41u)
      << "I5: 41 das 49 combinações têm de ser recusadas";
}

TEST(I5, ArestasQueNaoExistemDePROPOSITO) {
  // As duas ausências que o cenário golden 06 explica, afirmadas como ausências.
  EXPECT_FALSE(is_valid(TradeState::DeliveryFailed, TradeTrigger::Settle))
      << "I5: a recompra devolve para Compensado; não há atalho para Liquidado";
  EXPECT_FALSE(is_valid(TradeState::Executed, TradeTrigger::Net))
      << "I5: todo negócio passa por Alocado — é ele que fixa o titular";
  EXPECT_FALSE(is_valid(TradeState::Settled, TradeTrigger::Settle)) << "I5: liquidar duas vezes";
  EXPECT_FALSE(is_valid(TradeState::Closed, TradeTrigger::Execute)) << "I5: terminal é terminal";
  EXPECT_FALSE(is_valid(TradeState::None, TradeTrigger::Settle))
      << "I5: não se liquida o que não foi executado";
}

TEST(I5, OCicloCompletoDoGrafo) {
  TradeState s = TradeState::None;
  for (auto g : {TradeTrigger::Execute, TradeTrigger::Allocate, TradeTrigger::Net,
                 TradeTrigger::Settle, TradeTrigger::Close}) {
    const TradeState proximo = next_state(s, g);
    ASSERT_NE(proximo, TradeState::None) << "I5: o caminho feliz tem de existir inteiro";
    s = proximo;
  }
  EXPECT_EQ(s, TradeState::Closed) << "I5: `Liquidado --> [*]` é alcançável";

  // E o desvio da falha de entrega, com a volta pela recompra.
  s = TradeState::Netted;
  s = next_state(s, TradeTrigger::Fail);
  ASSERT_EQ(s, TradeState::DeliveryFailed);
  s = next_state(s, TradeTrigger::BuyIn);
  EXPECT_EQ(s, TradeState::Netted) << "I5: a recompra volta para Compensado";
}

TEST(I5, ApplyRecusaTransicaoForaDoGrafo) {
  // A tabela é uma coisa; o `apply` usá-la é outra. Aqui a afirmação é sobre o motor.
  using namespace rv::testing;
  Engine m;
  ASSERT_TRUE(m.aplica(dia(20260908, 20260909, 20260910)).is_ok());
  ASSERT_TRUE(m.aplica(cadastro(kPetr4, "PETR4", 1, 3'000'000'000, 20260908)).is_ok());
  ASSERT_TRUE(m.aplica(negocio(1, kCpfA, kPetr4, codec::Side::Buy, Qty::from_units(10).raw(),
                               3'000'000'000, 0, 20260910)).is_ok());
  ASSERT_TRUE(m.aplica(alocacao(1, 1, kCpfA, kCpfA, kPetr4, codec::Side::Buy,
                                Qty::from_units(10).raw(), -30'000'000, 20260910)).is_ok());

  // Alocar de novo: `Alocado --Allocate--> ?` não está no grafo.
  const Status st = m.aplica(alocacao(2, 1, kCpfA, kCpfA, kPetr4, codec::Side::Buy,
                                      Qty::from_units(10).raw(), -30'000'000, 20260910));
  EXPECT_EQ(st.code(), Err::InvalidTransition) << "I5 aplicado pelo motor";
  EXPECT_EQ(classify(st), ApplyClass::Rejected);
}
