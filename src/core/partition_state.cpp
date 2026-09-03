#include "core/partition_state.hpp"

#include <cstring>

namespace rv::core {
namespace {
template <class T>
[[nodiscard]] bool coluna(Arena& a, T*& destino, uint32_t n) noexcept {
  destino = a.alloc_array<T>(n);
  if (destino == nullptr) return false;
  std::memset(static_cast<void*>(destino), 0, sizeof(T) * n);
  return true;
}
}  // namespace

bool InstrumentTable::init(Arena& a, uint32_t cap) noexcept {
  const bool ok = coluna(a, external_id, cap) && coluna(a, symbol, cap) &&
                  coluna(a, isin, cap) && coluna(a, type, cap) &&
                  coluna(a, price_factor, cap) && coluna(a, lot_size, cap) &&
                  coluna(a, closing_price, cap) && coluna(a, previous_close, cap) &&
                  coluna(a, closing_date, cap);
  if (!ok) return false;
  capacity = cap;
  return true;
}

bool TradeTable::init(Arena& a, uint32_t cap) noexcept {
  const bool ok = coluna(a, trade_id, cap) && coluna(a, broker_note_id, cap) &&
                  coluna(a, account, cap) && coluna(a, instrument, cap) && coluna(a, qty, cap) &&
                  coluna(a, price, cap) && coluna(a, cash, cap) &&
                  coluna(a, settlement_date, cap) && coluna(a, side, cap) &&
                  coluna(a, state, cap) && coluna(a, next_of_account, cap);
  if (!ok) return false;
  capacity = cap;
  return true;
}

bool PartitionState::init(Arena& a, PartitionId pid, const PartitionCapacity& c) noexcept {
  id = pid;
  cap = c;

  if (!custody.init(a, c.positions)) return false;
  if (!cash.init(a, c.accounts)) return false;
  if (!instruments.init(a, c.instruments)) return false;
  if (!trades.init(a, c.trades)) return false;

  if (!account_index.init(a, c.accounts)) return false;
  if (!position_index.init(a, c.positions)) return false;
  if (!instrument_index.init(a, c.instruments)) return false;
  if (!trade_index.init(a, c.trades)) return false;
  if (!applied_actions.init(a, c.corporate_actions_per_gen)) return false;
  if (!applied_income.init(a, c.corporate_actions_per_gen)) return false;

  if (!coluna(a, account_first_trade, c.accounts)) return false;
  if (!coluna(a, account_document, c.accounts)) return false;
  for (uint32_t i = 0; i < c.accounts; ++i) account_first_trade[i] = TradeTable::kNil;

  exceptions = a.alloc_array<ExceptionRecord>(c.exceptions);
  if (exceptions == nullptr) return false;
  exception_capacity = c.exceptions;

  // A arena NÃO é selada aqui: o `Outbox` e o `Wal` ainda vão alocar dela no warm-up. Quem compõe
  // o processo sela depois de montar tudo — e é o `seal()` que transforma "não aloque no caminho
  // quente" de regra escrita em regra aplicada.
  arena_bytes = a.used();
  return !a.overflowed();
}

// A internação é por ordem de PRIMEIRA APARIÇÃO NO LOG. É isso que a torna determinística: o
// mesmo prefixo de log produz os mesmos ids, em qualquer máquina, em qualquer replay (I12).
// Um contador global, um relógio ou um hash do endereço dariam ids diferentes a cada execução.
uint32_t PartitionState::intern_account(DocumentId doc) noexcept {
  if (cash.count >= cash.capacity) return DenseIndex::kEmpty;
  bool novo = false;
  const uint32_t id_novo = cash.count;
  const uint32_t r = account_index.insert_or_get(doc.v, id_novo, novo);
  if (r == DenseIndex::kEmpty) return DenseIndex::kEmpty;  // tabela cheia: o chamador trata
  if (novo) {
    cash.clear_row(id_novo);
    account_document[id_novo] = doc.v;
    account_first_trade[id_novo] = TradeTable::kNil;
    ++cash.count;
  }
  return r;
}

uint32_t PartitionState::intern_instrument(uint32_t external_id) noexcept {
  if (instruments.count >= instruments.capacity) return DenseIndex::kEmpty;
  bool novo = false;
  const uint32_t slot_novo = instruments.count;
  const uint32_t r = instrument_index.insert_or_get(external_id, slot_novo, novo);
  if (r == DenseIndex::kEmpty) return DenseIndex::kEmpty;
  if (novo) {
    // `price_factor == 0` significa NÃO DESCRITO. Zero não é um fator válido, então ele serve
    // de marca sem custar um campo. Um instrumento citado por um negócio antes de `ClosingPriceSet`
    // ganha a linha — para que o cadastro possa preenchê-la depois — mas o negócio é rejeitado
    // (Err::InstrumentNotDescribed): sem o fator de cotação, `grossAmount` sairia errado (I7).
    instruments.external_id[slot_novo] = external_id;
    instruments.price_factor[slot_novo] = 0;
    instruments.lot_size[slot_novo] = 0;
    ++instruments.count;
  }
  return r;
}

uint32_t PartitionState::intern_position(uint32_t account, uint32_t instrument) noexcept {
  const uint64_t chave = (static_cast<uint64_t>(account) << 32) | instrument;
  if (custody.count >= custody.capacity) return DenseIndex::kEmpty;
  bool novo = false;
  const uint32_t slot_novo = custody.count;
  const uint32_t r = position_index.insert_or_get(chave, slot_novo, novo);
  if (r == DenseIndex::kEmpty) return DenseIndex::kEmpty;
  if (novo) {
    custody.clear_row(slot_novo);
    custody.account[slot_novo] = account;
    custody.instrument[slot_novo] = instrument;
    ++custody.count;
  }
  return r;
}

// Os dois checksums que `EodMarked` carrega. Somar em `uint64` com transbordo é DE PROPÓSITO:
// o valor não significa nada sozinho, ele só precisa mudar quando qualquer bucket mudar. Somar
// em `int64` com verificação de estouro custaria mais e não acrescentaria nada.
//
// Por que percorrer a coluna e não manter um acumulador incremental: um acumulador erraria junto
// com o bucket, e a âncora existe justamente para pegar o erro. O percurso é O(posições) e
// acontece uma vez por dia.
uint64_t PartitionState::custody_checksum() const noexcept {
  uint64_t s = 0;
  for (uint32_t i = 0; i < custody.count; ++i) {
    s += static_cast<uint64_t>(custody.available[i].raw());
    s += static_cast<uint64_t>(custody.blocked[i].raw());
    s += static_cast<uint64_t>(custody.leftovers[i].raw());
    s += static_cast<uint64_t>(custody.overdue_buy[i].raw());
    s += static_cast<uint64_t>(custody.overdue_sell[i].raw());
    s += static_cast<uint64_t>(custody.avg_price[i].raw());
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      s += static_cast<uint64_t>(custody.pending_buy[i * kSettlementSlots + k].raw());
      s += static_cast<uint64_t>(custody.pending_sell[i * kSettlementSlots + k].raw());
    }
  }
  return s;
}

