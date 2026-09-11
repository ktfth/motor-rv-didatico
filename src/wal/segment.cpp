#include "wal/segment.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "base/crc32c.hpp"

namespace rv::wal {

uint32_t format_segment_path(char* out, size_t cap, const char* dir, PartitionId part,
                             uint64_t first_lsn) noexcept {
  if (out == nullptr || cap == 0) return 0;
  const int n = std::snprintf(out, cap, "%s/p%u_%016llx.wal", dir ? dir : ".", part.v,
                              static_cast<unsigned long long>(first_lsn));
  return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<uint32_t>(n) : 0;
}

void Segment::move_from(Segment& o) noexcept {
  fd_ = o.fd_;
  first_lsn_ = o.first_lsn_;
  epoch_ = o.epoch_;
  capacity_ = o.capacity_;
  block_size_ = o.block_size_;
  std::memcpy(path_, o.path_, sizeof(path_));

  o.fd_ = -1;
  o.first_lsn_ = {};
  o.epoch_ = 0;
  o.capacity_ = 0;
  o.block_size_ = kFallbackBlock;
  o.path_[0] = '\0';
}

void Segment::close() noexcept {
  if (fd_ >= 0) {
    (void)::close(fd_);
    fd_ = -1;
  }
}

Status Segment::open_create(const char* dir, PartitionId part, Lsn first_lsn, uint32_t epoch,
                            uint64_t capacity_bytes, uint32_t block_size, BlockSource source,
                            bool direct) noexcept {
  close();
  if (format_segment_path(path_, sizeof(path_), dir, part, first_lsn.v) == 0) {
    return Status::fail(Err::InvalidArgument);
  }

  int flags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC;
  if (direct) {
    flags |= O_DIRECT | O_DSYNC;
  }

  fd_ = ::open(path_, flags, 0644);
  if (fd_ < 0 && direct) {
    // Fallback se O_DIRECT for recusado pelo filesystem (e.g. tmpfs)
    flags = O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC | O_DSYNC;
    fd_ = ::open(path_, flags, 0644);
  }
  if (fd_ < 0) {
    return Status::fail(Err::IoError);
  }

  alignas(kSegmentHdrBytes) std::byte hdr_buf[kSegmentHdrBytes]{};
  auto* sh = reinterpret_cast<SegmentHdr*>(hdr_buf);
  sh->magic = kSegmentMagic;
  sh->crc32c = 0;
  sh->format_version = kSegmentFormatVersion;
  sh->partition = static_cast<uint16_t>(part.v);
  sh->epoch = epoch;
  sh->first_lsn = first_lsn.v;
  sh->segment_bytes = capacity_bytes;
  sh->block_size = block_size;
  sh->block_source = static_cast<uint8_t>(source);
  sh->created_ts_ns = 0;

  const uint32_t crc = rv::crc32c(0, sh, sizeof(SegmentHdr));
  sh->crc32c = crc;

  const ssize_t w = ::pwrite(fd_, hdr_buf, kSegmentHdrBytes, 0);
  if (w != static_cast<ssize_t>(kSegmentHdrBytes)) {
    close();
    return Status::fail(Err::IoError);
  }

  first_lsn_ = first_lsn;
  epoch_ = epoch;
  capacity_ = capacity_bytes;
  block_size_ = block_size;
  return kOk;
}

Status Segment::open_existing(const char* path, bool direct) noexcept {
  close();
  if (path == nullptr || path[0] == '\0') {
    return Status::fail(Err::InvalidArgument);
  }
  std::strncpy(path_, path, sizeof(path_) - 1);
  path_[sizeof(path_) - 1] = '\0';

  int flags = O_RDWR | O_CLOEXEC;
  if (direct) {
    flags |= O_DIRECT | O_DSYNC;
  }

  fd_ = ::open(path_, flags, 0644);
  if (fd_ < 0 && direct) {
    flags = O_RDWR | O_CLOEXEC;
    fd_ = ::open(path_, flags, 0644);
  }
  if (fd_ < 0) {
    return Status::fail(Err::IoError);
  }

  alignas(kSegmentHdrBytes) std::byte hdr_buf[kSegmentHdrBytes]{};
  const ssize_t r = ::pread(fd_, hdr_buf, kSegmentHdrBytes, 0);
  if (r != static_cast<ssize_t>(kSegmentHdrBytes)) {
    close();
    return Status::fail(Err::IoError);
  }

  auto* sh = reinterpret_cast<SegmentHdr*>(hdr_buf);
  if (sh->magic != kSegmentMagic) {
    close();
    return Status::fail(Err::BadMagic);
  }
  if (sh->format_version != kSegmentFormatVersion) {
    close();
    return Status::fail(Err::BadMagic);
  }

  SegmentHdr check = *sh;
  check.crc32c = 0;
  const uint32_t crc = rv::crc32c(0, &check, sizeof(check));
  if (crc != sh->crc32c) {
    close();
    return Status::fail(Err::BadCrc);
  }

  first_lsn_ = Lsn{sh->first_lsn};
  epoch_ = sh->epoch;
  capacity_ = sh->segment_bytes;
  block_size_ = sh->block_size;
  return kOk;
}

}  // namespace rv::wal
