// Os backends de I/O do WAL contra o dispositivo de verdade.
//
// ---------------------------------------------------------------------------------------------
// O QUE ESTAS SÉRIES SÃO — E O QUE ELAS NÃO SÃO
//
// Elas NÃO são `wal.append_para_duravel_us`. Essa métrica mede do `append` de um evento até o
// grupo que o contém ficar durável, e para existir precisa do escritor do WAL (`segment.cpp`,
// `group_commit.cpp`, `wal.cpp`), que ainda não foi escrito. Publicar o número daqui com aquele
// nome seria a pior espécie de mentira de baseline: um número certo com o rótulo errado.
//
// O que elas são: o PISO FÍSICO. Uma escrita alinhada de N bytes, submetida e colhida, com
// `O_DIRECT | O_DSYNC` quando o filesystem aceita. Nenhum group commit pode ficar abaixo disso,
// e é contra isso que o group commit será julgado quando existir — se ele entregar 64 KiB em
// aproximadamente o tempo de uma escrita de 64 KiB, ele não custa nada; se entregar em muito
// mais, o custo está no motor e não no disco.
//
// A comparação entre 4 KiB e 64 KiB é a que decide o desenho: se 64 KiB custa quase o mesmo que
// 4 KiB, agrupar dez mil eventos numa escrita é de graça — que é a hipótese inteira do group
// commit (docs/wal.md, ADR-0012).
//
// `O_DIRECT` pode não existir no filesystem em que o harness roda (overlay de contêiner, tmpfs).
// Quando não existe, a série continua sendo medida — com `O_DSYNC` apenas — e a `nota` diz isso.
// Um número medido através do page cache não é comparável com um medido sem ele, e essa
// diferença tem de viajar junto com o número, não na memória de quem rodou.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/arena.hpp"
#include "base/metrics.hpp"
#include "bench/contrato.hpp"
#include "bench/harness.hpp"
#include "bench/nucleo_bench.hpp"
#include "bench/suites.hpp"
#include "core/apply.hpp"
#include "core/partition_state.hpp"
#include "wal/io_backend.hpp"
#include "wal/io_uring_backend.hpp"
#include "wal/pwrite_backend.hpp"
#include "wal/recovery.hpp"
#include "wal/wal.hpp"

namespace rv::bench {
namespace {

constexpr uint64_t kArquivoBytes = 256ULL << 20;  // 256 MiB: o segmento roda em círculo aqui
constexpr uint32_t kEscritasPorRepeticao = 400;

// Um arquivo de medição que se apaga sozinho. Sem isto, uma execução interrompida deixa 256 MiB
// no disco de quem rodou — e a próxima medição mede um filesystem mais cheio.
class ArquivoTemporario {
 public:
  explicit ArquivoTemporario(const std::string& dir) : caminho_(dir + "/motor-rv-bench-wal.tmp") {}
  ~ArquivoTemporario() {
    if (fd_ >= 0) (void)::close(fd_);
    if (!caminho_.empty()) (void)::unlink(caminho_.c_str());
  }
  ArquivoTemporario(const ArquivoTemporario&) = delete;
  ArquivoTemporario& operator=(const ArquivoTemporario&) = delete;
  ArquivoTemporario(ArquivoTemporario&&) = delete;
  ArquivoTemporario& operator=(ArquivoTemporario&&) = delete;

  // Tenta O_DIRECT primeiro; sem ele, O_DSYNC sozinho. `direto()` diz o que aconteceu.
  [[nodiscard]] bool abre(std::string& erro) {
    const int base = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_DSYNC;
    fd_ = ::open(caminho_.c_str(), base | O_DIRECT, 0644);
    direto_ = fd_ >= 0;
    if (fd_ < 0) fd_ = ::open(caminho_.c_str(), base, 0644);
    if (fd_ < 0) {
      // `errno` cru, e não `strerror`: `strerror` não é seguro entre threads (o `.clang-tidy`
      // deste projeto trata `concurrency-*` como erro) e `strerror_r` tem duas assinaturas
      // incompatíveis entre libcs. O número é inequívoco e cabe no relatório.
      erro = "não consegui abrir " + caminho_ + " (errno=" + std::to_string(errno) + ")";
      return false;
    }
    // Pré-aloca: sem isto, a primeira escrita de cada extent paga alocação do filesystem, e o
    // aquecimento do harness não alcança as 256 MiB inteiras.
    if (::ftruncate(fd_, static_cast<off_t>(kArquivoBytes)) != 0) {
      erro = "ftruncate falhou (errno=" + std::to_string(errno) + ")";
      return false;
    }
    return true;
  }

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] bool direto() const noexcept { return direto_; }

