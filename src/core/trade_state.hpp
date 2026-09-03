#pragma once
// A máquina de estados do negócio (I5) — o grafo de docs/dominio.md como TABELA.
//
// A tabela é derivada de UMA declaração: `kEdges`. Grafo no documento e tabela no código que
// discordam é o tipo de divergência que só aparece em produção; aqui o `switch` não existe, e o
// teste percorre `kEdges` inteiro mais o produto (estado × gatilho) para provar que tudo o que
// não está declarado é rejeitado.
//
// Duas arestas que faltam de propósito, e que são o ponto do cenário golden 06:
//   - `FalhaEntrega -> Liquidado` NÃO existe. A recompra devolve o negócio para `Compensado`, e
//     só de lá ele liquida. O atalho daria estado certo e histórico errado — e o histórico é o
//     que a API de movimentações expõe (R11).
//   - `Executado -> Compensado` NÃO existe. Todo negócio passa por `Alocado`, mesmo quando a
//     alocação é trivial: é o evento `Alocado` que fixa o titular, e sem ele a partição não sabe
//     de quem é a posição.

#include <cstddef>
#include <cstdint>

namespace rv::core {

enum class TradeState : uint8_t {
  None = 0,        // ainda não existe
  Executed = 1,
  Allocated = 2,
  Netted = 3,
  Settled = 4,
  DeliveryFailed = 5,
  Closed = 6,      // terminal: liquidado e baixado, ou encerrado por tratamento da B3
  Count = 7,
};

enum class TradeTrigger : uint8_t {
  Execute = 0,   // TradeExecuted
  Allocate = 1,  // TradeAllocated
  Net = 2,       // BatchNetted
  Settle = 3,    // TradeSettled{outcome=Settled}
  Fail = 4,      // TradeSettled{outcome=DeliveryFailure}
  BuyIn = 5,     // TradeSettled{outcome=BoughtIn} — a recompra devolve para Compensado
  Close = 6,     // baixa: liquidado consumido, ou encerramento por multa
  Count = 7,
};

struct Edge {
  TradeState de;
  TradeTrigger gatilho;
  TradeState para;
};

// O grafo de docs/dominio.md, aresta por aresta. Esta lista é a ÚNICA fonte.
inline constexpr Edge kEdges[] = {
    {TradeState::None,           TradeTrigger::Execute,  TradeState::Executed},
    {TradeState::Executed,       TradeTrigger::Allocate, TradeState::Allocated},
    {TradeState::Allocated,      TradeTrigger::Net,      TradeState::Netted},
    {TradeState::Netted,         TradeTrigger::Settle,   TradeState::Settled},
    {TradeState::Netted,         TradeTrigger::Fail,     TradeState::DeliveryFailed},
    {TradeState::DeliveryFailed, TradeTrigger::BuyIn,    TradeState::Netted},
    {TradeState::DeliveryFailed, TradeTrigger::Close,    TradeState::Closed},
    {TradeState::Settled,        TradeTrigger::Close,    TradeState::Closed},
};
inline constexpr uint32_t kEdgeCount = sizeof(kEdges) / sizeof(kEdges[0]);

// A tabela densa, construída em COMPILAÇÃO a partir de `kEdges`. Zero custo em runtime, e
// impossível que ela e a lista discordem.
namespace detail {
struct TransitionTable {
  TradeState v[static_cast<std::size_t>(TradeState::Count)][static_cast<std::size_t>(TradeTrigger::Count)]{};
};
consteval TransitionTable build_table() {
  TransitionTable t{};
  for (auto& linha : t.v) {
    for (auto& c : linha) c = TradeState::None;
  }
  for (const Edge& e : kEdges) {
    t.v[static_cast<std::size_t>(e.de)][static_cast<std::size_t>(e.gatilho)] = e.para;
  }
  return t;
}
inline constexpr TransitionTable kTable = build_table();
}  // namespace detail

// `TradeState::None` de volta significa "transição fora do grafo": o `apply` devolve
// `Err::InvalidTransition` e NÃO toca em bucket nenhum.
//
// Repare que `None` é ao mesmo tempo "não existe ainda" e "transição inválida". Isso é seguro
// porque a única aresta que SAI de `None` é `Execute`: qualquer outro gatilho sobre `None` é de
// fato inválido, e o duplo sentido não esconde nenhum caso.
[[nodiscard]] constexpr TradeState next_state(TradeState s, TradeTrigger g) noexcept {
  return detail::kTable.v[static_cast<std::size_t>(s)][static_cast<std::size_t>(g)];
}

[[nodiscard]] constexpr bool is_valid(TradeState s, TradeTrigger g) noexcept {
  return next_state(s, g) != TradeState::None;
}

[[nodiscard]] constexpr const char* to_string(TradeState s) noexcept {
  switch (s) {
    case TradeState::None: return "None";
    case TradeState::Executed: return "Executed";
    case TradeState::Allocated: return "Allocated";
    case TradeState::Netted: return "Netted";
    case TradeState::Settled: return "Settled";
    case TradeState::DeliveryFailed: return "DeliveryFailed";
    case TradeState::Closed: return "Closed";
    case TradeState::Count: break;
  }
  return "?";
}

// As afirmações do cenário golden 06, em compilação.
static_assert(next_state(TradeState::None, TradeTrigger::Execute) == TradeState::Executed);
static_assert(next_state(TradeState::DeliveryFailed, TradeTrigger::BuyIn) == TradeState::Netted);
static_assert(!is_valid(TradeState::DeliveryFailed, TradeTrigger::Settle),
              "recompra devolve para Compensado; não existe atalho para Liquidado");
static_assert(!is_valid(TradeState::Executed, TradeTrigger::Net),
              "todo negócio passa por Alocado: é ele que fixa o titular");
static_assert(!is_valid(TradeState::Settled, TradeTrigger::Settle), "liquidar duas vezes não");
static_assert(!is_valid(TradeState::Closed, TradeTrigger::Execute), "estado terminal é terminal");

}  // namespace rv::core
