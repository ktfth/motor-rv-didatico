// I10 — nenhuma saída é liberada com `lsn > durable_lsn`.
//
// O invariante é verificável porque existe UM portão: `Outbox::ready(durable)`. Este teste ataca
// o portão de três lados — a mecânica do próprio outbox, o loop da partição com `durable_lsn`
// controlado pelo teste, e o congelamento depois de um fail-stop.
//
// O `MemoryJournal` é o que torna isso possível: com io_uring de verdade, "segurar `durable_lsn`
// em 5 enquanto `last_lsn` chega a 20" dependeria de sorte.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <vector>

#include "cenario.hpp"
#include "core/partition.hpp"
#include "core/testing/memory_journal.hpp"
#include "engine_fixture.hpp"

using namespace rv;
using namespace rv::testing;
using rv::core::testing::MemoryJournal;

namespace {
struct Bancada {
  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> memoria{new std::byte[kBytes]};
  Arena arena{memoria.get(), kBytes};
  core::PartitionState estado;
  core::Outbox outbox;
  Metrics metricas;
  MemoryJournal diario;
  core::Inbox* entrada;
  std::unique_ptr<core::Partition<MemoryJournal>> particao;

  Bancada() {
    core::PartitionCapacity c{};
    c.accounts = 128;
    c.instruments = 32;
    c.positions = 512;
    c.trades = 4096;
    c.corporate_actions_per_gen = 256;
    c.exceptions = 128;
    EXPECT_TRUE(estado.init(arena, PartitionId{0}, c));
    EXPECT_TRUE(outbox.init(arena, 4096, 1 << 20));
    entrada = arena.emplace<core::Inbox>();
    EXPECT_NE(entrada, nullptr);
    arena.seal();
    particao = std::make_unique<core::Partition<MemoryJournal>>(estado, diario, *entrada, outbox,
                                                                metricas);
  }

  bool empurra(const Evento& e, uint64_t ts) {
    core::IngressFrame* f = entrada->claim();
    if (f == nullptr) return false;
    f->arrival_ts_ns = ts;
    f->tmpl = e.tmpl;
    f->len = e.len;
    std::memcpy(f->payload, e.bytes, e.len);
    entrada->publish();
    return true;
  }
};
}  // namespace

TEST(I10, OutboxSoLiberaAtePortaoDuravel) {
  Bancada b;
  for (uint64_t i = 1; i <= 20; ++i) {
    std::byte carga[64]{};
    ASSERT_TRUE(b.outbox.stage(Lsn{i}, core::OutKind::Confirmation, ByteSpan{carga, 64}).is_ok());
  }
  EXPECT_EQ(b.outbox.ready(Lsn{0}), 0u) << "nada durável, nada sai";
  EXPECT_EQ(b.outbox.ready(Lsn{7}), 7u);
  EXPECT_EQ(b.outbox.ready(Lsn{20}), 20u);
  EXPECT_EQ(b.outbox.ready(Lsn{1000}), 20u) << "não inventa saída que não existe";

  // Liberar em duas levas: o portão anda com `durable_lsn`, nunca à frente.
  b.outbox.commit(b.outbox.ready(Lsn{7}));
  EXPECT_EQ(b.outbox.pending(), 13u);
  EXPECT_EQ(b.outbox.ready(Lsn{7}), 0u) << "o que já saiu não sai de novo";
  EXPECT_EQ(b.outbox.ready(Lsn{13}), 6u);
}

TEST(I10, LoopNaoPublicaAlemDoDuravel) {
  Bancada b;
  b.diario.set_auto_durable(false);  // o teste é quem decide quando algo vira durável

  const auto eventos = gera_sessao(4242, 40);
  size_t enfileirados = 0;
  while (enfileirados < eventos.size() && b.empurra(eventos[enfileirados], 100 + enfileirados)) {
    ++enfileirados;
  }
  ASSERT_GT(enfileirados, 30u);

  b.particao->poll(1'000);
  EXPECT_GT(b.diario.last_lsn().v, 30u) << "o log andou";
  EXPECT_EQ(b.diario.durable_lsn().v, 0u) << "mas nada é durável ainda";
  EXPECT_EQ(b.particao->published(), 0u) << "logo, NADA foi publicado";

  // Libera metade e confirma que só a metade sai.
  const uint64_t metade = b.diario.last_lsn().v / 2;
  b.diario.advance_durable(Lsn{metade});
  b.particao->poll(2'000);
  const uint64_t publicado_ate_aqui = b.particao->published();
  EXPECT_LE(publicado_ate_aqui, metade);

  b.diario.advance_durable(b.diario.last_lsn());
  b.particao->poll(3'000);
  EXPECT_GE(b.particao->published(), publicado_ate_aqui);
  EXPECT_EQ(b.outbox.pending(), 0u);
}

TEST(I10, FailStopCongelaOOutboxParaSempre) {
  Bancada b;
  std::byte carga[32]{};
  ASSERT_TRUE(b.outbox.stage(Lsn{1}, core::OutKind::Audit, ByteSpan{carga, 32}).is_ok());
  ASSERT_EQ(b.outbox.ready(Lsn{10}), 1u);

  b.outbox.freeze();
  EXPECT_EQ(b.outbox.ready(Lsn{10}), 0u);
  EXPECT_TRUE(b.outbox.frozen());
  // E continua congelado mesmo empilhando mais: depois de um fail-stop, a saída que foi produzida
  // por um evento que corrompeu o estado NÃO pode ser externalizada.
  ASSERT_TRUE(b.outbox.stage(Lsn{2}, core::OutKind::Audit, ByteSpan{carga, 32}).is_ok());
  EXPECT_EQ(b.outbox.ready(Lsn{100}), 0u);
}

TEST(I10, ContrapressaoDoWalNaoConsomeEvento) {
  // `WalFull` é falha SEM EFEITO: o evento continua no ring e volta na próxima volta.
  Bancada b;
  const auto eventos = gera_sessao(9, 5);
  for (size_t i = 0; i < eventos.size(); ++i) ASSERT_TRUE(b.empurra(eventos[i], i));

  b.diario.set_full(true);
  const uint32_t aplicados = b.particao->poll(1'000);
  EXPECT_EQ(aplicados, 0u);
  EXPECT_EQ(b.diario.last_lsn().v, 0u) << "nada entrou no log";
  EXPECT_GT(b.metricas.wal_full, 0u);

  b.diario.set_full(false);
  const uint32_t depois = b.particao->poll(2'000);
  EXPECT_EQ(depois, eventos.size()) << "os MESMOS eventos, na mesma ordem";
}