 private:
  std::string caminho_;
  int fd_ = -1;
  bool direto_ = false;
};

// Buffer alinhado ao bloco: `O_DIRECT` recusa qualquer outra coisa, e recusar é o comportamento
// certo — corrigir aqui esconderia o bug que O_DIRECT existe para expor.
class BufferAlinhado {
 public:
  explicit BufferAlinhado(size_t bytes) : bytes_(bytes) {
    if (::posix_memalign(&p_, 4096, bytes) != 0) p_ = nullptr;
    if (p_ != nullptr) std::memset(p_, 0x5A, bytes);
  }
  ~BufferAlinhado() { std::free(p_); }
  BufferAlinhado(const BufferAlinhado&) = delete;
  BufferAlinhado& operator=(const BufferAlinhado&) = delete;
  BufferAlinhado(BufferAlinhado&&) = delete;
  BufferAlinhado& operator=(BufferAlinhado&&) = delete;

  [[nodiscard]] std::byte* dados() const noexcept { return static_cast<std::byte*>(p_); }
  [[nodiscard]] size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] bool ok() const noexcept { return p_ != nullptr; }

 private:
  void* p_ = nullptr;
  size_t bytes_;
};

// Uma escrita submetida e colhida, cronometrada. É o ciclo que o group commit vai executar por
// grupo: `submit` e depois `reap` até a completion daquele token voltar.
[[nodiscard]] bool escreve_e_colhe(wal::IoBackend& be, const wal::WriteRequest& req) {
  if (be.submit(req).is_error()) return false;
  wal::Completion out[wal::kMaxInflight];
  for (uint32_t voltas = 0; voltas < 1'000'000; ++voltas) {
    const uint32_t n = be.reap(std::span<wal::Completion>{out, wal::kMaxInflight});
    for (uint32_t i = 0; i < n; ++i) {
      if (out[i].token == req.token) return out[i].ok();
    }
  }
  return false;
}

struct Cenario {
  const char* nome_pwrite;
  const char* nome_uring;
  uint32_t bytes;
};

constexpr Cenario kCenarios[] = {
    {"wal.pwrite_4KiB.commit", "wal.uring_4KiB.commit", 4u << 10},
    {"wal.pwrite_64KiB.commit", "wal.uring_64KiB.commit", 64u << 10},
    {"wal.pwrite_256KiB.commit", "wal.uring_256KiB.commit", 256u << 10},
};

}  // namespace

