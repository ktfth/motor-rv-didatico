#include "wal/segment_reader.hpp"

#include <cstring>

#include "base/crc32c.hpp"

namespace rv::wal {

const char* to_string(ReplayStopReason r) noexcept {
  switch (r) {
    case ReplayStopReason::Clean:
      return "Clean";
    case ReplayStopReason::BadMagic:
      return "BadMagic";
    case ReplayStopReason::BadCrc:
      return "BadCrc";
    case ReplayStopReason::LsnGap:
      return "LsnGap";
    case ReplayStopReason::EpochMismatch:
      return "EpochMismatch";
    case ReplayStopReason::SegmentDiscontinuity:
      return "SegmentDiscontinuity";
    case ReplayStopReason::ShortSegment:
      return "ShortSegment";
  }
  return "Unknown";
}

Status SegmentReader::open(ByteSpan mapped, Lsn expected_first) noexcept {
  mapped_ = mapped;
  offset_ = kSegmentHdrBytes;
  resume_offset_ = kSegmentHdrBytes;
  stop_reason_ = ReplayStopReason::Clean;

  if (mapped_.size() < kSegmentHdrBytes) {
    stop_reason_ = ReplayStopReason::ShortSegment;
    return Status::fail(Err::BadMagic);
  }

  const auto* sh = reinterpret_cast<const SegmentHdr*>(mapped_.data());
  if (sh->magic != kSegmentMagic) {
    stop_reason_ = ReplayStopReason::BadMagic;
    return Status::fail(Err::BadMagic);
  }
  if (sh->format_version != kSegmentFormatVersion) {
    stop_reason_ = ReplayStopReason::BadMagic;
    return Status::fail(Err::BadMagic);
  }

  SegmentHdr check = *sh;
  check.crc32c = 0;
  const uint32_t crc = rv::crc32c(0, &check, sizeof(check));
  if (crc != sh->crc32c) {
    stop_reason_ = ReplayStopReason::BadCrc;
    return Status::fail(Err::BadCrc);
  }

  epoch_ = sh->epoch;
  first_lsn_ = Lsn{sh->first_lsn};
  block_size_ = is_valid_block(sh->block_size) ? sh->block_size : kFallbackBlock;

  if (expected_first.v != 0 && first_lsn_.v > expected_first.v) {
    stop_reason_ = ReplayStopReason::SegmentDiscontinuity;
    return Status::fail(Err::LsnGap);
  }

  expected_lsn_ = first_lsn_;
  last_valid_lsn_ = first_lsn_.v > 1 ? Lsn{first_lsn_.v - 1} : Lsn{};

  return kOk;
}

Result<bool> SegmentReader::next(core::EventView& ev) noexcept {
  while (offset_ < mapped_.size()) {
    const size_t rem = mapped_.size() - offset_;
    if (rem < sizeof(WalHdr)) {
      // Menor que um cabeçalho: se for tudo zero até o fim do mapeamento, é fim limpo
      bool all_zeros = true;
      for (size_t i = 0; i < rem; ++i) {
        if (mapped_.data()[offset_ + i] != std::byte{0}) {
          all_zeros = false;
          break;
        }
      }
      if (all_zeros) {
        stop_reason_ = ReplayStopReason::Clean;
        return false;
      }
      stop_reason_ = ReplayStopReason::ShortSegment;
      return false;
    }

    // Checa se os primeiros 8 bytes são zeros
    const uint64_t w0 = load_le<uint64_t>(mapped_.data() + offset_);
    if (w0 == 0) {
      if ((offset_ % block_size_) == 0) {
        // Bloco alinhado com zeros: início da região pré-alocada/pré-zerada
        stop_reason_ = ReplayStopReason::Clean;
        return false;
      }
      // Padding do grupo anterior: avança até a próxima fronteira de bloco
      offset_ = pad_to_block(offset_, block_size_);
      continue;
    }

    const uint32_t magic = load_le<uint32_t>(mapped_.data() + offset_);
    if (magic != kWalMagic) {
      if ((offset_ % block_size_) != 0) {
        // Lixo ou padding desalinhado: tenta o próximo bloco
        offset_ = pad_to_block(offset_, block_size_);
        continue;
      }
      stop_reason_ = ReplayStopReason::BadMagic;
      return false;
    }

    WalHdr h{};
    std::memcpy(&h, mapped_.data() + offset_, sizeof(h));

    if (h.epoch != epoch_) {
      stop_reason_ = ReplayStopReason::EpochMismatch;
      return false;
    }

    if (h.len > kMaxPayload) {
      stop_reason_ = ReplayStopReason::BadMagic;
      return false;
    }

    const uint32_t rec_bytes = record_bytes(h.len);
    if (offset_ + rec_bytes > mapped_.size()) {
      stop_reason_ = ReplayStopReason::ShortSegment;
      return false;
    }

    // Conferência do CRC32C
    WalHdr check = h;
    check.crc32c = 0;
    uint32_t crc = rv::crc32c(0, &check, sizeof(check));
    crc = rv::crc32c(crc, mapped_.data() + offset_ + sizeof(WalHdr), h.len);
    if (crc != h.crc32c) {
      stop_reason_ = ReplayStopReason::BadCrc;
      return false;
    }

    // Conferência da continuidade do LSN (I8)
    if (expected_lsn_.v != 0 && h.lsn != expected_lsn_.v) {
      stop_reason_ = ReplayStopReason::LsnGap;
      return false;
    }

    // Registro validado com sucesso
    ev.lsn = Lsn{h.lsn};
    ev.ts_ns = h.ts_ns;
    ev.payload = mapped_.data() + offset_ + sizeof(WalHdr);
    ev.tmpl = h.tmpl;
    ev.len = h.len;
    ev.reserved = 0;

    last_valid_lsn_ = Lsn{h.lsn};
    expected_lsn_ = Lsn{h.lsn + 1};
    offset_ += rec_bytes;
    resume_offset_ = pad_to_block(offset_, block_size_);
    return true;
  }

  stop_reason_ = ReplayStopReason::Clean;
  return false;
}

}  // namespace rv::wal
