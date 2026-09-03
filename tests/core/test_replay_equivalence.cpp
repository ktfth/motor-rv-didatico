// I11 — equivalência de replay — e I12 — determinismo.
//
// I11 diz: "estado após N eventos == estado após snapshot em k + replay k+1..N, para todo k".
// A afirmação só tem valor se o "==" for total. Por isso a comparação abaixo não escolhe campos:
// ela passa um CRC por TODAS as colunas dos dois ledgers, a tabela de instrumentos, a tabela de
// negócios, a fila de exceção e os contadores. Um campo novo que alguém esqueça de restaurar na
// imagem de estado quebra este teste — que é exatamente o que se quer dele.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "base/crc32c.hpp"
#include "cenario.hpp"
#include "core/state_image.hpp"
#include "core/testing/memory_journal.hpp"
#include "engine_fixture.hpp"

using namespace rv;
using namespace rv::testing;

namespace {

// Impressão digital de TODO o estado observável. Deliberadamente exaustiva.
uint32_t impressao(const core::PartitionState& s) {
  uint32_t c = 0;
  auto add = [&c](const void* p, size_t n) { c = crc32c(c, p, n); };

  const uint32_t np = s.custody.count;
  add(s.custody.available, sizeof(Qty) * np);
  add(s.custody.pending_buy, sizeof(Qty) * np * core::kSettlementSlots);
  add(s.custody.pending_sell, sizeof(Qty) * np * core::kSettlementSlots);
  add(s.custody.overdue_buy, sizeof(Qty) * np);
  add(s.custody.overdue_sell, sizeof(Qty) * np);
  add(s.custody.blocked, sizeof(Qty) * np);
  add(s.custody.leftovers, sizeof(Qty) * np);
  add(s.custody.avg_price, sizeof(Price) * np);
  add(s.custody.account, sizeof(uint32_t) * np);
  add(s.custody.instrument, sizeof(uint32_t) * np);
  add(s.custody.flags, sizeof(uint8_t) * np);

  const uint32_t na = s.cash.count;
  add(s.cash.cash, sizeof(Money) * na);
  add(s.cash.pending, sizeof(Money) * na * core::kSettlementSlots);
  add(s.cash.overdue, sizeof(Money) * na);
  add(s.cash.income_receivable, sizeof(Money) * na);
  add(s.account_document, sizeof(uint64_t) * na);
  add(s.account_first_trade, sizeof(uint32_t) * na);

  const uint32_t ni = s.instruments.count;
  add(s.instruments.symbol, 12 * ni);
  add(s.instruments.isin, 12 * ni);
  add(s.instruments.type, ni);
  add(s.instruments.price_factor, sizeof(uint32_t) * ni);
  add(s.instruments.lot_size, sizeof(uint32_t) * ni);
  add(s.instruments.closing_price, sizeof(Price) * ni);
  add(s.instruments.previous_close, sizeof(Price) * ni);
  add(s.instruments.closing_date, sizeof(DateYmd) * ni);

  const uint32_t nt = s.trades.count;
  add(s.trades.trade_id, sizeof(uint64_t) * nt);
  add(s.trades.account, sizeof(uint32_t) * nt);
  add(s.trades.instrument, sizeof(uint32_t) * nt);
  add(s.trades.qty, sizeof(Qty) * nt);
  add(s.trades.price, sizeof(Price) * nt);
  add(s.trades.cash, sizeof(Money) * nt);
  add(s.trades.settlement_date, sizeof(DateYmd) * nt);
  add(s.trades.side, nt);
  add(s.trades.state, nt);
  add(s.trades.next_of_account, sizeof(uint32_t) * nt);

  const uint32_t ne = s.exception_count < s.exception_capacity ? s.exception_count
                                                               : s.exception_capacity;
  add(s.exceptions, sizeof(core::ExceptionRecord) * ne);

  const uint64_t escalares[] = {s.custody_checksum(),
                                s.cash_checksum(),
                                s.business_date.v,
                                s.prev_business_date.v,
                                s.window.dates[0].v,
                                s.window.dates[1].v,
                                s.window.dates[2].v,
                                s.flags,
                                s.exception_count,
                                s.trades.count,
                                s.custody.count,
                                s.cash.count,
                                s.instruments.count,
                                s.applied_actions.size()};
  add(escalares, sizeof escalares);
  return c;
}

// Uma partição isolada, com arena própria — para poder ter várias no mesmo teste.
struct Particao {
  static constexpr size_t kBytes = 96u << 20;
  std::unique_ptr<std::byte[]> memoria{new std::byte[kBytes]};
  Arena arena{memoria.get(), kBytes};
  core::PartitionState estado;
  core::Outbox outbox;
  Metrics metricas;
  core::ApplyContext ctx{nullptr, nullptr};
  uint64_t lsn = 0;

