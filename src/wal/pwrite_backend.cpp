#include "wal/pwrite_backend.hpp"

#include <unistd.h>

#include <cerrno>

namespace rv::wal {

Status PwriteBackend::register_files(std::span<const int> fds) noexcept {
  if (fds.size() > kMaxFiles) return Status::fail(Err::InvalidArgument, static_cast<uint32_t>(fds.size()));
  n_files_ = 0;
  for (const int fd : fds) {
    if (fd < 0) return Status::fail(Err::InvalidArgument, n_files_);
    fds_[n_files_++] = fd;
  }
  return kOk;
}

Status PwriteBackend::register_buffers(std::span<const MutBytes> bufs) noexcept {
  if (bufs.size() > kMaxBuffers) return Status::fail(Err::InvalidArgument, static_cast<uint32_t>(bufs.size()));
  n_bufs_ = 0;
  for (const MutBytes& b : bufs) {
    if (b.data() == nullptr || b.empty()) return Status::fail(Err::InvalidArgument, n_bufs_);
    bufs_[n_bufs_++] = RegBuf{b.data(), b.size()};
  }
  return kOk;
}

Status PwriteBackend::submit(const WriteRequest& req) noexcept {
  if (count_ >= kMaxInflight) return Status::fail(Err::WalFull, count_);
  if (req.file_idx >= n_files_) return Status::fail(Err::InvalidArgument, req.file_idx);

  // A validação do buffer registrado é o ponto do arquivo que quase ninguém escreveria — o
  // `pwrite` funcionaria igual sem ela. Ela existe porque o io_uring EXIGE que `buf` esteja
  // dentro do buffer de índice `buf_idx`, e um índice trocado ali vira `EFAULT` em produção
  // depois de passar verde em todo teste feito com pwrite. O backend fácil tem de recusar o que
  // o backend difícil recusa; caso contrário ele não é um substituto, é uma armadilha.
  if (req.buf_idx >= n_bufs_) return Status::fail(Err::InvalidArgument, req.buf_idx);
  const RegBuf& rb = bufs_[req.buf_idx];
  const auto* fim = rb.base + rb.len;
  if (req.buf < rb.base || req.buf + req.len > fim) return Status::fail(Err::InvalidArgument, req.buf_idx);

  const ssize_t n = ::pwrite(fds_[req.file_idx], req.buf, req.len, static_cast<off_t>(req.offset));

  Completion c{};
  c.token = req.token;
  c.expected = req.len;
  // A convenção do CQE, reproduzida à mão: bytes escritos, ou `-errno`. Traduzir para `Status`
  // aqui obrigaria o WAL a ter dois caminhos de tratamento de erro, um por backend.
  c.res = (n < 0) ? -static_cast<int32_t>(errno) : static_cast<int32_t>(n);
  if (n > 0) bytes_written_ += static_cast<uint64_t>(n);

  ring_[(head_ + count_) % kMaxInflight] = c;
  ++count_;
  return kOk;
}

uint32_t PwriteBackend::reap(std::span<Completion> out) noexcept {
  uint32_t n = 0;
  while (n < out.size() && count_ > 0) {
    if (order_ == Order::Fifo) {
      out[n] = ring_[head_];
      head_ = (head_ + 1) % kMaxInflight;
    } else {
      out[n] = ring_[(head_ + count_ - 1) % kMaxInflight];
    }
    --count_;
    ++n;
  }
  return n;
}

}  // namespace rv::wal