void registra_wal(Runner& r, const Carga& carga, const std::string& dir) {
  ArquivoTemporario arq{dir};
  std::string erro;
  if (!arq.abre(erro)) {
    for (const Cenario& c : kCenarios) {
      (void)r.pula("wal", c.nome_pwrite, Forma::DuracaoUs, erro);
      (void)r.pula("wal", c.nome_uring, Forma::DuracaoUs, erro);
    }
    (void)r.pula("wal", kSerieWalAppendParaDuravel, Forma::DuracaoUs, erro);
    (void)r.pula("wal", kSerieWalRecuperacao, Forma::DuracaoS, erro);
    return;
  }
  const std::string nota_direto =
      arq.direto() ? std::string("O_DIRECT|O_DSYNC")
                   : std::string(
                         "SEM O_DIRECT (o filesystem recusou): a escrita passa pelo page "
                         "cache e o número NÃO é comparável com um medido em O_DIRECT");

  BufferAlinhado buf{1u << 20};
  if (!buf.ok()) {
    for (const Cenario& c : kCenarios) {
      (void)r.pula("wal", c.nome_pwrite, Forma::DuracaoUs, "posix_memalign falhou");
      (void)r.pula("wal", c.nome_uring, Forma::DuracaoUs, "posix_memalign falhou");
    }
    (void)r.pula("wal", kSerieWalAppendParaDuravel, Forma::DuracaoUs, "posix_memalign falhou");
    (void)r.pula("wal", kSerieWalRecuperacao, Forma::DuracaoS, "posix_memalign falhou");
    return;
  }

  const int fds[1] = {arq.fd()};
  const MutBytes bufs[1] = {MutBytes{buf.dados(), buf.bytes()}};

  // ------------------------------------------------------------------ pwrite
  {
    wal::PwriteBackend be;
    const bool pronto = be.register_files(std::span<const int>{fds, 1}).is_ok() &&
                        be.register_buffers(std::span<const MutBytes>{bufs, 1}).is_ok();
    for (const Cenario& c : kCenarios) {
      if (!pronto) {
        (void)r.pula("wal", c.nome_pwrite, Forma::DuracaoUs, "registro de arquivo/buffer falhou");
        continue;
      }
      (void)r.medir_latencia("wal", c.nome_pwrite, Forma::DuracaoUs, [&be, &buf, &c](Histogram& h) {
        uint64_t off = 0;
        uint64_t n = 0;
        const uint64_t t0 = agora_ns();
        for (uint32_t i = 0; i < kEscritasPorRepeticao; ++i) {
          wal::WriteRequest req{};
          req.buf = buf.dados();
          req.offset = off;
          req.token = i + 1;
          req.len = c.bytes;
          req.buf_idx = 0;
          req.file_idx = 0;
          const uint64_t a0 = agora_ns();
          if (!escreve_e_colhe(be, req)) break;
          h.record(agora_ns() - a0);
          off += c.bytes;
          if (off + c.bytes > kArquivoBytes) off = 0;
          ++n;
        }
        return Amostra{n, agora_ns() - t0};
      });
      r.anota(nota_direto);
    }
  }

  // ------------------------------------------------------------------ io_uring
  {
    wal::UringBackend be;
    if (be.open().is_error()) {
      for (const Cenario& c : kCenarios) {
        (void)r.pula("wal", c.nome_uring, Forma::DuracaoUs,
                     "io_uring indisponível nesta máquina (kernel, seccomp ou contêiner)");
      }
    } else {
      const bool pronto = be.register_files(std::span<const int>{fds, 1}).is_ok() &&
                          be.register_buffers(std::span<const MutBytes>{bufs, 1}).is_ok();
      const std::string nota_ring =
          nota_direto + (be.defer_taskrun() ? ", DEFER_TASKRUN=on" : ", DEFER_TASKRUN=off") +
          (be.single_issuer() ? ", SINGLE_ISSUER=on" : ", SINGLE_ISSUER=off");

      for (const Cenario& c : kCenarios) {
        if (!pronto) {
          (void)r.pula("wal", c.nome_uring, Forma::DuracaoUs,
                       "registro de arquivo/buffer no ring falhou");
          continue;
        }
        (void)r.medir_latencia("wal", c.nome_uring, Forma::DuracaoUs,
                               [&be, &buf, &c](Histogram& h) {
                                 uint64_t off = 0;
                                 uint64_t n = 0;
                                 const uint64_t t0 = agora_ns();
                                 for (uint32_t i = 0; i < kEscritasPorRepeticao; ++i) {
                                   wal::WriteRequest req{};
                                   req.buf = buf.dados();
                                   req.offset = off;
                                   req.token = i + 1;
                                   req.len = c.bytes;
                                   req.buf_idx = 0;
                                   req.file_idx = 0;
                                   const uint64_t a0 = agora_ns();
                                   if (!escreve_e_colhe(be, req)) break;
                                   h.record(agora_ns() - a0);
                                   off += c.bytes;
                                   if (off + c.bytes > kArquivoBytes) off = 0;
                                   ++n;
                                 }
                                 return Amostra{n, agora_ns() - t0};
                               });
        r.anota(nota_ring);
      }
    }
  }

  // ------------------------------------------------------------------ WAL de alto nível
  if (carga.eventos.empty()) {
    (void)r.pula("wal", kSerieWalAppendParaDuravel, Forma::DuracaoUs, "carga vazia");
    (void)r.pula("wal", kSerieWalRecuperacao, Forma::DuracaoS, "carga vazia");
    return;
  }

  // 1. Latência append -> durável com group commit
  const std::string dir_append = dir + "/motor-rv-bench-wal-append";
  std::error_code ec;
  std::filesystem::remove_all(dir_append, ec);
  std::filesystem::create_directories(dir_append, ec);

  (void)r.medir_latencia("wal", kSerieWalAppendParaDuravel, Forma::DuracaoUs, [&](Histogram& h) {
    std::filesystem::remove_all(dir_append, ec);
    std::filesystem::create_directories(dir_append, ec);

    wal::PwriteBackend be;
    wal::Wal wal{be};
    wal::WalOptions opts{};
    opts.dir = dir_append.c_str();
    opts.partition = PartitionId{0};
    opts.block_size = 4096;
    opts.direct = arq.direto();
    opts.window_ns = 100'000;

    if (wal.open(opts).is_error()) {
      return Amostra{0, 0};
    }

    uint64_t n_eventos = 0;
    const uint64_t t0 = agora_ns();
    constexpr uint32_t kLoteRajada = 16;
    constexpr uint32_t kTotalLotes = 25;  // 400 eventos
    const size_t total_carga = carga.eventos.size();
    uint64_t t_evs[kLoteRajada]{};

    for (uint32_t lote = 0; lote < kTotalLotes; ++lote) {
      for (uint32_t i = 0; i < kLoteRajada; ++i) {
        const size_t idx = (lote * kLoteRajada + i) % total_carga;
        const auto& ev = carga.eventos[idx];
        t_evs[i] = agora_ns();
        auto app = wal.append(ev.tmpl, ByteSpan{ev.bytes, ev.len}, t_evs[i]);
        if (!app.is_ok()) break;
        ++n_eventos;
      }
      if (wal.force_commit(agora_ns()).is_error()) break;
      const uint64_t t_duravel = agora_ns();
      for (uint32_t i = 0; i < kLoteRajada; ++i) {
        h.record(t_duravel - t_evs[i]);
      }
    }

    wal.close();
    return Amostra{n_eventos, agora_ns() - t0};
  });
  r.anota(std::string("append -> durável com group commit (") +
          (arq.direto() ? "O_DIRECT" : "page cache") + ")");
  std::filesystem::remove_all(dir_append, ec);

  // 2. Tempo de recuperação a partir de log pré-gravado com a carga
  const std::string dir_rec = dir + "/motor-rv-bench-wal-rec";
  std::filesystem::remove_all(dir_rec, ec);
  std::filesystem::create_directories(dir_rec, ec);

  bool log_preparado = false;
  {
    wal::PwriteBackend be;
    wal::Wal wal{be};
    wal::WalOptions opts{};
    opts.dir = dir_rec.c_str();
    opts.partition = PartitionId{0};
    opts.block_size = 4096;
    opts.direct = arq.direto();
    opts.window_ns = 100'000;
    if (wal.open(opts).is_ok()) {
      const size_t limite = carga.eventos.size();
      for (size_t i = 0; i < limite; ++i) {
        const auto& ev = carga.eventos[i];
        (void)wal.append(ev.tmpl, ByteSpan{ev.bytes, ev.len}, agora_ns());
      }
      (void)wal.force_commit(agora_ns());
      wal.close();
      log_preparado = true;
    }
  }

  if (!log_preparado) {
    (void)r.pula("wal", kSerieWalRecuperacao, Forma::DuracaoS,
                 "falha ao preparar log de recuperação");
  } else {
    auto mem_rec = std::make_shared<std::vector<std::byte>>(kArenaDeMedicao);
    const core::PartitionCapacity cap = capacidade_de_medicao();

    (void)r.medir("wal", kSerieWalRecuperacao, Forma::DuracaoS, [&] {
      Arena arena{mem_rec->data(), mem_rec->size()};
      core::PartitionState state{};
      if (!state.init(arena, PartitionId{0}, cap)) return Amostra{0, 0};

      wal::WalTail tail{};
      Metrics m{};
      const uint64_t t0 = agora_ns();
      auto res = wal::recover(state, arena, m, dir_rec.c_str(), PartitionId{0}, tail);
      const uint64_t dt = agora_ns() - t0;
      return (res.is_ok() && res->records_applied > 0) ? Amostra{1, dt} : Amostra{0, dt};
    });
    r.anota("tempo para recover reler log e reproduzir o estado da partição");
  }
  std::filesystem::remove_all(dir_rec, ec);
}

}  // namespace rv::bench
