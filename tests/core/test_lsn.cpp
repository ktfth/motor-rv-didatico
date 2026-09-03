// I8 — `lsn` estritamente crescente por partição, sem lacunas no log válido.
// I9 — `durable_lsn <= last_lsn`, e avança APENAS em ordem FIFO de grupos.
//
// Este arquivo existe porque os dois invariantes estavam rotulados em `tests/wal/test_format.cpp`,
// que verifica o LAYOUT dos registros e não afirma nada sobre LSN. Uma revisão independente
// apontou o buraco: `scripts/check_invariants.py` conferia que existia um teste com o rótulo, e o
// rótulo era auto-declarado no CMakeLists. Rótulo não é verificação.
//
// O escritor do WAL ainda não existe (é a próxima fase). Quem atribui LSN hoje é o
// `MemoryJournal`, e é sobre ele que I8 e I9 são afirmados — inclusive porque ele permite ao teste
// decidir EXATAMENTE quando `durable_lsn` avança, o que com io_uring de verdade dependeria de
// sorte. Quando o `Wal` existir, estes mesmos testes passam a rodar contra os dois.

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
  core::Inbox* entrada = nullptr;
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

TEST(I8, LsnCresceDeUmEmUmSemLacuna) {
  Bancada b;
  const auto eventos = gera_sessao(8080, 60);
  size_t n = 0;
  while (n < eventos.size() && b.empurra(eventos[n], 100 + n)) ++n;
  ASSERT_GT(n, 50u);
  b.particao->poll(1'000);

  const auto& regs = b.diario.records();
  ASSERT_EQ(regs.size(), n) << "todo evento entregue tem de estar no log, aceito ou rejeitado";
  for (size_t i = 0; i < regs.size(); ++i) {
    EXPECT_EQ(regs[i].lsn.v, i + 1) << "I8: sem lacuna e sem repetição, começando em 1";
    if (i > 0) {
      EXPECT_GT(regs[i].lsn.v, regs[i - 1].lsn.v) << "I8: estritamente crescente";
    }
  }
  EXPECT_EQ(b.diario.last_lsn().v, regs.size());
}

TEST(I8, EventoRejeitadoTambemRecebeLsn) {
  // O log é a verdade do que CHEGOU, não do que foi aceito. Se o rejeitado não entrasse, o replay
  // teria uma lacuna — e uma lacuna é indistinguível de corrupção na recuperação.
  Bancada b;
  ASSERT_TRUE(b.empurra(faz(dia(kD0, kD1, kD2)), 1));
  ASSERT_TRUE(b.empurra(faz(cadastro(kPetr4, "PETR4", 1, 3'000'000'000, kD0)), 2));
  // Venda sem posição e sem flag: será rejeitada pelo apply.
  ASSERT_TRUE(b.empurra(faz(negocio(1, kCpfA, kPetr4, codec::Side::Sell,
                                    Qty::from_units(100).raw(), 3'000'000'000, 0, kD2)), 3));
  b.particao->poll(1'000);

  EXPECT_EQ(b.metricas.apply_rejected, 1u);
  EXPECT_EQ(b.diario.records().size(), 3u) << "o rejeitado está no log";
  EXPECT_EQ(b.diario.last_lsn().v, 3u);
  EXPECT_EQ(b.estado.applied_lsn.v, 3u) << "applied_lsn avança mesmo na rejeição";
}

TEST(I9, DuravelNuncaPassaDoUltimo) {
  Bancada b;
  b.diario.set_auto_durable(false);
  const auto eventos = gera_sessao(9090, 40);
  size_t n = 0;
  while (n < eventos.size() && b.empurra(eventos[n], n)) ++n;

  for (int volta = 0; volta < 5; ++volta) {
    b.particao->poll(static_cast<uint64_t>(volta) * 1000);
    EXPECT_LE(b.diario.durable_lsn().v, b.diario.last_lsn().v) << "I9 na volta " << volta;
  }
  EXPECT_EQ(b.diario.durable_lsn().v, 0u) << "nada foi marcado durável pelo teste";

  b.diario.advance_durable(b.diario.last_lsn());
  b.particao->poll(9'000);
  EXPECT_EQ(b.diario.durable_lsn().v, b.diario.last_lsn().v);
}

TEST(I9, DuravelSoAvancaPeloPrefixo) {
  // A propriedade que I9 realmente afirma: `durable_lsn` é o maior LSN cujo grupo E TODOS OS
  // ANTERIORES completaram. Marcar o grupo 3 como completo enquanto o 1 não completou não pode
  // fazer `durable_lsn` chegar a 3 — é isso que impede o outbox de externalizar o que ainda pode
  // sumir num crash (I10).
  Bancada b;
  b.diario.set_auto_durable(false);
  const auto eventos = gera_sessao(1234, 30);
  size_t n = 0;
  while (n < eventos.size() && b.empurra(eventos[n], n)) ++n;
  b.particao->poll(1'000);
  const uint64_t ultimo = b.diario.last_lsn().v;
  ASSERT_GT(ultimo, 10u);

  // O `MemoryJournal` expõe o avanço como um prefixo justamente porque é isso que o WAL promete:
  // não existe API para marcar durável um LSN sem marcar os anteriores.
  uint64_t anterior = 0;
  for (uint64_t alvo : {ultimo / 4, ultimo / 2, ultimo}) {
    b.diario.advance_durable(Lsn{alvo});
    b.particao->poll(2'000);
    EXPECT_GE(b.diario.durable_lsn().v, anterior) << "durable_lsn é monótono";
    EXPECT_LE(b.diario.durable_lsn().v, ultimo);
    EXPECT_LE(b.particao->published(), b.diario.durable_lsn().v)
        << "I10: nada publicado além do durável";
    anterior = b.diario.durable_lsn().v;
  }
}

TEST(I9, FailStopCongelaOAvanco) {
  Bancada b;
  const auto eventos = gera_sessao(555, 20);
  size_t n = 0;
  while (n < eventos.size() && b.empurra(eventos[n], n)) ++n;
  b.particao->poll(1'000);
  const uint64_t antes = b.diario.last_lsn().v;

  b.diario.halt();
  b.particao->poll(2'000);
  EXPECT_EQ(b.diario.last_lsn().v, antes) << "depois do fail-stop nada mais entra no log";
  EXPECT_TRUE(b.diario.halted());
}
