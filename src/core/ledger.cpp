#include "core/ledger.hpp"

namespace rv::core {
namespace {
// Alocar coluna a coluna, cada uma contígua: é o que faz o percurso do EOD tocar só a coluna que
// interessa. Um `nullptr` em qualquer uma faz o `init` inteiro falhar — meia inicialização seria
// pior que nenhuma.
template <class T>
[[nodiscard]] bool coluna(Arena& a, T*& destino, uint32_t n) noexcept {
  destino = a.alloc_array<T>(n);
  if (destino == nullptr) return false;
  for (uint32_t i = 0; i < n; ++i) destino[i] = T{};
  return true;
}
}  // namespace

bool CustodyLedger::init(Arena& a, uint32_t cap) noexcept {
  const uint32_t np = cap * kSettlementSlots;
  const bool ok = coluna(a, available, cap) && coluna(a, pending_buy, np) &&
                  coluna(a, pending_sell, np) && coluna(a, overdue_buy, cap) &&
                  coluna(a, overdue_sell, cap) && coluna(a, blocked, cap) &&
                  coluna(a, leftovers, cap) && coluna(a, avg_price, cap) &&
                  coluna(a, account, cap) && coluna(a, instrument, cap) && coluna(a, flags, cap);
  if (!ok) return false;
  capacity = cap;
  count = 0;
  return true;
}

void CustodyLedger::clear_row(uint32_t i) noexcept {
  available[i] = Qty{};
  overdue_buy[i] = Qty{};
  overdue_sell[i] = Qty{};
  blocked[i] = Qty{};
  leftovers[i] = Qty{};
  avg_price[i] = Price{};
  flags[i] = 0;
  for (uint32_t k = 0; k < kSettlementSlots; ++k) {
    pending_buy[i * kSettlementSlots + k] = Qty{};
    pending_sell[i * kSettlementSlots + k] = Qty{};
  }
}

bool CashLedger::init(Arena& a, uint32_t cap) noexcept {
  const bool ok = coluna(a, cash, cap) && coluna(a, pending, cap * kSettlementSlots) &&
                  coluna(a, overdue, cap) && coluna(a, income_receivable, cap);
  if (!ok) return false;
  capacity = cap;
  count = 0;
  return true;
}

void CashLedger::clear_row(uint32_t i) noexcept {
  cash[i] = Money{};
  overdue[i] = Money{};
  income_receivable[i] = Money{};
  for (uint32_t k = 0; k < kSettlementSlots; ++k) pending[i * kSettlementSlots + k] = Money{};
}

}  // namespace rv::core
