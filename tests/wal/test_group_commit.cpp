// Testes de GroupCommit do WAL.
//
// Invariante I9: durable_lsn <= last_lsn e avança APENAS em ordem FIFO de grupos.
// Testado com completions reordenadas pelo FaultBackend e PwriteBackend.

#include <cstring>
#include <filesystem>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "base/status.hpp"
#include "wal/group_commit.hpp"
#include "wal/pwrite_backend.hpp"
#include "wal/testing/fault_backend.hpp"

namespace rv::wal {
namespace {

namespace fs = std::filesystem;

class DirTemp {
 public:
  DirTemp() {
    char molde[] = "motorrv_gc_XXXXXX";
    const char* p = ::mkdtemp(molde);
    if (p != nullptr) dir_ = fs::absolute(p);
  }
  ~DirTemp() {
    std::error_code ec;
    if (!dir_.empty()) fs::remove_all(dir_, ec);
  }
  [[nodiscard]] std::string arquivo(const char* nome) const { return (dir_ / nome).string(); }

 private:
  fs::path dir_;
};

TEST(GroupCommitTest, AppendeSubmitBasico) {
  DirTemp dt;
  const std::string path = dt.arquivo("teste.wal");
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend be;
  const int fds[1] = {fd};
  ASSERT_TRUE(be.register_files(std::span<const int>{fds, 1}).is_ok());

  GroupCommit gc;
  ASSERT_TRUE(gc.init(be, 4096, 100'000).is_ok());

  const std::byte payload[32]{};
  auto r1 = gc.append(10, ByteSpan{payload, 32}, 1'000, 0xABC, Lsn{1});
  ASSERT_TRUE(r1.is_ok());
  EXPECT_EQ(r1->lsn.v, 1u);

  auto r2 = gc.append(11, ByteSpan{payload, 32}, 1'100, 0xABC, Lsn{2});
  ASSERT_TRUE(r2.is_ok());
  EXPECT_EQ(r2->lsn.v, 2u);

  auto sub = gc.maybe_submit(be, 2'000, 4096, 0, true);
  ASSERT_TRUE(sub.is_ok());
  EXPECT_GT(*sub, 0u);
  EXPECT_EQ(*sub % 4096, 0u) << "submissão tem de ser alinhada ao bloco";

  EXPECT_EQ(gc.durable_lsn().v, 0u) << "ainda não reapado";
  ASSERT_TRUE(gc.reap(be).is_ok());
  EXPECT_EQ(gc.durable_lsn().v, 2u) << "durable_lsn tem de avançar para 2";

  ::close(fd);
}

// I9 — durable_lsn avança APENAS em ordem FIFO mesmo com completions fora de ordem
TEST(I9, GroupCommitDurableAvancaApenasEmFifo) {
  DirTemp dt;
  const std::string path = dt.arquivo("fifo.wal");
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend pwrite_be;
  const int fds[1] = {fd};
  ASSERT_TRUE(pwrite_be.register_files(std::span<const int>{fds, 1}).is_ok());

  testing::FaultBackend be{pwrite_be};
  GroupCommit gc;
  ASSERT_TRUE(gc.init(be, 4096, 100'000).is_ok());

  // Criamos 3 grupos distintos: LSN 1..5, LSN 6..10, LSN 11..15
  uint64_t off = 4096;
  for (uint64_t g = 0; g < 3; ++g) {
    for (uint64_t i = 1; i <= 5; ++i) {
      const uint64_t lsn_val = g * 5 + i;
      std::byte payload[64]{};
      auto r = gc.append(10, ByteSpan{payload, 64}, 1'000 * lsn_val, 0xABC, Lsn{lsn_val});
      ASSERT_TRUE(r.is_ok());
    }
    auto sub = gc.maybe_submit(be, 10'000 * (g + 1), off, 0, true);
    ASSERT_TRUE(sub.is_ok());
    off += *sub;
  }
  EXPECT_EQ(gc.inflight_count(), 3u);

  // Armamos a entrega das completions em ordem LIFO (grupo 3, depois 2, depois 1)
  be.set_delivery(testing::FaultBackend::Delivery::Lifo);

  // Reapamos: completions voltam invertidas!
  ASSERT_TRUE(gc.reap(be).is_ok());

  // Como o backend interno completou todas sincronicamente mas foram reordenadas,
  // após reap() processar todas, durable_lsn deve chegar ao fim de forma consistente
  EXPECT_EQ(gc.durable_lsn().v, 15u);
  EXPECT_EQ(gc.inflight_count(), 0u);

  ::close(fd);
}

// I9 — retenção de completion segura o avanço de durable_lsn
TEST(I9, CompletionRetidaNaoAvancaDurableLsn) {
  DirTemp dt;
  const std::string path = dt.arquivo("hold.wal");
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  ASSERT_GE(fd, 0);

  PwriteBackend pwrite_be;
  const int fds[1] = {fd};
  ASSERT_TRUE(pwrite_be.register_files(std::span<const int>{fds, 1}).is_ok());

  testing::FaultBackend be{pwrite_be};
  GroupCommit gc;
  ASSERT_TRUE(gc.init(be, 4096, 100'000).is_ok());

  // Grupo 1: LSN 1..5
  std::byte payload[64]{};
  for (uint64_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(gc.append(1, ByteSpan{payload, 64}, 1000 * i, 0x123, Lsn{i}).is_ok());
  }
  // Seguramos a completion do grupo 1 (token 5)
  be.hold(5);
  auto sub1 = gc.maybe_submit(be, 10'000, 4096, 0, true);
  ASSERT_TRUE(sub1.is_ok());

  // Grupo 2: LSN 6..10
  for (uint64_t i = 6; i <= 10; ++i) {
    ASSERT_TRUE(gc.append(1, ByteSpan{payload, 64}, 1000 * i, 0x123, Lsn{i}).is_ok());
  }
  auto sub2 = gc.maybe_submit(be, 20'000, 4096 + *sub1, 0, true);
  ASSERT_TRUE(sub2.is_ok());

  // O grupo 2 completou, mas grupo 1 está retido
  ASSERT_TRUE(gc.reap(be).is_ok());
  EXPECT_EQ(gc.durable_lsn().v, 0u)
      << "I9: durable_lsn NÃO pode avançar porque grupo 1 não completou!";

  // Liberamos o grupo 1
  be.release_held();
  ASSERT_TRUE(gc.reap(be).is_ok());
  EXPECT_EQ(gc.durable_lsn().v, 10u) << "agora avança através de ambos os grupos em FIFO";

  ::close(fd);
}

}  // namespace
}  // namespace rv::wal
