// A implementação de `apply` — dez eventos, dez funções, um despacho.
//
// Leia `src/core/apply.hpp` primeiro: lá está o contrato de determinismo D1..D7, que é o que
// governa cada linha deste arquivo. Aqui está só o "como".
//
// Padrão que se repete em todas as funções, e que vale enunciar uma vez:
//
//   1. DECODIFICA e valida tamanho. Payload curto é rejeição, não leitura fora dos limites.
//   2. RESOLVE identidades (documento → conta, instrumento → slot, conta×instrumento → posição).
//      A internação é por ordem de primeira aparição no log: determinística por construção.
//   3. CHECA as pré-condições de negócio e devolve `Status` — sem mutar nada. Um evento rejeitado
//      deixa o estado byte a byte idêntico; é isso que o teste afirma, e não só os campos que
//      alguém lembrou de conferir.
//   4. MUTA. Só depois de todas as checagens.
//   5. VERIFICA os invariantes em build de debug (`RV_INVARIANT`).

#include "core/apply.hpp"

#include <cstring>

#include "base/metrics.hpp"
#include "base/rounding.hpp"
#include "codec/events.hpp"
#include "codec/events_decode.hpp"
#include "core/outbox.hpp"
#include "core/partition_state.hpp"

namespace rv::core {
namespace {

using namespace rv::codec;

// Em debug, uma violação de invariante aborta com o número na mão. Em release, ela vira o
// `Status` que o chamador já trata — o número do invariante fica na métrica.
// `RV_CHECK` afirma um invariante DEPOIS da mutação — é a rede de "isto não deveria ser possível",
// não a validação da entrada. Toda rejeição de negócio acontece ANTES de mutar, explicitamente,
// com o seu próprio `Err` (passo 3 do padrão descrito no topo do arquivo).
//
// A primeira versão passava um `Err` de rejeição aqui, e uma revisão independente mostrou o
// estrago: em release, uma violação de I3 devolvia `NegativeBucket` — código 202, faixa de
// REJEIÇÃO — depois de já ter mutado. A partição não parava, o outbox não congelava, e a posição
// ficava envenenada: todo evento seguinte sobre ela era "rejeitado" já tendo aplicado seus efeitos.
// Em debug, o mesmo evento abortava o processo. Mesmo log, resultados diferentes por build — o que
// destrói a própria promessa de I11.
//
// Agora os dois ramos concordam sobre a CLASSE: violação de invariante depois da mutação é
// corrupção, logo `StateCorrupt` (fatal). Debug aborta na hora para dar a pilha; release para a
// partição e congela a saída. O que muda é quando se descobre, não o que significa.
#if MOTOR_RV_INVARIANT_ASSERTS
#define RV_CHECK(id, cond)                                          \
  do {                                                              \
    if (!(cond)) {                                                  \
      panic_precondition("invariante " #id " em " __FILE__, #cond);  \
    }                                                               \
  } while (0)
#else
#define RV_CHECK(id, cond)                              \
  do {                                                  \
    if (!(cond)) return Status::fail(Err::StateCorrupt); \
  } while (0)
#endif

// ---------------------------------------------------------------- utilidades

// Empilha a atualização de uma posição no outbox. É o ÚNICO caminho de saída do núcleo, e é por
// isso que I10 é verificável: `Outbox::ready(durable)` é um portão, não uma convenção.
//
// Falta de espaço aqui é FATAL, não uma saída perdida em silêncio: uma atualização que o evento
// produziu e que ninguém verá quebraria a promessa de que o read model é função do log.
[[nodiscard]] Status publica_posicao(PartitionState& s, ApplyContext& ctx, const EventView& ev,
                                     uint32_t pos) noexcept {
  if (ctx.outbox == nullptr) return kOk;
  const PositionUpdate u{s.account_document[s.custody.account[pos]],
                         s.custody.available[pos].raw(), s.custody.avg_price[pos].raw(),
                         s.instruments.external_id[s.custody.instrument[pos]], 0};
  const Status st = ctx.outbox->stage(ev.lsn, OutKind::ReadModelUpdate,
                                      ByteSpan{reinterpret_cast<const std::byte*>(&u), sizeof u});
  return st.is_ok() ? kOk : Status::fail(Err::ArenaExhausted, static_cast<uint32_t>(pos));
}

// Desliga um negócio da lista encadeada de uma conta. Devolve false se ele não estava lá — o que
// é informação, não erro silencioso: significa que a lista e `trades.account[]` divergiram.
[[nodiscard]] bool unlink_trade(PartitionState& s, uint32_t conta, uint32_t t) noexcept {
  uint32_t atual = s.account_first_trade[conta];
  if (atual == TradeTable::kNil) return false;
  if (atual == t) {
    s.account_first_trade[conta] = s.trades.next_of_account[t];
    return true;
  }
  while (s.trades.next_of_account[atual] != TradeTable::kNil) {
    if (s.trades.next_of_account[atual] == t) {
      s.trades.next_of_account[atual] = s.trades.next_of_account[t];
      return true;
    }
    atual = s.trades.next_of_account[atual];
  }
  return false;
}

template <class T>
[[nodiscard]] Result<const T*> decode(const EventView& ev) noexcept {
  if (ev.len < T::kBlockLength) return Status::fail(Err::ShortPayload, ev.len);
  return view_bytes<T>(ByteSpan{ev.payload, ev.len});
}

[[nodiscard]] Money fees_of(const TradeExecuted& t) noexcept {
  return Money::from_raw(t.brokerage_fee) + Money::from_raw(t.exchange_fee) +
         Money::from_raw(t.clearing_fee) + Money::from_raw(t.taxes);
}

// A base do preço médio é o que o investidor POSSUI — a quantidade de I1 —, não o que está livre.
//
// Este é o ponto mais fácil de errar do motor inteiro, e o cenário golden 03 existe para fixá-lo.
// Quando uma compra liquida, as ações que estão reservadas para uma venda ainda pendente CONTINUAM
// sendo do investidor: elas entram na média. Usar `available` daria 87 + 100 = 187 onde o correto
// é 137 + 100 = 237, e o preço médio sairia errado para sempre, sem violar nenhum outro invariante.
[[nodiscard]] Qty avg_price_base(const PartitionState& s, uint32_t pos) noexcept {
  return s.custody.custody_today(pos);
}

// ---------------------------------------------------------------- 1. DayOpened

// Abre o dia e ROTACIONA a janela de liquidação. O que não couber na janela nova venceu sem
// liquidar e vai para `overdue` — é aqui que a falha de entrega silenciosa é capturada, e é por
// isso que o bucket `overdue` existe (ver o cabeçalho de core/ledger.hpp).
[[nodiscard]] Status apply_day_opened(PartitionState& s, const EventView& ev, ApplyContext&) noexcept {
  const auto r = decode<DayOpened>(ev);
  if (!r) return r.status();
  const DayOpened& e = **r;

  const SettlementWindow antiga = s.window;
  SettlementWindow nova{};
  nova.dates[0] = DateYmd{e.business_date};
  nova.dates[1] = DateYmd{e.settle_d1};
  nova.dates[2] = DateYmd{e.settle_d2};
  if (nova.dates[0].empty()) return Status::fail(Err::InvalidArgument);

  // O dia só anda para a frente. Reabrir um dia anterior reordenaria buckets e quebraria I11.
  if (!s.business_date.empty() && nova.dates[0] <= s.business_date) {
    return Status::fail(Err::InvalidTransition, e.business_date);
  }

  // Rearranja os buckets: cada data antiga vai para o slot que ela ocupa na janela nova, ou para
  // `overdue` se saiu da janela. O percurso é O(posições) e acontece uma vez por dia.
  for (uint32_t i = 0; i < s.custody.count; ++i) {
    Qty nb[kSettlementSlots]{};
    Qty ns[kSettlementSlots]{};
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      const Qty b = s.custody.pending_buy[i * kSettlementSlots + k];
      const Qty v = s.custody.pending_sell[i * kSettlementSlots + k];
      if (b.is_zero() && v.is_zero()) continue;
      const uint32_t j = nova.slot_of(antiga.dates[k]);
      if (j == SettlementWindow::kNoSlot) {
        s.custody.overdue_buy[i] += b;
        s.custody.overdue_sell[i] += v;
      } else {
        nb[j] += b;
        ns[j] += v;
      }
    }
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      s.custody.pending_buy[i * kSettlementSlots + k] = nb[k];
      s.custody.pending_sell[i * kSettlementSlots + k] = ns[k];
    }
  }
  for (uint32_t i = 0; i < s.cash.count; ++i) {
    Money np[kSettlementSlots]{};
    for (uint32_t k = 0; k < kSettlementSlots; ++k) {
      const Money p = s.cash.pending[i * kSettlementSlots + k];
      if (p.is_zero()) continue;
      const uint32_t j = nova.slot_of(antiga.dates[k]);
      if (j == SettlementWindow::kNoSlot) {
        s.cash.overdue[i] += p;
      } else {
        np[j] += p;
      }
    }
    for (uint32_t k = 0; k < kSettlementSlots; ++k) s.cash.pending[i * kSettlementSlots + k] = np[k];
  }

