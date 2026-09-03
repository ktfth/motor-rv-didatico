#include "wal/io_uring_backend.hpp"

#include <sys/uio.h>

#include <cerrno>
#include <cstring>

namespace rv::wal {

namespace {

// liburing devolve `-errno`; `Status::detail` é um `uint32_t`. Uma função para a conversão, e não
// um `static_cast` espalhado, porque errar o sinal aqui produz um número de erro sem sentido no
// relatório de fail-stop — que é o único lugar onde alguém vai ler isso.
[[nodiscard]] uint32_t errno_de(int rc) noexcept {
  return static_cast<uint32_t>(rc < 0 ? -rc : rc);
}

}  // namespace

UringBackend::~UringBackend() { close(); }

Status UringBackend::open(uint32_t entries) noexcept {
  if (open_) return Status::fail(Err::InvalidArgument);
  if (entries == 0) return Status::fail(Err::InvalidArgument);

  io_uring_params p{};
  p.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;

  int rc = ::io_uring_queue_init_params(entries, &ring_, &p);
  if (rc == 0) {
    defer_taskrun_ = true;
    single_issuer_ = true;
  } else {
    // Kernel antigo, contêiner com io_uring restrito, ou as flags recusadas juntas. Cair para o
    // ring simples é melhor que não subir: o motor perde a garantia de que o trabalho de
    // completion não interrompe o `apply`, e essa perda aparece no log e na métrica de cauda —
    // não vira silêncio.
    io_uring_params simples{};
    rc = ::io_uring_queue_init_params(entries, &ring_, &simples);
    if (rc < 0) return Status::fail(Err::IoError, errno_de(rc));
    defer_taskrun_ = false;
    single_issuer_ = false;
  }

  open_ = true;
  inflight_ = 0;
  for (Slot& s : slots_) s = Slot{};
  return kOk;
}

void UringBackend::close() noexcept {
  if (!open_) return;
  if (buffers_registered_) {
    (void)::io_uring_unregister_buffers(&ring_);
    buffers_registered_ = false;
  }
  if (files_registered_) {
    (void)::io_uring_unregister_files(&ring_);
    files_registered_ = false;
  }
  ::io_uring_queue_exit(&ring_);
  open_ = false;
}

Status UringBackend::register_files(std::span<const int> fds) noexcept {
  if (!open_) return Status::fail(Err::InvalidArgument);
  if (fds.empty()) return Status::fail(Err::InvalidArgument);
  if (files_registered_) {
    (void)::io_uring_unregister_files(&ring_);
    files_registered_ = false;
  }
  const int rc = ::io_uring_register_files(&ring_, fds.data(), static_cast<unsigned>(fds.size()));
  if (rc < 0) return Status::fail(Err::IoError, errno_de(rc));
  files_registered_ = true;
  return kOk;
}

Status UringBackend::register_buffers(std::span<const MutBytes> bufs) noexcept {
  if (!open_) return Status::fail(Err::InvalidArgument);
  if (bufs.empty() || bufs.size() > kMaxBuffers) {
    return Status::fail(Err::InvalidArgument, static_cast<uint32_t>(bufs.size()));
  }
  // Array na pilha, dimensionado em compilação: registrar buffers acontece no warm-up, mas mesmo
  // aí o motor não chama o alocador (CODING_RULES §1) — a regra não tem exceção "só desta vez".
  iovec iov[kMaxBuffers]{};
  for (size_t i = 0; i < bufs.size(); ++i) {
    if (bufs[i].data() == nullptr || bufs[i].empty()) {
      return Status::fail(Err::InvalidArgument, static_cast<uint32_t>(i));
    }
    iov[i].iov_base = static_cast<void*>(bufs[i].data());
    iov[i].iov_len = bufs[i].size();
  }
  if (buffers_registered_) {
    (void)::io_uring_unregister_buffers(&ring_);
    buffers_registered_ = false;
  }
  const int rc = ::io_uring_register_buffers(&ring_, iov, static_cast<unsigned>(bufs.size()));
  if (rc < 0) return Status::fail(Err::IoError, errno_de(rc));
  buffers_registered_ = true;
  return kOk;
}

Status UringBackend::submit(const WriteRequest& req) noexcept {
  if (!open_) return Status::fail(Err::InvalidArgument);
  if (inflight_ >= kMaxInflight) return Status::fail(Err::WalFull, inflight_);

  // Slot ANTES do SQE: se não houver onde anotar o tamanho pedido, é melhor não submeter do que
  // submeter uma escrita cuja completion não se sabe interpretar.
  uint32_t idx = kMaxInflight;
  for (uint32_t i = 0; i < kMaxInflight; ++i) {
    if (!slots_[i].used) { idx = i; break; }
  }
  if (idx == kMaxInflight) return Status::fail(Err::WalFull, inflight_);

  io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_);
  if (sqe == nullptr) return Status::fail(Err::WalFull, inflight_);

  // `write_fixed` + `IOSQE_FIXED_FILE`: nem o buffer nem o descritor são traduzidos por escrita.
  // O `const_cast` é o preço de uma API C que não distingue leitura de escrita na assinatura;
  // o kernel só lê deste ponteiro.
  void* buf = const_cast<void*>(static_cast<const void*>(req.buf));
  ::io_uring_prep_write_fixed(sqe, static_cast<int>(req.file_idx), buf, req.len, req.offset,
                              static_cast<int>(req.buf_idx));
  sqe->flags |= IOSQE_FIXED_FILE;
  ::io_uring_sqe_set_data64(sqe, req.token);

  const int rc = ::io_uring_submit(&ring_);
  if (rc < 0) return Status::fail(Err::IoError, errno_de(rc));

  slots_[idx] = Slot{req.token, req.len, true};
  ++inflight_;
  return kOk;
}

uint32_t UringBackend::take_expected(uint64_t token) noexcept {
  // O token é o `last_lsn` do grupo, estritamente crescente por partição (I8): não há dois
  // pedidos em voo com o mesmo. Busca linear em no máximo `kMaxInflight` entradas, uma vez por
  // grupo — medir isso seria medir ruído.
  for (Slot& s : slots_) {
    if (s.used && s.token == token) {
      const uint32_t e = s.expected;
      s.used = false;
      --inflight_;
      return e;
    }
  }
  return 0;  // CQE sem pedido correspondente: `expected == 0` faz `ok()` ser falso e o WAL para.
}

uint32_t UringBackend::reap(std::span<Completion> out) noexcept {
  if (!open_ || out.empty()) return 0;

  uint32_t n = 0;
  for (int volta = 0; volta < 2; ++volta) {
    while (n < out.size()) {
      io_uring_cqe* cqe = nullptr;
      if (::io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr) break;
      const uint64_t token = ::io_uring_cqe_get_data64(cqe);
      const int32_t res = cqe->res;
      ::io_uring_cqe_seen(&ring_, cqe);
      out[n] = Completion{token, res, take_expected(token)};
      ++n;
    }
    // Com DEFER_TASKRUN o trabalho de completion só roda quando ESTA thread entra no kernel.
    // Espiar a fila em memória pode, legitimamente, não ver nada que já terminou. Uma entrada
    // explícita resolve — e só na primeira volta vazia, para que o caminho normal (busy-poll com
    // CQEs prontos) não pague uma syscall por chamada.
    if (n > 0 || !defer_taskrun_ || volta == 1) break;
    if (::io_uring_get_events(&ring_) < 0) break;
  }
  return n;
}

}  // namespace rv::wal