  static core::PartitionCapacity capacidade() {
    core::PartitionCapacity c{};
    c.accounts = 256;
    c.instruments = 32;
    c.positions = 2048;
    c.trades = 8192;
    c.corporate_actions_per_gen = 512;
    c.exceptions = 512;
    c.outbox_slots = 256;
    c.outbox_payload_bytes = 1 << 16;
    return c;
  }

  // `inicializa = false` deixa a arena INTACTA: é o que a recuperação precisa, porque
  // `load_state_image` faz o próprio `init` e depende de a arena estar virgem para reproduzir
  // exatamente os mesmos deslocamentos. Inicializar duas vezes na mesma arena alocaria em cima
  // e a imagem não caberia — que foi o que este teste pegou na primeira execução.
  explicit Particao(bool inicializa = true) {
    if (!inicializa) return;
    EXPECT_TRUE(estado.init(arena, PartitionId{0}, capacidade()));
    EXPECT_TRUE(outbox.init(arena, capacidade().outbox_slots, capacidade().outbox_payload_bytes));
    ctx = core::ApplyContext{&outbox, &metricas};
  }

  Status aplica(const Evento& e, uint64_t ts) {
    alignas(8) static thread_local std::byte buf[core::kMaxIngressPayload];
    std::memcpy(buf, e.bytes, e.len);
    const core::EventView ev{Lsn{++lsn}, ts, buf, e.tmpl, e.len, 0};
    return core::apply(estado, ev, ctx);
  }
};

}  // namespace

TEST(I11, ReplayDoZeroReproduzOMesmoEstado) {
  const auto eventos = gera_sessao(20260902, 400);
  Particao a, b;
  for (size_t i = 0; i < eventos.size(); ++i) {
    (void)a.aplica(eventos[i], 1000 + i);
    (void)b.aplica(eventos[i], 1000 + i);
  }
  EXPECT_EQ(impressao(a.estado), impressao(b.estado));
  // A sessão tem 400 negócios; parte é rejeitada de propósito (venda sem posição, data fora da
  // janela). O que o teste exige é que sobre motor de verdade dos dois lados — e que haja
  // rejeição, porque replay que só reproduz aceitação não prova nada sobre o caso difícil.
  EXPECT_GT(a.estado.trades.count, 200u) << "a sessão precisa exercitar o motor de verdade";
  EXPECT_GT(a.metricas.apply_rejected, 20u) << "e precisa conter eventos REJEITADOS";
  EXPECT_EQ(a.metricas.apply_rejected, b.metricas.apply_rejected)
      << "o replay tem de rejeitar os MESMOS eventos";
}

TEST(I12, TimestampNaoInfluenciaOEstado) {
  // Mesmos eventos, `ts_ns` completamente diferentes. Se um único caminho do `apply` olhasse o
  // relógio de auditoria, este teste falharia — é a forma mais direta de afirmar D2.
  const auto eventos = gera_sessao(777, 250);
  Particao a, b;
  Lcg r{42};
  for (size_t i = 0; i < eventos.size(); ++i) {
    (void)a.aplica(eventos[i], 1);
    (void)b.aplica(eventos[i], r.next());
  }
  EXPECT_EQ(impressao(a.estado), impressao(b.estado));
}