uint64_t PartitionState::cash_checksum() const noexcept {
  uint64_t s = 0;
  for (uint32_t i = 0; i < cash.count; ++i) {
    s += static_cast<uint64_t>(cash.cash[i].raw());
    s += static_cast<uint64_t>(cash.overdue[i].raw());
    s += static_cast<uint64_t>(cash.income_receivable[i].raw());
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      s += static_cast<uint64_t>(cash.pending[i * kSettlementSlots + k].raw());
    }
  }
  return s;
}

void PartitionState::push_exception(const ExceptionRecord& r, bool marca_divergencia) noexcept {
  // A fila é um buffer circular: quando enche, a entrada mais antiga é descartada e o contador
  // de descarte sobe. Encher a fila NÃO pode derrubar a partição — a fila existe para registrar
  // problemas, e um mecanismo de registro que derruba o processo quando há muitos problemas é o
  // pior comportamento possível exatamente no pior momento.
  if (exception_capacity == 0) return;
  // O comentário acima prometia um contador de descarte e ele não existia: a fila sobrescrevia em
  // silêncio, e `exception_count` (que conta PUSH, não ocupação) não distinguia "12 exceções" de
  // "12 mil, das quais restam 256". Agora o descarte é contado e vai para o snapshot.
  if (exception_count >= exception_capacity) ++exception_dropped;
  exceptions[exception_count % exception_capacity] = r;
  ++exception_count;
  if (marca_divergencia) flags |= kFlagReconDivergence;
}

}  // namespace rv::core
