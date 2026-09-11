// Suíte de caos e testes de crash (tests/chaos/):
// Cauda rasgada, kill -9, truncamento, corrupção, recuperação a partir de snapshot.
//
// Invariantes verificados:
//   I8  — LSN estritamente crescente, sem lacuna no log recuperado
//   I9  — durabilidade avança estritamente por prefixo FIFO
//   I10 — outbox não libera nenhuma saída durante o replay
//   I11 — estado após recuperação == estado de execução direta
//   I12 — determinismo do replay

#include <filesystem>
#include <memory>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "base/arena.hpp"
#include "base/metrics.hpp"
#include "cenario.hpp"
#include "core/apply.hpp"
#include "core/outbox.hpp"
#include "core/partition.hpp"
#include "core/partition_state.hpp"
#include "engine_fixture.hpp"
#include "wal/pwrite_backend.hpp"
#include "wal/recovery.hpp"
#include "wal/segment_reader.hpp"
#include "wal/testing/fault_backend.hpp"
#include "wal/wal.hpp"

namespace rv::wal {
namespace {

namespace fs = std::filesystem;
using namespace rv::testing;

class DirTemp {
 public:
  DirTemp() {
    char molde[] = "motorrv_chaos_XXXXXX";
    const char* p = ::mkdtemp(molde);
    if (p != nullptr) dir_ = fs::absolute(p);
  }
  ~DirTemp() {
    std::error_code ec;
    if (!dir_.empty()) fs::remove_all(dir_, ec);
  }
  [[nodiscard]] const char* path() const { return dir_.c_str(); }
  [[nodiscard]] std::string arquivo(const char* nome) const { return (dir_ / nome).string(); }