TEST(I11, ImagemEmKMaisReplayEquivaleAExecucaoInteira) {
  const auto eventos = gera_sessao(31415, 300);
  Particao referencia;
  for (size_t i = 0; i < eventos.size(); ++i) (void)referencia.aplica(eventos[i], 1000 + i);
  const uint32_t alvo = impressao(referencia.estado);

  // Para VÁRIOS k, e não um só: o k interessante é o que cai no meio de uma sequência
  // correlacionada (entre o negócio e sua alocação, entre o corporativo e sua duplicata).
  for (size_t k : {size_t{1}, size_t{7}, eventos.size() / 3, eventos.size() / 2,
                   eventos.size() - 10, eventos.size()}) {
    Particao meio;
    for (size_t i = 0; i < k; ++i) (void)meio.aplica(eventos[i], 1000 + i);

    std::vector<std::byte> imagem(core::state_image_bytes(meio.estado.arena_bytes));
    uint64_t escrito = 0;
    ASSERT_TRUE(core::save_state_image(meio.estado, meio.arena.base(), MutBytes{imagem}, &escrito)
                    .is_ok())
        << "k=" << k;
    EXPECT_EQ(escrito, imagem.size());

    // A recuperação usa uma arena NOVA, em outro endereço. É o que prova que a imagem não
    // depende de onde a memória estava.
    Particao recuperada{false};
    ASSERT_TRUE(core::load_state_image(recuperada.estado, recuperada.arena, ByteSpan{imagem})
                    .is_ok())
        << "k=" << k;
    ASSERT_TRUE(recuperada.outbox.init(recuperada.arena, 256, 1 << 16));
    recuperada.ctx = core::ApplyContext{&recuperada.outbox, &recuperada.metricas};
    recuperada.lsn = k;

    EXPECT_EQ(impressao(meio.estado), impressao(recuperada.estado))
        << "a imagem em k=" << k << " não restaurou o estado exato";

    for (size_t i = k; i < eventos.size(); ++i) (void)recuperada.aplica(eventos[i], 1000 + i);
    EXPECT_EQ(impressao(recuperada.estado), alvo) << "I11 falhou em k=" << k;
  }
}

TEST(I11, ImagemRecusaConfiguracaoDiferente) {
  // Restaurar numa partição dimensionada de outro jeito mudaria os deslocamentos dentro da arena
  // e produziria estado silenciosamente errado. A recusa tem de ser explícita.
  Particao p;
  const auto eventos = gera_sessao(1, 20);
  for (size_t i = 0; i < eventos.size(); ++i) (void)p.aplica(eventos[i], i);

  std::vector<std::byte> imagem(core::state_image_bytes(p.estado.arena_bytes));
  ASSERT_TRUE(core::save_state_image(p.estado, p.arena.base(), MutBytes{imagem}, nullptr).is_ok());

  auto* h = reinterpret_cast<core::StateImageHeader*>(imagem.data());
  h->cap.trades *= 2;  // configuração diferente, CRC do cabeçalho agora inválido
  Particao outra{false};
  const Status st = core::load_state_image(outra.estado, outra.arena, ByteSpan{imagem});
  EXPECT_TRUE(st.is_error());
  EXPECT_EQ(st.code(), Err::BadCrc);
}

TEST(I11, ImagemDetectaCorrupcaoDeCabecalho) {
  Particao p;
  std::vector<std::byte> imagem(core::state_image_bytes(p.estado.arena_bytes));
  ASSERT_TRUE(core::save_state_image(p.estado, p.arena.base(), MutBytes{imagem}, nullptr).is_ok());
  imagem[8] ^= std::byte{0xFF};
  Particao outra{false};
  EXPECT_EQ(core::load_state_image(outra.estado, outra.arena, ByteSpan{imagem}).code(),
            Err::BadCrc);
}