  s.window = nova;
  s.business_date = nova.dates[0];
  s.prev_business_date = DateYmd{e.prev_business_date};
  // Poda do conjunto de idempotência por DATA DE EVENTO, nunca por relógio (I12 protegendo I6).
  s.applied_actions.maybe_rotate(s.business_date, s.cap.idempotency_window_days);
  s.applied_income.maybe_rotate(s.business_date, s.cap.idempotency_window_days);
  return kOk;
}

// ---------------------------------------------------------------- 2. TradeExecuted

[[nodiscard]] Status apply_trade_executed(PartitionState& s, const EventView& ev,
                                          ApplyContext& ctx) noexcept {
  const auto r = decode<TradeExecuted>(ev);
  if (!r) return r.status();
  const TradeExecuted& e = **r;

  if (s.trades.count >= s.trades.capacity) return Status::fail(Err::ArenaExhausted);
  if (s.trade_index.contains(e.trade_id)) return Status::fail(Err::AlreadyApplied);

  const uint32_t acct = s.intern_account(DocumentId{e.account});
  if (acct == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  const uint32_t inst = s.intern_instrument(e.instrument);
  if (inst == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  if (s.instruments.price_factor[inst] == 0) {
    return Status::fail(Err::InstrumentNotDescribed, e.instrument);
  }
  const uint32_t slot = s.window.slot_of(DateYmd{e.settlement_date});
  if (slot == SettlementWindow::kNoSlot) {
    return Status::fail(Err::OutsideSettlementWindow, e.settlement_date);
  }
  const uint32_t pos = s.intern_position(acct, inst);
  if (pos == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);

  const Qty qty = Qty::from_raw(e.qty);
  const Price price = Price::from_raw(e.price);
  if (qty.raw() <= 0 || price.raw() < 0) return Status::fail(Err::InvalidArgument);

  const Money bruto = notional_half_even(qty, price, s.instruments.price_factor[inst]);
  const Money custos = fees_of(e);
  const bool compra = e.side == static_cast<uint8_t>(Side::Buy);
  const bool descoberto_ok = (e.flags & (1U << 1)) != 0;

  // Checagem de I3 ANTES de qualquer mutação: venda maior que a posição livre só passa com a
  // flag explícita. Sem ela, o evento é rejeitado e o estado não se move (cenário golden 04).
  if (!compra && !descoberto_ok && s.custody.available[pos] < qty) {
    return Status::fail(Err::ShortSaleNotAllowed, static_cast<uint32_t>(pos));
  }

  // ---- daqui para baixo, muta ----
  const Money financeiro = compra ? -(bruto + custos) : (bruto - custos);
  if (compra) {
    s.custody.pending_buy[pos * kSettlementSlots + slot] += qty;
  } else {
    s.custody.available[pos] -= qty;
    s.custody.pending_sell[pos * kSettlementSlots + slot] += qty;
    if (descoberto_ok) s.custody.flags[pos] |= CustodyLedger::kShortAllowed;
  }
  s.cash.pending[acct * kSettlementSlots + slot] += financeiro;

  const uint32_t t = s.trades.count++;
  s.trades.trade_id[t] = e.trade_id;
  s.trades.broker_note_id[t] = e.broker_note_id;
  s.trades.account[t] = acct;
  s.trades.instrument[t] = inst;
  s.trades.qty[t] = qty;
  s.trades.price[t] = price;
  s.trades.cash[t] = financeiro;
  s.trades.settlement_date[t] = DateYmd{e.settlement_date};
  s.trades.side[t] = e.side;
  s.trades.state[t] = static_cast<uint8_t>(next_state(TradeState::None, TradeTrigger::Execute));
  s.trades.next_of_account[t] = s.account_first_trade[acct];
  s.account_first_trade[acct] = t;
  bool novo = false;
  (void)s.trade_index.insert_or_get(e.trade_id, t, novo);

  RV_CHECK(I3, s.custody.buckets_non_negative(pos));
  return publica_posicao(s, ctx, ev, pos);
}

// ---------------------------------------------------------------- 3. TradeAllocated

[[nodiscard]] Status apply_trade_allocated(PartitionState& s, const EventView& ev,
                                           ApplyContext&) noexcept {
  const auto r = decode<TradeAllocated>(ev);
  if (!r) return r.status();
  const TradeAllocated& e = **r;

  const uint32_t t = s.trade_index.find(e.trade_id);
  if (t == DenseIndex::kEmpty) return Status::fail(Err::NotFound, static_cast<uint32_t>(e.trade_id));

  const auto atual = static_cast<TradeState>(s.trades.state[t]);
  const TradeState proximo = next_state(atual, TradeTrigger::Allocate);
  if (proximo == TradeState::None) return Status::fail(Err::InvalidTransition, s.trades.state[t]);

  const uint32_t de = s.intern_account(DocumentId{e.from_account});
  const uint32_t para = s.intern_account(DocumentId{e.to_account});
  if (de == DenseIndex::kEmpty || para == DenseIndex::kEmpty) {
    return Status::fail(Err::ArenaExhausted);
  }
  const uint32_t inst = s.trades.instrument[t];
  const uint32_t slot = s.window.slot_of(DateYmd{e.settlement_date});
  if (slot == SettlementWindow::kNoSlot) {
    return Status::fail(Err::OutsideSettlementWindow, e.settlement_date);
  }

  // v1 NÃO fatia negócio (ADR-0025). O schema declara que `TradeAllocated` pode fatiar, mas
  // fatiar exige criar uma linha nova na tabela de negócios — e mover só os buckets, mantendo uma
  // linha só, quebra I2 nas duas contas: a soma dos negócios pendentes da origem continua contando
  // o negócio inteiro enquanto o `a_liquidar` dela já perdeu a parcela. Rejeitar é explícito;
  // aceitar pela metade seria errado em silêncio.
  if (Qty::from_raw(e.qty) != s.trades.qty[t]) {
    return Status::fail(Err::QtyMismatch, static_cast<uint32_t>(e.trade_id));
  }

  // Alocação para a mesma conta é o caso comum (o investidor negocia direto) e não move bucket
  // nenhum — mas o evento AINDA assim tem de existir e avançar a máquina de estados, porque é ele
  // que fixa o titular. É a aresta `Executado -> Alocado` de docs/dominio.md, e o motivo pelo qual
  // `Executado -> Compensado` não existe.
  if (de != para) {
    const uint32_t pos_de = s.intern_position(de, inst);
    const uint32_t pos_para = s.intern_position(para, inst);
    if (pos_de == DenseIndex::kEmpty || pos_para == DenseIndex::kEmpty) {
      return Status::fail(Err::ArenaExhausted);
    }
    const Qty q = Qty::from_raw(e.qty);
    const Money dinheiro = Money::from_raw(e.cash_amount);
    const bool compra = e.side == static_cast<uint8_t>(Side::Buy);
    const uint32_t k_de = pos_de * kSettlementSlots + slot;
    const uint32_t k_para = pos_para * kSettlementSlots + slot;

    // ---- CHECAGENS ANTES DE QUALQUER MUTAÇÃO (passo 3 do padrão do arquivo) ----
    //
    // A primeira versão desta função não tinha nenhuma: era o único manipulador que mexia em
    // quantidade sem verificar nada, e uma revisão independente mediu o efeito — alocar a perna de
    // venda deixava `disponivel` do destino em −200 com a flag de descoberto desligada, violando I3
    // sem que nenhum assert disparasse. O caminho `de != para` não era exercitado por teste algum:
    // todos alocavam `doc → doc`.
    if (compra) {
      if (s.custody.pending_buy[k_de] < q) return Status::fail(Err::QtyMismatch, pos_de);
    } else {
      if (s.custody.pending_sell[k_de] < q) return Status::fail(Err::QtyMismatch, pos_de);
      const bool destino_pode_descobrir =
          (s.custody.flags[pos_para] & CustodyLedger::kShortAllowed) != 0;
      if (!destino_pode_descobrir && s.custody.available[pos_para] < q) {
        return Status::fail(Err::ShortSaleNotAllowed, pos_para);
      }
    }

    // ---- daqui para baixo, muta ----
    if (compra) {
      s.custody.pending_buy[k_de] -= q;
      s.custody.pending_buy[k_para] += q;
    } else {
      s.custody.pending_sell[k_de] -= q;
      s.custody.pending_sell[k_para] += q;
      s.custody.available[pos_de] += q;   // devolve o que a execução reservou na origem
      s.custody.available[pos_para] -= q; // e reserva no destino
    }
    s.cash.pending[de * kSettlementSlots + slot] -= dinheiro;
    s.cash.pending[para * kSettlementSlots + slot] += dinheiro;

    // O negócio muda de dono: sai da lista da origem, entra na do destino.
    //
    // A primeira versão só emendava no destino. O nó ficava nas DUAS listas, e como os laços de
    // `BatchNetted` e `TradeSettled` percorriam `account_first_trade` sem conferir o dono, um lote
    // da corretora avançava a máquina de estados de negócios que já eram do investidor. Desligar
    // é O(negócios da origem); a alternativa seria uma lista duplamente encadeada, que custaria
    // quatro bytes por negócio para acelerar uma operação rara.
    (void)unlink_trade(s, de, t);
    s.trades.account[t] = para;
    s.trades.next_of_account[t] = s.account_first_trade[para];
    s.account_first_trade[para] = t;

    RV_CHECK(I3, s.custody.buckets_non_negative(pos_de));
    RV_CHECK(I3, s.custody.buckets_non_negative(pos_para));
  }

  s.trades.state[t] = static_cast<uint8_t>(proximo);
  return kOk;
}

// ---------------------------------------------------------------- 4. BatchNetted

// O net que a câmara calculou. O motor NÃO o recalcula a partir do evento — ele COMPARA com o que
// já tinha, porque a diferença entre os dois é o sinal mais barato de que um negócio se perdeu no
// caminho (I2). Se o motor apenas copiasse o número da câmara, um negócio faltando no drop copy
// passaria despercebido até a reconciliação da depositária, dias depois.
[[nodiscard]] Status apply_batch_netted(PartitionState& s, const EventView& ev,
                                        ApplyContext&) noexcept {
  const auto r = decode<BatchNetted>(ev);
  if (!r) return r.status();
  const BatchNetted& e = **r;

  const uint32_t acct = s.intern_account(DocumentId{e.account});
  if (acct == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  const DateYmd data{e.settlement_date};
  const uint32_t slot = s.window.slot_of(data);
  if (slot == SettlementWindow::kNoSlot) {
    return Status::fail(Err::OutsideSettlementWindow, e.settlement_date);
  }

  const Money nosso = s.cash.pending[acct * kSettlementSlots + slot];
  const Money deles = Money::from_raw(e.net_amount);
  if (nosso != deles) {
    s.push_exception(ExceptionRecord{e.account, nosso.raw() - deles.raw(), 0, e.settlement_date,
                                     static_cast<uint16_t>(Err::AmountMismatch), 0});
    return Status::fail(Err::AmountMismatch, e.settlement_date);
  }

  // Avança para `Compensado` os negócios alocados da conta com esta data. A lista encadeada por
  // conta existe exatamente para que isto não varra a partição inteira.
  uint32_t contados = 0;
  for (uint32_t t = s.account_first_trade[acct]; t != TradeTable::kNil;
       t = s.trades.next_of_account[t]) {
    // Cinto de segurança: a corretude do laço não pode depender só da integridade da lista.
    if (s.trades.account[t] != acct) continue;
    if (s.trades.settlement_date[t] != data) continue;
    const auto atual = static_cast<TradeState>(s.trades.state[t]);
    const TradeState proximo = next_state(atual, TradeTrigger::Net);
    if (proximo == TradeState::None) continue;  // já compensado, ou em falha: não é erro
    s.trades.state[t] = static_cast<uint8_t>(proximo);
    ++contados;
  }
  if (e.trade_count != 0 && contados != e.trade_count) {
    s.push_exception(ExceptionRecord{e.account, static_cast<int64_t>(contados) - e.trade_count, 0,
                                     e.settlement_date, static_cast<uint16_t>(Err::QtyMismatch), 0});
  }
  return kOk;
}

// ---------------------------------------------------------------- 5. TradeSettled

[[nodiscard]] Status apply_trade_settled(PartitionState& s, const EventView& ev,
                                         ApplyContext& ctx) noexcept {
  const auto r = decode<TradeSettled>(ev);
  if (!r) return r.status();
  const TradeSettled& e = **r;

  const uint32_t acct = s.intern_account(DocumentId{e.account});
  if (acct == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  const uint32_t inst = s.intern_instrument(e.instrument);
  if (inst == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  const uint32_t pos = s.find_position(acct, inst);
  if (pos == DenseIndex::kEmpty) return Status::fail(Err::NotFound, e.instrument);

  const DateYmd data{e.settlement_date};
  const uint32_t slot = s.window.slot_of(data);
  const Qty q = Qty::from_raw(e.qty);
  const Money dinheiro = Money::from_raw(e.cash_amount);
  const bool compra = e.side == static_cast<uint8_t>(Side::Buy);
  const auto resultado = static_cast<SettleOutcome>(e.outcome);

  // `outcome` vem de fora. Sem esta validação, um valor fora da enumeração não casava com `case`
  // nenhum, o `switch` abaixo não fazia nada, a máquina de estados era avançada com o gatilho
  // `Fail` (a última alternativa do ternário original) e a função devolvia Ok — um evento
  // inventado marcando negócios como falha de entrega.
  TradeTrigger gatilho{};
  switch (resultado) {
    case SettleOutcome::Settled:         gatilho = TradeTrigger::Settle; break;
    case SettleOutcome::DeliveryFailure: gatilho = TradeTrigger::Fail;   break;
    case SettleOutcome::BoughtIn:        gatilho = TradeTrigger::BuyIn;  break;
    default:                             return Status::fail(Err::InvalidArgument, e.outcome);
  }

  switch (resultado) {
    case SettleOutcome::Settled: {
      if (slot == SettlementWindow::kNoSlot) {
        return Status::fail(Err::OutsideSettlementWindow, e.settlement_date);
      }
      const uint32_t k = pos * kSettlementSlots + slot;
      if (compra) {
        if (s.custody.pending_buy[k] < q) return Status::fail(Err::QtyMismatch);
        // O preço médio muda AQUI e só aqui, no lado da compra (I4). A base é o que se possui —
        // ver `avg_price_base`.
        s.custody.avg_price[pos] = average_price_half_even(
            avg_price_base(s, pos), s.custody.avg_price[pos], q, Money::from_raw(e.cost_basis));
        s.custody.pending_buy[k] -= q;
        s.custody.available[pos] += q;
      } else {
        if (s.custody.pending_sell[k] < q) return Status::fail(Err::QtyMismatch);
        s.custody.pending_sell[k] -= q;  // o preço médio NÃO se move em venda (I4)
      }
      s.cash.cash[acct] += dinheiro;
      s.cash.pending[acct * kSettlementSlots + slot] -= dinheiro;
      break;
    }
    case SettleOutcome::DeliveryFailure: {
      if (slot == SettlementWindow::kNoSlot) {
        return Status::fail(Err::OutsideSettlementWindow, e.settlement_date);
      }
      const uint32_t k = pos * kSettlementSlots + slot;
      // A obrigação NÃO desaparece por não ter sido honrada: ela muda de bucket. As ações
      // continuam depositadas, então I1 tem de continuar batendo — é por isso que `overdue_sell`
      // entra na soma de I1 (cenário golden 06).
      if (compra) {
        if (s.custody.pending_buy[k] < q) return Status::fail(Err::QtyMismatch);
        s.custody.pending_buy[k] -= q;
        s.custody.overdue_buy[pos] += q;
      } else {
        if (s.custody.pending_sell[k] < q) return Status::fail(Err::QtyMismatch);
        s.custody.pending_sell[k] -= q;
        s.custody.overdue_sell[pos] += q;
      }
      s.cash.pending[acct * kSettlementSlots + slot] -= dinheiro;
      s.cash.overdue[acct] += dinheiro;
      s.push_exception(ExceptionRecord{e.account, q.raw(), e.instrument, e.settlement_date,
                                       static_cast<uint16_t>(Err::QtyMismatch), 0});
      break;
    }
    case SettleOutcome::BoughtIn: {
      // A recompra honrou a entrega que faltou. O bucket vencido baixa e o financeiro pendente
      // sai de `overdue`. A máquina de estados volta para `Compensado`, nunca direto para
      // `Liquidado` — o histórico importa porque é ele que a API de movimentações expõe (R11).
      if (compra) {
        if (s.custody.overdue_buy[pos] < q) return Status::fail(Err::QtyMismatch);
        // A contraparte finalmente entregou: a quantidade sai do vencido e ENTRA em `disponivel`.
        // A primeira versão só decrementava `overdue_buy` — as ações sumiam do ledger, e nenhum
        // invariante acusava, porque `overdue_buy` conta em I13 e `disponivel` também.
        s.custody.overdue_buy[pos] -= q;
        s.custody.available[pos] += q;
      } else {
        if (s.custody.overdue_sell[pos] < q) return Status::fail(Err::QtyMismatch);
        s.custody.overdue_sell[pos] -= q;
      }
      s.cash.overdue[acct] -= dinheiro;
      s.cash.cash[acct] += dinheiro;
      break;
    }
  }

  // Avança a máquina de estados dos negócios cobertos por esta liquidação.
  for (uint32_t t = s.account_first_trade[acct]; t != TradeTable::kNil;
       t = s.trades.next_of_account[t]) {
    if (s.trades.account[t] != acct) continue;  // idem
    if (s.trades.instrument[t] != inst) continue;
    if (s.trades.settlement_date[t] != data) continue;
    if (s.trades.side[t] != e.side) continue;
    const TradeState proximo = next_state(static_cast<TradeState>(s.trades.state[t]), gatilho);
    if (proximo != TradeState::None) s.trades.state[t] = static_cast<uint8_t>(proximo);
  }

  RV_CHECK(I3, s.custody.buckets_non_negative(pos));
  return publica_posicao(s, ctx, ev, pos);
}

// ---------------------------------------------------------------- 6. CorporateActionApplied

[[nodiscard]] Status apply_corporate_action(PartitionState& s, const EventView& ev,
                                            ApplyContext& ctx) noexcept {
  const auto r = decode<CorporateActionApplied>(ev);
  if (!r) return r.status();
  const CorporateActionApplied& e = **r;

  const uint32_t acct = s.intern_account(DocumentId{e.account});
  if (acct == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  const uint32_t inst = s.intern_instrument(e.instrument);
  if (inst == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);
  const uint32_t pos = s.intern_position(acct, inst);
  if (pos == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);

  const auto tipo = static_cast<ActionType>(e.type);
  const uint32_t num = e.factor_num;
  const uint32_t den = e.factor_den;

  // ---- VALIDAÇÃO ANTES DE CONSUMIR A CHAVE DE IDEMPOTÊNCIA ----
  //
  // A primeira versão inseria a chave e só depois validava. Um evento REJEITADO saía com a chave
  // consumida para sempre: a correção do mesmo evento, reenviada, seria recusada como duplicata.
  // Além de contradizer o passo 3 do padrão do arquivo ("rejeitar não muta nada"), isso deixava
  // uma posição permanentemente sem o evento corporativo dela.
  switch (tipo) {
    case ActionType::Bonus:
    case ActionType::Split:
    case ActionType::ReverseSplit:
      if (num == 0 || den == 0) return Status::fail(Err::InvalidArgument, e.type);
      break;
    case ActionType::Subscription:
      if (e.qty_delta < 0) return Status::fail(Err::InvalidArgument, e.type);
      break;
    case ActionType::LeftoversAuction:
      break;
    default:
      // `type` vem de fora e não é validado pelo decodificador: sem este ramo, um valor fora da
      // enumeração não casava com `case` nenhum, o `switch` não fazia nada, e a função seguia
      // creditando `withheld_tax` e devolvendo Ok — um evento inventado passando por bom.
      return Status::fail(Err::InvalidArgument, e.type);
  }

  // I6: idempotência por (evento, conta). A chave é EXATA — ver base/pair_index.hpp e por que
  // misturar os 96 bits em 64 não é aceitável quando o efeito de uma colisão é pular um evento
  // corporativo de um investidor.
  bool cheio = false;
  if (!s.applied_actions.insert(e.action_id, acct, cheio)) {
    if (cheio) return Status::fail(Err::ArenaExhausted);
    return Status::fail(Err::AlreadyApplied, static_cast<uint32_t>(e.action_id));
  }

  const Qty base = s.custody.custody_today(pos);
  Qty delta_calculado{};

  switch (tipo) {
    case ActionType::Split:
    case ActionType::ReverseSplit: {
      // Duas famílias de bucket, dois tratamentos — e a diferença é I1.
      //
      // POSSUÍDO (`disponivel`, `bloqueado`): a fração vai a leilão, logo vira `sobras`, que I1
      // soma. IN VOO (`a_liquidar_*`, vencidos): a fração continua sendo do próprio bucket, e
      // mandá-la para `sobras` faria a depositária divergir do motor todo dia.
      Qty sobra_total{};
      Qty s1{};
      s.custody.available[pos] = scale_qty_trunc(s.custody.available[pos], num, den, s1);
      sobra_total += s1;
      s.custody.blocked[pos] = scale_qty_trunc(s.custody.blocked[pos], num, den, s1);
      sobra_total += s1;

      s.custody.overdue_buy[pos] = scale_qty_keep_fraction(s.custody.overdue_buy[pos], num, den);
      s.custody.overdue_sell[pos] = scale_qty_keep_fraction(s.custody.overdue_sell[pos], num, den);
      for (uint32_t k = 0; k < kSettlementSlots; ++k) {
        const uint32_t j = pos * kSettlementSlots + k;
        s.custody.pending_buy[j] = scale_qty_keep_fraction(s.custody.pending_buy[j], num, den);
        s.custody.pending_sell[j] = scale_qty_keep_fraction(s.custody.pending_sell[j], num, den);
      }

      // As sobras já existentes reescalam como quantidade possuída, e a fração da própria sobra
      // se junta às demais (cenário golden 08: `sobras` fica sempre na unidade CORRENTE).
      s.custody.leftovers[pos] = scale_qty_trunc(s.custody.leftovers[pos], num, den, s1) + s1;
      s.custody.leftovers[pos] += sobra_total;

      // O preço médio anda no sentido inverso da quantidade: o valor da posição se conserva.
      s.custody.avg_price[pos] = scale_price_half_even(s.custody.avg_price[pos], num, den);
      delta_calculado = s.custody.custody_today(pos) - base;
      break;
    }
    case ActionType::Bonus: {
      Qty sobra{};
      const Qty total = scale_qty_trunc(base, num, den, sobra);
      const Qty inteiras = total - base;
      if (inteiras.raw() < 0) return Status::fail(Err::InvalidArgument);
      // O custo atribuído é decisão da companhia e vem no evento — o motor não o inventa. Se
      // inventasse (zero, ou rateio do preço médio), a apuração de ganho sairia errada e estaria
      // internamente consistente, que é o pior jeito de errar.
      const Money custo = Money::from_raw(
          mul_div<Rounding::HalfEven>(inteiras.raw(), e.unit_cost, Qty::kOne));
      s.custody.avg_price[pos] =
          average_price_half_even(base, s.custody.avg_price[pos], inteiras, custo);
      s.custody.available[pos] += inteiras;
      s.custody.leftovers[pos] += sobra;
      delta_calculado = inteiras + sobra;
      break;
    }
    case ActionType::Subscription: {
      const Qty exercida = Qty::from_raw(e.qty_delta);
      // O direito é proporcional à posição: 1:4 vem como num=1, den=4. Exercer mais do que se tem
      // direito é entrada inválida, e `tests/domain/golden/11` é explícito sobre isso — a primeira
      // versão validava só o sinal e o teto nunca era calculado.
      if (num != 0 && den != 0) {
        Qty resto{};
        const Qty direitos = scale_qty_trunc(base, num, den, resto);
        if (exercida > direitos) return Status::fail(Err::QtyMismatch, static_cast<uint32_t>(pos));
      }
      const Money desembolso = Money::from_raw(e.cash_delta);
      s.custody.avg_price[pos] =
          average_price_half_even(base, s.custody.avg_price[pos], exercida, desembolso);
      s.custody.available[pos] += exercida;
      s.cash.cash[acct] -= desembolso;
      delta_calculado = exercida;
      break;
    }
    case ActionType::LeftoversAuction: {
      // O leilão da fração: a sobra sai, o dinheiro entra. O preço vem no evento, na unidade
      // corrente — nunca de uma conversão implícita aqui dentro.
      delta_calculado = -s.custody.leftovers[pos];
      s.custody.leftovers[pos] = Qty{};
      s.cash.cash[acct] += Money::from_raw(e.cash_delta);
      break;
    }
  }

  s.cash.income_receivable[acct] -= Money::from_raw(e.withheld_tax);

  // Quando a depositária informa a variação de quantidade, o motor VERIFICA em vez de confiar.
  // Divergência vai para a fila de exceção; o ledger fica com o número do motor, porque é ele
  // que o replay reproduz.
  if ((e.flags & 1U) != 0 && delta_calculado.raw() != e.qty_delta) {
    s.push_exception(ExceptionRecord{e.account, delta_calculado.raw() - e.qty_delta, e.instrument,
                                     e.ex_date, static_cast<uint16_t>(Err::QtyMismatch), 0});
  }

  RV_CHECK(I3, s.custody.buckets_non_negative(pos));
  return publica_posicao(s, ctx, ev, pos);
}

// ---------------------------------------------------------------- 7. DividendPaid

// O motor VERIFICA os números do pagador; não os recalcula (cenário golden 10). O valor creditado
// é decisão do emissor e da depositária, com arredondamento próprio; recalcular faria o motor e o
// extrato do investidor discordarem em centavos, todo mês, sem que ninguém tivesse errado.
[[nodiscard]] Status apply_dividend_paid(PartitionState& s, const EventView& ev,
                                         ApplyContext&) noexcept {
  const auto r = decode<DividendPaid>(ev);
  if (!r) return r.status();
  const DividendPaid& e = **r;

  const uint32_t acct = s.intern_account(DocumentId{e.account});
  if (acct == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);

  const Money bruto = Money::from_raw(e.gross_amount);
  const Money retido = Money::from_raw(e.withheld_tax);
  const Money liquido = Money::from_raw(e.net_amount);

  // Checagem 1: a aritmética interna do próprio evento.
  if (bruto - retido != liquido) return Status::fail(Err::AmountMismatch);

  // Checagem 2: o bruto bate com posição × taxa dentro de UM CENTAVO — a folga a que o pagador
  // tem direito ao arredondar. É o único cruzamento que liga o dinheiro à posição, e é o que
  // pega um provento com a base de outra conta.
  const Money esperado = Money::from_raw(
      mul_div<Rounding::HalfEven>(e.qty_basis, e.rate_per_share, Qty::kOne));
  const int64_t folga = bruto.raw() - esperado.raw();
  if (folga > 100 || folga < -100) {  // 100 unidades de 1e-4 = R$ 0,01
    s.push_exception(ExceptionRecord{e.account, folga, e.instrument, e.ex_date,
                                     static_cast<uint16_t>(Err::AmountMismatch), 0});
    return Status::fail(Err::AmountMismatch, e.instrument);
  }

  // I6 também vale para provento: `docs/dominio.md` lista dividendo e JCP como eventos
  // corporativos e `tests/domain/golden/10` declara "Exerce I6". A primeira versão não tinha
  // idempotência nenhuma — `action_id` existia no schema e nunca era lido, e um arquivo da B3
  // reentregue creditava o provento duas vezes.
  //
  // O conjunto é SEPARADO do de eventos corporativos, e não o mesmo com a chave torcida: um
  // `action_id` de provento e um de desdobramento podem coincidir sem que um tenha a ver com o
  // outro, e compartilhar o conjunto faria um cancelar o outro.
  bool cheio_i = false;
  if (!s.applied_income.insert(e.action_id, acct, cheio_i)) {
    if (cheio_i) return Status::fail(Err::ArenaExhausted);
    return Status::fail(Err::AlreadyApplied, static_cast<uint32_t>(e.action_id));
  }

  if (static_cast<IncomeStage>(e.stage) == IncomeStage::Accrued) {
    s.cash.income_receivable[acct] += liquido;
  } else {
    s.cash.income_receivable[acct] -= liquido;
    s.cash.cash[acct] += liquido;
  }
  return kOk;
}

// ---------------------------------------------------------------- 8. ClosingPriceSet

[[nodiscard]] Status apply_closing_price(PartitionState& s, const EventView& ev,
                                         ApplyContext&) noexcept {
  const auto r = decode<ClosingPriceSet>(ev);
  if (!r) return r.status();
  const ClosingPriceSet& e = **r;
  if (e.price_factor == 0) return Status::fail(Err::InvalidArgument);

  const uint32_t inst = s.intern_instrument(e.instrument);
  if (inst == DenseIndex::kEmpty) return Status::fail(Err::ArenaExhausted);

  std::memcpy(s.instruments.symbol[inst], e.symbol, sizeof(e.symbol));
  std::memcpy(s.instruments.isin[inst], e.isin, sizeof(e.isin));
  s.instruments.type[inst] = e.type;
  s.instruments.price_factor[inst] = e.price_factor;
  s.instruments.lot_size[inst] = e.lot_size;
  s.instruments.previous_close[inst] = Price::from_raw(e.previous_close);
  s.instruments.closing_price[inst] = Price::from_raw(e.closing_price);
  s.instruments.closing_date[inst] = DateYmd{e.date};
  return kOk;
}

// ---------------------------------------------------------------- 9. CustodyReconciled

// Divergência marca o snapshot e vai para a fila de exceção; NÃO ajusta o ledger e NÃO bloqueia
// o EOD (cenário golden 14). Ajustar apagaria a evidência e faria o replay reproduzir a correção
// como se fosse verdade original.
[[nodiscard]] Status apply_custody_reconciled(PartitionState& s, const EventView& ev,
                                              ApplyContext&) noexcept {
  const auto r = decode<CustodyReconciled>(ev);
  if (!r) return r.status();
  const CustodyReconciled& e = **r;

  const ByteSpan resto = ByteSpan{ev.payload, ev.len}.subspan(CustodyReconciled::kBlockLength);
  const auto g = view_group<CustodyReconciledDivergence>(resto, kMaxCustodyReconciledDivergence);
  if (!g) return g.status();

  for (const auto& d : *g) {
    s.push_exception(ExceptionRecord{d.account, d.qty_delta, d.instrument, e.date,
                                     static_cast<uint16_t>(Err::QtyMismatch), 0},
                     /*marca_divergencia=*/true);
  }
  return kOk;
}

// ---------------------------------------------------------------- 10. EodMarked

// A âncora de I11. Dois `uint64` que o replay confere contra o estado que ele reconstruiu: se
// divergirem, o replay NÃO reproduziu a execução original, e continuar significaria publicar um
// snapshot errado. Por isso é fatal — é a única classe de erro em que parar é a resposta certa.
[[nodiscard]] Status apply_eod_marked(PartitionState& s, const EventView& ev,
                                      ApplyContext&) noexcept {
  const auto r = decode<EodMarked>(ev);
  if (!r) return r.status();
  const EodMarked& e = **r;

  if (e.custody_checksum != 0 || e.cash_checksum != 0) {
    if (s.custody_checksum() != e.custody_checksum || s.cash_checksum() != e.cash_checksum) {
      return Status::fail(Err::StateCorrupt, e.date);
    }
  }
  if ((e.flags & 1U) != 0) s.flags |= PartitionState::kFlagReconDivergence;
  return kOk;
}

}  // namespace

// ---------------------------------------------------------------- despacho

Status apply(PartitionState& state, const EventView& ev, ApplyContext& ctx) noexcept {
  Status st{};
  switch (static_cast<Tmpl>(ev.tmpl)) {
    case Tmpl::DayOpened:              st = apply_day_opened(state, ev, ctx); break;
    case Tmpl::TradeExecuted:          st = apply_trade_executed(state, ev, ctx); break;
    case Tmpl::TradeAllocated:         st = apply_trade_allocated(state, ev, ctx); break;
    case Tmpl::BatchNetted:            st = apply_batch_netted(state, ev, ctx); break;
    case Tmpl::TradeSettled:           st = apply_trade_settled(state, ev, ctx); break;
    case Tmpl::CorporateActionApplied: st = apply_corporate_action(state, ev, ctx); break;
    case Tmpl::DividendPaid:           st = apply_dividend_paid(state, ev, ctx); break;
    case Tmpl::ClosingPriceSet:        st = apply_closing_price(state, ev, ctx); break;
    case Tmpl::CustodyReconciled:      st = apply_custody_reconciled(state, ev, ctx); break;
    case Tmpl::EodMarked:              st = apply_eod_marked(state, ev, ctx); break;
    default:                           st = Status::fail(Err::UnknownTemplate, ev.tmpl); break;
  }

  // `applied_lsn` avança mesmo quando o evento é rejeitado: o log é a verdade do que CHEGOU, e o
  // replay tem de parar exatamente no mesmo ponto que a execução original.
  state.applied_lsn = ev.lsn;

  if (ctx.metrics != nullptr) {
    if (st.is_ok()) {
      ++ctx.metrics->apply_accepted;
    } else if (st.is_fatal()) {
      ++ctx.metrics->apply_fatal;
    } else {
      ctx.metrics->count_reject(static_cast<uint16_t>(st.code()));
    }
  }
  return st;
}

}  // namespace rv::core
