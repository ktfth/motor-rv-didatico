#pragma once
// Segmento do WAL (docs/wal.md).
//
// Cada segmento é um arquivo de tamanho fixo (1 GiB em produção), iniciado com SegmentHdr de
// 4096 bytes alinhado a 4096 bytes. Os registros começam no offset kSegmentHdrBytes.

#include <cstddef>
#include <cstdint>

#include "base/ids.hpp"
#include "base/status.hpp"
#include "wal/block_align.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {

inline constexpr size_t kMaxPathBytes = 256;

// Gera o caminho padronizado: "<dir>/p<part>_<lsn:016x>.wal"
[[nodiscard]] uint32_t format_segment_path(char* out, size_t cap, const char* dir, PartitionId part,
                                           uint64_t first_lsn) noexcept;

class Segment {
 public:
  Segment() noexcept = default;
  ~Segment() noexcept { close(); }

  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

  Segment(Segment&& o) noexcept { move_from(o); }
  Segment& operator=(Segment&& o) noexcept {
    if (this != &o) {
      close();
      move_from(o);
    }
    return *this;
  }

  // Cria um novo segmento, formata o caminho e grava o SegmentHdr inicial no offset 0.
  [[nodiscard]] Status open_create(const char* dir, PartitionId part, Lsn first_lsn, uint32_t epoch,
                                   uint64_t capacity_bytes, uint32_t block_size, BlockSource source,
                                   bool direct = false) noexcept;

  // Abre um segmento existente e valida o cabeçalho.
  [[nodiscard]] Status open_existing(const char* path, bool direct = false) noexcept;

  void close() noexcept;

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] Lsn first_lsn() const noexcept { return first_lsn_; }
  [[nodiscard]] uint32_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] uint32_t block_size() const noexcept { return block_size_; }
  [[nodiscard]] const char* path() const noexcept { return path_; }

 private:
  void move_from(Segment& o) noexcept;

  int fd_ = -1;
  Lsn first_lsn_{};
  uint32_t epoch_ = 0;
  uint64_t capacity_ = 0;
  uint32_t block_size_ = kFallbackBlock;
  char path_[kMaxPathBytes]{};
};

}  // namespace rv::wal
