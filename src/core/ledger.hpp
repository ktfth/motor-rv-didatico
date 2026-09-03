#pragma once
// Os dois ledgers de docs/dominio.md, em SoA (structure of arrays).
//
// Por que SoA e não um `struct Posicao` por conta×instrumento: o `apply` de um negócio toca
// `disponivel`, um bucket a liquidar e o preço médio — três campos de uma posição. O EOD e a
// reconciliação percorrem UM campo de todas as posições. Com AoS, o percurso do EOD arrastaria a
// posição inteira para o cache a cada linha; com SoA, ele lê exatamente a coluna que precisa.
//
// ---------------------------------------------------------------------------------------------
// A JANELA DE LIQUIDAÇÃO — a decisão menos óbvia deste arquivo
//
// Os buckets a liquidar são indexados por DATA. A tentação é usar `day_index % 3`, já que o ciclo
// é D+2. Está errado: D+2 conta PREGÕES, e três pregões consecutivos podem estar a cinco dias de
// calendário de distância (sexta liquida na terça). Um índice por dia de calendário colidiria.
//
// A solução: as três datas de liquidação em voo são as MESMAS para toda a partição — vêm de
// `DayOpened{business_date, settle_d1, settle_d2}` — então elas moram no estado da partição, não
// na posição. A posição guarda só os três valores; achar o slot é comparar três `uint32`, que é
// uma linha de cache e um salto previsível.
//
// E o bucket VENCIDO. Numa janela de três slots que rotaciona a cada dia, uma venda que falhou na
// entrega (cenário golden 06) ficaria sem lugar: sua data já saiu da janela, mas a obrigação não
// acabou — e I1 depende dela, porque as ações continuam depositadas. Por isso existe `overdue`:
// um bucket sem data, para onde vai o que venceu sem liquidar. Ele conta em I1 exatamente como os
// outros. Sem ele, a reconciliação acusaria divergência todos os dias após uma falha de entrega.

#include <cstdint>

#include "base/arena.hpp"
#include "base/fixed.hpp"
#include "base/ids.hpp"

namespace rv::core {

// D+0, D+1, D+2. Não é configurável: é o ciclo da B3 (ADR-0010, escopo v1 à vista).
inline constexpr uint32_t kSettlementSlots = 3;

// ---------------------------------------------------------------- custódia
// Uma linha por (conta × instrumento). Quantidades em escala 1e-8.
struct CustodyLedger {
  Qty* available = nullptr;    // [n]        livre para negociar
  Qty* pending_buy = nullptr;  // [n * kSettlementSlots]  compras a receber, por data
  Qty* pending_sell = nullptr; // [n * kSettlementSlots]  vendas a entregar, por data
  Qty* overdue_buy = nullptr;  // [n]        venceu sem liquidar (falha da contraparte)
  Qty* overdue_sell = nullptr; // [n]        venceu sem entregar (cenário golden 06)
  Qty* blocked = nullptr;      // [n]        garantia, empréstimo
  Qty* leftovers = nullptr;    // [n]        sobras, na unidade CORRENTE do instrumento
  Price* avg_price = nullptr;  // [n]        muda só em Liquidado(compra) e evento corporativo (I4)

  uint32_t* account = nullptr;     // [n] dono da linha
  uint32_t* instrument = nullptr;  // [n]
  uint8_t* flags = nullptr;        // [n] bit0: venda a descoberto autorizada (I3)

  uint32_t count = 0;
  uint32_t capacity = 0;

  static constexpr uint8_t kShortAllowed = 1U << 0;

  [[nodiscard]] bool init(Arena& a, uint32_t cap) noexcept;

  // I1 — o que a depositária guarda HOJE: o que está livre mais o que está reservado para
  // entrega. Compra pendente NÃO entra: as ações ainda não são suas.
  [[nodiscard]] Qty custody_today(uint32_t i) const noexcept {
    Qty s = available[i] + blocked[i] + leftovers[i] + overdue_sell[i];
    for (uint32_t k = 0; k < kSettlementSlots; ++k) s += pending_sell[i * kSettlementSlots + k];
    return s;
  }

  // I13 — a posição depois que tudo pendente liquidar: o número que o investidor vê no aplicativo.
  // Venda pendente NÃO entra: ela vai embora.
  [[nodiscard]] Qty custody_projected(uint32_t i) const noexcept {
    Qty s = available[i] + blocked[i] + leftovers[i] + overdue_buy[i];
    for (uint32_t k = 0; k < kSettlementSlots; ++k) s += pending_buy[i * kSettlementSlots + k];
    return s;
  }

  // I3 — nenhum bucket negativo, exceto `available` quando a conta tem a flag de descoberto.
  [[nodiscard]] bool buckets_non_negative(uint32_t i) const noexcept {
    if (available[i].is_negative() && (flags[i] & kShortAllowed) == 0) return false;
    if (blocked[i].is_negative() || leftovers[i].is_negative()) return false;
    if (overdue_buy[i].is_negative() || overdue_sell[i].is_negative()) return false;
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      if (pending_buy[i * kSettlementSlots + k].is_negative()) return false;
      if (pending_sell[i * kSettlementSlots + k].is_negative()) return false;
    }
    return true;
  }

  void clear_row(uint32_t i) noexcept;
};

// ---------------------------------------------------------------- financeiro
// Uma linha por conta. Valores em BRL, escala 1e-4.
struct CashLedger {
  Money* cash = nullptr;               // [n]
  Money* pending = nullptr;            // [n * kSettlementSlots]  net por data de liquidação
  Money* overdue = nullptr;            // [n]  venceu sem liquidar
  Money* income_receivable = nullptr;  // [n]  proventos a receber
  uint32_t count = 0;
  uint32_t capacity = 0;

  [[nodiscard]] bool init(Arena& a, uint32_t cap) noexcept;
  void clear_row(uint32_t i) noexcept;
};

// ---------------------------------------------------------------- janela de datas
// As três datas de liquidação em voo, iguais para toda a partição. Vêm de `DayOpened`; o motor
// nunca as calcula (I12: calcular exigiria ler o calendário durante o replay).
struct SettlementWindow {
  DateYmd dates[kSettlementSlots]{};

  // Achar o slot é comparar três inteiros. `kNoSlot` quando a data não está na janela — o que
  // acontece com evento atrasado e com falha de entrega, e é tratado, não ignorado.
  static constexpr uint32_t kNoSlot = 0xFFFFFFFFU;
  [[nodiscard]] constexpr uint32_t slot_of(DateYmd d) const noexcept {
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      if (dates[k] == d) return k;
    }
    return kNoSlot;
  }
  [[nodiscard]] constexpr bool is_past(DateYmd d) const noexcept {
    return !d.empty() && !dates[0].empty() && d < dates[0];
  }
};

}  // namespace rv::core