 private:
  fs::path dir_;
};

core::PartitionCapacity capacidade_teste() {
  core::PartitionCapacity c{};
  c.accounts = 128;
  c.instruments = 32;
  c.positions = 512;
  c.trades = 4096;
  c.corporate_actions_per_gen = 256;
  c.exceptions = 128;
  c.outbox_slots = 256;
  c.outbox_payload_bytes = 64u << 10;
  return c;
}

// I8, I11, I12 — Recuperação limpa produz exatamente o mesmo estado
TEST(ChaosRecovery, ReplayDeterministaSemCrash) {
  DirTemp dt;
  PwriteBackend be;
  Wal wal{be};

  WalOptions opts{};
  opts.dir = dt.path();
  opts.partition = PartitionId{0};
  opts.block_size = 4096;
  ASSERT_TRUE(wal.open(opts).is_ok());

  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> mem1{new std::byte[kBytes]};
  Arena a1{mem1.get(), kBytes};

  core::PartitionState s1{};
  ASSERT_TRUE(s1.init(a1, PartitionId{0}, capacidade_teste()));

  core::Outbox outbox1{};
  ASSERT_TRUE(outbox1.init(a1, 256, 64u << 10));
  core::Inbox* entrada1 = a1.emplace<core::Inbox>();
  ASSERT_NE(entrada1, nullptr);
  a1.seal();

  Metrics m1{};
  core::Partition<Wal> part1{s1, wal, *entrada1, outbox1, m1};

  const auto eventos = gera_sessao(2026, 60);
  for (size_t i = 0; i < eventos.size(); ++i) {
    core::IngressFrame* f = nullptr;
    while ((f = entrada1->claim()) == nullptr) {
      part1.poll(1000 + i);
    }
    f->arrival_ts_ns = 1000 + i;
    f->tmpl = eventos[i].tmpl;
    f->len = eventos[i].len;
    std::memcpy(f->payload, eventos[i].bytes, eventos[i].len);
    entrada1->publish();
    part1.poll(1000 + i);
  }
  while (entrada1->peek() != nullptr) {
    part1.poll(200'000);
  }
  ASSERT_TRUE(part1.force_commit(300'000).is_ok());

  const uint64_t custody_chk = s1.custody_checksum();
  const uint64_t cash_chk = s1.cash_checksum();
  const uint32_t trades_cnt = s1.trades.count;
  const uint64_t last_applied_lsn = s1.applied_lsn.v;

  wal.close();

  // Agora simulamos reinício do processo com recuperação a partir do WAL (I11, I12)
  std::unique_ptr<std::byte[]> mem2{new std::byte[kBytes]};
  Arena a2{mem2.get(), kBytes};
  core::PartitionState s2{};
  ASSERT_TRUE(s2.init(a2, PartitionId{0}, capacidade_teste()));

  Metrics m2{};
  WalTail tail{};
  auto r_rec = recover(s2, a2, m2, dt.path(), PartitionId{0}, tail);
  ASSERT_TRUE(r_rec.is_ok());

  const RecoveryReport& rep = *r_rec;
  EXPECT_EQ(rep.stop_reason, ReplayStopReason::Clean);
  EXPECT_EQ(rep.last_valid_lsn.v, last_applied_lsn) << "I8: LSN final coincide sem lacunas";
  EXPECT_EQ(s2.applied_lsn.v, last_applied_lsn);
  EXPECT_EQ(s2.custody_checksum(), custody_chk) << "I11: reconciliação de custódia idêntica";
  EXPECT_EQ(s2.cash_checksum(), cash_chk) << "I11: financeiro idêntico";
  EXPECT_EQ(s2.trades.count, trades_cnt) << "I11: livro de negócios recuperado";
}

// I8, I10 — Cauda rasgada (torn write por crash no meio do bloco)
TEST(ChaosRecovery, CaudaRasgadaRecuperaAteUltimoValido) {
  DirTemp dt;
  PwriteBackend be;
  Wal wal{be};

  WalOptions opts{};
  opts.dir = dt.path();
  opts.partition = PartitionId{0};
  opts.block_size = 4096;
  ASSERT_TRUE(wal.open(opts).is_ok());

  // Gravamos 2 grupos de eventos válidos de sessão
  const auto eventos = gera_sessao(1234, 10);
  for (size_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(
        wal.append(eventos[i].tmpl, ByteSpan{eventos[i].bytes, eventos[i].len}, 1000 * (i + 1))
            .is_ok());
  }
  ASSERT_TRUE(wal.force_commit(10'000).is_ok());
  const uint64_t g1_end_lsn = wal.durable_lsn().v;
  EXPECT_EQ(g1_end_lsn, 5u);
  const uint64_t g1_offset = wal.segment_offset();

  for (size_t i = 5; i < 10; ++i) {
    ASSERT_TRUE(
        wal.append(eventos[i].tmpl, ByteSpan{eventos[i].bytes, eventos[i].len}, 1000 * (i + 1))
            .is_ok());
  }
  ASSERT_TRUE(wal.force_commit(20'000).is_ok());
  const uint64_t g2_end_lsn = wal.durable_lsn().v;
  EXPECT_EQ(g2_end_lsn, 10u);

  char seg_path[256]{};
  std::strncpy(seg_path, wal.segment().path(), sizeof(seg_path) - 1);
  wal.close();

  // Simulamos cauda rasgada truncando no meio do primeiro registro do segundo grupo
  int fd = ::open(seg_path, O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::ftruncate(fd, static_cast<off_t>(g1_offset + 30)), 0);
  ::close(fd);

  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> mem{new std::byte[kBytes]};
  Arena a{mem.get(), kBytes};
  core::PartitionState s{};
  ASSERT_TRUE(s.init(a, PartitionId{0}, capacidade_teste()));

  Metrics m{};
  WalTail tail{};
  auto r_rec = recover(s, a, m, dt.path(), PartitionId{0}, tail);
  ASSERT_TRUE(r_rec.is_ok());

  // A recuperação deve recuperar com segurança pelo menos o grupo 1 ou os registros íntegros
  EXPECT_GE(r_rec->last_valid_lsn.v, g1_end_lsn) << "I8: grupo 1 completo tem de ser recuperado";
  EXPECT_LE(r_rec->last_valid_lsn.v, g2_end_lsn);
  EXPECT_NE(r_rec->stop_reason, ReplayStopReason::Clean) << "detectou o corte na cauda";
}

// I11 — Snapshot EOD com recuperação subsequente
TEST(ChaosRecovery, SnapshotMaisReplayParcial) {
  DirTemp dt;
  PwriteBackend be;
  Wal wal{be};

  WalOptions opts{};
  opts.dir = dt.path();
  opts.partition = PartitionId{0};
  opts.block_size = 4096;
  ASSERT_TRUE(wal.open(opts).is_ok());

  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> mem1{new std::byte[kBytes]};
  Arena a1{mem1.get(), kBytes};

  core::PartitionState s1{};
  ASSERT_TRUE(s1.init(a1, PartitionId{0}, capacidade_teste()));

  core::Outbox outbox1{};
  ASSERT_TRUE(outbox1.init(a1, 256, 64u << 10));
  core::Inbox* entrada1 = a1.emplace<core::Inbox>();
  a1.seal();

  Metrics m1{};
  core::Partition<Wal> part1{s1, wal, *entrada1, outbox1, m1};

  const auto eventos = gera_sessao(777, 40);
  // Aplicamos os primeiros 20 eventos
  for (size_t i = 0; i < 20; ++i) {
    core::IngressFrame* f = nullptr;
    while ((f = entrada1->claim()) == nullptr) part1.poll(1000 + i);
    f->arrival_ts_ns = 1000 + i;
    f->tmpl = eventos[i].tmpl;
    f->len = eventos[i].len;
    std::memcpy(f->payload, eventos[i].bytes, eventos[i].len);
    entrada1->publish();
    part1.poll(1000 + i);
  }
  while (entrada1->peek() != nullptr) part1.poll(50'000);
  ASSERT_TRUE(part1.force_commit(60'000).is_ok());

  // Tiramos o snapshot no LSN 20
  const Lsn snap_lsn = s1.applied_lsn;
  ASSERT_TRUE(wal.snapshot(s1, snap_lsn).is_ok());

  // Aplicamos mais 20 eventos (21..40)
  for (size_t i = 20; i < 40; ++i) {
    core::IngressFrame* f = nullptr;
    while ((f = entrada1->claim()) == nullptr) part1.poll(1000 + i);
    f->arrival_ts_ns = 1000 + i;
    f->tmpl = eventos[i].tmpl;
    f->len = eventos[i].len;
    std::memcpy(f->payload, eventos[i].bytes, eventos[i].len);
    entrada1->publish();
    part1.poll(1000 + i);
  }
  while (entrada1->peek() != nullptr) part1.poll(150'000);
  ASSERT_TRUE(part1.force_commit(160'000).is_ok());

  const uint64_t target_custody_chk = s1.custody_checksum();
  const uint64_t target_applied_lsn = s1.applied_lsn.v;
  wal.close();

  // Recuperação a partir de snapshot + replay do prefixo 21..40 (I11)
  std::unique_ptr<std::byte[]> mem2{new std::byte[kBytes]};
  Arena a2{mem2.get(), kBytes};
  core::PartitionState s2{};

  Metrics m2{};
  WalTail tail{};
  auto r_rec = recover(s2, a2, m2, dt.path(), PartitionId{0}, tail);
  ASSERT_TRUE(r_rec.is_ok());
  EXPECT_EQ(r_rec->image_lsn.v, snap_lsn.v) << "carregou a imagem do snapshot no ponto de corte";
  EXPECT_EQ(s2.applied_lsn.v, target_applied_lsn);
  EXPECT_EQ(s2.custody_checksum(), target_custody_chk) << "I11: estado reconstituído é idêntico";
}

// I9, I10 — Falha de I/O em voo via FaultBackend causa halt imediato e impede liberação no outbox
TEST(ChaosRecovery, FaultBackendFalhaIoInterrompeDurabilidade) {
  DirTemp dt;
  PwriteBackend base_be;
  testing::FaultBackend fault_be{base_be};
  Wal wal{fault_be};

  WalOptions opts{};
  opts.dir = dt.path();
  opts.partition = PartitionId{0};
  opts.block_size = 4096;
  ASSERT_TRUE(wal.open(opts).is_ok());

  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> mem1{new std::byte[kBytes]};
  Arena a1{mem1.get(), kBytes};

  core::PartitionState s1{};
  ASSERT_TRUE(s1.init(a1, PartitionId{0}, capacidade_teste()));

  core::Outbox outbox1{};
  ASSERT_TRUE(outbox1.init(a1, 256, 64u << 10));
  core::Inbox* entrada1 = a1.emplace<core::Inbox>();
  a1.seal();

  Metrics m1{};
  core::Partition<Wal> part1{s1, wal, *entrada1, outbox1, m1};

  const auto eventos = gera_sessao(555, 20);
  // Aplica os primeiros 10 eventos com sucesso
  for (size_t i = 0; i < 10; ++i) {
    core::IngressFrame* f = nullptr;
    while ((f = entrada1->claim()) == nullptr) part1.poll(1000 + i);
    f->arrival_ts_ns = 1000 + i;
    f->tmpl = eventos[i].tmpl;
    f->len = eventos[i].len;
    std::memcpy(f->payload, eventos[i].bytes, eventos[i].len);
    entrada1->publish();
    part1.poll(1000 + i);
  }
  while (entrada1->peek() != nullptr) part1.poll(20'000);
  ASSERT_TRUE(wal.force_commit(300'000).is_ok());
  EXPECT_EQ(wal.durable_lsn().v, 10u);
  const uint64_t custody_10 = s1.custody_checksum();

  // Injeta erro no commit do próximo grupo (LSN 20)
  fault_be.inject_error(20, EIO);

  for (size_t i = 10; i < 20; ++i) {
    core::IngressFrame* f = nullptr;
    while ((f = entrada1->claim()) == nullptr) part1.poll(300'000 + i);
    f->arrival_ts_ns = 300'000 + i;
    f->tmpl = eventos[i].tmpl;
    f->len = eventos[i].len;
    std::memcpy(f->payload, eventos[i].bytes, eventos[i].len);
    entrada1->publish();
    part1.poll(300'000 + i);
  }
  while (entrada1->peek() != nullptr) part1.poll(500'000);
  // O force_commit deve falhar devido ao erro injetado
  EXPECT_TRUE(wal.force_commit(600'000).is_error());
  EXPECT_TRUE(wal.halted());
  EXPECT_EQ(wal.durable_lsn().v, 10u)
      << "I9: durabilidade congelada no último LSN com commit íntegro";

  // I10: o outbox só libera mensagens até o LSN durável (10)
  EXPECT_LE(outbox1.ready(wal.durable_lsn()), 10u) << "I10: saídas retidas";

  wal.close();

  // Na recuperação, o estado restaurado deve ser exatamente o do prefixo seguro (LSN 10)
  std::unique_ptr<std::byte[]> mem2{new std::byte[kBytes]};
  Arena a2{mem2.get(), kBytes};
  core::PartitionState s2{};
  ASSERT_TRUE(s2.init(a2, PartitionId{0}, capacidade_teste()));

  Metrics m2{};
  WalTail tail{};
  auto r_rec = recover(s2, a2, m2, dt.path(), PartitionId{0}, tail);
  ASSERT_TRUE(r_rec.is_ok());
  EXPECT_EQ(s2.applied_lsn.v, 10u);
  EXPECT_EQ(s2.custody_checksum(), custody_10)
      << "I11: recuperação isola falhas e recupera até LSN 10";
}

// I8, I12 — Corrupção de CRC32C no meio de segmento interrompe recuperação com BadCrc
TEST(ChaosRecovery, CorrupcaoCrcNoMeioDoSegmentoInterrompeReplay) {
  DirTemp dt;
  PwriteBackend be;
  Wal wal{be};

  WalOptions opts{};
  opts.dir = dt.path();
  opts.partition = PartitionId{0};
  opts.block_size = 4096;
  ASSERT_TRUE(wal.open(opts).is_ok());

  const auto eventos = gera_sessao(999, 20);
  for (size_t i = 0; i < 20; ++i) {
    ASSERT_TRUE(
        wal.append(eventos[i].tmpl, ByteSpan{eventos[i].bytes, eventos[i].len}, 1000 * (i + 1))
            .is_ok());
  }
  ASSERT_TRUE(wal.force_commit(30'000).is_ok());
  EXPECT_EQ(wal.durable_lsn().v, 20u);

  char seg_path[256]{};
  std::strncpy(seg_path, wal.segment().path(), sizeof(seg_path) - 1);
  wal.close();

  // Localiza o registro 10 varrendo os WalHdrs a partir de kSegmentHdrBytes
  int fd = ::open(seg_path, O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0);

  off_t alvo_offset = kSegmentHdrBytes;
  while (true) {
    WalHdr h{};
    if (::pread(fd, &h, sizeof(h), alvo_offset) != sizeof(h)) break;
    if (h.magic != kWalMagic) break;
    if (h.lsn == 10u) break;
    alvo_offset += static_cast<off_t>(sizeof(WalHdr) + h.len);
    alvo_offset = static_cast<off_t>((static_cast<uint64_t>(alvo_offset) + 7u) & ~7ULL);
  }
  ASSERT_GT(alvo_offset, static_cast<off_t>(kSegmentHdrBytes));

  // Corrompe 1 byte do payload do registro 10
  const off_t pos_corrupcao = alvo_offset + static_cast<off_t>(sizeof(WalHdr)) + 1;
  uint8_t b = 0;
  ASSERT_EQ(::pread(fd, &b, 1, pos_corrupcao), 1);
  b ^= 0xAA;
  ASSERT_EQ(::pwrite(fd, &b, 1, pos_corrupcao), 1);
  ::close(fd);

  // Agora executamos recover(): deve recuperar até LSN 9 e parar com BadCrc no LSN 10
  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> mem{new std::byte[kBytes]};
  Arena a{mem.get(), kBytes};
  core::PartitionState s{};
  ASSERT_TRUE(s.init(a, PartitionId{0}, capacidade_teste()));

  Metrics m{};
  WalTail tail{};
  auto r_rec = recover(s, a, m, dt.path(), PartitionId{0}, tail);
  ASSERT_TRUE(r_rec.is_ok());
  EXPECT_EQ(r_rec->last_valid_lsn.v, 9u) << "I8: para no último registro íntegro contíguo";
  EXPECT_EQ(r_rec->stop_reason, ReplayStopReason::BadCrc)
      << "identificou a violação de integridade por CRC";
  EXPECT_EQ(s.applied_lsn.v, 9u);
}

// I8 — Violação de contiguidade de LSN (salto) é rejeitada
TEST(ChaosRecovery, SaltoDeLsnRejeitadoDeterminante) {
  DirTemp dt;
  PwriteBackend be;
  Wal wal{be};

  WalOptions opts{};
  opts.dir = dt.path();
  opts.partition = PartitionId{0};
  opts.block_size = 4096;
  ASSERT_TRUE(wal.open(opts).is_ok());

  const auto eventos = gera_sessao(888, 15);
  for (size_t i = 0; i < 15; ++i) {
    ASSERT_TRUE(
        wal.append(eventos[i].tmpl, ByteSpan{eventos[i].bytes, eventos[i].len}, 1000 * (i + 1))
            .is_ok());
  }
  ASSERT_TRUE(wal.force_commit(20'000).is_ok());

  char seg_path[256]{};
  std::strncpy(seg_path, wal.segment().path(), sizeof(seg_path) - 1);
  wal.close();

  // Localiza o registro 8 e altera seu LSN para 12 (criando uma lacuna sem 8, 9, 10, 11)
  int fd = ::open(seg_path, O_RDWR | O_CLOEXEC);
  ASSERT_GE(fd, 0);

  off_t alvo_offset = kSegmentHdrBytes;
  while (true) {
    WalHdr h{};
    if (::pread(fd, &h, sizeof(h), alvo_offset) != sizeof(h)) break;
    if (h.magic != kWalMagic) break;
    if (h.lsn == 8u) break;
    alvo_offset += static_cast<off_t>(sizeof(WalHdr) + h.len);
    alvo_offset = static_cast<off_t>((static_cast<uint64_t>(alvo_offset) + 7u) & ~7ULL);
  }
  ASSERT_GT(alvo_offset, static_cast<off_t>(kSegmentHdrBytes));

  // Altera o LSN do cabeçalho
  WalHdr corrompido{};
  ASSERT_EQ(::pread(fd, &corrompido, sizeof(corrompido), alvo_offset),
            static_cast<ssize_t>(sizeof(corrompido)));
  corrompido.lsn = 12u;
  ASSERT_EQ(::pwrite(fd, &corrompido, sizeof(corrompido), alvo_offset),
            static_cast<ssize_t>(sizeof(corrompido)));
  ::close(fd);

  static constexpr size_t kBytes = 64u << 20;
  std::unique_ptr<std::byte[]> mem{new std::byte[kBytes]};
  Arena a{mem.get(), kBytes};
  core::PartitionState s{};
  ASSERT_TRUE(s.init(a, PartitionId{0}, capacidade_teste()));

  Metrics m{};
  WalTail tail{};
  auto r_rec = recover(s, a, m, dt.path(), PartitionId{0}, tail);
  ASSERT_TRUE(r_rec.is_ok());
  EXPECT_EQ(r_rec->last_valid_lsn.v, 7u)
      << "I8: recuperação para antes da lacuna / corrupção de LSN";
  EXPECT_EQ(s.applied_lsn.v, 7u);
}

}  // namespace
}  // namespace rv::wal
