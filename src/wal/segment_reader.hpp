#pragma once
// Leitor sequencial de segmentos do WAL (docs/wal.md, contratos-internos.md).
//
// Valida o SegmentHdr e cada registro (magic, epoch, CRC32C, LSN contínuo).
// Realiza travessia de padding de blocos entre grupos de commit (ADR-0013).

#include <cstddef>
#include <cstdint>

#include "base/bytes.hpp"
#include "base/ids.hpp"
#include "base/status.hpp"
#include "core/apply.hpp"
#include "wal/wal_format.hpp"

namespace rv::wal {

enum class ReplayStopReason : uint8_t {
  Clean = 0,
  BadMagic,
  BadCrc,
  LsnGap,
  EpochMismatch,
  SegmentDiscontinuity,
  ShortSegment
};

[[nodiscard]] const char* to_string(ReplayStopReason r) noexcept;

class SegmentReader {
 public:
  SegmentReader() noexcept = default;

  [[nodiscard]] Status open(ByteSpan mapped, Lsn expected_first = Lsn{}) noexcept;

  // Devolve true se leu um registro válido em `ev`, false no fim do log ou falha.
  [[nodiscard]] Result<bool> next(core::EventView& ev) noexcept;

  [[nodiscard]] ReplayStopReason stop_reason() const noexcept { return stop_reason_; }
  [[nodiscard]] uint64_t resume_offset() const noexcept { return resume_offset_; }
  [[nodiscard]] Lsn last_valid_lsn() const noexcept { return last_valid_lsn_; }
  [[nodiscard]] Lsn first_lsn() const noexcept { return first_lsn_; }
  [[nodiscard]] uint32_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] uint32_t block_size() const noexcept { return block_size_; }

 private:
  ByteSpan mapped_{};
  uint64_t offset_ = kSegmentHdrBytes;
  uint64_t resume_offset_ = kSegmentHdrBytes;
  uint32_t block_size_ = kFallbackBlock;
  uint32_t epoch_ = 0;
  Lsn first_lsn_{};
  Lsn expected_lsn_{};
  Lsn last_valid_lsn_{};
  ReplayStopReason stop_reason_ = ReplayStopReason::Clean;
};

}  // namespace rv::wal
